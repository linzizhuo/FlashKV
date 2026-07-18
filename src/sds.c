#include "sds.h"
#include <string.h>
#include <stdlib.h>
#include <malloc.h>
#include <assert.h> // assert()

const char *SDS_NOINIT = "SDS_NOINIT";
/*
    根据长度静态解析类型，长度取决于alloc字段，而非len字段。
    为了对抗malloc会多给的情况，这里是第一层防御，若实际情况真的给多了。就动态适配一个别的头（这里又和取消内存对齐串起来了，取消内存对齐好像貌似是一个好处很明显的事情）
*/
char sdsReqType(size_t string_size)
{
    if (string_size < 1 << 5)
        return SDS_TYPE_5;
    if (string_size <= (1 << 8) - sizeof(struct sdshdr8) - 1)
        return SDS_TYPE_8;
    if (string_size <= (1 << 16) - sizeof(struct sdshdr16) - 1)
        return SDS_TYPE_16;
#if (LONG_MAX == LLONG_MAX)
    if (string_size <= (1ll << 32) - sizeof(struct sdshdr32) - 1)
        return SDS_TYPE_32;
    return SDS_TYPE_64;
#else
    return SDS_TYPE_32;
#endif
}

static inline size_t sdsTypeMaxSize(char type)
{
    if (type == SDS_TYPE_5)
        return (1 << 5) - 1;
    if (type == SDS_TYPE_8)
        return (1 << 8) - 1;
    if (type == SDS_TYPE_16)
        return (1 << 16) - 1;
#if (LONG_MAX == LLONG_MAX)
    if (type == SDS_TYPE_32)
        return (1ll << 32) - 1;
#endif
    return -1; /* this is equivalent to the max SDS_TYPE_64 or SDS_TYPE_32 */
}

static inline int adjustTypeIfNeeded(char *type, int *hdrlen, size_t bufsize)
{
    size_t usable = bufsize - *hdrlen - 1;
    if (*type != SDS_TYPE_5 && usable > sdsTypeMaxSize(*type))
    {
        *type = sdsReqType(usable);
        *hdrlen = sdsHdrSize(*type);
        return 1;
    }
    return 0;
}
/* 创建一个新的 sds 字符串，内容由 'init' 指针和 'initlen' 指定。
 *
 * 若 'init' 传 NULL，字符串初始化为零字节（全零）。
 * 若传 SDS_NOINIT，缓冲区保持未初始化状态。
 *
 * 返回的字符串始终以 \0 结尾（所有 sds 字符串都如此），因此即使你
 * 这样创建：
 *
 *   mystring = sdsnewlen("abc", 3);
 *
 * 你也可以直接用 printf() 打印它，因为末尾隐式含有一个 \0。但该字符
 * 串本身是二进制安全的，中间可以包含 \0 字符，因为长度信息存储在 sds
 * header 中。 * */
sds _sdsnewlen(const void *init, size_t initlen, int trymalloc)
{
    char type = sdsReqType(initlen);
    if (type == SDS_TYPE_5 && initlen == 0) // 自动扩容
        type = SDS_TYPE_8;
    int hdrlen = sdsHdrSize(type);
    if (trymalloc) // 溢出检测
    {
        if (initlen + hdrlen + 1 <= initlen)
            return NULL;
    }
    else
    {
        assert(initlen + hdrlen + 1 > initlen);
    }

    void *sh = malloc(hdrlen + initlen + 1);
    if (!sh)
    {
        if (trymalloc)
            return NULL;
        abort();
    }

    size_t usable = malloc_usable_size(sh);
    while (adjustTypeIfNeeded(&type, &hdrlen, usable))
        ;
    return sdsnewplacement(sh, usable, type, init, initlen);
}

/* 从预先分配的缓冲区中初始化一个 sds
 *
 * 参数:
 * - `buf`    : 预先分配好的缓冲区。
 * - `bufsize`: 缓冲区的总大小（>= `sdsReqSize(initlen, type)`）。可以传入
 *              比实际需要更大的 `bufsize`，但可用大小不会超过
 *              `sdsTypeMaxSize(type)`。
 * - `type`   : SDS 类型，可辅助 `sdsReqType(length)` 计算类型。
 * - `init`   : 要拷贝的初始字符串，传 `SDS_NOINIT` 则跳过初始化。
 * - `initlen`: 初始字符串的长度。
 *
 * 返回值:
 * - 指向 `buf` 内部 sds 的指针。
 */
