#include "sds.h"
#include <string.h>
#include <stdlib.h>

size_t sdsSerialize(const sds s, void **buf)
{
    size_t len = sdslen(s);
    size_t total = 4 + len;
    unsigned char *p = malloc(total);
    if (!p)
    {
        *buf = NULL;
        return 0;
    }

    memcpy(p, &len, 4); /* 4B 长度 */
    if (len > 0)
        memcpy(p + 4, s, len); /* 数据 */

    *buf = p;
    return total;
}

sds sdsDeserialize(const void *buf)
{
    const unsigned char *p = buf;
    uint32_t len;
    memcpy(&len, p, 4);           /* 读 4B 长度 */
    return sdsnewlen(p + 4, len); /* 读数据 */
}

sds sdsnew(const char *init)
{
    size_t initlen = (init == NULL) ? 0 : strlen(init);
    return sdsnewlen(init, initlen);
}

sds sdsnewlen(const void *init, size_t initlen)
{
    struct sdshdr64 *p = (struct sdshdr64 *)malloc(sizeof(struct sdshdr64) + initlen + 1);
    if (p == NULL)
        return NULL;

    p->len = initlen;
    p->alloc = initlen + 1;

    if (init != NULL)
        memcpy(p->buf, init, initlen);
    p->buf[initlen] = '\0';
    return p->buf;
}
sds sdsdup(const sds s)
{
    return sdsnewlen(s, sdslen(s));
}

int sdsCompare(const void *key1, const void *key2)
{
    sds s1 = (sds)key1, s2 = (sds)key2;
    uint64_t len1 = sdslen(s1), len2 = sdslen(s2);

    if (len1 != len2)
        return 1;                // 不相等
    return memcmp(s1, s2, len1); // 0 表示相等，其他表示不等
}
void sdsfree(void *s)
{
    if (s == NULL)
        return;
    free(SDS_HDR(64, s));
}

size_t sdslen(const sds str)
{
    return SDS_HDR(64, str)->len;
}
/* 使用MurmurHash2算法，快，均匀 */
uint64_t sdsHash(const void *key)
{
    const char *s = (sds)key;
    size_t len = sdslen((sds)key);

    // MurmurHash2 64-bit
    const uint64_t m = 0xc6a4a7935bd1e995ULL;
    const int r = 47;
    uint64_t h = 0xdeadbeefdeadbeefULL ^ (len * m);

    const uint64_t *data = (const uint64_t *)s;
    const uint64_t *end = data + (len / 8);

    while (data != end)
    {
        uint64_t k = *data++;
        k *= m;
        k ^= k >> r;
        k *= m;
        h ^= k;
        h *= m;
    }

    const unsigned char *p = (const unsigned char *)data;
    switch (len & 7)
    {
    case 7:
        h ^= (uint64_t)p[6] << 48;
        /* fall through */
    case 6:
        h ^= (uint64_t)p[5] << 40;
        /* fall through */
    case 5:
        h ^= (uint64_t)p[4] << 32;
        /* fall through */
    case 4:
        h ^= (uint64_t)p[3] << 24;
        /* fall through */
    case 3:
        h ^= (uint64_t)p[2] << 16;
        /* fall through */
    case 2:
        h ^= (uint64_t)p[1] << 8;
        /* fall through */
    case 1:
        h ^= (uint64_t)p[0];
        h *= m;
    }

    h ^= h >> r;
    h *= m;
    h ^= h >> r;
    return h;
}

int sdsWrite(Io *io, sds s)
{
    uint32_t len = (uint32_t)sdslen(s);
    int rc;

    /* 写 4B 长度 */
    rc = addIo(io, (const char *)&len, 4);
    if (rc != 4)
        return ERR;

    /* 写数据 */
    if (len > 0)
    {
        rc = addIo(io, s, len);
        if (rc != (int)len)
            return ERR;
    }

    return (int)(4 + len);
}

int sdsRead(Io *io, sds *s)
{
    uint32_t len;
    int rc;

    /* 读 4B 长度 */
    rc = readIo(io, (char *)&len, 4);
    if (rc != OK)
        return ERR;

    /* 分配 sds */
    *s = sdsnewlen(NULL, len);
    if (*s == NULL)
        return ERR;

    /* 读数据 */
    if (len > 0)
    {
        rc = readIo(io, *s, len);
        if (rc != OK)
        {
            sdsfree(*s);
            *s = NULL;
            return ERR;
        }
    }

    return (int)(4 + len);
}