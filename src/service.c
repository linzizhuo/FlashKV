#include "service.h"
#include "server.h"
#include "dict_type.h"
#include "sds.h"
#include "val_obj.h"
#include "ttl.h"

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

/* 从 RespObj 创建 sds — 几乎所有命令都要走这一步 */
static inline sds respKeyToSds(const RespObj *o)
{
    return sdsnewlen(o->str, o->len);
}

/* 字符串 → long long，ok 表示是否完全解析成功 */
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
 *  惰性删除
 *
 *  每个可能访问 key 的命令在执行前调一次。
 *  h 是预计算的 hash 指针，此函数不修改 *h。
 *  返回 true 表示 key 已过期并被删除，调用方应返回 nil / 0 / -2。
 * ================================================================ */

static bool expireIfNeeded(struct dict *db, struct dict *expires,
                           const void *key, hash_t *h)
{
    tstamp_t *when = keyTtlFind(expires, key, *h);
    if (!when) return false;                       /* 没设 TTL */
    if ((time_t)(*when) >= time(NULL)) return false;  /* 未过期 */

    dictDelete(expires, key, h);  /* 先删 expires（释放其 sdsdup 的 key） */
    dictDelete(db, key, h);       /* 再删主 dict（释放主 key + ValObj） */
    return true;
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

    struct dict *db      = svc->db[c->dbnum];
    struct dict *expires = svc->expires[c->dbnum];
    hash_t h = db->type->hash(key);

    if (expireIfNeeded(db, expires, key, &h)) {
        addReplyNull(c);
        goto done;
    }

    ValObj *obj = dictfind(db, key, &h);
    if (!obj) {
        addReplyNull(c);
    } else if (obj->type == VAL_INT) {
        addReplyInteger(c, obj->val.ll);
    } else if (obj->type == VAL_STRING) {
        addReplyBulkSds(c, obj->val.str);
    } else {
        addReplyNull(c);
    }
done:
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

    /* 构造 ValObj */
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

    struct dict *db      = svc->db[c->dbnum];
    struct dict *expires = svc->expires[c->dbnum];
    hash_t h = db->type->hash(key);

    ValObj *old = dictfind(db, key, &h);
    dictReplace(db, key, obj, &h);
    dictDelete(expires, key, &h);   /* SET 覆盖 → 清除旧 TTL */

    if (old) {
        valObjFree(old);   /* 释放被覆盖的旧 ValObj */
        sdsfree(key);      /* 旧 key 仍在 dict 里，新 key 未被插入 */
    }
    /* 新 key: 所有权已在 dictReplace 中转移给 dict */
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

    struct dict *db      = svc->db[c->dbnum];
    struct dict *expires = svc->expires[c->dbnum];
    hash_t h = db->type->hash(key);

    expireIfNeeded(db, expires, key, &h);
    addReplyInteger(c, dictfind(db, key, &h) ? 1 : 0);
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

    struct dict *db      = svc->db[c->dbnum];
    struct dict *expires = svc->expires[c->dbnum];
    hash_t h = db->type->hash(key);

    int ret = dictDelete(db, key, &h);
    dictDelete(expires, key, &h);   /* 顺手清 TTL */
    addReplyInteger(c, ret == DICT_OK ? 1 : 0);
    sdsfree(key);
}

