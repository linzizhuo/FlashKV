#include "resp.h"
#include <stdio.h>
#include <assert.h>
#include <string.h>
#include <stdlib.h>

/* ---------- 辅助函数 ---------- */

/* 十六进制打印，方便调试 */
/* 检查字符串 RespObj 是否正确 */
static void checkStr(const RespObj *o, const char *expected, size_t elen) {
    assert(o->type == RESP_STR);
    assert(o->len == elen);
    assert(memcmp(o->str, expected, elen) == 0);
}

/* ---------- Simple String ---------- */

static void test_simple_string(void) {
    printf("=== test_simple_string ===\n");

    const char *buf = "+OK\r\n";
    RespObj cmd;
    int ret = respParse((void *)buf, strlen(buf), &cmd);
    assert(ret == 5);                        // 返回消耗字节数
    assert(cmd.type == RESP_STR);
    assert(cmd.len == 2);
    assert(memcmp(cmd.str, "OK", 2) == 0);
    /* 零拷贝：指针指向 buf 内部 */
    assert(cmd.str == buf + 1);

    printf("   ✅\n");
}

static void test_simple_string_incomplete(void) {
    printf("=== test_simple_string_incomplete ===\n");

    const char *buf = "+OK\r";               // 缺少 \n
    RespObj cmd;
    int ret = respParse((void *)buf, strlen(buf), &cmd);
    assert(ret == RESP_AGAIN);

    printf("   ✅\n");
}

static void test_simple_string_too_long(void) {
    printf("=== test_simple_string_too_long ===\n");

    /* 构造超长行（超过 MAX_LINE_LEN = 16384） */
    int len = 17000;
    char *buf = malloc(len + 1);
    buf[0] = '+';
    memset(buf + 1, 'A', len - 3);
    buf[len - 2] = '\r';
    buf[len - 1] = '\n';

    RespObj cmd;
    int ret = respParse(buf, len, &cmd);
    assert(ret == RESP_ERR);                 // 超长 → 协议错误

    free(buf);
    printf("   ✅\n");
}

/* ---------- Error ---------- */

static void test_error(void) {
    printf("=== test_error ===\n");

    const char *buf = "-ERR unknown command\r\n";
    RespObj cmd;
    int ret = respParse((void *)buf, strlen(buf), &cmd);
    assert(ret > 0);
    assert(cmd.type == RESP_ERR_TYPE);
    assert(cmd.len == 19);                   // "ERR unknown command"
    assert(memcmp(cmd.str, "ERR unknown command", 19) == 0);

    printf("   ✅\n");
}

/* ---------- Integer ---------- */

static void test_integer(void) {
    printf("=== test_integer ===\n");

    RespObj cmd;
    int ret;

    /* 正数 */
    ret = respParse(":1\r\n", 4, &cmd);
    assert(ret == 4);
    assert(cmd.type == RESP_INT);
    assert(cmd.integer == 1);

    /* 零 */
    ret = respParse(":0\r\n", 4, &cmd);
    assert(cmd.integer == 0);

    /* 负数 */
    ret = respParse(":-100\r\n", 7, &cmd);
    assert(cmd.integer == -100);

    /* 大数 */
    ret = respParse(":9223372036854775807\r\n", 24, &cmd);
    assert(cmd.integer == 9223372036854775807LL);

    /* 不完整 */
    ret = respParse(":42", 3, &cmd);
    assert(ret == RESP_AGAIN);

    /* 不是数字 */
    ret = respParse(":abc\r\n", 6, &cmd);
    assert(ret == RESP_ERR);

    printf("   ✅\n");
}

/* ---------- Bulk String ---------- */

