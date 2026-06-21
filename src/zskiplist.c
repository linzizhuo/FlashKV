#include <stdlib.h>

#include "zskiplist.h"

/* ======================== 内部辅助 ======================== */

/* 掷硬币决定新节点高度，p=0.25，最高 32 层 */
static int zslRandomLevel(void)
{
    int level = 1;
    while ((random() & 0xFFFF) < (ZSKIPLIST_P * 0xFFFF))
        level++;
    return (level < ZSKIPLIST_MAXLEVEL) ? level : ZSKIPLIST_MAXLEVEL;
}

/* 分配并初始化一个跳表节点（接管 sds 所有权） */
static zskiplistNode *zslCreateNode(int level, double score, sds ele)
{
    zskiplistNode *zn = malloc(sizeof(zskiplistNode) +
                               level * sizeof(struct zskiplistLevel));
    if (!zn) return NULL;
    zn->score = score;
    zn->ele = ele;
    zn->backward = NULL;
    /* forward/span 由调用方 zslinsert 覆盖，此处不初始化 */
    return zn;
}

/*
 * 搜索：找到 target (score, ele) 的前驱节点。
 *
 *   - mode = ZSL_SEARCH_LT : 停在 predecessor（insert/delete 用）
 *   - mode = ZSL_SEARCH_LE : 可越过等值节点（rank 用）
 *
 * 结果写入 update[0..zsl->level-1]。若 rank 非 NULL，则填入各层累计跨度。
 */
typedef enum { ZSL_SEARCH_LT, ZSL_SEARCH_LE } zslSearchMode;

static void zslSearch(zskiplist *zsl, double score, sds ele,
                      zskiplistNode **update, unsigned long *rank,
                      zslSearchMode mode)
{
    zskiplistNode *x = zsl->header;
    int cmp_op = (mode == ZSL_SEARCH_LE) ? 0 : -1; /* sdsCompare 返回值阈值 */

    for (int i = zsl->level - 1; i >= 0; i--) {
        if (rank) rank[i] = (i == zsl->level - 1) ? 0 : rank[i + 1];
        while (x->level[i].forward) {
            zskiplistNode *n = x->level[i].forward;
            if (n->score < score) {
                if (rank) rank[i] += x->level[i].span;
                x = n;
            } else if (n->score == score &&
                       sdsCompare(n->ele, ele) <= cmp_op) {
                if (rank) rank[i] += x->level[i].span;
                x = n;
            } else {
                break;
            }
        }
        update[i] = x;
    }
}

/* 内部函数：从跳表中摘除节点 x，update[] 需已经过搜索 */
static void zslDeleteNode(zskiplist *zsl, zskiplistNode *x, zskiplistNode **update)
{
    for (int i = 0; i < zsl->level; i++) {
        if (update[i]->level[i].forward == x) {
            update[i]->level[i].span += x->level[i].span - 1;
            update[i]->level[i].forward = x->level[i].forward;
        } else {
            update[i]->level[i].span--;
        }
    }

    if (x->level[0].forward) {
        x->level[0].forward->backward = x->backward;
    } else {
        zsl->tail = x->backward;
    }

    while (zsl->level > 1 &&
           zsl->header->level[zsl->level - 1].forward == NULL) {
        zsl->level--;
    }

    zsl->length--;
}

/* ======================== 公开 API ======================== */

zskiplist *zslnew(void)
{
    zskiplist *zsl = malloc(sizeof(*zsl));
    if (!zsl) return NULL;

    zsl->level = 1;
    zsl->length = 0;
    zsl->tail = NULL;

    zsl->header = malloc(sizeof(zskiplistNode) +
                         ZSKIPLIST_MAXLEVEL * sizeof(struct zskiplistLevel));
    if (!zsl->header) {
        free(zsl);
        return NULL;
    }

    zsl->header->score = 0;
    zsl->header->ele = NULL;
    zsl->header->backward = NULL;
    for (int i = 0; i < ZSKIPLIST_MAXLEVEL; i++) {
        zsl->header->level[i].forward = NULL;
        zsl->header->level[i].span = 0;
    }

    return zsl;
}

void zslfree(zskiplist *zsl)
{
    if (!zsl) return;

    zskiplistNode *node = zsl->header->level[0].forward;
    zskiplistNode *next;

    while (node) {
        next = node->level[0].forward;
        sdsfree(node->ele);
        free(node);
        node = next;
    }

    free(zsl->header);
    free(zsl);
}

