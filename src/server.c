#define _POSIX_C_SOURCE 199309L

#include "server.h"
#include "log.h"
#include "resp.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define MAX_EVENTS 1024
#define BUF_SIZE 4096
#define MAX_PIPELINE_BATCH 16  /* handleRead 单轮最多处理命令数，平衡吞吐与延迟 */

static long long mstime(void);

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

static Connection *connNew(int fd, struct service *svc) {
    Connection *c = malloc(sizeof(*c));
    if (!c) return NULL;
    c->fd       = fd;
    c->state    = CONN_STATE_READ;
    c->rbuf     = NULL;
    c->wbuf     = NULL;
    c->svc      = svc;
    c->dbnum    = 0;

    c->rbuf = malloc(BUF_SIZE);
    if (!c->rbuf) goto fail;
    c->rcap = BUF_SIZE;
    c->rlen = 0;

    c->wbuf = malloc(BUF_SIZE);
    if (!c->wbuf) goto fail;
    c->wcap = BUF_SIZE;
    c->wlen = 0;

    setNonBlock(fd);
    {
        int opt = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));
    }
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

/* 读取 → 解析 RESP → 执行命令 → 追加响应
 *
 * 循环消费缓冲区中的完整命令（支持 pipeline）。
 * 单轮最多处理 MAX_PIPELINE_BATCH 条，
 * 剩余数据留在 rbuf，由 handleWrite 写完后触发下一轮。
 *
 * skip_read=true: 不读 socket，仅消费 rbuf 已有数据（handleWrite 尾触发）。
 * skip_read=false: 先读 socket，再消费 rbuf（事件循环 EPOLLIN）。 */

static void handleRead(Connection *c, int skip_read) {
    if (!skip_read) {
        size_t space = c->rcap - c->rlen - 1;
        if (space == 0) {
            addReplyError(c, "request too large");
            c->state = CONN_STATE_WRITE;
            c->rlen  = 0;
            return;
        }

        ssize_t n = read(c->fd, c->rbuf + c->rlen, space);
        if (n > 0) {
            c->rlen += (size_t)n;
            c->rbuf[c->rlen] = '\0';
            LOG_DEBUG("recv fd=%d (%zd bytes)", c->fd, n);
        } else if (n == 0) {
            c->state = CONN_STATE_CLOSE;
            return;
        } else if (errno != EAGAIN) {
            LOG_WARN("read fd=%d: %s", c->fd, strerror(errno));
            c->state = CONN_STATE_CLOSE;
            return;
        }
    }

    if (c->rlen == 0) return;

    {
        int processed = 0;
        while (c->rlen > 0 && processed < MAX_PIPELINE_BATCH) {
            RespObj obj;
            int ret = respParse(c->rbuf, c->rlen, &obj);

            if (ret > 0) {
                if (obj.type == RESP_ARRAY && obj.len > 0) {
                    processCommand(c, c->svc, obj.elements, (int)obj.len);
                } else {
                    addReplyError(c, "expected array");
                }
                if ((size_t)ret < c->rlen)
                    memmove(c->rbuf, c->rbuf + ret, c->rlen - ret);
                c->rlen -= (size_t)ret;
                respFreeObj(&obj);
                c->state = CONN_STATE_WRITE;
                processed++;

            } else if (ret == RESP_AGAIN) {
                if (c->rlen >= c->rcap - 1) {
                    addReplyError(c, "request too large");
                    c->state = CONN_STATE_WRITE;
                    c->rlen  = 0;
                } else if (processed == 0) {
                    c->state = CONN_STATE_READ;
                }
                break;

            } else {
                addReplyError(c, "protocol error");
                c->state = CONN_STATE_WRITE;
                c->rlen  = 0;
                break;
            }
        }
    }
}

static void handleWrite(Connection *c) {
    while (c->wlen > 0) {
        ssize_t n = write(c->fd, c->wbuf, c->wlen);
        if (n > 0) {
            c->wlen -= (size_t)n;
            memmove(c->wbuf, c->wbuf + n, c->wlen);
        } else if (n == -1 && errno == EAGAIN) {
            return;
        } else {
            LOG_WARN("write fd=%d: %s", c->fd, strerror(errno));
            c->state = CONN_STATE_CLOSE;
            return;
        }
    }

    /* wbuf 清空后，若 rbuf 还有未消费数据（handleRead 被 MAX_PIPELINE_BATCH 截断），
     * 立即触发下一轮处理，不等待 EPOLLIN */
    if (c->rlen > 0) {
        handleRead(c, 1);
        if (c->state == CONN_STATE_WRITE) {
            handleWrite(c);
        }
        return;
    }

    c->state = CONN_STATE_READ;
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

    Connection *c = connNew(fd, &s->svc);
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

/* ---------- 定期任务 ---------- */

static long long mstime(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void databasesCron(struct Server *s)
{
    /* 每次只跑一个库，轮转，16 个库 1.6 秒一圈 */
    kvdbActiveExpireCycle(s->svc.kvs[s->cron_db]);
    if (++s->cron_db >= s->svc.dbsize)
        s->cron_db = 0;
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
    s->last_cron_ms = mstime();
    s->cron_db = 0;

    /* 初始化服务层（16 个数据库） */
    if (serviceInit(&s->svc, 16) != SERVICE_OK) {
        LOG_ERROR("serviceInit failed");
        close(s->epoll_fd);
        close(s->listen_fd);
        free(s);
        return NULL;
    }

    /* 将 listen_fd 加入 epoll */
    struct epoll_event ev = {
        .events = EPOLLIN,
        .data.fd = s->listen_fd,
    };
    if (epoll_ctl(s->epoll_fd, EPOLL_CTL_ADD, s->listen_fd, &ev) == -1) {
        LOG_ERROR("epoll_ctl add listen: %s", strerror(errno));
        serviceFree(&s->svc);
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
        int n = epoll_wait(s->epoll_fd, events, MAX_EVENTS, 100);
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
                    handleRead(c, 0);
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

        /* 每 100ms 执行一次定期任务（过期 key 抽样删除） */
        long long now = mstime();
        if (now - s->last_cron_ms >= 100) {
            databasesCron(s);
            s->last_cron_ms = now;
        }
    }

    LOG_INFO("server stopped");
}

void serverDestroy(struct Server *s) {
    if (!s) return;
    close(s->listen_fd);
    close(s->epoll_fd);
    serviceFree(&s->svc);
    free(s);
    LOG_INFO("server destroyed");
}
