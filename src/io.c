#include "io.h"
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>

Io *newIo(const char *path, size_t cap, int flags, mode_t mode)
{
    Io *io = malloc(sizeof(Io) + (cap << 1));
    if (!io)
        return NULL;

    io->fd = open(path, flags, mode);
    if (io->fd == -1)
    {
        free(io);
        return NULL;
    }

    io->buflen = cap;
    io->write_idx = 0;
    io->read_idx = 0;
    io->read_pos = 0;
    return io;
}

int getbufIo(Io *io, char **buf)
{
    if (!io)
        return ERR;
    if (buf)
        *buf = wbuf(io) + io->write_idx;
    return (int)(io->buflen - io->write_idx);
}

int flushIo(Io *io, int dir)
{
    if (!io || io->fd == -1)
        return ERR;

    if (dir == FLUSH_WRITE)
    {
        if (io->write_idx == 0)
            return OK;

        size_t written = 0;
        while (written < io->write_idx)
        {
            ssize_t n = write(io->fd, wbuf(io) + written, io->write_idx - written);
            if (n < 0)
            {
                if (errno == EINTR)
                    continue;
                return ERR;
            }
            if (n == 0)
                return ERR;
            written += (size_t)n;
        }
        io->write_idx = 0;
        return OK;
    }
    else /* FLUSH_READ */
    {
        /* 缓冲区还有未消费数据，不动，最简单的策略了 */
        if (io->read_pos < io->read_idx)
            return OK;

        ssize_t n;
        while ((n = read(io->fd, rbuf(io), io->buflen)) < 0)
        {
            if (errno == EINTR)
                continue;
            return ERR;
        }
        if (n == 0)
            return ERR; /* EOF */
        io->read_idx = (size_t)n;
        io->read_pos = 0;
        return OK;
    }
}

void freeIo(Io *io)
{
    if (!io)
        return;
    if (io->fd != -1)
    {
        if (io->write_idx > 0)
            flushIo(io, FLUSH_WRITE);
        fsync(io->fd);
        close(io->fd);
    }
    free(io);
}

int addIo(Io *io, const char *src, size_t n)
{
    if (!io || !src)
        return ERR;

    /* 剩余空间足够 */
    if (n <= io->buflen - io->write_idx)
    {
        memcpy(wbuf(io) + io->write_idx, src, n);
        io->write_idx += n;
        return (int)n;
    }

    /* 不够但 n ≤ 缓冲区总大小：先 flush，再拷 */
    if (n <= io->buflen)
    {
        if (flushIo(io, FLUSH_WRITE) != OK)
            return ERR;
        memcpy(wbuf(io), src, n);
        io->write_idx = n;
        return (int)n;
    }

    /* n > 缓冲区总大小：flush 后绕过 buf 直接 write */
    if (flushIo(io, FLUSH_WRITE) != OK)
        return ERR;

    size_t written = 0;
    while (written < n)
    {
        ssize_t wn = write(io->fd, src + written, n - written);
        if (wn < 0)
        {
            if (errno == EINTR)
                continue;
            return ERR;
        }
        if (wn == 0)
            return ERR;
        written += (size_t)wn;
    }
    return (int)n;
}

int readIo(Io *io, char *dst, size_t n)
{
    if (!io || !dst)
        return ERR;

    size_t avail = io->read_idx - io->read_pos;

    /* 阶段 1：n ≤ 缓冲区已有数据，直接拷 */
    if (n <= avail)
    {
        memcpy(dst, rbuf(io) + io->read_pos, n);
        io->read_pos += n;
        return OK;
    }

    /* 阶段 2：消费掉残量 */
    if (avail > 0)
    {
        memcpy(dst, rbuf(io) + io->read_pos, avail);
        io->read_pos += avail;
        dst += avail;
        n -= avail;
    }

    /* 阶段 3：n ≤ buflen → 补数据再拷 */
    if (n <= io->buflen)
    {
        if (flushIo(io, FLUSH_READ) != OK)
            return ERR;
        if (n > io->read_idx)
            return ERR;

        memcpy(dst, rbuf(io), n);
        io->read_pos = n;
        return OK;
    }

    /* 阶段 4：n > buflen → 绕过缓冲区直读 */
    while (n > 0)
    {
        ssize_t r = read(io->fd, dst, n);
        if (r < 0)
        {
            if (errno == EINTR)
                continue;
            return ERR;
        }
        if (r == 0)
            return ERR;
        dst += (size_t)r;
        n -= (size_t)r;
    }
    return OK;
}