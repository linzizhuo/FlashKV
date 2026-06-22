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
 *  RESP 响应写入（追加模式，支持 pipeline）
 *
 *  所有函数将 RESP 协议数据追加到 c->wbuf 末尾，
 *  空间不足时自动扩容。
 *  handleRead 循环处理多条命令，响应逐条追加，
 *  循环结束后一次性写回客户端。
 * ================================================================ */

/* 确保 wbuf 有 space 字节剩余空间，不足则 2× 扩容 */
static int replyEnsure(Connection *c, size_t space)
{
    size_t needed = c->wlen + space;
    if (needed <= c->wcap) return 1;

    size_t newcap = c->wcap * 2;
    if (newcap < needed) newcap = needed;
    if (newcap < 256)     newcap = 256;

    char *p = realloc(c->wbuf, newcap);
    if (!p) {
        c->wlen = 0;
        return 0;
    }
    c->wbuf = p;
    c->wcap = newcap;
    return 1;
}

void addReplySimpleString(Connection *c, const char *str)
{
    size_t slen = strlen(str);
    /* "+" + str + "\r\n" */
    size_t total = 1 + slen + 2;
    if (!replyEnsure(c, total)) return;
    char *p = c->wbuf + c->wlen;
    *p = '+';
    memcpy(p + 1, str, slen);
    p[1 + slen]     = '\r';
    p[1 + slen + 1] = '\n';
    c->wlen += total;
}

void addReplyError(Connection *c, const char *msg)
{
    size_t mlen = strlen(msg);
    /* "-ERR " + msg + "\r\n" */
    size_t total = 5 + mlen + 2;
    if (!replyEnsure(c, total)) return;
    char *p = c->wbuf + c->wlen;
    memcpy(p, "-ERR ", 5);
    memcpy(p + 5, msg, mlen);
    p[5 + mlen]     = '\r';
    p[5 + mlen + 1] = '\n';
    c->wlen += total;
}

void addReplyInteger(Connection *c, long long val)
{
    /* snprintf to stack, then append */
    char tmp[32];
    int n = snprintf(tmp, sizeof(tmp), ":%lld\r\n", val);
    if (n <= 0) return;
    if (!replyEnsure(c, (size_t)n)) return;
    memcpy(c->wbuf + c->wlen, tmp, (size_t)n);
    c->wlen += (size_t)n;
}

void addReplyBulkString(Connection *c, const char *str, size_t len)
{
    /* "$" + len + "\r\n" + str + "\r\n" */
    char hdr[32];
    int hlen = snprintf(hdr, sizeof(hdr), "$%zu\r\n", len);
    if (hlen <= 0) return;

    size_t total = (size_t)hlen + len + 2;
    if (!replyEnsure(c, total)) return;

    char *p = c->wbuf + c->wlen;
    memcpy(p, hdr, (size_t)hlen);
    memcpy(p + hlen, str, len);
    p[hlen + len]     = '\r';
    p[hlen + len + 1] = '\n';
    c->wlen += total;
}

void addReplyBulkSds(Connection *c, void *s)
{
    sds str = (sds)s;
    addReplyBulkString(c, str, sdslen(str));
}

void addReplyNull(Connection *c)
{
    if (!replyEnsure(c, 5)) return;
    memcpy(c->wbuf + c->wlen, "$-1\r\n", 5);
    c->wlen += 5;
}

