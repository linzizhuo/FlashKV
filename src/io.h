#ifndef _IO_H
#define _IO_H
#include <stddef.h>   /* size_t */
#include <sys/stat.h> /* mode_t */
#include "config.h"
#include <fcntl.h>    /* O_WRONLY, O_CREAT, O_RDONLY... */
/* 零拷贝路径：getbufIo → 直接写 buf → commitIo 提交 */
#define commitIo(io, n)((io)->write_idx += (n))
#define wbuf(io) ((io)->buf)
#define rbuf(io) ((io)->buf + (io)->buflen)
    typedef struct Io
{
    int fd;
    size_t buflen;    /* 单个 buf 大小 */
    size_t write_idx; /* 待 flush 数据量 */
    size_t read_idx;  /* 缓冲区已缓存数据量 */
    size_t read_pos;  /* 消费位置 */
    char buf[];       /* 柔性数组：前半写、后半读，各 buflen */
} Io;

/* API */
Io *newIo(const char *path, size_t cap, int flags, mode_t mode);
void freeIo(Io *io);              /* flush + fsync + close + free */
int getbufIo(Io *io, char **buf); /* 返回剩余空间 */
int addIo(Io *io, const char *buf, size_t n); /* 追加写入 */
int flushIo(Io *io, int dir);              /* write(fd, buf, idx)，idx 归零 */
int readIo(Io *io, char *buf, size_t n);

#endif