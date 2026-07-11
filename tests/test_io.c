#include "io.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>

int main(void)
{
    const char *tmp = "/tmp/test_io.rdb";

    /* ---- 写 ---- */
    Io *io = newIo(tmp, 16, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    assert(io != NULL);

    /* 1. 小数据：进 buf，不触发 flush */
    assert(addIo(io, "hello", 5) == 5);
    assert(addIo(io, "world", 5) == 5);
    assert(io->idx == 10);                          /* 还在 buf 里 */

    /* 2. 中等数据：buf 装不下，触发 flush + memcpy */
    assert(addIo(io, "abcdefgh", 8) == 8);          /* 10+8=18>16，flush 后拷入 */
    assert(io->idx == 8);

    /* 3. 大块数据：n > cap，绕过 buf 直接 write */
    char big[64];
    memset(big, 'X', 64);
    assert(addIo(io, big, 64) == 64);               /* flush buf → 直接 write 64B */
    assert(io->idx == 0);

    /* 4. flush 之后再写小数据 */
    assert(addIo(io, "end", 3) == 3);
    assert(flushIo(io) == OK);

    freeIo(io);

    /* ---- 读回验证 ---- */
    int fd = open(tmp, O_RDONLY);
    assert(fd != -1);

    char buf[128];
    ssize_t total = 0, n;
    while ((n = read(fd, buf + total, sizeof(buf) - total)) > 0)
        total += n;
    close(fd);

    /* 期望：hello(5) + world(5) + abcdefgh(8) + big(64) + end(3) = 85 */
    assert(total == 85);
    assert(memcmp(buf, "hello", 5) == 0);
    assert(memcmp(buf + 5, "world", 5) == 0);
    assert(memcmp(buf + 10, "abcdefgh", 8) == 0);
    for (int i = 18; i < 82; i++)
        assert(buf[i] == 'X');
    assert(memcmp(buf + 82, "end", 3) == 0);

    /* ---- getbufIo ---- */
    io = newIo(tmp, 32, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    assert(io != NULL);

    char *dst;
    int avail = getbufIo(io, &dst);
    assert(avail == 32);
    assert(dst == io->buf);
    memcpy(dst, "test", 4);
    io->idx += 4;                     /* getbufIo 路径手动推进 idx */

    avail = getbufIo(io, &dst);
    assert(avail == 28);              /* 32 - 4 */
    assert(dst == io->buf + 4);

    /* getbufIo 只探测，不设指针 */
    avail = getbufIo(io, NULL);
    assert(avail == 28);

    freeIo(io);

    /* ---- 空文件 ---- */
    io = newIo(tmp, 16, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    assert(flushIo(io) == OK);        /* 空 buf flush = noop */
    freeIo(io);

    unlink(tmp);
    printf("PASS: test_io\n");
    return 0;
}
