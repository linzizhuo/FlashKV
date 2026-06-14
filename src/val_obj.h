#ifndef _VAL_OBJ_H
#define _VAL_OBJ_H
#include"sds.h"
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
        void *l;  // list *
        void *zs; // zset *
    } val;
} ValObj;

static inline void valObjFree(void *ptr)
{
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
    case VAL_ZSET: /* zsetFree */;
        break;
    default:
        break;
    }
    free(o);
}

#endif