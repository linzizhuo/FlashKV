#define _POSIX_C_SOURCE 199309L   /* clock_gettime, CLOCK_MONOTONIC */

#include "kvdb.h"
#include "dict_type.h"
#include "ttl.h"
#include "sds.h"

#include <stdbool.h>
#include <stdlib.h>

#define DICT_HT_INITIAL_SIZE 4

struct kvdb
{
    struct dict *dict;     /* 主存储：key → ValObj */
    struct dict *expires;  /* TTL 字典：key → 绝对秒时间戳（inline） */
};

/* ---- 内部：惰性删除 ---- */

static bool expireIfNeeded(kvdb *kv, const void *key, hash_t *h)
{
    tstamp_t *when = keyTtlFind(kv->expires, key, *h);
    if (!when) return false;
    if ((time_t)(*when) >= time(NULL)) return false;

    dictDelete(kv->expires, key, h);
    dictDelete(kv->dict,   key, h);
    return true;
}

/* ---- 生命周期 ---- */

kvdb *kvdbNew(void)
{
    kvdb *kv = malloc(sizeof(*kv));
    if (!kv) return NULL;

    kv->dict    = dictnew(DICT_HT_INITIAL_SIZE, &dictTypeSds);
    kv->expires = dictnew(DICT_HT_INITIAL_SIZE, &dictTTL);

    if (!kv->dict || !kv->expires) {
        dictfree(kv->dict);
        dictfree(kv->expires);
        free(kv);
        return NULL;
    }
    return kv;
}

void kvdbFree(kvdb *kv)
{
    if (!kv) return;
    dictfree(kv->dict);
    dictfree(kv->expires);
    free(kv);
}

/* ---- key-value ---- */

ValObj *kvdbGet(kvdb *kv, const void *key)
{
    hash_t h = kv->dict->type->hash(key);
    expireIfNeeded(kv, key, &h);
    return dictfind(kv->dict, key, &h);
}

ValObj *kvdbSet(kvdb *kv, const void *key, ValObj *val)
{
    hash_t h = kv->dict->type->hash(key);
    ValObj *old = dictfind(kv->dict, key, &h);

    if (old) {
        /* key 已存在：原地覆写值，不产生新 key */
        dictReplace(kv->dict, (void *)key, val, &h);
    } else {
        /* 新 key：dup 后插入，kvdb 拥有 dupkey */
        sds dupkey = sdsdup((sds)key);
        if (!dupkey) return NULL;
        if (dictAdd(kv->dict, dupkey, val, &h) != DICT_OK) {
            /* rehash 间 key 被搬走了，极少发生 */
            sdsfree(dupkey);
            dictReplace(kv->dict, (void *)key, val, &h);
        }
    }

    dictDelete(kv->expires, key, &h);   /* SET 覆盖 → 清除旧 TTL */
    return old;
}

int kvdbDel(kvdb *kv, const void *key)
{
    hash_t h = kv->dict->type->hash(key);
    int ret = dictDelete(kv->dict, key, &h);
    dictDelete(kv->expires, key, &h);   /* 顺手清 TTL */
    return ret == DICT_OK ? 1 : 0;
}

int kvdbExists(kvdb *kv, const void *key)
{
    hash_t h = kv->dict->type->hash(key);
    expireIfNeeded(kv, key, &h);
    return dictfind(kv->dict, key, &h) ? 1 : 0;
}

/* ---- TTL ---- */

int kvdbExpire(kvdb *kv, const void *key, time_t when)
{
    hash_t h = kv->dict->type->hash(key);

    if (!dictfind(kv->dict, key, &h))
        return 0;   /* key 不存在 */

    tstamp_t *old = keyTtlFind(kv->expires, key, h);
    if (old) {
        *old = (tstamp_t)when;              /* 原地更新 */
    } else {
        sds expkey = sdsdup((sds)key);      /* expires 持独立 key */
        if (!expkey) return 0;
        dictAdd(kv->expires, expkey, (void *)when, &h);
    }
    return 1;
}

