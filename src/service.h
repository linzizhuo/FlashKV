#ifndef _SERVICE_H
#define _SERVICE_H

#include "resp.h"
#include "kvdb.h"

/* 服务层状态 */
struct service
{
    kvdb **kvs;
    unsigned int dbsize;
};

/* 全局实例（expire 等模块通过它访问服务层） */
extern struct service *service;

/* 前向声明，避免循环依赖 */
struct Connection;

/* 命令处理函数签名 */
typedef void (*CmdHandler)(struct Connection *c, struct service *svc,
                           RespObj *argv, int argc);

/* 命令表条目 */
typedef struct
{
    const char *name;
    int         arity;   /* 参数个数（不含命令名），-1 变长 */
    CmdHandler  handler;
} Command;

/* 定期抽样 */
void activeExpireCycle(int type);
/* SLOW→FAST 升级：调用方在 epoll_wait 前调用。
 * 内部检查 timelimit_exit + 频率控制，决定是否触发 FAST。 */
void activeExpireTryFast(void);

/* ---- 生命周期 ---- */
int  serviceInit(struct service *svc, unsigned int dbsize);
void serviceFree(struct service *svc);

/* ---- 命令分发 ---- */
int  processCommand(struct Connection *c, struct service *svc,
                    RespObj *argv, int argc);

/* ---- RESP 响应写入 ---- */
void addReplySimpleString(struct Connection *c, const char *str);
void addReplyError(struct Connection *c, const char *msg);
void addReplyInteger(struct Connection *c, long long val);
void addReplyBulkString(struct Connection *c, const char *str, size_t len);
void addReplyBulkSds(struct Connection *c, void *s);
void addReplyNull(struct Connection *c);
void addReplyOK(struct Connection *c);

#endif
