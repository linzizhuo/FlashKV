#include "dict.h"
#include "sds.h"
#include <stddef.h>
#include <stdlib.h>
#define ROTL64(x, r) (((x) << (r)) | ((x) >> (64 - (r))))

struct dictEntry
{
    void *key;
    void *val;
    struct dictEntry *next;
};

static dictEntry *dictEntryNew(void *key, void *val, struct dictEntry *next)
{
    dictEntry *entry = (dictEntry *)malloc(sizeof(struct dictEntry));
    entry->key = key;
    entry->val = val;
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

static unsigned long dicthtGetIdx(const struct dictht *ht, uint64_t hashVal)
{
    // return hashVal % d->ht.size;
    return hashVal & ht->sizemask;
}
/* 只差入key，作为add和replace的底层，好抽象啊！！！ */
dictEntry *dictAddRaw(struct dict *d, void *key, dictEntry **existing)
{
    if (existing)
        *existing = NULL; // 初始化一下

    /* 抽象出来...后面不需要改这里的代码 */
    uint64_t hashVal = d->type->hash(key);
    /* 定位位置，也封装出来... */
    unsigned long idx = dicthtGetIdx(&(d->ht), hashVal);

    dictEntry *entry = d->ht.table[idx];
    while (entry)
    {
        if (d->type->keyCompare(key, entry->key) == 0) // 相等
        {
            if (existing)
                *existing = entry; // 初始化一下
            return NULL;           // 插入失败
        }
        entry = entry->next;
    }
    // 头插
    d->ht.table[idx] = dictEntryNew(key, NULL, d->ht.table[idx]);
    d->ht.used++;
    return d->ht.table[idx];
}
int dictReplace(struct dict *d, void *key, void *val)
{
    dictEntry *entry = NULL;
    dictEntry *p = dictAddRaw(d, key, &entry);

    if (p == NULL)
        entry->val = val;
    else
        p->val = val;
    return DICT_OK;
}
int dictAdd(struct dict *d, void *key, void *val)
{
    dictEntry *p = dictAddRaw(d, key, NULL);

    if (p == NULL) // 插入失败，已经存在了
        return DICT_ERROR;
    else // 键插入成功
    {
        p->val = val;
        return DICT_OK;
    }
}
void * dictfind(struct dict* d, const void *key)
{
    uint64_t hash = d->type->hash(key);

    unsigned long idx = dicthtGetIdx(&d->ht, hash);
    // find
    dictEntry* p = d->ht.table[idx];

    while(p != NULL)
    {
        if(d->type->keyCompare(key, p->key) == 0) // 相等
            return p->val;
        p = p->next;
    }
    return NULL; // 没找到
}
struct dict *dictnew(unsigned long n, struct dictType *type)
{
    unsigned long size = 1ul << n;
    struct dict *p = malloc(sizeof(struct dict));
    if (p == NULL)
        return NULL;
    p->ht.table = calloc(size, sizeof(dictEntry *));
    if (p->ht.table == NULL)
    {
        free(p);
        return NULL;
    }
    p->type = type;
    p->ht.size = size;
    p->ht.used = 0;
    p->ht.sizemask = size - 1;
    return p;
}
void dictEntryFree(struct dict *d, struct dictEntry *de)
{
    d->type->keyFree(de->key);
    d->type->valFree(de->val);
    free(de);
}
void dicthtfree(struct dict *d, struct dictht *dht)
{
    if(dht->table == NULL)
        return;
    for (unsigned long i = 0; i < dht->size; i++)
    {
        struct dictEntry *dep = dht->table[i];
        while (dep != NULL)
        {
            struct dictEntry *next = dep->next;
            dictEntryFree(d, dep);
            dep = next;
        }
    }
    free(dht->table);
}
void dictfree(struct dict *d)
{
    if (d == NULL)
        return;
    dicthtfree(d, &d->ht);
    free(d);
}