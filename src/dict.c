#include "dict.h"
#include "sds.h"
#include <stddef.h>
#include <stdlib.h>
#define ROTL64(x, r) (((x) << (r)) | ((x) >> (64 - (r))))
#define MIN(a, b) (((a) < (b)) ? (a) : (b))
#define DICT_RANDOM_BUF_LEN 16  /* dictGetRandomKey 栈缓存容量，>此长度走兜底 */

static void dicthtfree(struct dict *d, struct dictht *dht);


struct dictEntry
{
    hash_t hash; // 缓存 hash，rehash 直接 & sizemask
    void *key;
    void *val;
    struct dictEntry *next;
};
static dictEntry *dictEntryNew(hash_t hash, void *key, void *val, struct dictEntry *next)
{
    dictEntry *entry = (dictEntry *)malloc(sizeof(struct dictEntry));
    entry->hash = hash;
    entry->key  = key;
    entry->val  = val;
    entry->next = next;
    return entry;
}
// static uint64_t dictHash(const struct dict *d, const void *key)
// {
//     /* 暂时逻辑是固定的，后面写了泛型会调用泛型中的函数，保证扩展性。 */
//     return dictSdsHash(key);
//     /* 完善泛型后会改成 d->dicttype->某一个函数指针*/
// }
/* 比较两个key */
// static int dictKeyCmp(const struct dict *d, const void *key1, const void *key2)
// {
// }
// dict.c - 实现（知道 entry 布局）
void *dictValGetPtr(struct dictEntry *entry) { return entry->val; }
void *dictValGetRef(struct dictEntry *entry) { return &entry->val; }

static void dicthtInit(struct dict* d, struct dictht* dht, unsigned long n)
{
    if(dht->table)
    {
        dicthtfree(d, dht);
        dht->table = NULL;
    }
    dht->table = calloc(n, sizeof(dictEntry *)); // ← 少了这个
    dht->size = n;
    dht->sizemask = n - 1;
    dht->used = 0;
}
// rehash函数...
// 通过逻辑设计当触发rehash时表1一定是空的
static int dictRehash(struct dict* d)
{
    if (d->rehashidx >= 0)
        return DICT_ERROR; // 已经在 rehash

    dicthtInit(d, &d->ht[1], d->ht[0].size << 1);
    d->rehashidx = 0;
    return DICT_OK;
}
/* ---- 收尾：ht[1] 顶替 ht[0] ---- */
static void dictRehashComplete(struct dict *d)
{
    d->rehashidx = -1;
    dicthtfree(d, d->ht);        /* 旧桶数组已无节点 */
    d->ht[0] = d->ht[1];
    d->ht[1].table = NULL;
    d->ht[1].size = d->ht[1].sizemask = d->ht[1].used = 0;
}
/* 以桶槽为单位搬迁 —— 搬 number 个桶槽（含空桶），适合批量预加载 */
static int dictRehashStep(struct dict *d, unsigned long number)
{
    unsigned long begin = d->rehashidx, end = MIN(d->ht[0].size, (unsigned long)d->rehashidx + number);
    for (unsigned long idx = begin; idx < end; idx++)
    {
        struct dictEntry *entry = d->ht[0].table[idx];
        while(entry)
        {
            struct dictEntry *next = entry->next;
            unsigned long val = entry->hash & d->ht[1].sizemask;

            entry->next = d->ht[1].table[val];
            d->ht[1].table[val] = entry;
            entry = next;
            d->ht[0].used--;
            d->ht[1].used++;
        }
        d->ht[0].table[idx] = NULL;
    }
    d->rehashidx = end;
    if (d->ht[0].used == 0)
        dictRehashComplete(d);
    return DICT_OK;
}
/* 搬 number 个非空桶 —— 跳过空桶，保证每次调用都有实际搬迁 */
static int dictRehashData(struct dict *d, unsigned long number)
{
    while (number > 0 && (unsigned long)d->rehashidx < d->ht[0].size) {
        /* 跳过空桶，不消耗 number */
        if (d->ht[0].table[d->rehashidx] == NULL) {
            d->rehashidx++;
            continue;
        }

        dictEntry *entry = d->ht[0].table[d->rehashidx];
        while (entry) {
            dictEntry *next = entry->next;
            unsigned long idx = entry->hash & d->ht[1].sizemask;

            entry->next = d->ht[1].table[idx];
            d->ht[1].table[idx] = entry;
            entry = next;
            d->ht[0].used--;
            d->ht[1].used++;
        }
        d->ht[0].table[d->rehashidx] = NULL;
        d->rehashidx++;
        number--;
    }

    if (d->ht[0].used == 0)
        dictRehashComplete(d);

    return DICT_OK;
}
static unsigned long dicthtGetIdx(const struct dictht *ht, hash_t hashVal)
{
    return hashVal & ht->sizemask;
}
static inline int dictIsRehashing(struct dict *d) {
    return d->rehashidx >= 0;
}
/* 只插入key，作为 add 和 replace 的底层 */
static dictEntry *dictAddRaw(struct dict *d, void *key, dictEntry **existing, void *hash)
{
    if (existing) *existing = NULL;

    /* rehash 期间每操作顺带搬 1 个桶 */
    if (dictIsRehashing(d))
        dictRehashData(d, 1);

    hash_t hashVal = (hash != NULL ? *((hash_t *)hash) : d->type->hash(key));

    int htidx = dictIsRehashing(d) ? 1 : 0;

    /* rehash 期间还要查 ht[0] 去重 */
    if (dictIsRehashing(d)) {
        unsigned long idx0 = dicthtGetIdx(&d->ht[0], hashVal);
        dictEntry *e = d->ht[0].table[idx0];
        while (e) {
            if (d->type->keyCompare(key, e->key) == 0) {
                if (existing) *existing = e;
                return NULL;
            }
            e = e->next;
        }
    }

    /* 在目标表中查重 */
    struct dictht *ht = &d->ht[htidx];
    unsigned long idx = dicthtGetIdx(ht, hashVal);
    dictEntry *entry = ht->table[idx];
    while (entry) {
        if (d->type->keyCompare(key, entry->key) == 0) {
            if (existing) *existing = entry;
            return NULL;           /* key 已存在 */
        }
        entry = entry->next;
    }
    /* 头插入 */
    ht->table[idx] = dictEntryNew(hashVal, key, NULL, ht->table[idx]);
    ht->used++;

    /* 负载因子 > 1 且没在 rehash → 触发扩容 */
    if (!dictIsRehashing(d) && d->ht[0].used > d->ht[0].size)
        dictRehash(d);

    return ht->table[idx];
}