static void test_bulk_string(void) {
    printf("=== test_bulk_string ===\n");

    RespObj cmd;
    int ret;

    /* 正常 bulk string */
    const char *buf = "$5\r\nhello\r\n";
    ret = respParse((void *)buf, strlen(buf), &cmd);
    assert(ret == 11);
    assert(cmd.type == RESP_STR);
    assert(cmd.len == 5);
    assert(memcmp(cmd.str, "hello", 5) == 0);

    /* 空 bulk string */
    const char *empty = "$0\r\n\r\n";
    ret = respParse((void *)empty, strlen(empty), &cmd);
    assert(ret == 6);
    assert(cmd.type == RESP_STR);
    assert(cmd.len == 0);

    /* NULL bulk string */
    const char *null = "$-1\r\n";
    ret = respParse((void *)null, strlen(null), &cmd);
    assert(ret == 5);
    assert(cmd.type == RESP_NIL);
    assert(cmd.str == NULL);
    assert(cmd.len == 0);

    /* 不完整：缺少数据 */
    const char *partial = "$10\r\nhel";
    ret = respParse((void *)partial, strlen(partial), &cmd);
    assert(ret == RESP_AGAIN);

    /* 不完整：缺少结尾 \r\n */
    const char *partial2 = "$5\r\nhello";
    ret = respParse((void *)partial2, strlen(partial2), &cmd);
    assert(ret == RESP_AGAIN);

    /* 声明 3 字节但数据结束位置不对 */
    const char *bad_end = "$3\r\nabcd\r\n";    // 11 字节，但 buf[7]='d' ≠ '\r'
    ret = respParse((void *)bad_end, strlen(bad_end), &cmd);
    assert(ret == RESP_ERR);

    printf("   ✅\n");
}

static void test_bulk_binary_safe(void) {
    printf("=== test_bulk_binary_safe ===\n");

    /* 含 null 字节和二进制数据：$9 + \r\n + hello\0wor + \r\n */
    unsigned char buf[] = "$9\r\nhello\x00wor\r\n";
    RespObj cmd;
    int ret = respParse(buf, sizeof(buf) - 1, &cmd);
    assert(ret == (int)(sizeof(buf) - 1));
    assert(cmd.type == RESP_STR);
    assert(cmd.len == 9);
    assert(memcmp(cmd.str, "hello\x00wor", 9) == 0);
    printf("   ✅\n");
}

/* ---------- Array ---------- */

static void test_array(void) {
    printf("=== test_array ===\n");

    RespObj cmd;
    int ret;

    /* *2\r\n$3\r\nGET\r\n$4\r\nname\r\n */
    const char *arr = "*2\r\n$3\r\nGET\r\n$4\r\nname\r\n";
    ret = respParse((void *)arr, strlen(arr), &cmd);
    assert(ret == (int)strlen(arr));
    assert(cmd.type == RESP_ARRAY);
    assert(cmd.len == 2);
    assert(cmd.elements != NULL);

    checkStr(&cmd.elements[0], "GET", 3);
    checkStr(&cmd.elements[1], "name", 4);

    respFreeObj(&cmd);
    printf("   ✅\n");
}

static void test_null_array(void) {
    printf("=== test_null_array ===\n");

    const char *buf = "*-1\r\n";
    RespObj cmd;
    int ret = respParse((void *)buf, strlen(buf), &cmd);
    assert(ret == 5);
    assert(cmd.type == RESP_NIL);
    assert(cmd.elements == NULL);
    assert(cmd.len == 0);

    respFreeObj(&cmd);                       // 不会崩溃
    printf("   ✅\n");
}

static void test_empty_array(void) {
    printf("=== test_empty_array ===\n");

    const char *buf = "*0\r\n";
    RespObj cmd;
    int ret = respParse((void *)buf, strlen(buf), &cmd);
    assert(ret == 4);
    assert(cmd.type == RESP_ARRAY);
    assert(cmd.len == 0);
    assert(cmd.elements != NULL);           // 空数组也有 calloc 分配

    respFreeObj(&cmd);
    printf("   ✅\n");
}

