#ifndef _SERVICE_H
#define _SERVICE_H

#include "resp.h"
#include "dict.h"

#define DICT_HT_INITIAL_SIZE 4

#define SERVICE_OK    0
#define SERVICE_ERR  -1   /* 协议错误（argv 为空/命令名非 STR） */
#define SERVICE_AGAIN -2   /* 数据不完整，等下一轮 read */

/* 服务层状态 */
struct service
{
    struct dict **db;
    struct dict **expires;   /* TTL 独立字典，key→绝对秒时间戳（inline 存储） */
    unsigned int dbsize;
};

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
