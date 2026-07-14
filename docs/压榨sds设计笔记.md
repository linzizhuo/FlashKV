在探讨如何优化一个字符串之前，我们先来想一个问题——为什么要对字符串进行优化？

目前的sds结构是这样的
struct __attribute__((__packed__)) sdshdr64
{
    uint64_t len; // 长度
    uint64_t alloc; // 空间大小
    unsigned char flags; // 标记种类
    char buf[]; // 柔性数组
};
主播查了一些资料，询问了ai，他们给出的答案往往是char一般很小，len和alloc利用不充分，很多情况下头部占了大部分空间。
这些说法很合理，也确实是实实在在的需求，但我仍不觉得满足。

我们都知道，现代操作系统是有字长的，即cpu输出的字节。
现在的操作系统大多都是64字节，而且c/c++的结构体/类都存在一个机制：内存对齐

在内存对齐的支持下，其实无论这个元素如何小，结构体的字长都是最大的那个元素，这样优化下来貌似起不到实际的效果？因为柔性数组本质是一个包装后的指针，不管其他成员如何精简，结构体的字长是很难改变的，所以最开始我热衷于将所有的元素都变成等长的，这样规整，利用率高。(补充：这里说的过于绝对，但其实是存在2-3个元素占用一个字长的情况，但压榨sds的方法就是放弃内存对齐，考虑这个其实是在讨论在内存对齐情况下，压榨一部分变量的长度能不能带来优化)

内存不齐的结构体会让内存空间存在浪费，但取消内存对齐的结构体也有缺点——随机访问性能下降。

问题似乎变成了：要时间还是要空间的博弈？

但其实主播想了一下，为什么redis会做结构体优化。
我的拙见是redis的开发者认为，内存操作足够快，在通用缓存这个场景，内存操作不会成为瓶颈，反而是整个redis最快的地方（这点很符合直觉）。

结构体优化不仅仅可以充分利用空间，其实还有一个隐藏的好处，在操作系统层面——提升内存碎片的利用率。

（以上都是本人的一些拙见，并不是绝对的，可能也存在考虑不周的情况。）

接下来是理性分析，先前有错误的地方，柔性数组并不是一个指针，而只是一种语法，本质是 &head + sizeof(head)，并不占用内存，后面在考虑的时候就可以把柔性数组撇掉了，这点是补充。

那现在来探讨一下，这个优化实际能节省多少内存？

uint_8 len
uint_8 alloc
uint_8 falgs

只占3字节！对比于原来节省14个字节，从17->3，在小数据多的场景下这个优化是十分显著的。（以此类推，这里不做扩展，事实上可以得出就算没有内存对齐，这个优化也是显著的）

开发初期为了尽早跑通demo，本人对一些设计进行了简化，比如sdsHead，他现在看起来只是一个数据结构，而非可以应用到底层的，成熟的结构。
因此这个优化方向就是来探讨，如何将sds压榨到极致。
在探讨sds之前，先做一些职责分析

一个字符串是什么样的？
char*
一个二进制安全的字符串是什么样的？
size_t len
char*
一个支持扩容的字符串是什么样的？
size_t len
size_t alloc
char* 
一个根据容量划分变量以达到最优性能的字符串呢？
size_t len
size_t alloc
uint8_t flags
char* 

优化方案：
redis的方案：
#define SDS_TYPE_5  0
#define SDS_TYPE_8  1
#define SDS_TYPE_16 2
#define SDS_TYPE_32 3
#define SDS_TYPE_64 4
#define SDS_TYPE_MASK 7
#define SDS_TYPE_BITS 3
但实际上redis并没有用到SDS_TYPE_5，我们舍弃

#define SDS_TYPE_8  1
#define SDS_TYPE_16 2
#define SDS_TYPE_32 3
#define SDS_TYPE_64 4
#define SDS_TYPE_MASK 7

