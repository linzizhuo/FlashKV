#include "resp.h"
#include <errno.h>
#include <limits.h>
#include <stdlib.h> // strtoll
#include <string.h> // memset
// 成功: 返回消耗的字节数（> 0）
// 协议错误: RESP_ERR (-1)
// 数据不完整: RESP_AGAIN (-2)
static int readLine(const void *buf, size_t len)
{
    const char *str = (const char *)buf;
    for (size_t idx = 0; idx + 1 < len; idx++)
    {
        if (str[idx] == '\r' && str[idx + 1] == '\n') // 换行符
            return idx;                               // 字符串的长度
        if (idx >= MAX_LINE_LEN)                      // 超过最大长度，return error
            return RESP_ERR;
    }
    return RESP_AGAIN;
}
// 0拷贝解析。
static int parseSimpleString(void *buf, size_t len, RespObj *out) // '+'
{
    int val = readLine(buf + 1, len - 1);
    if (val < 0)
        return val; // 错误
    out->str = (char *)buf + 1;
    out->type = RESP_STR;   // 设置字符串类型
    out->len = (size_t)val; // 字符串的长度
    return val + 3;         // 字符串长度 + 协议固定的长度
}

static int parseError(void *buf, size_t len, RespObj *out)
{
    int val = readLine(buf + 1, len - 1);
    if (val < 0)
        return val;
    out->type = RESP_ERR_TYPE;
    out->str = (char *)buf + 1;
    out->len = (size_t)val;
    return val + 3;
}

// buf 指向数字部分，返回数字和消耗的字节数
static int readNumber(const void *buf, size_t len, long long *val)
{
    int ret = readLine(buf, len);
    if (ret < 0)
        return ret;
    char *end;
    errno = 0;
    *val = strtoll((const char *)buf, &end, 10);
    if (errno == ERANGE || *end != '\r')
        return RESP_ERR;
    return ret; // 返回行长度（含 \r\n）
}

/*
    要扫描两次字符串，但实现简单。
*/
static int parseInteger(void *buf, size_t len, RespObj *out)
{
    long long val;
    int ret = readNumber(buf + 1, len - 1, &val);
    if (ret < 0)
        return ret;
    out->type = RESP_INT;
    out->integer = val;
    return ret + 3; // ':' + 数字行
}

/* ---------- Bulk String ---------- */
/*
    $5\r\nhello\r\n   → RESP_STR, len=5
    $-1\r\n           → RESP_NIL
*/
static int parseBulkString(void *buf, size_t len, RespObj *out)
{
    long long blen;
    int ret = readNumber(buf + 1, len - 1, &blen);
    if (ret < 0) return ret;

    if (blen == -1) {                     // null
        out->type = RESP_NIL;
        out->str = NULL;
        out->len = 0;
        return 1 + ret + 2;               // $ + "-1" + \r\n
    }
    if (blen < 0 || blen > MAX_BULK_LEN)
        return RESP_ERR;                       // 负数或超大长度，非法

    if (ret + blen + 5 > INT_MAX) return RESP_ERR;  // 防止 int 溢出（$ + 数字行 + 数据 + \r\n）
    size_t consumed = (size_t)(1 + ret + 2);          // $ + 数字行 + \r\n
    if (consumed + (size_t)blen + 2 > len)
        return RESP_AGAIN;                            // 数据还没到齐
    if (((char *)buf)[consumed + blen] != '\r' ||
        ((char *)buf)[consumed + blen + 1] != '\n')
        return RESP_ERR;                              // 结尾不是 \r\n

    out->type = RESP_STR;
    out->str = (void *)((char *)buf + consumed);
    out->len = (size_t)blen;

    return (int)(consumed + blen + 2);                // ... + 数据 + \r\n
}

/* ---------- Array ---------- */
/*
    *2\r\n$3\r\nGET\r\n$3\r\nkey\r\n   → RESP_ARRAY, len=2
    *-1\r\n                              → RESP_NIL
*/
static int respParseDepth(void *buf, size_t len, RespObj *out, int depth);

static int parseArray(void *buf, size_t len, RespObj *out, int depth)
{
    long long n;
    int ret = readNumber(buf + 1, len - 1, &n);
    if (ret < 0) return ret;

    if (n == -1) {
        out->type = RESP_NIL;
        out->len = 0;
        out->elements = NULL;
        return 1 + ret + 2;               // * + "-1" + \r\n
    }
    if (n < 0) return RESP_ERR;

    out->type = RESP_ARRAY;
    out->len = (size_t)n;
    out->elements = malloc((size_t)n * sizeof(RespObj));
    if (!out->elements) return RESP_ERR;

    size_t consumed = (size_t)(1 + ret + 2);   // * + 数字行 + \r\n
    for (long long i = 0; i < n; i++) {
        int r = respParseDepth((char *)buf + consumed, len - consumed, &out->elements[i], depth);
        if (r < 0) {
            for (long long j = 0; j < i; j++)  // 递归释放已解析的元素
                respFreeObj(&out->elements[j]);
            free(out->elements);
            out->elements = NULL;
            return r;
        }
        consumed += (size_t)r;
        if (consumed > INT_MAX) {               // 防止返回值溢出
            for (long long j = 0; j <= i; j++)
                respFreeObj(&out->elements[j]);
            free(out->elements);
            out->elements = NULL;
            return RESP_ERR;
        }
    }
    return (int)consumed;
}

/* ---------- 释放 ---------- */
/*
 * 递归释放 RespObj 中动态分配的资源。
 *
 * 职责（且仅此职责）：
 *   释放 RESP_ARRAY 类型中由 malloc 分配的 elements 数组。
 *   若元素本身也是 RESP_ARRAY，则递归释放其子元素。
 *
 * 不适用的场景（调用方自行管理）：
 *   - RESP_STR / RESP_ERR_TYPE 的 str 指针指向调用方的缓冲区（零拷贝），不由本函数管理。
 *   - RESP_INT / RESP_NIL 没有动态资源。
 *   - RespObj 结构体本身（如在栈上声明）不由本函数释放。
 */
void respFreeObj(RespObj *o)
{
    if (!o) return;
    if (o->type == RESP_ARRAY && o->elements) {
        for (size_t i = 0; i < o->len; i++)
            respFreeObj(&o->elements[i]);
        free(o->elements);
        o->elements = NULL;
    }
}

/* ---------- 内部递归入口（带深度限制） ---------- */
static int respParseDepth(void *buf, size_t len, RespObj *out, int depth)
{
    if (depth > MAX_PARSE_DEPTH) return RESP_ERR;
    if (len < 1) return RESP_AGAIN;

    char *str = buf;
    switch (str[0])
    {
    case '+': return parseSimpleString(buf, len, out);
    case '-': return parseError(buf, len, out);
    case ':': return parseInteger(buf, len, out);
    case '$': return parseBulkString(buf, len, out);
    case '*': return parseArray(buf, len, out, depth + 1);
    default:  return RESP_ERR;
    }
}

/* ---------- 入口 ---------- */
/*
 * 从 buf 中尝试解析一个完整的 RESP 对象。
 *
 * 返回值：成功时返回消耗的字节数（> 0），
 *         RESP_ERR (-1) 协议错误，
 *         RESP_AGAIN (-2) 数据不完整。
 *
 * 成功时 out 的各字段被填充。
 * 失败时 out 被零初始化（可安全传给 respFreeObj）。
 */
int respParse(void *buf, size_t len, RespObj *out)
{
    memset(out, 0, sizeof(*out));
    return respParseDepth(buf, len, out, 0);
}