long long kvdbTTL(kvdb *kv, const void *key)
{
    hash_t h = kv->dict->type->hash(key);

    expireIfNeeded(kv, key, &h);
    if (!dictfind(kv->dict, key, &h))
        return -2;   /* 不存在或已过期 */

    tstamp_t *when = keyTtlFind(kv->expires, key, h);
    if (!when)
        return -1;   /* 无 TTL */

    time_t remain = (time_t)(*when) - time(NULL);
    return remain > 0 ? (long long)remain : 0;
}

int kvdbPersist(kvdb *kv, const void *key)
{
    hash_t h = kv->dict->type->hash(key);

    if (!dictfind(kv->dict, key, &h))
        return 0;
    if (!keyTtlFind(kv->expires, key, h))
        return 0;

    dictDelete(kv->expires, key, &h);
    return 1;
}

/* ---- 定期抽样删除过期 key ---- */

#define ACTIVE_EXPIRE_LOOKUPS  20   /* 每轮采样数 */
#define ACTIVE_EXPIRE_MAX_LOOPS 16  /* 最多轮数 */
#define ACTIVE_EXPIRE_THRESHOLD 5   /* 过期数 < 此值退出 (LOOKUPS * 0.25) */
#define ACTIVE_EXPIRE_TIME_LIMIT 1000 /* 单次最大耗时 (us)，避免阻塞事件循环 */

static long long ustime(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

void kvdbActiveExpireCycle(kvdb *kv)
{
    /* 快速路径：无 TTL 的 key */
    if (kv->expires->ht[0].used == 0 && kv->expires->ht[1].used == 0)
        return;

    time_t now = time(NULL);
    long long start = ustime();

    for (int loop = 0; loop < ACTIVE_EXPIRE_MAX_LOOPS; loop++) {
        int expired = 0;

        for (int i = 0; i < ACTIVE_EXPIRE_LOOKUPS; i++) {
            dictEntry *de = dictGetRandomKey(kv->expires);
            if (!de) goto done;  /* expires 被删空 */

            sds key = (sds)dictEntryGetKey(de);
            tstamp_t *when = (tstamp_t *)dictEntryGetVal(kv->expires, de);

            if (now >= (time_t)(*when)) {
                dictDelete(kv->expires, key, NULL);
                dictDelete(kv->dict, key, NULL);
                expired++;
            }
        }

        /* 过期比例 < 25%：没必要继续挖 */
        if (expired < ACTIVE_EXPIRE_THRESHOLD)
            break;

        /* 超时保护：不阻塞事件循环超过 TIME_LIMIT us */
        if (ustime() - start > ACTIVE_EXPIRE_TIME_LIMIT)
            break;
    }
done:;
}

/* 填充率 < 10% 时缩容，对主 dict 和 expires dict 对称处理。
 * 与 kvdbActiveExpireCycle 在同一 cron 频率被调用 (100ms, 轮转 DB)。
 *
 * 注意：dictShrink 内部走 dictExpand → rehash，搬迁仍由
 * 后续读写操作的 dictRehashData(d,1) 渐进完成，不会在这里阻塞。 */
void kvdbTryResize(kvdb *kv)
{
    if (dictNeedsResize(kv->dict))
        dictShrink(kv->dict);
    if (dictNeedsResize(kv->expires))
        dictShrink(kv->expires);
}

/* ---- ZSET ---- */

zset *kvdbGetZset(kvdb *kv, const void *key, int *found)
{
    ValObj *obj = kvdbGet(kv, key);
    if (!obj) {
        if (found) *found = 0;
        return NULL;
    }
    if (obj->type != VAL_ZSET) {
        if (found) *found = -1;
        return NULL;
    }
    if (found) *found = 1;
    return obj->val.zs;
}

zset *kvdbGetOrCreateZset(kvdb *kv, const void *key)
{
    ValObj *obj = kvdbGet(kv, key);
    if (obj) {
        if (obj->type != VAL_ZSET) return NULL;
        return obj->val.zs;
    }

    /* key 不存在：创建 zset 并写入 */
    ValObj *newobj = valObjCreateZset();
    if (!newobj) return NULL;

    ValObj *old = kvdbSet(kv, key, newobj);
    if (old) valObjFree(old);     /* 理论上不可能，防御性释放 */

    return newobj->val.zs;
}
