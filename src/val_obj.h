#ifndef _VAL_OBJ_H
#define _VAL_OBJ_H
#include "sds.h"
#include "zset.h"
#include <stdlib.h>
#include <stdint.h>
#include "config.h"
#include "string.h"
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

static inline int valObjWrite(Io *io, const ValObj *val)
{
    if (!io || !val)
        return ERR;

    switch (val->type)
    {
    case DATA_STRING:
        return sdsWrite(io, val->val.str);
    case DATA_INT:
    {
        long long ll = val->val.ll;
        int rc = addIo(io, (const char *)&ll, sizeof(ll));
        return rc == sizeof(ll) ? (int)sizeof(ll) : ERR;
    }
    case DATA_ZSET:
        return zsetWrite(io, val->val.zs);
    case DATA_LIST:
        /* return listWrite(io, val->val.l); */
        return ERR;
    case DATA_SET:
        /* return setWrite(io, val->val.s); */
        return ERR;
    case DATA_HASH:
        /* return hashWrite(io, val->val.h); */
        return ERR;
    default:
        return ERR;
    }
}

static inline size_t valObjSerialize(const ValObj *val, void **buf)
{
    if (!val || !buf)
        return 0;
    switch (val->type)
    {
    case DATA_STRING:
        return sdsSerialize(val->val.str, buf);
    case DATA_INT:
        buf = malloc(sizeof(long long));
        memcpy(*buf, &val->val.ll, sizeof(long long));
        return sizeof(long long);
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
/*
    反序列化，根据 type 调用对应的反序列化函数
    buf 只含类型特定数据，不含 type 字节
    返回新 ValObj（调用方 valObjFree 释放），失败返回 NULL
*/
static inline ValObj *valObjDeserialize(int type, const void *buf)
{
    if (!buf)
        return NULL;

    ValObj *o = malloc(sizeof(*o));
    if (!o)
        return NULL;
    o->type = (enum DataType)type;

    switch (o->type)
    {
    case DATA_STRING:
        o->val.str = sdsDeserialize(buf);
        if (!o->val.str)
        {
            free(o);
            return NULL;
        }
        return o;
    case DATA_INT:
        memcpy(&o->val.ll, buf, sizeof(long long));
        return o;
    case DATA_ZSET:
        /* o->val.zs = zsetDeserialize(buf); */
        break;
    case DATA_LIST:
        /* o->val.l = listDeserialize(buf); */
        break;
    case DATA_SET:
        /* setDeserialize */
        break;
    case DATA_HASH:
        /* hashDeserialize */
        break;
    default:
        break;
    }
    free(o);
    return NULL;
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

static inline int valObjRead(Io *io, int type, ValObj **val)
{
    ValObj *o = malloc(sizeof(*o));
    if (!o)
        return ERR;
    o->type = (enum DataType)type;

    switch (o->type)
    {
    case DATA_STRING:
    {
        sds s = NULL;
        if (sdsRead(io, &s) == ERR)
        {
            free(o);
            return ERR;
        }
        o->val.str = s;
        break;
    }
    case DATA_INT:
    {
        if (readIo(io, (char *)&o->val.ll, sizeof(o->val.ll)) != OK)
        {
            free(o);
            return ERR;
        }
        break;
    }
    case DATA_ZSET:
    {
        zset *zs = NULL;
        if (zsetRead(io, &zs) == ERR)
        {
            free(o);
            return ERR;
        }
        o->val.zs = zs;
        break;
    }
    /* LIST / SET / HASH 后续补 */
    default:
        free(o);
        return ERR;
    }
    *val = o;
    return OK;
}
#endif