#ifndef _VAL_OBJ_H
#define _VAL_OBJ_H
#include "sds.h"
#include "zset.h"
#include <stdlib.h>
#include <stdint.h>
#include "config.h"

typedef struct
{
    enum DataType type;
    union
    {
        sds str;
        long long ll;
        void *l;  // list *
        zset *zs; // zset: dict + skip list of (score, sds) pairs
    } val;
} ValObj;

static inline size_t valObjSerialize(const ValObj *val, void **buf)
{
    if (!val || !buf)
        return 0;
    switch (val->type)
    {
    case DATA_STRING:
        return sdsSerialize(val->val.str, buf);
    case DATA_INT:
        /* 内联: 直接写 8 字节 long long */
        return 0;
    case DATA_ZSET:
        /* zsetSerialize */
        return 0;
    case DATA_LIST:
        /* listSerialize */
        return 0;
    case DATA_SET:
        /* setSerialize */
        return 0;
    case DATA_HASH:
        /* hashSerialize */
        return 0;
    default:
        return 0;
    }
}

static inline ValObj *valObjDeserialize(const void *buf)
{
    if (!buf)
        return NULL;
    const unsigned char *p = buf;
    int type = p[0]; // 获取类型
}
/* ---- ZSET helpers ---- */
static inline ValObj *valObjCreateZset(void)
{
    ValObj *o = malloc(sizeof(*o));
    if (!o)
        return NULL;
    o->type = DATA_ZSET;
    o->val.zs = zsetNew();
    if (!o->val.zs)
    {
        free(o);
        return NULL;
    }
    return o;
}

static inline void valObjFree(void *ptr)
{
    if (!ptr)
        return;
    ValObj *o = (ValObj *)ptr;
    switch (o->type)
    {
    case DATA_STRING:
        sdsfree(o->val.str);
        break;
    case DATA_INT: /* nothing */;
        break;
    case DATA_LIST: /* listRelease */;
        break;
    case DATA_ZSET:
        zsetFree(o->val.zs);
        break;
    default:
        break;
    }
    free(o);
}
#endif