#include "service.h"
#include "server.h"
#include "sds.h"
#include "val_obj.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>

/* ================================================================
 *  RESP 响应写入
 *
 *  所有函数直接将 RESP 协议格式写入 c->wbuf，
 *  调用前应确保 c->state == CONN_STATE_WRITE 已由上层设置。
 * ================================================================ */

void addReplySimpleString(Connection *c, const char *str)
{
    int n = snprintf(c->wbuf, c->wcap, "+%s\r\n", str);
    c->wlen = (n > 0 && (size_t)n < c->wcap) ? (size_t)n : 0;
}

void addReplyError(Connection *c, const char *msg)
{
    int n = snprintf(c->wbuf, c->wcap, "-ERR %s\r\n", msg);
    c->wlen = (n > 0 && (size_t)n < c->wcap) ? (size_t)n : 0;
}

void addReplyInteger(Connection *c, long long val)
{
    int n = snprintf(c->wbuf, c->wcap, ":%lld\r\n", val);
    c->wlen = (n > 0 && (size_t)n < c->wcap) ? (size_t)n : 0;
}

void addReplyBulkString(Connection *c, const char *str, size_t len)
{
    int hdr = snprintf(c->wbuf, c->wcap, "$%zu\r\n", len);
    if (hdr < 0 || (size_t)hdr + len + 2 > c->wcap) {
        c->wlen = 0;
        return;
    }
    memcpy(c->wbuf + hdr, str, len);
    c->wbuf[hdr + len]     = '\r';
    c->wbuf[hdr + len + 1] = '\n';
    c->wlen = (size_t)hdr + len + 2;
}

void addReplyBulkSds(Connection *c, void *s)
{
    sds str = (sds)s;
    addReplyBulkString(c, str, sdslen(str));
}

void addReplyNull(Connection *c)
{
    if (c->wcap < 5) { c->wlen = 0; return; }
    memcpy(c->wbuf, "$-1\r\n", 5);
    c->wlen = 5;
}

void addReplyOK(Connection *c)
{
    if (c->wcap < 5) { c->wlen = 0; return; }
    memcpy(c->wbuf, "+OK\r\n", 5);
    c->wlen = 5;
}

/* ================================================================
 *  工具函数
 * ================================================================ */

static inline sds respKeyToSds(const RespObj *o)
{
    return sdsnewlen(o->str, o->len);
}

static long long parseLongLong(const RespObj *o, int *ok)
{
    char tmp[32];
    size_t n = o->len > 31 ? 31 : o->len;
    memcpy(tmp, o->str, n);
    tmp[n] = '\0';
    char *end;
    long long val = strtoll(tmp, &end, 10);
    if (ok) *ok = (*end == '\0');
    return val;
}

/* ================================================================
 *  命令：PING / SELECT
 * ================================================================ */

static void pingCommand(Connection *c, struct service *svc,
                        RespObj *argv, int argc)
{
    (void)svc; (void)argv; (void)argc;
    addReplySimpleString(c, "PONG");
}

static void selectCommand(Connection *c, struct service *svc,
                          RespObj *argv, int argc)
{
    (void)argc;
    if (argv[1].type != RESP_STR) {
        addReplyError(c, "wrong type for index");
        return;
    }
    char tmp[32];
    size_t n = argv[1].len > 31 ? 31 : argv[1].len;
    memcpy(tmp, argv[1].str, n);
    tmp[n] = '\0';
    char *end;
    long idx = strtol(tmp, &end, 10);
    if (*end != '\0' || idx < 0 || (unsigned long)idx >= svc->dbsize) {
        addReplyError(c, "index out of range");
        return;
    }
    c->dbnum = (unsigned int)idx;
    addReplyOK(c);
}

/* ================================================================
 *  命令：GET / SET / EXISTS / DEL
 * ================================================================ */

static void getCommand(Connection *c, struct service *svc,
                       RespObj *argv, int argc)
{
    (void)argc;
    if (argv[1].type != RESP_STR) {
        addReplyError(c, "wrong type for key");
        return;
    }
    sds key = respKeyToSds(&argv[1]);
    if (!key) { addReplyError(c, "OOM"); return; }

    ValObj *obj = kvdbGet(svc->kvs[c->dbnum], key);
    if (!obj) {
        addReplyNull(c);
    } else if (obj->type == VAL_INT) {
        addReplyInteger(c, obj->val.ll);
    } else if (obj->type == VAL_STRING) {
        addReplyBulkSds(c, obj->val.str);
    } else {
        addReplyNull(c);
    }
    sdsfree(key);
}

