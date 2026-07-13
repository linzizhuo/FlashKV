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


input是同样的逻辑，先批量写入缓冲区，随后上面需要read的时候才将缓冲区的内容交给上层
但是如果上层需要一个很大的数据，缓冲区需要多次IO，这反而影响了速度。
有两种方法：第一种是在结构体中配置一个buf*，用一次malloc去替代多次io
第二种是直接写入上层提供的buf中，这里选择第二种策略

优化：现在的结构只有写入的话是没有什么问题的，但如果要兼容写和读的话就有问题了，因为只有一个buf，还没有变量来管理缓冲区（这里指的没有变量管理缓冲区是指没有变量标识写/读缓冲区的分界，核心在缓冲区没法区分，而不是只有一个缓冲区）所以虽然可以实现读写（加tag）但读写都是半双工的，在频繁读写切换的场景极其浪费性能——每次切换都要刷新缓冲区！要想实现全双工就要对缓冲区进行划分。下面是方案：
typedef struct Io
{
    int fd;
    size_t buflen;    /* 单个 buf 大小 */
    size_t write_idx; /* 待 flush 数据量 */
    size_t read_idx;  /* 缓冲区已缓存数据量 */
    size_t read_pos;  /* 消费位置 */
    char buf[];       /* 柔性数组：前半写、后半读，各 buflen */
} Io;