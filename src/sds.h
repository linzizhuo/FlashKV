#ifndef _SDS_H
#define _SDS_H
#include <stddef.h>
#include <stdint.h>
#include "io.h"
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
sds sdsdup(const sds s);
/* 为字符串做hash的函数 */
uint64_t sdsHash(const void *key);
int sdsCompare(const void* key1, const void* key2);
size_t sdslen(const sds str);
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
