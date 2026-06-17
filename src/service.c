#include "service.h"
#include "server.h"
#include "dict_type.h"
#include "sds.h"
#include "val_obj.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

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
        c->wlen = 0;   /* buffer 不足，清空 */
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
 *  内部辅助
 * ================================================================ */

/* 大小写不敏感比较 RespObj 字符串 与 C 字符串 */
static int respStrEqCase(RespObj *o, const char *s)
{
    size_t slen = strlen(s);
    if (o->len != slen) return 0;
    return strncasecmp((const char *)o->str, s, slen) == 0;
}

/* ================================================================
 *  命令实现
 * ================================================================ */

static void pingCommand(Connection *c, struct service *svc,
                        RespObj *argv, int argc)
{
    (void)svc; (void)argv; (void)argc;
    addReplySimpleString(c, "PONG");
}

static void getCommand(Connection *c, struct service *svc,
                       RespObj *argv, int argc)
{
    (void)argc;
    if (argv[1].type != RESP_STR) {
        addReplyError(c, "wrong type for key");
        return;
    }

    sds key = sdsnewlen(argv[1].str, argv[1].len);
    if (!key) { addReplyError(c, "OOM"); return; }

    struct dict *db = svc->db[c->dbnum];
    ValObj *obj = dictfind(db, key);

    if (obj && obj->type == VAL_STRING) {
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
    if (argv[1].type != RESP_STR || argv[2].type != RESP_STR) {
        addReplyError(c, "wrong type for key or value");
        return;
    }

    sds key = sdsnewlen(argv[1].str, argv[1].len);
    sds val = sdsnewlen(argv[2].str, argv[2].len);
    if (!key || !val) {
        sdsfree(key); sdsfree(val);
        addReplyError(c, "OOM");
        return;
    }

    ValObj *obj = malloc(sizeof(ValObj));
    if (!obj) {
        sdsfree(key); sdsfree(val);
        addReplyError(c, "OOM");
        return;
    }
    obj->type   = VAL_STRING;
    obj->val.str = val;

    struct dict *db = svc->db[c->dbnum];

    /* 若 key 已存在，先记录旧值；dictReplace 覆写后释放旧值和新 key */
    ValObj *old = dictfind(db, key);
    dictReplace(db, key, obj);
    if (old) {
        valObjFree(old);   /* 释放被覆盖的旧 ValObj */
        sdsfree(key);      /* dict 保留了旧 key，新 key 未被插入，需释放 */
    }
    /* 新 key: 所有权已转移给 dict，不需释放 */

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
    sds key = sdsnewlen(argv[1].str, argv[1].len);
    if (!key) { addReplyError(c, "OOM"); return; }

    struct dict *db = svc->db[c->dbnum];
    addReplyInteger(c, dictfind(db, key) ? 1 : 0);
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
    sds key = sdsnewlen(argv[1].str, argv[1].len);
    if (!key) { addReplyError(c, "OOM"); return; }

    struct dict *db = svc->db[c->dbnum];
    int ret = dictDelete(db, key);
    addReplyInteger(c, ret == DICT_OK ? 1 : 0);
    sdsfree(key);
}

static void selectCommand(Connection *c, struct service *svc,
                          RespObj *argv, int argc)
{
    (void)argc;
    if (argv[1].type != RESP_STR) {
        addReplyError(c, "wrong type for index");
        return;
    }
    /* 将 bulk string 转成数字 */
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
 *  命令表
 * ================================================================ */

static Command cmd_table[] = {
    {"PING",    0,  pingCommand},
    {"GET",     1,  getCommand},
    {"SET",     2,  setCommand},
    {"EXISTS",  1,  existsCommand},
    {"DEL",     1,  delCommand},
    {"SELECT",  1,  selectCommand},
};

/* ================================================================
 *  命令分发
 * ================================================================ */

int processCommand(Connection *c, struct service *svc,
                   RespObj *argv, int argc)
{
    if (argc < 1) return SERVICE_ERR;
    if (argv[0].type != RESP_STR) return SERVICE_ERR;

    size_t ncmd = sizeof(cmd_table) / sizeof(cmd_table[0]);
    for (size_t i = 0; i < ncmd; i++) {
        if (!respStrEqCase(&argv[0], cmd_table[i].name))
            continue;

        /* 检查参数个数 */
        int expected = cmd_table[i].arity;
        if (expected >= 0 && argc - 1 != expected) {
            addReplyError(c, "wrong number of arguments");
            return SERVICE_OK;
        }

        cmd_table[i].handler(c, svc, argv, argc);
        return SERVICE_OK;
    }

    addReplyError(c, "unknown command");
    return SERVICE_OK;
}

/* ================================================================
 *  生命周期
 * ================================================================ */

int serviceInit(struct service *svc, unsigned int dbsize)
{
    if (!svc || dbsize == 0) return SERVICE_ERR;

    svc->dbsize = dbsize;

    svc->db = calloc(dbsize, sizeof(struct dict *));
    if (!svc->db) return SERVICE_ERR;

    for (unsigned int i = 0; i < dbsize; i++) {
        svc->db[i] = dictnew(DICT_HT_INITIAL_SIZE, &dictTypeSds);
        if (!svc->db[i]) {
            for (unsigned int j = 0; j < i; j++)
                dictfree(svc->db[j]);
            free(svc->db);
            svc->db = NULL;
            return SERVICE_ERR;
        }
    }
    return SERVICE_OK;
}

void serviceFree(struct service *svc)
{
    if (!svc || !svc->db) return;
    for (unsigned int i = 0; i < svc->dbsize; i++)
        dictfree(svc->db[i]);
    free(svc->db);
    svc->db = NULL;
}