void addReplyOK(Connection *c)
{
    if (!replyEnsure(c, 5)) return;
    memcpy(c->wbuf + c->wlen, "+OK\r\n", 5);
    c->wlen += 5;
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
 *  ZSET 辅助
 * ================================================================ */

/* RESP array 前缀 */
static void addReplyArray(Connection *c, unsigned long count)
{
    char tmp[32];
    int n = snprintf(tmp, sizeof(tmp), "*%lu\r\n", count);
    if (n <= 0) return;
    if (!replyEnsure(c, (size_t)n)) return;
    memcpy(c->wbuf + c->wlen, tmp, (size_t)n);
    c->wlen += (size_t)n;
}

/* ================================================================
 *  命令：ZADD key score member
 * ================================================================ */

static void zaddCommand(Connection *c, struct service *svc,
                        RespObj *argv, int argc)
{
    (void)argc;
    if (argv[1].type != RESP_STR || argv[2].type != RESP_STR || argv[3].type != RESP_STR) {
        addReplyError(c, "wrong type for key/score/member");
        return;
    }
    sds key = respKeyToSds(&argv[1]);
    if (!key) { addReplyError(c, "OOM"); return; }

    double score;
    {
        char tmp[64];
        size_t n = argv[2].len > 63 ? 63 : argv[2].len;
        memcpy(tmp, argv[2].str, n); tmp[n] = '\0';
        char *end;
        score = strtod(tmp, &end);
        if (*end != '\0') {
            addReplyError(c, "invalid float score");
            sdsfree(key);
            return;
        }
    }

    sds member = sdsnewlen(argv[3].str, argv[3].len);
    if (!member) { sdsfree(key); addReplyError(c, "OOM"); return; }

    zset *zs = kvdbGetOrCreateZset(svc->kvs[c->dbnum], key);
    if (!zs) {
        addReplyError(c, "WRONGTYPE key holds wrong kind of value or OOM");
        sdsfree(key); sdsfree(member);
        return;
    }

    /* zsetAdd 统一处理新增/更新/重复检测（O(1) dict + O(log N) skiplist） */
    int added = zsetAdd(zs, score, member);
    /* member 所有权已移交 zset，OOM 时内部释放 member，不要重复 sdsfree */

    sdsfree(key);
    addReplyInteger(c, added);
}

/* ================================================================
 *  命令：ZCARD key
 * ================================================================ */

static void zcardCommand(Connection *c, struct service *svc,
                         RespObj *argv, int argc)
{
    (void)argc;
    if (argv[1].type != RESP_STR) {
        addReplyError(c, "wrong type for key");
        return;
    }
    sds key = respKeyToSds(&argv[1]);
    if (!key) { addReplyError(c, "OOM"); return; }

    int found;
    zset *zs = kvdbGetZset(svc->kvs[c->dbnum], key, &found);
    if (found < 0)
        addReplyError(c, "WRONGTYPE key holds wrong kind of value");
    else if (found == 0)
        addReplyInteger(c, 0);
    else
        addReplyInteger(c, (long long)zsetLen(zs));
    sdsfree(key);
}

/* ================================================================
 *  命令：ZRANK key member
 * ================================================================ */

static void zrankCommand(Connection *c, struct service *svc,
                         RespObj *argv, int argc)
{
    (void)argc;
    if (argv[1].type != RESP_STR || argv[2].type != RESP_STR) {
        addReplyError(c, "wrong type for key/member");
        return;
    }
    sds key = respKeyToSds(&argv[1]);
    if (!key) { addReplyError(c, "OOM"); return; }
    sds member = sdsnewlen(argv[2].str, argv[2].len);
    if (!member) { sdsfree(key); addReplyError(c, "OOM"); return; }

    int found;
    zset *zs = kvdbGetZset(svc->kvs[c->dbnum], key, &found);
    if (found < 0) {
        addReplyError(c, "WRONGTYPE key holds wrong kind of value");
    } else if (found == 0) {
        addReplyNull(c);
    } else {
        zskiplistNode *n = zsetFind(zs, member);       /* O(1) dict */
        if (!n) {
            addReplyNull(c);
        } else {
            unsigned long r = zsetRank(zs, member);    /* O(log N) */
            addReplyInteger(c, (long long)(r - 1));    /* 0-based */
        }
    }
    sdsfree(key);
    sdsfree(member);
}

/* ================================================================
 *  命令：ZSCORE key member
 * ================================================================ */

static void zscoreCommand(Connection *c, struct service *svc,
                          RespObj *argv, int argc)
{
    (void)argc;
    if (argv[1].type != RESP_STR || argv[2].type != RESP_STR) {
        addReplyError(c, "wrong type for key/member");
        return;
    }
    sds key = respKeyToSds(&argv[1]);
    if (!key) { addReplyError(c, "OOM"); return; }
    sds member = sdsnewlen(argv[2].str, argv[2].len);
    if (!member) { sdsfree(key); addReplyError(c, "OOM"); return; }

    int found;
    zset *zs = kvdbGetZset(svc->kvs[c->dbnum], key, &found);
    if (found < 0) {
        addReplyError(c, "WRONGTYPE key holds wrong kind of value");
    } else if (found == 0) {
        addReplyNull(c);
    } else {
        zskiplistNode *n = zsetFind(zs, member);       /* O(1) dict */
        if (!n) {
            addReplyNull(c);
        } else {
            char tmp[64];
            int len = snprintf(tmp, sizeof(tmp), "%.17g", n->score);
            addReplyBulkString(c, tmp, (size_t)len);
        }
    }
    sdsfree(key);
    sdsfree(member);
}

/* ================================================================
 *  命令：ZRANGE key start stop [WITHSCORES]
 *        ZRANGE key min max BYSCORE [WITHSCORES]
 * ================================================================ */

static void zrangeCommand(Connection *c, struct service *svc,
                          RespObj *argv, int argc)
{
    if (argv[1].type != RESP_STR || argv[2].type != RESP_STR ||
        argv[3].type != RESP_STR) {
        addReplyError(c, "wrong type for key/start/stop");
        return;
    }
    sds key = respKeyToSds(&argv[1]);
    if (!key) { addReplyError(c, "OOM"); return; }

    /* 检测 BYSCORE 模式 */
    int byscore = 0;
    int withscores = 0;
    int argp = 4;

    if (argc > 4 && argv[4].type == RESP_STR &&
        strncasecmp((const char *)argv[4].str, "BYSCORE", argv[4].len) == 0 &&
        argv[4].len == 7) {
        byscore = 1;
        argp++;
    }

    if (argc > argp) {
        if (argv[argp].type != RESP_STR ||
            strncasecmp((const char *)argv[argp].str, "WITHSCORES", argv[argp].len) != 0 ||
            argv[argp].len != 10) {
            addReplyError(c, "syntax error, expected WITHSCORES");
            sdsfree(key);
            return;
        }
        withscores = 1;
    }

    int found;
    zset *zs = kvdbGetZset(svc->kvs[c->dbnum], key, &found);
    if (found < 0) {
        addReplyError(c, "WRONGTYPE key holds wrong kind of value");
        sdsfree(key);
        return;
    }
    if (found == 0) { addReplyArray(c, 0); sdsfree(key); return; }

    if (byscore) {
        /* ---- BYSCORE 模式 ---- */
        double min, max;
        {
            char tmp[64]; size_t n; char *end;
            n = argv[2].len > 63 ? 63 : argv[2].len;
            memcpy(tmp, argv[2].str, n); tmp[n] = '\0';
            min = strtod(tmp, &end);
            if (*end != '\0') { addReplyError(c, "invalid min"); sdsfree(key); return; }
            n = argv[3].len > 63 ? 63 : argv[3].len;
            memcpy(tmp, argv[3].str, n); tmp[n] = '\0';
            max = strtod(tmp, &end);
            if (*end != '\0') { addReplyError(c, "invalid max"); sdsfree(key); return; }
        }

        unsigned long count;
        zskiplistNode **nodes = zsetRange(zs, min, max, &count);
        addReplyArray(c, withscores ? count * 2 : count);
        for (unsigned long i = 0; i < count; i++) {
            addReplyBulkSds(c, nodes[i]->ele);
            if (withscores) {
                char tmp[64];
                int n = snprintf(tmp, sizeof(tmp), "%.17g", nodes[i]->score);
                addReplyBulkString(c, tmp, (size_t)n);
            }
        }
        free(nodes);
        sdsfree(key);
        return;
    }

    /* ---- 排名模式（原有逻辑） ---- */
    int ok1, ok2;
    long long start = parseLongLong(&argv[2], &ok1);
    long long stop  = parseLongLong(&argv[3], &ok2);
    if (!ok1 || !ok2) {
        addReplyError(c, "invalid start/stop");
        sdsfree(key);
        return;
    }

    unsigned long len = zsetLen(zs);
    if (len == 0) { addReplyArray(c, 0); sdsfree(key); return; }

    /* 负索引转换 */
    if (start < 0) start = (long long)len + start;
    if (stop  < 0) stop  = (long long)len + stop;
    if (start < 0) start = 0;
    if (stop  < 0 || start > (long long)len - 1 || start > stop) {
        addReplyArray(c, 0); sdsfree(key); return;
    }
    if (stop > (long long)len - 1) stop = (long long)len - 1;

    unsigned long count = (unsigned long)(stop - start + 1);
    addReplyArray(c, withscores ? count * 2 : count);

    zskiplistNode *x = zsetByRank(zs, (unsigned long)(start + 1));
    for (unsigned long i = 0; i < count && x; i++) {
        addReplyBulkSds(c, x->ele);
        if (withscores) {
            char tmp[64];
            int n = snprintf(tmp, sizeof(tmp), "%.17g", x->score);
            addReplyBulkString(c, tmp, (size_t)n);
        }
        x = x->level[0].forward;
    }
    sdsfree(key);
}

/* ================================================================
 *  命令：ZREM key member
 * ================================================================ */

static void zremCommand(Connection *c, struct service *svc,
                        RespObj *argv, int argc)
{
    (void)argc;
    if (argv[1].type != RESP_STR || argv[2].type != RESP_STR) {
        addReplyError(c, "wrong type for key/member");
        return;
    }
    sds key = respKeyToSds(&argv[1]);
    if (!key) { addReplyError(c, "OOM"); return; }
    sds member = sdsnewlen(argv[2].str, argv[2].len);
    if (!member) { sdsfree(key); addReplyError(c, "OOM"); return; }

    int found;
    zset *zs = kvdbGetZset(svc->kvs[c->dbnum], key, &found);
    if (found < 0) {
        addReplyError(c, "WRONGTYPE key holds wrong kind of value");
    } else if (found == 0) {
        addReplyInteger(c, 0);
    } else {
        int deleted = zsetDel(zs, member);             /* O(1) dict + O(log N) skiplist */
        addReplyInteger(c, deleted);
    }
    sdsfree(key);
    sdsfree(member);
}

/* ================================================================
 *  命令：ZCOUNT key min max
 * ================================================================ */

static void zcountCommand(Connection *c, struct service *svc,
                          RespObj *argv, int argc)
{
    (void)argc;
    if (argv[1].type != RESP_STR || argv[2].type != RESP_STR ||
        argv[3].type != RESP_STR) {
        addReplyError(c, "wrong type for key/min/max");
        return;
    }
    sds key = respKeyToSds(&argv[1]);
    if (!key) { addReplyError(c, "OOM"); return; }

    double min, max;
    {
        char tmp[64]; size_t n; char *end;
        n = argv[2].len > 63 ? 63 : argv[2].len;
        memcpy(tmp, argv[2].str, n); tmp[n] = '\0';
        min = strtod(tmp, &end);
        if (*end != '\0') { addReplyError(c, "invalid min"); sdsfree(key); return; }
        n = argv[3].len > 63 ? 63 : argv[3].len;
        memcpy(tmp, argv[3].str, n); tmp[n] = '\0';
        max = strtod(tmp, &end);
        if (*end != '\0') { addReplyError(c, "invalid max"); sdsfree(key); return; }
    }

    int found;
    zset *zs = kvdbGetZset(svc->kvs[c->dbnum], key, &found);
    if (found < 0)
        addReplyError(c, "WRONGTYPE key holds wrong kind of value");
    else if (found == 0)
        addReplyInteger(c, 0);
    else
        addReplyInteger(c, (long long)zsetCount(zs, min, max));
    sdsfree(key);
}

/* ================================================================
 *  命令：ZREMRANGEBYSCORE key min max
 * ================================================================ */

static void zremrangebyscoreCommand(Connection *c, struct service *svc,
                                    RespObj *argv, int argc)
{
    (void)argc;
    if (argv[1].type != RESP_STR || argv[2].type != RESP_STR ||
        argv[3].type != RESP_STR) {
        addReplyError(c, "wrong type for key/min/max");
        return;
    }
    sds key = respKeyToSds(&argv[1]);
    if (!key) { addReplyError(c, "OOM"); return; }

    double min, max;
    {
        char tmp[64]; size_t n; char *end;
        n = argv[2].len > 63 ? 63 : argv[2].len;
        memcpy(tmp, argv[2].str, n); tmp[n] = '\0';
        min = strtod(tmp, &end);
        if (*end != '\0') { addReplyError(c, "invalid min"); sdsfree(key); return; }
        n = argv[3].len > 63 ? 63 : argv[3].len;
        memcpy(tmp, argv[3].str, n); tmp[n] = '\0';
        max = strtod(tmp, &end);
        if (*end != '\0') { addReplyError(c, "invalid max"); sdsfree(key); return; }
    }

    int found;
    zset *zs = kvdbGetZset(svc->kvs[c->dbnum], key, &found);
    if (found < 0)
        addReplyError(c, "WRONGTYPE key holds wrong kind of value");
    else if (found == 0)
        addReplyInteger(c, 0);
    else
        addReplyInteger(c, (long long)zsetDelRange(zs, min, max));
    sdsfree(key);
}

/* ================================================================
 *  命令表
 *
 *  按字典序排列，供 bsearch 二分查找。
 * ================================================================ */

static Command cmd_table[] = {
    {"DEL",             1,  delCommand},
    {"EXISTS",          1,  existsCommand},
    {"EXPIRE",          2,  expireCommand},
    {"EXPIREAT",        2,  expireatCommand},
    {"GET",             1,  getCommand},
    {"PERSIST",         1,  persistCommand},
    {"PEXPIRE",         2,  pexpireCommand},
    {"PEXPIREAT",       2,  pexpireatCommand},
    {"PING",            0,  pingCommand},
    {"PTTL",            1,  pttlCommand},
    {"SELECT",          1,  selectCommand},
    {"SET",             2,  setCommand},
    {"TTL",             1,  ttlCommand},
    {"ZADD",            3,  zaddCommand},
    {"ZCARD",           1,  zcardCommand},
    {"ZCOUNT",          3,  zcountCommand},
    {"ZRANGE",         -1,  zrangeCommand},
    {"ZRANK",           2,  zrankCommand},
    {"ZREM",            2,  zremCommand},
    {"ZREMRANGEBYSCORE",3,  zremrangebyscoreCommand},
    {"ZSCORE",          2,  zscoreCommand},
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
