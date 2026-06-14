#ifndef _SDS_H
#define _SDS_H
#include <stddef.h>
#include <stdint.h>
typedef char* sds;

struct __attribute__((__packed__)) sdshdr64
{
    uint64_t len; // 长度
    uint64_t alloc; // 空间大小
    unsigned char flags; // 标记种类
    char buf[]; // 柔性数组
};

#define SDS_HDR(T, s) ((struct sdshdr##T *)((s) - (sizeof(struct sdshdr##T))))

sds sdsnew(const char *init);
sds sdsnewlen(const void *init, size_t initlen);
/* 为字符串做hash的函数 */
uint64_t sdsHash(const void *key);
int sdsCompare(const void* key1, const void* key2);
size_t sdslen(const sds str);
void sdsfree(void *s);

#endif
