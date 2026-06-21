#ifndef _VAL_OBJ_H
#define _VAL_OBJ_H
#include "sds.h"
#include "zset.h"
#include <stdlib.h>
#include <stdint.h>

enum ValType
{
    VAL_STRING,
    VAL_LIST,
    VAL_ZSET,
    VAL_SET,
    VAL_HASH,
    VAL_INT,
};

typedef struct
{
    enum ValType type;
    union
    {
        sds str;
        long long ll;
        void *l;   // list *
        zset *zs;   // zset: dict + skip list of (score, sds) pairs
    } val;
} ValObj;

/* ---- ZSET helpers ---- */

static inline ValObj *valObjCreateZset(void)
{
    ValObj *o = malloc(sizeof(*o));
    if (!o) return NULL;
    o->type = VAL_ZSET;
    o->val.zs = zsetNew();
    if (!o->val.zs) {
        free(o);
        return NULL;
    }
    return o;
}

static inline void valObjFree(void *ptr)
{
    if (!ptr) return;
    ValObj *o = (ValObj *)ptr;
    switch (o->type)
    {
    case VAL_STRING:
        sdsfree(o->val.str);
        break;
    case VAL_INT: /* nothing */;
        break;
    case VAL_LIST: /* listRelease */;
        break;
    case VAL_ZSET:
        zsetFree(o->val.zs);
        break;
    default:
        break;
    }
    free(o);
}
#endif