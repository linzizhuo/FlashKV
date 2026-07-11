#include "io.h"
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>

Io *newIo(const char *path, size_t cap, int flags, mode_t mode)
{
    Io *io = malloc(sizeof(Io) + cap);
    if (!io)
        return NULL;

    io->fd = open(path, flags, mode);
    if (io->fd == -1)
    {
        free(io);
        return NULL;
    }

    io->len = cap;
    io->idx = 0;
    return io;
}

int getbufIo(Io *io, char **buf)
{
    if (!io)
        return ERR;
    if (buf)
        *buf = io->buf + io->idx;
    return (int)(io->len - io->idx);
}

int flushIo(Io *io)
{
    if (!io || io->fd == -1)
        return ERR;
    if (io->idx == 0)
        return OK;

    size_t written = 0;
    while (written < io->idx)
    {
        ssize_t n = write(io->fd, io->buf + written, io->idx - written);
        if (n < 0) {
            if (errno == EINTR) continue;
            return ERR;
        }
        if (n == 0) return ERR;
        written += (size_t)n;
    }

    io->idx = 0;
    return OK;
}

void freeIo(Io *io)
{
    if (!io)
        return;

    if (io->fd != -1)
    {
        if (io->idx > 0)
            flushIo(io);
        fsync(io->fd);
        close(io->fd);
    }
    free(io);
}

int addIo(Io *io, const char *buf, size_t n)
{
    if (!io || !buf)
        return ERR;

    /* 剩余空间足够：直接拷进 buf */
    if (n <= io->len - io->idx)
    {
        memcpy(io->buf + io->idx, buf, n);
        io->idx += n;
        return (int)n;
    }

    /* 不够，但 n ≤ 缓冲区总大小：先 flush，再拷 */
    if (n <= io->len)
    {
        if (flushIo(io) != OK)
            return ERR;
        memcpy(io->buf, buf, n);
        io->idx = n;
        return (int)n;
    }

    /* n > 缓冲区总大小：先 flush，然后绕过 buf 直接 write */
    if (flushIo(io) != OK)
        return ERR;

    size_t written = 0;
    while (written < n)
    {
        ssize_t wn = write(io->fd, buf + written, n - written);
        if (wn < 0) {
            if (errno == EINTR) continue;
            return ERR;
        }
        if (wn == 0) return ERR;
        written += (size_t)wn;
    }
    return (int)n;
}