# RESP 协议解析模块

## 概述

FlashKV 的 RESP（Redis Serialization Protocol）实现，负责将 TCP 字节流解析为结构化命令，以及将执行结果格式化为 RESP 协议写回。

## 常量

```c
#define RESP_OK          0   // 解析成功
#define RESP_ERR        -1   // 协议错误
#define RESP_AGAIN      -2   // 数据不完整，需继续读取
#define MAX_LINE_LEN     (16 * 1024)            // 单行最大 16KB
#define MAX_BULK_LEN     (512 * 1024 * 1024)    // Bulk String 最大 512MB
#define MAX_PARSE_DEPTH  1024                   // 数组嵌套最大层数
```

## enum RespType

| 枚举值 | 对应协议 | 说明 |
|--------|---------|------|
| `RESP_STR` | `+` / `$` | Simple String 或 Bulk String |
| `RESP_ERR_TYPE` | `-` | Error |
| `RESP_INT` | `:` | Integer |
| `RESP_ARRAY` | `*` | Array |
| `RESP_NIL` | `$-1` / `*-1` | Null |

## struct RespObj

RESP 对象容器。通过 `type` 区分具体类型，使用 `union` 共享存储。

```c
typedef struct RespObj {
    enum RespType type;       // 类型标签
    size_t len;               // 字符串长度 / 数组元素个数
    union {
        void *str;            // RESP_STR / RESP_ERR_TYPE 的数据指针
        long long integer;    // RESP_INT 的整数值
        struct RespObj *elements; // RESP_ARRAY 的子元素数组
    };
} RespObj;
```

### 使用示例

```c
// 解析数组
RespObj cmd;
int ret = respParse(buf, len, &cmd);
if (ret == RESP_ERR) { /* 协议错 */ }
if (ret == RESP_AGAIN) { /* 数据不完整 */ }
// 成功时 ret 为消耗的字节数
if (cmd.type == RESP_ARRAY) {
    for (int i = 0; i < cmd.len; i++) {
        RespObj *arg = &cmd.elements[i];
        if (arg->type == RESP_STR)
            printf("%.*s\n", arg->len, (char *)arg->str);
    }
    respFreeObj(&cmd);  // 释放数组 elements
}
```

## 公共 API

### respParse

```c
int respParse(void *buf, size_t len, RespObj *out);
```

从 `buf` 中尝试解析一个完整的 RESP 对象。

| 返回值 | 含义 |
|--------|------|
| `> 0` | 成功，返回消耗的字节数 |
| `RESP_ERR (-1)` | 协议错误（格式异常） |
| `RESP_AGAIN (-2)` | 数据不完整，需继续读取 |

**设计要点：**

- **零拷贝**——`out->str` 直接指向 `buf` 内的位置，不分配额外内存。因此 `buf` 必须在处理完 `RespObj` 之前保持有效。
- **流式友好**——返回 `RESP_AGAIN` 时可保留缓冲区，等待新数据追加后继续解析。
- **数组递归**——`RESP_ARRAY` 的子元素通过 `respParse` 递归解析，`elements` 由 `malloc` 分配。
- **安全性保护**：
  - 数组嵌套深度限制（`MAX_PARSE_DEPTH`），防止栈溢出 DoS
  - 整数解析 ERANGE 检查，拒绝溢出输入
  - 总消耗字节数 `INT_MAX` 溢出保护
- **失败即清理**——`respParse` 返回错误时的语义：失败时 `*out` 被 `memset` 零初始化（`type=0`, `str=NULL`, `len=0`），调用方可安全传 `out` 给 `respFreeObj`

### respFreeObj

```c
void respFreeObj(RespObj *o);
```

递归释放 `RespObj` 中动态分配的资源。

**职责（且仅此职责）：**
- 释放 `RESP_ARRAY` 类型中由 `malloc` 分配的 `elements` 数组
- 若子元素本身也是 `RESP_ARRAY`，递归释放其子元素
- `o` 为 NULL 时无操作

**不适用的场景：**
- `RESP_STR` / `RESP_ERR_TYPE` 的 `str` 指针指向调用方的缓冲区（零拷贝），不由本函数管理
- `RESP_INT` / `RESP_NIL` 没有动态资源
- `RespObj` 结构体本身（如在栈上声明）不由本函数释放

| 参数 | 说明 |
|------|------|
| `o` | 指向由 `respParse` 填充的 `RespObj`，可以为 NULL |

## 内部设计（不对外暴露）

解析器采用递归下降风格，按首字节分流：

```
respParse()                // 入口: memset(out,0) → respParseDepth(buf, len, out, 0)
  └── respParseDepth()     // 深度检查 + 按首字节分流
        ├── '+' → parseSimpleString()
        ├── '-' → parseError()
        ├── ':' → parseInteger()
        ├── '$' → parseBulkString()
        └── '*' → parseArray() → 递归 respParseDepth()
```

底层公用函数：

| 函数 | 作用 |
|------|------|
| `readLine(buf, len)` | 在 `buf` 中搜索 `\r\n`，返回内容长度或错误码 |
| `readNumber(buf, len, *val)` | 读取 `\r\n` 结尾的十进制数字 |