static void test_array_mixed_types(void) {
    printf("=== test_array_mixed_types ===\n");

    /* *3\r\n+OK\r\n:42\r\n$5\r\nhello\r\n */
    const char *buf = "*3\r\n+OK\r\n:42\r\n$5\r\nhello\r\n";
    RespObj cmd;
    int ret = respParse((void *)buf, strlen(buf), &cmd);
    assert(ret == (int)strlen(buf));
    assert(cmd.type == RESP_ARRAY);
    assert(cmd.len == 3);

    /* 元素 0: simple string */
    assert(cmd.elements[0].type == RESP_STR);
    assert(cmd.elements[0].len == 2);
    assert(memcmp(cmd.elements[0].str, "OK", 2) == 0);

    /* 元素 1: integer */
    assert(cmd.elements[1].type == RESP_INT);
    assert(cmd.elements[1].integer == 42);

    /* 元素 2: bulk string */
    checkStr(&cmd.elements[2], "hello", 5);

    respFreeObj(&cmd);
    printf("   ✅\n");
}

static void test_nested_array(void) {
    printf("=== test_nested_array ===\n");

    /* *2\r\n*1\r\n$3\r\nGET\r\n$4\r\nname\r\n
       外层 2 元素: [ [GET], name ]  */
    const char *buf = "*2\r\n*1\r\n$3\r\nGET\r\n$4\r\nname\r\n";
    RespObj cmd;
    int ret = respParse((void *)buf, strlen(buf), &cmd);
    assert(ret == (int)strlen(buf));
    assert(cmd.type == RESP_ARRAY);
    assert(cmd.len == 2);

    /* 元素 0: 内嵌数组 */
    RespObj *inner = &cmd.elements[0];
    assert(inner->type == RESP_ARRAY);
    assert(inner->len == 1);
    assert(inner->elements != NULL);
    checkStr(&inner->elements[0], "GET", 3);

    /* 元素 1: bulk string */
    checkStr(&cmd.elements[1], "name", 4);

    respFreeObj(&cmd);
    printf("   ✅\n");
}

static void test_array_incomplete(void) {
    printf("=== test_array_incomplete ===\n");

    RespObj cmd;
    int ret;

    /* 数组长度已知但元素不完整 */
    const char *buf = "*2\r\n$3\r\nGET\r\n";  // 只有 1 个元素
    ret = respParse((void *)buf, strlen(buf), &cmd);
    assert(ret == RESP_AGAIN);

    /* *2\r\n$3\r\nG                    —— 数据切在 bulk string 中间 */
    const char *buf2 = "*2\r\n$3\r\nG";
    ret = respParse((void *)buf2, strlen(buf2), &cmd);
    assert(ret == RESP_AGAIN);

    printf("   ✅\n");
}

static void test_array_bad_number(void) {
    printf("=== test_array_bad_number ===\n");

    RespObj cmd;
    int ret;

    /* 数组长度不是数字 */
    ret = respParse("*abc\r\n", 6, &cmd);
    assert(ret == RESP_ERR);

    /* 负数但非 -1 */
    ret = respParse("*-2\r\n", 5, &cmd);
    assert(ret == RESP_ERR);

    printf("   ✅\n");
}

/* ---------- 入口边界 ---------- */

static void test_empty_buf(void) {
    printf("=== test_empty_buf ===\n");

    RespObj cmd;
    int ret = respParse("", 0, &cmd);
    assert(ret == RESP_AGAIN);              // 空缓冲区

    printf("   ✅\n");
}

static void test_unknown_first_byte(void) {
    printf("=== test_unknown_first_byte ===\n");

    RespObj cmd;
    int ret;

    ret = respParse("X\r\n", 3, &cmd);
    assert(ret == RESP_ERR);

    ret = respParse(" ", 1, &cmd);
    assert(ret == RESP_ERR);

    ret = respParse("\x00", 1, &cmd);
    assert(ret == RESP_ERR);

    printf("   ✅\n");
}

