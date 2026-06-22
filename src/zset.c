#include <stdlib.h>

#include "zset.h"
#include "sds.h"

/* ======================== DictType：member → zskiplistNode* ========================
 *
 * key = sds (member 字符串), val = zskiplistNode *
 * keyFree / valFree 均为 NULL —— 内存由 skiplist 统一管理，dict 只持引用。
 * 释放顺序必须先 dictDelete 再 zsldel（否则 zsldel free sds 后 dict key 悬空）。
 */

static struct dictType zsetDictType = {
    .hash       = sdsHash,
    .keyCompare = sdsCompare,
    .keyFree    = NULL,          /* skiplist owns sds */
    .valFree    = NULL,          /* skiplist owns node */
    .valGet     = dictValGetPtr
};

/* ======================== 生命周期 ======================== */

zset *zsetNew(void)
{
    zset *zs = malloc(sizeof(*zs));
    if (!zs) return NULL;

    zs->zsl = zslnew();
    if (!zs->zsl) { free(zs); return NULL; }

    zs->dict = dictnew(4, &zsetDictType);   /* 初始 16 槽 */
    if (!zs->dict) { zslfree(zs->zsl); free(zs); return NULL; }

    return zs;
}

void zsetFree(zset *zs)
{
    if (!zs) return;
    /* 先释放 dict（不 free key/val，只摘 entry），再释放 skiplist（真正 free sds+node） */
    dictfree(zs->dict);
    zslfree(zs->zsl);
    free(zs);
}

/* ======================== 核心操作 ======================== */

/* 插入或更新 (score, ele)。接管 sds 所有权。
 * 返回 1=新增 member, 0=member 已存在（score 相同或已更新） */
int zsetAdd(zset *zs, double score, sds ele)
{
    hash_t h = sdsHash(ele);
    /* dictfind 返回 valGet(entry) = entry->val = zskiplistNode * */
    zskiplistNode *old = (zskiplistNode *)dictfind(zs->dict, ele, (void *)&h);

    if (old) {
        /* --- member 已存在 --- */
        if (old->score == score) {
            /* score 相同 → 无操作 */
            sdsfree(ele);
            return 0;
        }

        /* score 不同 → 更新。顺序：先摘 dict entry，再删旧 skiplist node，最后插新。 */
        dictDelete(zs->dict, ele, (void *)&h);
        zsldel(zs->zsl, old->score, old->ele);       /* 释放旧 sds + old node */

        zskiplistNode *node = zslinsert(zs->zsl, score, ele);
        if (!node) {
            /* OOM：ele 已由 zslinsert 释放；旧数据已丢，无力恢复 */
            return 0;
        }
        dictAdd(zs->dict, ele, node, (void *)&h);
        return 0;
    }

    /* --- member 不存在，全新插入 --- */
    zskiplistNode *node = zslinsert(zs->zsl, score, ele);
    if (!node) {
        /* OOM：ele 已由 zslinsert 释放 */
        return 0;
    }

    if (dictAdd(zs->dict, ele, node, (void *)&h) != DICT_OK) {
        /* 理论上不会发生（刚查过不存在），防御性回滚 */
        zsldel(zs->zsl, score, ele);
        return 0;
    }

    return 1;
}

/* 删除 member。返回 1=成功, 0=不存在 */
int zsetDel(zset *zs, sds ele)
{
    hash_t h = sdsHash(ele);
    /* dictfind 返回 valGet(entry) = entry->val = zskiplistNode * */
    zskiplistNode *node = (zskiplistNode *)dictfind(zs->dict, ele, (void *)&h);
    if (!node) return 0;

    /* 顺序：先摘 dict（不 free key/val），再 free skiplist node */
    dictDelete(zs->dict, ele, (void *)&h);
    zsldel(zs->zsl, node->score, node->ele);

    return 1;
}

/* O(1) 查找 member。返回 node 或 NULL */
zskiplistNode *zsetFind(zset *zs, sds ele)
{
    hash_t h = sdsHash(ele);
    /* dictfind 返回 valGet(entry) = entry->val = zskiplistNode * */
    return (zskiplistNode *)dictfind(zs->dict, ele, (void *)&h);
}

/* ======================== 查询 (透传 skiplist) ======================== */

unsigned long zsetRank(zset *zs, sds ele)
{
    zskiplistNode *node = zsetFind(zs, ele);
    if (!node) return 0;
    return zslrank(zs->zsl, node->score, ele);
}

zskiplistNode *zsetByRank(zset *zs, unsigned long rank)
{
    return zslbyrank(zs->zsl, rank);
}

unsigned long zsetCount(zset *zs, double min, double max)
{
    return zslcount(zs->zsl, min, max);
}

unsigned long zsetDelRange(zset *zs, double min, double max)
{
    if (!zs || min > max) return 0;

    /* 遍历 L0 score 区间，逐一调用 zsetDel 同步清理 dict+skiplist */
    zskiplistNode *x = zs->zsl->header;
    for (int i = zs->zsl->level - 1; i >= 0; i--) {
        while (x->level[i].forward && x->level[i].forward->score < min)
            x = x->level[i].forward;
    }
    x = x->level[0].forward;

    unsigned long removed = 0;
    while (x && x->score <= max) {
        zskiplistNode *next = x->level[0].forward;
        sds member = x->ele;

        /* 先摘 dict entry，再删 skiplist node（zsldel 内部 free sds+node） */
        hash_t h = sdsHash(member);
        dictDelete(zs->dict, member, (void *)&h);
        zsldel(zs->zsl, x->score, member);

        removed++;
        x = next;
    }
    return removed;
}

unsigned long zsetLen(zset *zs)
{
    return zs->zsl->length;
}

zskiplistNode **zsetRange(zset *zs, double min, double max, unsigned long *count)
{
    return zslrange(zs->zsl, min, max, count);
}