static void setCommand(Connection *c, struct service *svc,
                       RespObj *argv, int argc)
{
    (void)argc;
    if (argv[1].type != RESP_STR ||
        (argv[2].type != RESP_STR && argv[2].type != RESP_INT)) {
        addReplyError(c, "wrong type for key or value");
        return;
    }

    sds key = respKeyToSds(&argv[1]);
    if (!key) { addReplyError(c, "OOM"); return; }

    ValObj *obj = malloc(sizeof(ValObj));
    if (!obj) { sdsfree(key); addReplyError(c, "OOM"); return; }

    if (argv[2].type == RESP_INT) {
        obj->type   = VAL_INT;
        obj->val.ll = argv[2].integer;
    } else {
        sds val = sdsnewlen(argv[2].str, argv[2].len);
        if (!val) { sdsfree(key); free(obj); addReplyError(c, "OOM"); return; }
        obj->type   = VAL_STRING;
        obj->val.str = val;
    }

    ValObj *old = kvdbSet(svc->kvs[c->dbnum], key, obj);
    if (old) valObjFree(old);
    sdsfree(key);   /* kvdb 内部已 dup，调用方始终释放 */
    addReplyOK(c);
}

static void existsCommand(Connection *c, struct service *svc,
                          RespObj *argv, int argc)
{
    (void)argc;
    if (argv[1].type != RESP_STR) {
        addReplyError(c, "wrong type for key");
        return;
    }
    sds key = respKeyToSds(&argv[1]);
    if (!key) { addReplyError(c, "OOM"); return; }

    addReplyInteger(c, kvdbExists(svc->kvs[c->dbnum], key));
    sdsfree(key);
}

static void delCommand(Connection *c, struct service *svc,
                       RespObj *argv, int argc)
{
    (void)argc;
    if (argv[1].type != RESP_STR) {
        addReplyError(c, "wrong type for key");
        return;
    }
    sds key = respKeyToSds(&argv[1]);
    if (!key) { addReplyError(c, "OOM"); return; }

    addReplyInteger(c, kvdbDel(svc->kvs[c->dbnum], key));
    sdsfree(key);
}

/* ================================================================
 *  命令：EXPIRE / PEXPIRE / EXPIREAT / PEXPIREAT
 *
 *  内部统一转 time_t 绝对秒，交给 kvdbExpire。
 * ================================================================ */

enum ExpireMode { EXPIRE_SEC, PEXPIRE_MS, EXPIREAT_SEC, PEXPIREAT_MS };

static void expireGenericCommand(Connection *c, struct service *svc,
                                 RespObj *argv, int argc, enum ExpireMode mode)
{
    (void)argc;
    if (argv[1].type != RESP_STR || argv[2].type != RESP_STR) {
        addReplyError(c, "wrong type for key or time");
        return;
    }

    sds key = respKeyToSds(&argv[1]);
    if (!key) { addReplyError(c, "OOM"); return; }

    int ok;
    long long val = parseLongLong(&argv[2], &ok);
    if (!ok || val < 0) {
        addReplyError(c, "invalid expire time");
        sdsfree(key);
        return;
    }

    time_t when;
    switch (mode) {
    case EXPIRE_SEC:    when = time(NULL) + (time_t)val;       break;
    case PEXPIRE_MS:    when = time(NULL) + (time_t)(val/1000); break;
    case EXPIREAT_SEC:  when = (time_t)val;                     break;
    case PEXPIREAT_MS:  when = (time_t)(val/1000);              break;
    default:            when = 0; break;
    }

    addReplyInteger(c, kvdbExpire(svc->kvs[c->dbnum], key, when));
    sdsfree(key);
}

static void expireCommand(Connection *c, struct service *svc,
                          RespObj *argv, int argc)
{ expireGenericCommand(c, svc, argv, argc, EXPIRE_SEC); }
static void pexpireCommand(Connection *c, struct service *svc,
                           RespObj *argv, int argc)
{ expireGenericCommand(c, svc, argv, argc, PEXPIRE_MS); }
static void expireatCommand(Connection *c, struct service *svc,
                            RespObj *argv, int argc)
{ expireGenericCommand(c, svc, argv, argc, EXPIREAT_SEC); }
static void pexpireatCommand(Connection *c, struct service *svc,
                             RespObj *argv, int argc)
{ expireGenericCommand(c, svc, argv, argc, PEXPIREAT_MS); }

/* ================================================================
 *  命令：TTL / PTTL
 *
 *  kvdbTTL 返回秒，PTTL 时乘 1000。
 * ================================================================ */

