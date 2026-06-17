#include "server.h"
#include "log.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

#define MAX_EVENTS 1024
#define BUF_SIZE 4096

/* ---------- 辅助函数 ---------- */

static int setNonBlock(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1) return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static int listenOnPort(int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd == -1) {
        LOG_ERROR("socket: %s", strerror(errno));
        return -1;
    }

    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port   = htons(port),
        .sin_addr   = { .s_addr = htonl(INADDR_ANY) },
    };

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) == -1) {
        LOG_ERROR("bind(%d): %s", port, strerror(errno));
        close(fd);
        return -1;
    }

    if (listen(fd, 128) == -1) {
        LOG_ERROR("listen: %s", strerror(errno));
        close(fd);
        return -1;
    }

    if (setNonBlock(fd) == -1) {
        close(fd);
        return -1;
    }

    LOG_INFO("listening on port %d", port);
    return fd;
}

/* ---------- 连接管理 ---------- */

static Connection *connNew(int fd) {
    Connection *c = malloc(sizeof(*c));
    if (!c) return NULL;
    c->fd       = fd;
    c->state    = CONN_STATE_READ;
    c->rbuf     = NULL;
    c->wbuf     = NULL;

    c->rbuf = malloc(BUF_SIZE);
    if (!c->rbuf) goto fail;
    c->rcap = BUF_SIZE;
    c->rlen = 0;

    c->wbuf = malloc(BUF_SIZE);
    if (!c->wbuf) goto fail;
    c->wcap = BUF_SIZE;
    c->wlen = 0;
    c->dbnum = 0;

    setNonBlock(fd);
    LOG_DEBUG("accept fd=%d", fd);
    return c;

fail:
    free(c->rbuf);
    free(c->wbuf);
    free(c);
    return NULL;
}

static void connFree(Connection *c) {
    if (!c) return;
    LOG_DEBUG("close fd=%d", c->fd);
    close(c->fd);
    free(c->rbuf);
    free(c->wbuf);
    free(c);
}

/* ---------- 事件处理 ---------- */

/* 读取客户端请求（由上层解析协议） */
static void handleRead(Connection *c) {
    ssize_t n = read(c->fd, c->rbuf, c->rcap - 1);  /* -1 留 '\0' 空间 */
    if (n > 0) {
        c->rlen = (size_t)n;
        c->rbuf[n] = '\0';  /* 方便调试看字符串 */
        LOG_DEBUG("recv fd=%d: %.*s", c->fd, (int)n, c->rbuf);

        /*
         * TODO: 解析 RESP 协议 + 执行命令
         *
         *   - 调用 respParse(c->rbuf, c->rlen) → 参数
         *   - 调用引擎接口 → 结果
         *   - 写入 c->wbuf
         *
         * 临时 demo：原样 echo 回去
         */
        c->wlen = (size_t)snprintf(c->wbuf, c->wcap, "+OK\r\n");
        c->state = CONN_STATE_WRITE;
    } else if (n == 0) {
        /* 客户端关闭 */
        c->state = CONN_STATE_CLOSE;
    } else {
        if (errno != EAGAIN) {
            LOG_WARN("read fd=%d: %s", c->fd, strerror(errno));
            c->state = CONN_STATE_CLOSE;
        }
    }
}

static void handleWrite(Connection *c) {
    ssize_t n = write(c->fd, c->wbuf, c->wlen);
    if (n > 0) {
        /* 假设一次写完（简单场景，复杂场景要处理部分写入） */
        c->state = CONN_STATE_READ;
    } else if (n == -1 && errno != EAGAIN) {
        LOG_WARN("write fd=%d: %s", c->fd, strerror(errno));
        c->state = CONN_STATE_CLOSE;
    }
}

static void handleAccept(struct Server *s, int epoll_fd) {
    struct sockaddr_in addr;
    socklen_t addrlen = sizeof(addr);

    int fd = accept(s->listen_fd, (struct sockaddr *)&addr, &addrlen);
    if (fd == -1) {
        if (errno != EAGAIN) {
            LOG_ERROR("accept: %s", strerror(errno));
        }
        return;
    }

    Connection *c = connNew(fd);
    if (!c) {
        close(fd);
        return;
    }

    struct epoll_event ev = {
        .events = EPOLLIN,
        .data.ptr = c,
    };
    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, fd, &ev) == -1) {
        LOG_ERROR("epoll_ctl add fd=%d: %s", fd, strerror(errno));
        connFree(c);
    }
}

static void handleClose(Connection *c, int epoll_fd) {
    epoll_ctl(epoll_fd, EPOLL_CTL_DEL, c->fd, NULL);
    connFree(c);
}

/* ---------- 服务器生命周期 ---------- */

struct Server *serverCreate(int port) {
    struct Server *s = malloc(sizeof(*s));
    if (!s) return NULL;

    s->listen_fd = listenOnPort(port);
    if (s->listen_fd == -1) {
        free(s);
        return NULL;
    }

    s->epoll_fd = epoll_create1(0);
    if (s->epoll_fd == -1) {
        LOG_ERROR("epoll_create: %s", strerror(errno));
        close(s->listen_fd);
        free(s);
        return NULL;
    }

    s->stop = 0;

    /* 将 listen_fd 加入 epoll */
    struct epoll_event ev = {
        .events = EPOLLIN,
        .data.fd = s->listen_fd,
    };
    if (epoll_ctl(s->epoll_fd, EPOLL_CTL_ADD, s->listen_fd, &ev) == -1) {
        LOG_ERROR("epoll_ctl add listen: %s", strerror(errno));
        close(s->epoll_fd);
        close(s->listen_fd);
        free(s);
        return NULL;
    }

    LOG_INFO("server created, epoll_fd=%d", s->epoll_fd);
    return s;
}

void serverRun(struct Server *s) {
    if (!s) return;

    struct epoll_event events[MAX_EVENTS];
    LOG_INFO("server started");

    while (!s->stop) {
        int n = epoll_wait(s->epoll_fd, events, MAX_EVENTS, -1);
        if (n == -1) {
            if (errno == EINTR) continue; /* 被信号打断 */
            LOG_ERROR("epoll_wait: %s", strerror(errno));
            break;
        }

        for (int i = 0; i < n; i++) {
            if (events[i].data.fd == s->listen_fd) {
                /* 新连接 */
                handleAccept(s, s->epoll_fd);
            } else {
                Connection *c = (Connection *)events[i].data.ptr;
                if (events[i].events & EPOLLIN) {
                    handleRead(c);
                }
                if (events[i].events & EPOLLOUT) {
                    handleWrite(c);
                }
                if (c->state == CONN_STATE_CLOSE) {
                    handleClose(c, s->epoll_fd);
                } else {
                    /* 根据需要更新 epoll 事件类型 */
                    uint32_t ev_mask = EPOLLIN;
                    if (c->state == CONN_STATE_WRITE) ev_mask |= EPOLLOUT;
                    struct epoll_event ev = {
                        .events = ev_mask,
                        .data.ptr = c,
                    };
                    epoll_ctl(s->epoll_fd, EPOLL_CTL_MOD, c->fd, &ev);
                }
            }
        }
    }

    LOG_INFO("server stopped");
}

void serverDestroy(struct Server *s) {
    if (!s) return;
    close(s->listen_fd);
    close(s->epoll_fd);
    free(s);
    LOG_INFO("server destroyed");
}