虽然不做sds5，但这里讲一下sds5的结构，提供一种优化思路：
#define SDS_TYPE_5  0
#define SDS_TYPE_BITS 3
#define SDS_TYPE_5_LEN(f) ((f) >> SDS_TYPE_BITS)  // 右移 3 位拿长度
struct __attribute__ ((__packed__)) sdshdr5 {
    unsigned char flags; /* 3 lsb of type, and 5 msb of string length */
    char buf[];
};
sds5在让flag和len共用了flags（不单独设置len和alloc）
低3bit存储sds类型标记，高5bit存储数据长度，容量就是2^5。

继续

宏：
#define SDS_HDR_VAR(T,s) struct sdshdr##T *sh = (void*)((s)-(sizeof(struct sdshdr##T)));
#define SDS_HDR(T,s) ((struct sdshdr##T *)((s)-(sizeof(struct sdshdr##T))))
结构体：
struct __attribute__ ((__packed__)) sdshdr8 {
    uint8_t len; /* used */
    uint8_t alloc; /* excluding the header and null terminator */
    unsigned char flags; /* 3 lsb of type, 5 unused bits */
    char buf[];
};

struct __attribute__ ((__packed__)) sdshdr16 {
    uint16_t len; /* used */
    uint16_t alloc; /* excluding the header and null terminator */
    unsigned char flags; /* 3 lsb of type, 5 unused bits */
    char buf[];
};

struct __attribute__ ((__packed__)) sdshdr32 {
    uint32_t len; /* used */
    uint32_t alloc; /* excluding the header and null terminator */
    unsigned char flags; /* 3 lsb of type, 5 unused bits */
    char buf[];
};

struct __attribute__ ((__packed__)) sdshdr64 {
    uint64_t len; /* used */
    uint64_t alloc; /* excluding the header and null terminator */
    unsigned char flags; /* 3 lsb of type, 5 unused bits */
    char buf[];
};

这里直接照搬redis的源码，接下来是静态路由，依旧是照抄redis源码。

static inline unsigned char sdsType(sds s) {
    unsigned char flags = s[-1];
    return flags & SDS_TYPE_MASK;
}

/* Returns a user data bit stored in the SDS header by sdsSetAuxBit. The bit
 * index is 0-4. Returns 0 or 1. Always returns 0 for SDS_TYPE_5. */
static inline int sdsGetAuxBit(sds s, int bit) {
    if (sdsType(s) == SDS_TYPE_5) 
        return 0;
    
    unsigned char flags = s[-1];
    return (flags & (1U << (SDS_TYPE_BITS + bit))) != 0U;
}

/* Stores a bit in an unused area in the SDS header, except for SDS_TYPE_5. The
 * bit index is 0-4. The value is 0 or 1. The aux bits are lost if the SDS is
 * auto-resized. This is only for special uses like immutable SDS embedded in
 * other structures. */
static inline void sdsSetAuxBit(sds s, int bit, int value) {
    if (sdsType(s) == SDS_TYPE_5) return;
    unsigned char flags = s[-1];
    if (value) {
        flags |= 1U << (SDS_TYPE_BITS + bit);
    } else {
        flags &= ~(1U << (SDS_TYPE_BITS + bit));
    }
    s[-1] = (char)flags;
}

static inline size_t sdslen(const sds s) {
    switch (sdsType(s)) {
        case SDS_TYPE_5: return SDS_TYPE_5_LEN(s);
        case SDS_TYPE_8:
            return SDS_HDR(8,s)->len;
        case SDS_TYPE_16:
            return SDS_HDR(16,s)->len;
        case SDS_TYPE_32:
            return SDS_HDR(32,s)->len;
        case SDS_TYPE_64:
            return SDS_HDR(64,s)->len;
    }
    return 0;
}

static inline size_t sdsavail(const sds s) {
    switch(sdsType(s)) {
        case SDS_TYPE_5: {
            return 0;
        }
        case SDS_TYPE_8: {
            SDS_HDR_VAR(8,s);
            return sh->alloc - sh->len;
        }
        case SDS_TYPE_16: {
            SDS_HDR_VAR(16,s);
            return sh->alloc - sh->len;
        }
        case SDS_TYPE_32: {
            SDS_HDR_VAR(32,s);
            return sh->alloc - sh->len;
        }
        case SDS_TYPE_64: {
            SDS_HDR_VAR(64,s);
            return sh->alloc - sh->len;
        }
    }
    return 0;
}