static void test_incomplete_headers(void) {
    printf("=== test_incomplete_headers ===\n");

    RespObj cmd;

    /* 只有类型字节，没有后续数据 */
    assert(respParse("+", 1, &cmd) == RESP_AGAIN);
    assert(respParse("-", 1, &cmd) == RESP_AGAIN);
    assert(respParse(":", 1, &cmd) == RESP_AGAIN);
    assert(respParse("$", 1, &cmd) == RESP_AGAIN);
    assert(respParse("*", 1, &cmd) == RESP_AGAIN);

    printf("   ✅\n");
}

/* ---------- 复杂真实场景 ---------- */

static void test_redis_hello_command(void) {
    printf("=== test_redis_hello_command ===\n");

    /* 模拟 Redis 数组命令: *3\r\n$5\r\nHELLO\r\n$5\r\nproto\r\n$1\r\n3\r\n */
    const char *buf = "*3\r\n$5\r\nHELLO\r\n$5\r\nproto\r\n$1\r\n3\r\n";
    RespObj cmd;
    int ret = respParse((void *)buf, strlen(buf), &cmd);
    assert(ret == (int)strlen(buf));
    assert(cmd.type == RESP_ARRAY);
    assert(cmd.len == 3);

    assert(cmd.elements[0].type == RESP_STR);
    assert(cmd.elements[0].len == 5);
    assert(memcmp(cmd.elements[0].str, "HELLO", 5) == 0);

    assert(cmd.elements[1].type == RESP_STR);

    assert(cmd.elements[2].type == RESP_STR);

    respFreeObj(&cmd);
    printf("   ✅\n");
}

/* ---------- 空指针安全 ---------- */

static void test_free_null_safety(void) {
    printf("=== test_free_null_safety ===\n");

    /* respFreeObj(NULL) 不崩溃 */
    respFreeObj(NULL);
    printf("   ✅\n");
}

/* ---------- 嵌套数组泄漏检测 ---------- */

static void test_nested_array_no_crash_on_deep(void) {
    printf("=== test_nested_array_no_crash_on_deep ===\n");

    /* 构建深度 20 的嵌套数组: *1\r\n*1\r\n...$3\r\nABC\r\n */
    int depth = 20;
    /* 计算缓冲区大小：
       每层 *1\r\n = 4 字节
       最内层 $3\r\nABC\r\n = 8 字节
       加上开头 null 终止 */
    size_t bufsz = (size_t)depth * 4 + 9 + 1;
    char *buf = malloc(bufsz);
    size_t pos = 0;
    for (int i = 0; i < depth; i++) {
        memcpy(buf + pos, "*1\r\n", 4);
        pos += 4;
    }
    memcpy(buf + pos, "$3\r\nABC\r\n", 9);
    pos += 9;
    buf[pos] = '\0';

    RespObj cmd;
    int ret = respParse(buf, pos, &cmd);
    assert(ret == (int)pos);
    assert(cmd.type == RESP_ARRAY);

    /* 解析后释放（当前实现仅释放外层，内层泄漏 — 见 review 发现 #1） */
    respFreeObj(&cmd);

    free(buf);
    printf("   ✅ 深度 %d 嵌套未崩溃\n", depth);
}

/* ---------- main ---------- */

int main(void) {
    printf("======== RESP 单元测试 ========\n\n");

    test_simple_string();
    test_simple_string_incomplete();
    test_simple_string_too_long();

    test_error();

    test_integer();

    test_bulk_string();
    test_bulk_binary_safe();

    test_array();
    test_null_array();
    test_empty_array();
    test_array_mixed_types();
    test_nested_array();
    test_array_incomplete();
    test_array_bad_number();

    test_empty_buf();
    test_unknown_first_byte();
    test_incomplete_headers();

    test_redis_hello_command();

    test_free_null_safety();

    test_nested_array_no_crash_on_deep();

    printf("\n======== 🎉 全部测试通过 ========\n");
    return 0;
}