zskiplistNode *zslinsert(zskiplist *zsl, double score, sds ele)
{
    if (!zsl || !ele) return NULL;

    zskiplistNode *update[ZSKIPLIST_MAXLEVEL];
    unsigned long rank[ZSKIPLIST_MAXLEVEL];

    /* 1. 搜索前驱 */
    zslSearch(zsl, score, ele, update, rank, ZSL_SEARCH_LT);

    /* 2. 已存在相同 (score, ele) → 释放传入的 ele，返回已有节点 */
    zskiplistNode *x = update[0]->level[0].forward;
    if (x && x->score == score && sdsCompare(x->ele, ele) == 0) {
        sdsfree(ele);
        return x;
    }

    /* 3. 掷硬币 + 先分配节点 */
    int level = zslRandomLevel();
    x = zslCreateNode(level, score, ele);
    if (!x) {
        sdsfree(ele);
        return NULL;
    }

    /* 4. 分配成功后才修改跳表状态：提升 level */
    if (level > zsl->level) {
        for (int i = zsl->level; i < level; i++) {
            rank[i] = 0;
            update[i] = zsl->header;
            update[i]->level[i].span = zsl->length;
        }
        zsl->level = level;
    }

    /* 5. 插入各层，更新 forward + span */
    for (int i = 0; i < level; i++) {
        x->level[i].forward = update[i]->level[i].forward;
        update[i]->level[i].forward = x;
        x->level[i].span = update[i]->level[i].span - (rank[0] - rank[i]);
        update[i]->level[i].span = (rank[0] - rank[i]) + 1;
    }
    for (int i = level; i < zsl->level; i++) {
        update[i]->level[i].span++;
    }

    /* 6. 修复 backward + tail */
    x->backward = (update[0] == zsl->header) ? NULL : update[0];
    if (x->level[0].forward) {
        x->level[0].forward->backward = x;
    } else {
        zsl->tail = x;
    }

    zsl->length++;
    return x;
}

int zsldel(zskiplist *zsl, double score, sds ele)
{
    if (!zsl || !ele) return 0;

    zskiplistNode *update[ZSKIPLIST_MAXLEVEL];

    zslSearch(zsl, score, ele, update, NULL, ZSL_SEARCH_LT);

    zskiplistNode *x = update[0]->level[0].forward;
    if (!x || x->score != score || sdsCompare(x->ele, ele) != 0) {
        return 0;
    }

    zslDeleteNode(zsl, x, update);
    sdsfree(x->ele);
    free(x);
    return 1;
}

unsigned long zslrank(zskiplist *zsl, double score, sds ele)
{
    if (!zsl || !ele) return 0;

    zskiplistNode *update[ZSKIPLIST_MAXLEVEL];
    unsigned long rank[ZSKIPLIST_MAXLEVEL];

    zslSearch(zsl, score, ele, update, rank, ZSL_SEARCH_LE);

    /* LE 模式下 update[0] 可能就是目标自身 */
    if (update[0]->ele && sdsCompare(update[0]->ele, ele) == 0 &&
        update[0]->score == score) {
        return rank[0];
    }

    return 0;
}

zskiplistNode *zslbyrank(zskiplist *zsl, unsigned long rank)
{
    if (!zsl || rank == 0 || rank > zsl->length) return NULL;

    zskiplistNode *x = zsl->header;
    unsigned long traversed = 0;

    for (int i = zsl->level - 1; i >= 0; i--) {
        while (x->level[i].forward && traversed + x->level[i].span <= rank) {
            traversed += x->level[i].span;
            x = x->level[i].forward;
        }
        if (traversed == rank) {
            return x;
        }
    }

    return NULL;
}

unsigned long zslcount(zskiplist *zsl, double min, double max)
{
    if (!zsl || min > max) return 0;

    /* 找到第一个 score >= min 的节点 */
    zskiplistNode *x = zsl->header;
    for (int i = zsl->level - 1; i >= 0; i--) {
        while (x->level[i].forward && x->level[i].forward->score < min) {
            x = x->level[i].forward;
        }
    }

    x = x->level[0].forward;
    if (!x || x->score > max) return 0;

    unsigned long count = 0;
    while (x && x->score <= max) {
        count++;
        x = x->level[0].forward;
    }

    return count;
}

unsigned long zsldelrange(zskiplist *zsl, double min, double max)
{
    if (!zsl || min > max) return 0;

    zskiplistNode *update[ZSKIPLIST_MAXLEVEL];
    zskiplistNode *x;
    unsigned long removed = 0;

    x = zsl->header;
    for (int i = zsl->level - 1; i >= 0; i--) {
        while (x->level[i].forward && x->level[i].forward->score < min) {
            x = x->level[i].forward;
        }
        update[i] = x;
    }

    x = x->level[0].forward;

    while (x && x->score <= max) {
        zskiplistNode *next = x->level[0].forward;
        zslDeleteNode(zsl, x, update);
        sdsfree(x->ele);
        free(x);
        removed++;
        x = next;
    }

    return removed;
}