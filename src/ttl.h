#ifndef _TTL_H
#define _TTL_H
#include "dict_type.h"
#include <time.h>
// 外层包装，将dictEntry的key 从void*包装成time_t
// extern struct dictType dictTTL;
typedef struct dict ttlTable;
typedef void* tstamp_t;

static inline int keyTtlreplace(ttlTable *table, void *key, tstamp_t time, hash_t hash)
{
    return dictReplace(table, key, time, &hash);
}
static inline int keyTtlAdd(ttlTable *table, void *key, tstamp_t time, hash_t hash)
{
    return dictAdd(table, key, time, &hash);
}
// 返回NULL就是没设置，返回不是NULL就是设置了...
static inline tstamp_t *keyTtlFind(ttlTable *table, const void *key, hash_t hash)
{
    return (tstamp_t *)dictfind(table, key, &hash);
}
static inline int keyTtlDelete(ttlTable *table, const void *key, hash_t hash)
{
    return dictDelete(table, key, &hash);
}
#endif