int dictReplace(struct dict *d, void *key, void *val, void *hash)
{
    dictEntry *entry = NULL;
    dictEntry *p = dictAddRaw(d, key, &entry, hash);

    if (p == NULL)
        entry->val = val;   /* key 已存在，覆写值（旧值由调用方释放） */
    else
        p->val = val;       /* key 新插入，设置值 */
    return DICT_OK;
}
int dictAdd(struct dict *d, void *key, void *val, void* hash)
{
    dictEntry *p = dictAddRaw(d, key, NULL, hash);

    if (p == NULL) // 插入失败，已经存在了
        return DICT_ERROR;
    else // 键插入成功
    {
        p->val = val;
        return DICT_OK;
    }
}
void * dictfind(struct dict* d, const void *key, void* hash)
{
    hash_t hashVal = (hash != NULL ? *((hash_t*)hash) : d->type->hash(key));
    if (dictIsRehashing(d))
        dictRehashData(d, 1);

    /* 查两表（未 rehash 时 ht[1] 为空，第二圈立即退出） */
    for (int t = 0; t <= 1; t++)
    {
        unsigned long idx = dicthtGetIdx(&d->ht[t], hashVal);
        dictEntry *p = d->ht[t].table[idx];

        while (p != NULL)
        {
            if (d->type->keyCompare(key, p->key) == 0)
                return d->type->valGet(p);
            p = p->next;
        }

        if (!dictIsRehashing(d))
            break; /* 不 rehash 只查 ht[0] */
    }
    return NULL;
}
struct dict *dictnew(unsigned long n, struct dictType *type)
{
    unsigned long size = 1ul << n;
    struct dict *p = malloc(sizeof(struct dict));
    if (p == NULL)
        return NULL;

    /* 初始化 ht[0] */
    p->ht[0].table = calloc(size, sizeof(dictEntry *));
    if (p->ht[0].table == NULL) {
        free(p);
        return NULL;
    }
    p->ht[0].size     = size;
    p->ht[0].sizemask = size - 1;
    p->ht[0].used     = 0;

    /* ht[1] 初始为空 */
    p->ht[1].table    = NULL;
    p->ht[1].size     = 0;
    p->ht[1].sizemask = 0;
    p->ht[1].used     = 0;

