#ifndef _SDS_H
#define _SDS_H
#include <stddef.h>
#include <stdint.h>
#include "io.h"

typedef char *sds;

#define SDS_TYPE_5 0
#define SDS_TYPE_8 1
#define SDS_TYPE_16 2
#define SDS_TYPE_32 3
#define SDS_TYPE_64 4
#define SDS_TYPE_MASK 7
#define SDS_TYPE_BITS 3
// #define SDS_NULL_TERM 1 // \0 terminator, 1 byte
#define SDS_TYPE_5_LEN(s) (((unsigned char)(s[-1])) >> SDS_TYPE_BITS) // 右移 3 位拿长度
#define SDS_HDR_VAR(T, s) struct sdshdr##T *sh = (void *)((s) - (sizeof(struct sdshdr##T)));

#define SDS_HDR(T, s) ((struct sdshdr##T *)((s) - (sizeof(struct sdshdr##T))))


struct __attribute__ ((__packed__)) sdshdr5 {
    unsigned char flags; /* 3 lsb of type, and 5 msb of string length */
    char buf[];
};

struct __attribute__((__packed__)) sdshdr8
{
    uint8_t len;         /* used */
    uint8_t alloc;       /* excluding the header and null terminator */
    unsigned char flags; /* 3 lsb of type, 5 unused bits */
    char buf[];
};

struct __attribute__((__packed__)) sdshdr16
{
    uint16_t len;        /* used */
    uint16_t alloc;      /* excluding the header and null terminator */
    unsigned char flags; /* 3 lsb of type, 5 unused bits */
    char buf[];
};

struct __attribute__((__packed__)) sdshdr32
{
    uint32_t len;        /* used */
    uint32_t alloc;      /* excluding the header and null terminator */
    unsigned char flags; /* 3 lsb of type, 5 unused bits */
    char buf[];
};

struct __attribute__((__packed__)) sdshdr64
{
    uint64_t len; // 长度
    uint64_t alloc; // 空间大小
    unsigned char flags; // 标记种类
    char buf[]; // 柔性数组
};


static inline unsigned char sdsType(sds s)
{
    unsigned char flags = s[-1];
    return flags & SDS_TYPE_MASK;
}

/* 从 sds header 的保留位中读取一个用户数据位。bit 索引 0-4。8-3
 * 对 SDS_TYPE_5 始终返回 0。 */
static inline int sdsGetAuxBit(sds s, int bit)
{
    if (sdsType(s) == SDS_TYPE_5)
        return 0;

    unsigned char flags = s[-1];
    return (flags & (1U << (SDS_TYPE_BITS + bit))) != 0U;
}

static inline void sdsSetAuxBit(sds s, int bit, int value)
{
    if (sdsType(s) == SDS_TYPE_5)
        return;
    unsigned char flags = s[-1];
    if (value)
    {
        flags |= 1U << (SDS_TYPE_BITS + bit);
    }
    else
    {
        flags &= ~(1U << (SDS_TYPE_BITS + bit));
    }
    s[-1] = (char)flags;
}

static inline size_t sdslen(const sds s)
{
    switch (sdsType(s))
    {
    case SDS_TYPE_5:
        return SDS_TYPE_5_LEN(s);
    case SDS_TYPE_8:
        return SDS_HDR(8, s)->len;
    case SDS_TYPE_16:
        return SDS_HDR(16, s)->len;
    case SDS_TYPE_32:
        return SDS_HDR(32, s)->len;
    case SDS_TYPE_64:
        return SDS_HDR(64, s)->len;
    }
    return 0;
}
// 获取剩余容量的
static inline size_t sdsavail(const sds s)
{
    switch (sdsType(s))
    {
    case SDS_TYPE_5:
    {
        return 0;
    }
    case SDS_TYPE_8:
    {
        SDS_HDR_VAR(8, s);
        return sh->alloc - sh->len;
    }
    case SDS_TYPE_16:
    {
        SDS_HDR_VAR(16, s);
        return sh->alloc - sh->len;
    }
    case SDS_TYPE_32:
    {
        SDS_HDR_VAR(32, s);
        return sh->alloc - sh->len;
    }
    case SDS_TYPE_64:
    {
        SDS_HDR_VAR(64, s);
        return sh->alloc - sh->len;
    }
    }
    return 0;
}
// 修改长度
static inline void sdssetlen(sds s, size_t newlen)
{
    switch (sdsType(s))
    {
    case SDS_TYPE_5:
    {
        unsigned char *fp = ((unsigned char *)s) - 1;
        *fp = SDS_TYPE_5 | (newlen << SDS_TYPE_BITS);
    }
    break;
    case SDS_TYPE_8:
        SDS_HDR(8, s)->len = newlen;
        break;
    case SDS_TYPE_16:
        SDS_HDR(16, s)->len = newlen;
        break;
    case SDS_TYPE_32:
        SDS_HDR(32, s)->len = newlen;
        break;
    case SDS_TYPE_64:
        SDS_HDR(64, s)->len = newlen;
        break;
    }
}
// 追加长度
static inline void sdsinclen(sds s, size_t inc)
{
    switch (sdsType(s))
    {
    case SDS_TYPE_5:
    {
        unsigned char *fp = ((unsigned char *)s) - 1;
        unsigned char newlen = SDS_TYPE_5_LEN(s) + inc;
        *fp = SDS_TYPE_5 | (newlen << SDS_TYPE_BITS);
    }
    break;
    case SDS_TYPE_8:
        SDS_HDR(8, s)->len += inc;
        break;
    case SDS_TYPE_16:
        SDS_HDR(16, s)->len += inc;
        break;
    case SDS_TYPE_32:
        SDS_HDR(32, s)->len += inc;
        break;
    case SDS_TYPE_64:
        SDS_HDR(64, s)->len += inc;
        break;
    }
}

