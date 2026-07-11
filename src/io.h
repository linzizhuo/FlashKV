#ifndef _IO_H
#define _IO_H
#include <stddef.h>   /* size_t */
#include <sys/stat.h> /* mode_t */
#include "config.h"
#include <fcntl.h>    /* O_WRONLY, O_CREAT, O_RDONLY... */
/* 零拷贝路径：getbufIo → 直接写 buf → commitIo 提交 */
#define commitIo(io, n) ((io)->idx += (n))

typedef struct Io
{
    int fd;
    size_t len; /* 缓冲区总长 */
    size_t idx; /* 脏数据起点，左边待 flush，右边可用 */
    char buf[]; /* 柔性数组 */
} Io;

/* API */
Io *newIo(const char *path, size_t cap, int flags, mode_t mode);
void freeIo(Io *io);              /* flush + fsync + close + free */
int getbufIo(Io *io, char **buf); /* 返回剩余空间 */
int addIo(Io *io, const char *buf, size_t n); /* 追加写入 */
int flushIo(Io *io);              /* write(fd, buf, idx)，idx 归零 */

#endif