    p->type       = type;
    p->rehashidx  = -1;
    return p;
}
static void dictEntryFree(struct dict *d, struct dictEntry *de)
{
    d->type->keyFree(de->key);
    if(d->type->valFree) // 适配void*直接存储数据
        d->type->valFree(de->val);
    free(de);
}
static void dicthtfree(struct dict *d, struct dictht *dht)
{
    if (dht->table == NULL) return;

    unsigned long remaining = dht->used;
    for (unsigned long i = 0; i < dht->size && remaining > 0; i++) {
        struct dictEntry *dep = dht->table[i];
        while (dep != NULL) {
            struct dictEntry *next = dep->next;
            dictEntryFree(d, dep);
            dep = next;
            remaining--;
        }
    }
    free(dht->table);
}
void dictfree(struct dict *d)
{
    if (d == NULL) return;
    dicthtfree(d, &d->ht[0]);
    if (d->ht[1].table)
        dicthtfree(d, &d->ht[1]);
    free(d);
}

int dictDelete(struct dict *d, const void *key, void *hash)
{
    if (d == NULL || key == NULL)
        return DICT_ERROR;

    if (dictIsRehashing(d))
        dictRehashData(d, 1);

    hash_t hashVal = (hash != NULL ? *((hash_t *)hash) : d->type->hash(key));

    for (int t = 0; t <= 1; t++) {
        unsigned long idx = dicthtGetIdx(&d->ht[t], hashVal);
        dictEntry *p = d->ht[t].table[idx];
        dictEntry **prev = &d->ht[t].table[idx];

        while (p) {
            if (d->type->keyCompare(key, p->key) == 0) {
                *prev = p->next;
                dictEntryFree(d, p);
                d->ht[t].used--;
                return DICT_OK;
            }
            prev = &p->next;
            p = p->next;
        }

        if (!dictIsRehashing(d)) break;
    }
    return DICT_ERROR;
}

dictEntry *dictGetRandomKey(struct dict *d)
{
    /* 空 dict */
    if (d->ht[0].used == 0 && d->ht[1].used == 0)
        return NULL;

    /* 渐进式 rehash：先搬一个非空桶，降低采样偏差 */
    if (dictIsRehashing(d))
        dictRehashData(d, 1);

    struct dictht *ht = &d->ht[0];

    /* 随机重试找非空桶（负载因子 > 0.5 时平均 < 2 次） */
    unsigned long idx;
    do {
        if (dictIsRehashing(d)) {
            /* 按条目数加权选表 + 随机桶 */
            unsigned long total_used = d->ht[0].used + d->ht[1].used;
            if (((unsigned long)random() % total_used) < d->ht[0].used) {
                ht = &d->ht[0];
                idx = (unsigned long)d->rehashidx +
                      ((unsigned long)random() % (ht->size - (unsigned long)d->rehashidx));
            } else {
                ht = &d->ht[1];
                idx = (unsigned long)random() & ht->sizemask;
            }
        } else {
            idx = (unsigned long)random() & ht->sizemask;
        }
    } while (ht->table[idx] == NULL);

    /* 栈数组缓存链指针：一趟数完 O(1) 取，免去第二趟指针追迹 */

    /* 备选方案：水塘抽样 — 数学均匀但每节点调一次 random()，链长时比两趟慢 */
    // dictEntry *e = NULL;
    // int n = 0;
    // for (dictEntry *tmp = ht->table[idx]; tmp; tmp = tmp->next, n++) {
    //     if (random() % (n + 1) == 0)
    //         e = tmp;
    // }
    // return e;

    dictEntry *buf[DICT_RANDOM_BUF_LEN];
    dictEntry *head = ht->table[idx];
    int n = 0;
    for (dictEntry *tmp = head; tmp; tmp = tmp->next) {
        if (n < DICT_RANDOM_BUF_LEN)
            buf[n] = tmp;
        n++;
    }

    if (n <= DICT_RANDOM_BUF_LEN)
        return buf[(unsigned long)random() % n];

    /* 兜底：链长 > DICT_RANDOM_BUF_LEN（概率 ~10⁻¹⁴），两趟法 */
    int target = (int)((unsigned long)random() % (unsigned long)n);
    if (target < DICT_RANDOM_BUF_LEN)
        return buf[target];
    dictEntry *e = buf[DICT_RANDOM_BUF_LEN - 1]->next;
    for (int i = DICT_RANDOM_BUF_LEN; i < target; i++)
        e = e->next;
    return e;
}

void *dictEntryGetKey(const dictEntry *de) { return de->key; }

void *dictEntryGetVal(struct dict *d, const dictEntry *de) { return d->type->valGet((dictEntry *)de); }