static inline void sdssetlen(sds s, size_t newlen) {
    switch(sdsType(s)) {
        case SDS_TYPE_5:
            {
                unsigned char *fp = ((unsigned char*)s)-1;
                *fp = SDS_TYPE_5 | (newlen << SDS_TYPE_BITS);
            }
            break;
        case SDS_TYPE_8:
            SDS_HDR(8,s)->len = newlen;
            break;
        case SDS_TYPE_16:
            SDS_HDR(16,s)->len = newlen;
            break;
        case SDS_TYPE_32:
            SDS_HDR(32,s)->len = newlen;
            break;
        case SDS_TYPE_64:
            SDS_HDR(64,s)->len = newlen;
            break;
    }
}

static inline void sdsinclen(sds s, size_t inc) {
    switch(sdsType(s)) {
        case SDS_TYPE_5:
            {
                unsigned char *fp = ((unsigned char*)s)-1;
                unsigned char newlen = SDS_TYPE_5_LEN(s)+inc;
                *fp = SDS_TYPE_5 | (newlen << SDS_TYPE_BITS);
            }
            break;
        case SDS_TYPE_8:
            SDS_HDR(8,s)->len += inc;
            break;
        case SDS_TYPE_16:
            SDS_HDR(16,s)->len += inc;
            break;
        case SDS_TYPE_32:
            SDS_HDR(32,s)->len += inc;
            break;
        case SDS_TYPE_64:
            SDS_HDR(64,s)->len += inc;
            break;
    }
}

/* Return the total size of the allocation of the specified sds string,
 * including:
 * 1) The sds header before the pointer.
 * 2) The string.
 * 3) The free buffer at the end if any.
 * 4) The implicit null term.
 */
static inline size_t sdsAllocSize(sds s) {
    switch(sdsType(s)) {
        case SDS_TYPE_5:
            return sizeof(struct sdshdr5) + SDS_TYPE_5_LEN(s) + 1;
        case SDS_TYPE_8:
            return sizeof(struct sdshdr8) + SDS_HDR(8,s)->alloc + 1;
        case SDS_TYPE_16:
            return sizeof(struct sdshdr16) + SDS_HDR(16,s)->alloc + 1;
        case SDS_TYPE_32:
            return sizeof(struct sdshdr32) + SDS_HDR(32,s)->alloc + 1;
        case SDS_TYPE_64:
            return sizeof(struct sdshdr64) + SDS_HDR(64,s)->alloc + 1;
    }
    return 0;
}

/* sdsalloc() = sdsavail() + sdslen() */
static inline size_t sdsalloc(const sds s) {
    switch(sdsType(s)) {
        case SDS_TYPE_5:
            return SDS_TYPE_5_LEN(s);
        case SDS_TYPE_8:
            return SDS_HDR(8,s)->alloc;
        case SDS_TYPE_16:
            return SDS_HDR(16,s)->alloc;
        case SDS_TYPE_32:
            return SDS_HDR(32,s)->alloc;
        case SDS_TYPE_64:
            return SDS_HDR(64,s)->alloc;
    }
    return 0;
}

static inline void sdssetalloc(sds s, size_t newlen) {
    switch(sdsType(s)) {
        case SDS_TYPE_5:
            /* Nothing to do, this type has no total allocation info. */
            break;
        case SDS_TYPE_8:
            SDS_HDR(8,s)->alloc = newlen;
            break;
        case SDS_TYPE_16:
            SDS_HDR(16,s)->alloc = newlen;
            break;
        case SDS_TYPE_32:
            SDS_HDR(32,s)->alloc = newlen;
            break;
        case SDS_TYPE_64:
            SDS_HDR(64,s)->alloc = newlen;
            break;
    }
}

static inline int sdsHdrSize(char type) {
    switch(type&SDS_TYPE_MASK) {
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

很好！删掉sds5就是就是完整的设计方案！