主要职责：
    将上层与磁盘IO解耦，减少系统调用次数
    为上层提供共用缓冲区，减少malloc次数
只适用于单线程场景
struct Io{
    int fd; // 操作符
    size_t len; //缓冲区的总长度
    size_t idx; //输入缓冲区的起点
    char buf[]; //BUF_SIZE，idx左边是输入队列，右边是输入缓冲区
};

使用方式：
/* 生命周期 */
磁盘Io newIo("地址", len)
void freeIo(Io *io);                /* flush + fsync + close + free */

int getbufIo(Io* io, int**buf) // 返回值：-1 ERR，正数是长度，回调，缓冲区首地址
void addIo(Io *io, size_t n);        /* 提交 n 字节，idx += n */
/* 刷新 — idx 归零，脏数据写盘 */
int  flushIo(Io *io);                /* write(fd, buf, idx)，成功返回 0 */