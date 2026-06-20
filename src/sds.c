#include "sds.h"
#include <string.h>
#include <stdlib.h>

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
        return 1; // 不相等
    return memcmp(s1, s2, len1); // 0 表示相等，其他表示不等
}
void sdsfree(void *s)
{
    if (s == NULL) return;
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