/* ================================================================
 *  命令：EXPIRE / PEXPIRE / EXPIREAT / PEXPIREAT
 *
 *  四个命令共享同一实现，mode 决定时间语义：
 *    EXPIRE_SEC    秒级相对
 *    PEXPIRE_MS    毫秒相对
 *    EXPIREAT_SEC  秒级绝对
 *    PEXPIREAT_MS  毫秒绝对
 *  内部统一转换为绝对秒（time_t）存入 expires dict。
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

    struct dict *db      = svc->db[c->dbnum];
    struct dict *expires = svc->expires[c->dbnum];
    hash_t h = db->type->hash(key);

    if (!dictfind(db, key, &h)) {
        addReplyInteger(c, 0);   /* key 不存在 */
        sdsfree(key);
        return;
    }

    /* 统一转绝对秒 */
    time_t when;
    switch (mode) {
    case EXPIRE_SEC:    when = time(NULL) + (time_t)val;       break;
    case PEXPIRE_MS:    when = time(NULL) + (time_t)(val/1000); break;
    case EXPIREAT_SEC:  when = (time_t)val;                     break;
    case PEXPIREAT_MS:  when = (time_t)(val/1000);              break;
    default:            when = 0; break;
    }

    /* 原地更新 or 新插入（需 sdsdup 独立 key） */
    tstamp_t *old = keyTtlFind(expires, key, h);
    if (old) {
        *old = (tstamp_t)when;
        sdsfree(key);
    } else {
        sds expkey = sdsdup(key);
        if (!expkey) { addReplyError(c, "OOM"); sdsfree(key); return; }
        dictAdd(expires, expkey, (void *)when, &h);
        sdsfree(key);
    }
    addReplyInteger(c, 1);
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
 *  共享实现，millisec=0 返回秒，=1 返回毫秒。
 *  -2: key 不存在（或已过期并被惰性删除）
 *  -1: key 存在但无 TTL
 *  ≥0: 剩余存活时间
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

    struct dict *db      = svc->db[c->dbnum];
    struct dict *expires = svc->expires[c->dbnum];
    hash_t h = db->type->hash(key);

    expireIfNeeded(db, expires, key, &h);
    if (!dictfind(db, key, &h)) {
        addReplyInteger(c, -2);
        goto done;
    }

    tstamp_t *when = keyTtlFind(expires, key, h);
    if (!when) {
        addReplyInteger(c, -1);
    } else {
        time_t remain = (time_t)(*when) - time(NULL);
        if (remain < 0) remain = 0;
        addReplyInteger(c, millisec ? (long long)remain * 1000
                                    : (long long)remain);
    }
done:
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

    struct dict *db      = svc->db[c->dbnum];
    struct dict *expires = svc->expires[c->dbnum];
    hash_t h = db->type->hash(key);

    if (!dictfind(db, key, &h) || !keyTtlFind(expires, key, h)) {
        addReplyInteger(c, 0);
        goto done;
    }
    dictDelete(expires, key, &h);
    addReplyInteger(c, 1);
done:
    sdsfree(key);
}

/* ================================================================
 *  命令表
 *
 *  按字典序排列，供 bsearch 二分查找。
 *  arity = 参数个数（不含命令名），-1 表示变长。
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
 *
 *  server 层每读到一条完整命令就调一次。
 *  返回值 SERVICE_ERR → server 关闭连接。
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
    svc->db      = calloc(dbsize, sizeof(struct dict *));
    svc->expires = calloc(dbsize, sizeof(struct dict *));
    if (!svc->db || !svc->expires) {
        free(svc->db); free(svc->expires);
        svc->db = svc->expires = NULL;
        return SERVICE_ERR;
    }

    for (unsigned int i = 0; i < dbsize; i++) {
        svc->db[i]      = dictnew(DICT_HT_INITIAL_SIZE, &dictTypeSds);
        svc->expires[i] = dictnew(DICT_HT_INITIAL_SIZE, &dictTTL);
        if (!svc->db[i] || !svc->expires[i]) {
            for (unsigned int j = 0; j < i; j++) {
                dictfree(svc->db[j]);
                dictfree(svc->expires[j]);
            }
            dictfree(svc->db[i]);
            dictfree(svc->expires[i]);
            free(svc->db); free(svc->expires);
            svc->db = svc->expires = NULL;
            return SERVICE_ERR;
        }
    }
    return SERVICE_OK;
}

void serviceFree(struct service *svc)
{
    if (!svc || !svc->db) return;
    for (unsigned int i = 0; i < svc->dbsize; i++) {
        dictfree(svc->db[i]);
        dictfree(svc->expires[i]);
    }
    free(svc->db);
    free(svc->expires);
    svc->db = svc->expires = NULL;
}
