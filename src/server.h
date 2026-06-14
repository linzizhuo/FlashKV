#ifndef _SERVER_H
#define _SERVER_H

#include <stddef.h>

#define SERVER_PORT 6379

/* 连接状态 */
enum ConnState {
    CONN_STATE_READ,   /* 等待请求 */
    CONN_STATE_WRITE,  /* 准备响应 */
    CONN_STATE_CLOSE,  /* 关闭 */
};

/* 连接对象 */
typedef struct Connection {
    int fd;
    enum ConnState state;

    /* 读缓冲区 */
    char *rbuf;
    size_t rlen;
    size_t rcap;

    /* 写缓冲区 */
    char *wbuf;
    size_t wlen;
    size_t wcap;
} Connection;

/* 服务器 */
struct Server {
    int listen_fd;
    int epoll_fd;
    int stop;          /* 退出标志 */
};

/* API */
struct Server *serverCreate(int port);
void serverRun(struct Server *s);
void serverDestroy(struct Server *s);

#endif /* _SERVER_H */