sds sdsnewplacement(char *buf, size_t bufsize, char type, const char *init, size_t initlen)
{
    assert(bufsize >= sdsReqSize(initlen, type)); // 溢出检查
    int hdrlen = sdsHdrSize(type);
    size_t usable = bufsize - hdrlen - 1;
    sds s = buf + hdrlen;
    unsigned char *fp = ((unsigned char *)s) - 1;
    // 处理头部
    switch (type)
    {
    case SDS_TYPE_5:
    {
        *fp = type | (initlen << SDS_TYPE_BITS);
        break;
    }
    case SDS_TYPE_8:
    {
        SDS_HDR_VAR(8, s);
        sh->len = initlen;
        sh->alloc = usable;
        *fp = type;
        break;
    }
    case SDS_TYPE_16:
    {
        SDS_HDR_VAR(16, s);
        sh->len = initlen;
        sh->alloc = usable;
        *fp = type;
        break;
    }
    case SDS_TYPE_32:
    {
        SDS_HDR_VAR(32, s);
        sh->len = initlen;
        sh->alloc = usable;
        *fp = type;
        break;
    }
    case SDS_TYPE_64:
    {
        SDS_HDR_VAR(64, s);
        sh->len = initlen;
        sh->alloc = usable;
        *fp = type;
        break;
    }
    }
    if (init == SDS_NOINIT)
        init = NULL;
    else if (!init)
        memset(s, 0, initlen);
    else if (initlen)
        memcpy(s, init, initlen);
    s[initlen] = '\0';
    return s;
}
/* 扩大sds的尾部可用空间，将字符串传递给调用者供其使用
    参数：
        sds s 原本的字符串
        size_t addlen 追加的长度
        int greedy 是否贪婪
    返回值：
        sds
*/
sds _sdsmakeroomfor(sds s, size_t addlen, int greedy)
{
    size_t avail = sdsavail(s); // 获取剩余的
    if (avail >= addlen)
        return s; // 剩余的还够，直接返回

    // 计算需要获取的长度
    assert(addlen + sdslen(s) >= addlen); // 溢出检查
    size_t newlen = sdslen(s) + addlen;
    
    if (greedy == 1)    
        newlen = newlen < SDS_MAX_PREALLOC ? newlen * 2 : newlen + SDS_MAX_PREALLOC;

    char type = sdsReqType(newlen);
    if(type == SDS_TYPE_5)
        type = SDS_TYPE_8; // 5转8
    int hdrlen = sdsHdrSize(type);
    void *sh = malloc(newlen + hdrlen + 1); // 先分配
    if(!sh)
        return s;
    size_t bufsize = malloc_usable_size(sh);
    // 扩容
    while (adjustTypeIfNeeded(&type, &hdrlen, bufsize));
    // 初始化
    sds newsds = sdsnewplacement(sh, bufsize, type, s, sdslen(s));
    sdsfree(s);
    return newsds;
}
// 对外——只给两个语义明确的入口
sds sdsMakeRoomFor(sds s, size_t addlen)          // greedy = 1
{
    return _sdsmakeroomfor(s, addlen, 1);
}
sds sdsMakeRoomForNonGreedy(sds s, size_t addlen) // greedy = 0
{
    return _sdsmakeroomfor(s, addlen, 0);
}
/*
    给一个sds的尾部追加数据
    s 必须非 NULL，调用方保证。返回可能的新指针（realloc）
    data 必须非NULL，调用方保证。
    addlen -> 要追加的长度
*/
sds sdscatlen(sds s, const void* data, size_t datalen)
{
    assert(s != NULL && data != NULL); /* 不接受空数据，退化成扩容语义走 sdsMakeRoomFor */
    size_t len = sdslen(s);
    s = sdsMakeRoomFor(s, datalen); // 扩容
    memcpy(s + len, data, datalen);
    sdssetlen(s, len + datalen);
    s[sdslen(s)] = '\0';
    return s;
}

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
    return _sdsnewlen(init, initlen, 0);
}

sds sdstrynewlen(const void *init, size_t initlen)
{
    return _sdsnewlen(init, initlen, 1);
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
    free(s - sdsHdrSize(sdsType(s)));
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