static void ttlGenericCommand(Connection *c, struct service *svc,
                              RespObj *argv, int argc, int millisec)
{
    (void)argc;
    if (argv[1].type != RESP_STR) {
        addReplyError(c, "wrong type for key");
        return;
    }
    sds key = respKeyToSds(&argv[1]);
    if (!key) { addReplyError(c, "OOM"); return; }

    long long ttl = kvdbTTL(svc->kvs[c->dbnum], key);
    if (millisec && ttl > 0) ttl *= 1000;
    addReplyInteger(c, ttl);
    sdsfree(key);
}

static void ttlCommand(Connection *c, struct service *svc,
                       RespObj *argv, int argc)
{ ttlGenericCommand(c, svc, argv, argc, 0); }
static void pttlCommand(Connection *c, struct service *svc,
                        RespObj *argv, int argc)
{ ttlGenericCommand(c, svc, argv, argc, 1); }

/* ================================================================
 *  命令：PERSIST
 * ================================================================ */

static void persistCommand(Connection *c, struct service *svc,
                           RespObj *argv, int argc)
{
    (void)argc;
    if (argv[1].type != RESP_STR) {
        addReplyError(c, "wrong type for key");
        return;
    }
    sds key = respKeyToSds(&argv[1]);
    if (!key) { addReplyError(c, "OOM"); return; }

    addReplyInteger(c, kvdbPersist(svc->kvs[c->dbnum], key));
    sdsfree(key);
}

/* ================================================================
 *  命令表
 *
 *  按字典序排列，供 bsearch 二分查找。
 * ================================================================ */

static Command cmd_table[] = {
    {"DEL",       1,  delCommand},
    {"EXISTS",    1,  existsCommand},
    {"EXPIRE",    2,  expireCommand},
    {"EXPIREAT",  2,  expireatCommand},
    {"GET",       1,  getCommand},
    {"PERSIST",   1,  persistCommand},
    {"PEXPIRE",   2,  pexpireCommand},
    {"PEXPIREAT", 2,  pexpireatCommand},
    {"PING",      0,  pingCommand},
    {"PTTL",      1,  pttlCommand},
    {"SELECT",    1,  selectCommand},
    {"SET",       2,  setCommand},
    {"TTL",       1,  ttlCommand},
};

static int cmdCompare(const void *key, const void *elem)
{
    const RespObj *o   = (const RespObj *)key;
    const Command *cmd = (const Command *)elem;
    size_t slen   = strlen(cmd->name);
    size_t minlen = o->len < slen ? o->len : slen;

    int cmp = strncasecmp((const char *)o->str, cmd->name, minlen);
    if (cmp != 0) return cmp;
    if (o->len < slen) return -1;
    if (o->len > slen) return  1;
    return 0;
}

/* ================================================================
 *  命令分发
 * ================================================================ */

int processCommand(Connection *c, struct service *svc,
                   RespObj *argv, int argc)
{
    if (argc < 1) return SERVICE_ERR;
    if (argv[0].type != RESP_STR) return SERVICE_ERR;

    size_t ncmd = sizeof(cmd_table) / sizeof(cmd_table[0]);
    Command *cmd = bsearch(&argv[0], cmd_table, ncmd, sizeof(Command), cmdCompare);

    if (!cmd) {
        addReplyError(c, "unknown command");
        return SERVICE_OK;
    }
    if (cmd->arity >= 0 && argc - 1 != cmd->arity) {
        addReplyError(c, "wrong number of arguments");
        return SERVICE_OK;
    }

    cmd->handler(c, svc, argv, argc);
    return SERVICE_OK;
}

/* ================================================================
 *  生命周期
 * ================================================================ */

int serviceInit(struct service *svc, unsigned int dbsize)
{
    if (!svc || dbsize == 0) return SERVICE_ERR;

    svc->dbsize = dbsize;
    svc->kvs = calloc(dbsize, sizeof(kvdb *));
    if (!svc->kvs) return SERVICE_ERR;

    for (unsigned int i = 0; i < dbsize; i++) {
        svc->kvs[i] = kvdbNew();
        if (!svc->kvs[i]) {
            for (unsigned int j = 0; j < i; j++)
                kvdbFree(svc->kvs[j]);
            free(svc->kvs);
            svc->kvs = NULL;
            return SERVICE_ERR;
        }
    }
    return SERVICE_OK;
}

void serviceFree(struct service *svc)
{
    if (!svc || !svc->kvs) return;
    for (unsigned int i = 0; i < svc->dbsize; i++)
        kvdbFree(svc->kvs[i]);
    free(svc->kvs);
    svc->kvs = NULL;
}
