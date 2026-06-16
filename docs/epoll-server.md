# epoll 网络服务器

## 概述

单线程 epoll 事件驱动的 TCP 服务器骨架（Reactor 模式）。与 Redis 共享相同的 `epoll_wait + O_NONBLOCK` 模型，配合 RESP 解析器完成请求-响应循环。

## 常量

```c
#define SERVER_PORT 6379   // 默认端口
#define MAX_EVENTS   1024   // 每次 epoll_wait 返回的事件上限
#define BUF_SIZE     4096   // 单个 Connection 收发缓冲区大小
```

## enum ConnState

```c
enum ConnState {
    CONN_STATE_READ,   // 等待客户端请求，关注 EPOLLIN
    CONN_STATE_WRITE,  // 准备回写响应，关注 EPOLLIN | EPOLLOUT
    CONN_STATE_CLOSE,  // 标记待关闭，下一轮事件循环释放
};
```

状态转换：

```
READ ──(收到完整请求)──→ WRITE ──(写完响应)──→ READ
  │                       │
  └──(EOF / 错误)────────┴──→ CLOSE
```

## struct Connection

```c
typedef struct Connection {
    int           fd;     // 客户端 socket fd
    enum ConnState state; // 当前状态
    char *rbuf;           // 读缓冲区指针
    size_t rlen;          // 读缓冲区已用字节数
    size_t rcap;          // 读缓冲区容量（固定 BUF_SIZE）
    char *wbuf;           // 写缓冲区指针
    size_t wlen;          // 写缓冲区待发送字节数
    size_t wcap;          // 写缓冲区容量（固定 BUF_SIZE）
} Connection;
```

**生命周期：**
- `connNew(fd)` 创建，分配读写缓冲区各 4KB，设置 socket 为非阻塞
- `connFree(c)` 关闭 fd 并释放两块缓冲区

**注意事项：**
- 缓冲区大小固定为 `BUF_SIZE`（4096），无动态扩容。若上层协议（RESP）产生超过 4KB 的响应，写会截断或出错。
- `rlen` 由 `handleRead` 写入，当前实现为单次 `read()` 覆盖式写入（不累计）。**流式读取尚未实现**——需要配合 RESP 的 `RESP_AGAIN` 做缓冲区堆积。

## struct Server

```c
struct Server {
    int listen_fd;   // 监听 socket
    int epoll_fd;    // epoll 实例 fd
    int stop;        // 退出标志（信号安全写入）
};
```

## 公共 API

### serverCreate

```c
struct Server *serverCreate(int port);
```

创建并初始化服务器。内部执行：`socket + bind + listen + setNonBlock + epoll_create1`。失败返回 `NULL`，成功返回已注册监听 fd 的 Server 指针。

### serverRun

```c
void serverRun(struct Server *s);
```

主事件循环。阻塞在 `epoll_wait(s->epoll_fd, ..., -1)`，收到事件后分发：

| 事件来源 | 处理函数 | 逻辑 |
|---------|---------|------|
| `listen_fd` 可读 | `handleAccept()` | accept → connNew → epoll_ctl ADD |
| 客户端 fd 可读 | `handleRead()` | read → 解析（TODO）→ 填充 wbuf → 状态切 WRITE |
| 客户端 fd 可写 | `handleWrite()` | write → 状态切 READ |
| 状态为 CLOSE | `handleClose()` | epoll_ctl DEL → connFree |

循环在 `s->stop == 1` 时退出，或 `epoll_wait` 出错时 break。

### serverDestroy

```c
void serverDestroy(struct Server *s);
```

关闭 `listen_fd` 和 `epoll_fd`，释放 Server 结构体。**不会自动释放活跃连接**——目前没有连接表来跟踪所有 Connection，server 退出时活跃连接直接随进程消失。

## 内部函数（不对外暴露）

| 函数 | 职责 |
|------|------|
| `setNonBlock(fd)` | `fcntl(O_NONBLOCK)` |
| `listenOnPort(port)` | socket → bind → listen → setNonBlock |
| `connNew(fd)` | 分配 Connection + 缓冲区，setNonBlock |
| `connFree(c)` | 关闭 fd + 释放缓冲区 |
| `handleAccept(s, epoll_fd)` | accept → connNew → epoll_ctl ADD |
| `handleRead(c)` | read → 处理（TODO）→ 状态机 |
| `handleWrite(c)` | write → 状态机 |
| `handleClose(c, epoll_fd)` | epoll_ctl DEL → connFree |

## 已知局限

1. **无流式读取** — `handleRead` 每次 `read()` 覆盖 `rbuf`，不累计。遇到 RESP `AGAIN` 时无法保留已有数据。
2. **无连接追踪** — 没有连接池/哈希表来管理所有 Connection。`serverDestroy` 不释放连接，信号退出时有连接泄漏。
3. **写缓冲区不处理部分写入** — `handleWrite` 假设一次 `write()` 写完所有 `wbuf` 数据（注释已标注）。
4. **epoll_ctl MOD 冗余** — 主循环末尾无条件 `epoll_ctl MOD`，即使事件类型未变化也触发系统调用。
5. **单线程模型** — 没有线程池/进程池，一个慢客户端阻塞整个事件循环（epoll 模型下不阻塞 io，但后续若接入 CPU 密集型命令则会影响其他客户端）。