static inline size_t sdsAllocSize(sds s)
{
    switch (sdsType(s))
    {
    case SDS_TYPE_5:
        return sizeof(struct sdshdr5) + SDS_TYPE_5_LEN(s) + 1;
    case SDS_TYPE_8:
        return sizeof(struct sdshdr8) + SDS_HDR(8, s)->alloc + 1;
    case SDS_TYPE_16:
        return sizeof(struct sdshdr16) + SDS_HDR(16, s)->alloc + 1;
    case SDS_TYPE_32:
        return sizeof(struct sdshdr32) + SDS_HDR(32, s)->alloc + 1;
    case SDS_TYPE_64:
        return sizeof(struct sdshdr64) + SDS_HDR(64, s)->alloc + 1;
    }
    return 0;
}
/* sdsalloc() = sdsavail() + sdslen() */
static inline size_t sdsalloc(const sds s)
{
    switch (sdsType(s))
    {
    case SDS_TYPE_5:
        return SDS_TYPE_5_LEN(s);
    case SDS_TYPE_8:
        return SDS_HDR(8, s)->alloc;
    case SDS_TYPE_16:
        return SDS_HDR(16, s)->alloc;
    case SDS_TYPE_32:
        return SDS_HDR(32, s)->alloc;
    case SDS_TYPE_64:
        return SDS_HDR(64, s)->alloc;
    }
    return 0;
}

static inline void sdssetalloc(sds s, size_t newlen)
{
    switch (sdsType(s))
    {
    case SDS_TYPE_5:
        /* Nothing to do, this type has no total allocation info. 静态字符串 */
        break;
    case SDS_TYPE_8:
        SDS_HDR(8, s)->alloc = newlen;
        break;
    case SDS_TYPE_16:
        SDS_HDR(16, s)->alloc = newlen;
        break;
    case SDS_TYPE_32:
        SDS_HDR(32, s)->alloc = newlen;
        break;
    case SDS_TYPE_64:
        SDS_HDR(64, s)->alloc = newlen;
        break;
    }
}

static inline int sdsHdrSize(char type)
{
    switch (type & SDS_TYPE_MASK)
    {
    case SDS_TYPE_5:
        return sizeof(struct sdshdr5);
    case SDS_TYPE_8:
        return sizeof(struct sdshdr8);
    case SDS_TYPE_16:
        return sizeof(struct sdshdr16);
    case SDS_TYPE_32:
        return sizeof(struct sdshdr32);
    case SDS_TYPE_64:
        return sizeof(struct sdshdr64);
    }
    return 0;
}

sds sdsnew(const char *init);
sds sdsnewlen(const void *init, size_t initlen);
sds sdsdup(const sds s);
/* 为字符串做hash的函数 */
uint64_t sdsHash(const void *key);
int sdsCompare(const void* key1, const void* key2);

void sdsfree(void *s);

/*
    实际上并未使用过上述函数，留着算是预留接口
*/
size_t sdsSerialize(const sds s, void **buf);
/* 从 [4B len] [data] 格式反序列化回 sds。
 * 返回 = 新 sds，调用方负责 sdsfree()。 */
sds sdsDeserialize(const void *buf);
/*
    rdb的真正方式
*/
int sdsWrite(Io *io, sds s); /* [4B len][data] 直接进 io */
int sdsRead(Io *io, sds *s); // 读：返回字节数或 ERR，sds 由 *s 带出

#endif
