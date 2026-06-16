#ifndef _REDP_H
#define _REDP_H

#define RESP_OK 0  // 解析成功
#define RESP_ERR -1  // 协议错误
#define RESP_AGAIN -2 // 数据不完整，需要继续读，流式读取很有用。
#define MAX_LINE_LEN (16 * 1024)  // 单行最大 16KB
#define MAX_BULK_LEN (512 * 1024 * 1024)  // Bulk String 最大 512MB
#define MAX_PARSE_DEPTH 1024  // 数组嵌套递归最大深度
#include <stddef.h>

// *3\r\n$3\r\nSET\r\n$4\r\nname\r\n$6\r\nflashkv\r\n

enum RespType
{
    RESP_STR,      // +OK\r\n 和 $5\r\nhello\r\n 统一对待
    RESP_ERR_TYPE, // -ERR\r\n
    RESP_INT,      // :1\r\n
    RESP_ARRAY,    // *2\r\n...
    RESP_NIL,      // $-1\r\n
};

typedef struct RespObj
{
    enum RespType type;
    size_t len; // 所有类型都可能用到 len（字符串长度/数组个数/Error长度）
    union
    {
        void *str;                // RESP_STRING / RESP_ERROR / RESP_BULK 二进制安全
        long long integer;        // RESP_INT
        struct RespObj *elements; // RESP_ARRAY
    };
} RespObj;

int respParse(void *buf, size_t len, RespObj *out);
void respFreeObj(RespObj *o);

#endif