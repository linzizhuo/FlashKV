# 服务层设计文档

> 最后更新: 2026-06-21

## 1. 架构位置

```
Client ──→ epoll ──→ handleRead ──→ respParse (零拷贝协议解析)
                                         │
                                    ┌────┴────┐
                                    ▼         ▼
                            processCommand   addReply*
                                    │         │
                              bsearch 命令表  │
                                    │         │
                                    ▼         ▼
                              kvdb 引擎    c->wbuf (追加模式)
                                    │         │
                                    ▼         ▼
                              dict + expires  ──→ handleWrite → Client
```

服务层是 RESP 协议解析和 kvdb 存储引擎之间的胶水层，负责：

- **命令路由** — `bsearch` 二分查找命令表（O(log n)）
- **参数校验** — arity 检查，类型校验
- **业务逻辑** — GET/SET/DEL/EXPIRE/TTL/...
- **响应序列化** — `addReply*` 追加写入 `c->wbuf`，支持 pipeline 批量写回

---

## 2. 返回值约定

| 宏 | 值 | 含义 | 触发场景 |
|---|---|---|---|
| `SERVICE_OK` | 0 | 处理完成 | 命令执行成功 / 业务错误已写回客户端 |
| `SERVICE_ERR` | -1 | 协议违规 | argv 为空 / 命令名不是 RESP_STR |
| `SERVICE_AGAIN` | -2 | 数据不完整 | **预留**，由上层 `respParse` 返回 |

**关键区分**：业务错误（"unknown command"、"wrong type for key"）走 `addReplyError` 写回客户端，返回 `SERVICE_OK`。只有协议层面的违规（空数组、非字符串命令名）才返回 `SERVICE_ERR`。

---

## 3. 数据结构

### 3.1 `struct service` — 服务层状态

```c
struct service {
    kvdb        **kvs;       // 数据库数组，kvs[0..dbsize-1]
    unsigned int  dbsize;    // 数据库总数（启动时固定）
};
```

每个 `kvs[i]` 是一个 `kvdb` 实例——封装了主 dict + expires dict + 惰性删除 + key 所有权管理。7 个公开方法：`kvdbNew/Free/Get/Set/Del/Exists/Expire/TTL/Persist/ActiveExpireCycle`。

**`dbnum`（当前选中库）存储在 Connection 上**，不在 service 上。每个连接独立选择数据库。

```c
// server.h
typedef struct Connection {
    int fd;
    enum ConnState state;
    char *rbuf;  size_t rlen, rcap;   // 读缓冲区
    char *wbuf;  size_t wlen, wcap;   // 写缓冲区（追加模式）
    unsigned int    dbnum;            // 当前选中的数据库 (per-connection)
    struct service *svc;              // 服务层回指针
} Connection;
```

### 3.2 `CmdHandler` — 命令处理函数签名

```c
typedef void (*CmdHandler)(struct Connection *c, struct service *svc,
                           RespObj *argv, int argc);
```

| 参数 | 说明 |
|---|---|
| `c` | 连接对象，handler 通过 `addReply*` 追加写入 `c->wbuf` |
| `svc` | 服务层状态，`svc->kvs[c->dbnum]` 获取当前连接选中的库 |
| `argv` | RESP 数组元素，`argv[0]` 是命令名，`argv[1..]` 是参数 |
| `argc` | `argv` 数组长度 |

### 3.3 `Command` — 命令表条目

```c
typedef struct {
    const char *name;      // 命令名（大写，如 "GET"）
    int         arity;     // 参数个数（不含命令名），0 = 无参数
    CmdHandler  handler;
} Command;
```

命令表按**字典序排列**：

```c
static Command cmd_table[] = {
    {"DEL",       1,  delCommand},       // D...
    {"EXISTS",    1,  existsCommand},    // E...
    {"EXPIRE",    2,  expireCommand},
    {"EXPIREAT",  2,  expireatCommand},
    {"GET",       1,  getCommand},       // G...
    {"PERSIST",   1,  persistCommand},   // P...
    {"PEXPIRE",   2,  pexpireCommand},
    {"PEXPIREAT", 2,  pexpireatCommand},
    {"PING",      0,  pingCommand},
    {"PTTL",      1,  pttlCommand},
    {"SELECT",    1,  selectCommand},    // S...
    {"SET",       2,  setCommand},
    {"TTL",       1,  ttlCommand},
};
```

**按字典序是为了 `bsearch` 二分查找**，O(log n) 而非 O(n) 线性扫描。`cmdCompare` 使用 `strncasecmp` 实现大小写不敏感匹配。

---

## 4. 生命周期 API

### 4.1 `serviceInit`

```c
int serviceInit(struct service *svc, unsigned int dbsize);
```

- 分配 `dbsize` 个 `kvdb` 实例
- 成功返回 `SERVICE_OK`
- 任一 `kvdbNew` 失败则回滚已分配的，返回 `SERVICE_ERR`
- 调用方负责传入已分配好的 `struct service`（通常嵌入在 `struct Server` 中）

### 4.2 `serviceFree`

```c
void serviceFree(struct service *svc);
```

- 释放所有 `kvdb` 实例和 `kvs` 数组
- 置 `svc->kvs = NULL`
- 可安全传入 NULL 或已释放的 service

---

## 5. RESP 响应写入（追加模式，支持 Pipeline）

所有 `addReply*` 函数将 RESP 协议数据**追加**到 `c->wbuf` 末尾，空间不足时自动 **2× 扩容**。

**Pipeline 工作流**：

```
handleRead 循环:
  1. respParse → 解析一条完整命令
  2. processCommand → 命令处理 → addReply* 追加到 c->wbuf
  3. 如果 rbuf 还有数据 → goto 1 (下一条命令)
循环结束:
  4. c->state = CONN_STATE_WRITE
  5. 下次 epoll_wait 返回写就绪 → handleWrite 一次性写回客户端
```

这意味着：单次 `handleRead` 可以处理多条 pipeline 命令，响应逐条追加到 `wbuf`，最后一次性 `writev`/`write` 写回，减少系统调用。

### 5.1 `addReplyOK`

```c
void addReplyOK(Connection *c);
// → +OK\r\n  (5 bytes)
```

### 5.2 `addReplySimpleString`

```c
void addReplySimpleString(Connection *c, const char *str);
// → +{str}\r\n
```

### 5.3 `addReplyError`

```c
void addReplyError(Connection *c, const char *msg);
// → -ERR {msg}\r\n   (自动添加 "-ERR " 前缀)
```

### 5.4 `addReplyInteger`

```c
void addReplyInteger(Connection *c, long long val);
// → :{val}\r\n
```

### 5.5 `addReplyBulkString`

```c
void addReplyBulkString(Connection *c, const char *str, size_t len);
// → ${len}\r\n{str}\r\n  (二进制安全)
```

### 5.6 `addReplyBulkSds`

```c
void addReplyBulkSds(Connection *c, void *s);
// 等价于 addReplyBulkString(c, s, sdslen(s))
```

### 5.7 `addReplyNull`

```c
void addReplyNull(Connection *c);
// → $-1\r\n  (5 bytes)
```

### 5.8 内存管理

`replyEnsure` 内部逻辑：

```c
static int replyEnsure(Connection *c, size_t space) {
    size_t needed = c->wlen + space;
    if (needed <= c->wcap) return 1;    // 空间够，快速路径
    size_t newcap = c->wcap * 2;         // 2× 扩容
    if (newcap < needed) newcap = needed;
    if (newcap < 256)    newcap = 256;   // 最小 256 字节
    char *p = realloc(c->wbuf, newcap);
    if (!p) { c->wlen = 0; return 0; }   // OOM: 清空缓冲区
    c->wbuf = p;
    c->wcap = newcap;
    return 1;
}
```

---

## 6. 命令分发

### 6.1 `processCommand`

```c
int processCommand(struct Connection *c, struct service *svc,
                   RespObj *argv, int argc);
```

**执行流程：**

```
processCommand(c, svc, argv, argc)
  │
  ├─ argc < 1 ──────────────────────────→ SERVICE_ERR
  ├─ argv[0].type != RESP_STR ──────────→ SERVICE_ERR
  │
  ├─ bsearch(argv[0], cmd_table, ...)
  │    ├─ 命中 → 检查 arity
  │    │    ├─ 参数不对 → addReplyError("wrong number...") → SERVICE_OK
  │    │    └─ 正确 → handler(c, svc, argv, argc) → SERVICE_OK
  │    └─ 未命中 → addReplyError("unknown command") → SERVICE_OK
```

`cmdCompare` 使用 `strncasecmp`，命令名大小写不敏感（`get`/`Get`/`GET` 均可）。

---

## 7. 当前命令

| 命令 | arity | 实现要点 |
|------|-------|---------|
| `PING` | 0 | `+PONG` |
| `SELECT n` | 1 | 切换 `c->dbnum`，OOB 检查 |
| `SET key val` | 2 | RESP_INT → VAL_INT 快速路径；RESP_STR → SDS 存储 |
| `GET key` | 1 | VAL_INT → `:N`，VAL_STRING → `$len\r\n...\r\n`，不存在 → `$-1` |
| `DEL key` | 1 | 同时清除 expires 字典 |
| `EXISTS key` | 1 | 含惰性过期检查 |
| `EXPIRE key sec` | 2 | 相对秒 → 绝对 `time_t` |
| `PEXPIRE key ms` | 2 | 相对毫秒 → 绝对秒（`/1000`） |
| `EXPIREAT key ts` | 2 | 绝对秒 |
| `PEXPIREAT key ts` | 2 | 绝对毫秒 → 绝对秒（`/1000`） |
| `TTL key` | 1 | -2=不存在, -1=无TTL, ≥0=剩余秒 |
| `PTTL key` | 1 | 同上，剩余时间 ×1000 |
| `PERSIST key` | 1 | 移除 expires 条目 |

### 7.1 新增命令指南

1. 在 `service.c` 中编写 `static void xxxCommand(Connection *c, struct service *svc, RespObj *argv, int argc)`
2. 使用 `addReply*` 写入 `c->wbuf`
3. 在 `cmd_table[]` 中**按字典序**插入 `{"XXX", N, xxxCommand},`
4. 用 `sds key = respKeyToSds(&argv[1])` 获取 key，用完后 **必须 `sdsfree(key)`**（kvdb 内部会 `sdsdup`）

---

## 8. 多数据库模型

```
struct Server
  └── struct service svc
        └── kvs[0]  → kvdb (主 dict + expires dict)
            kvs[1]  → kvdb
            ...
            kvs[15] → kvdb

Connection
  └── dbnum = 0        ← SELECT 切换，per-connection
  └── svc → &Server.svc ← 回指针
```

- `dbsize` 在 `serverCreate` 时由 `serviceInit(&s->svc, 16)` 固定为 16
- `dbnum` 存在 Connection 上，每个连接独立选择库
- 命令 handler 通过 `svc->kvs[c->dbnum]` 获取当前库

---

## 9. Key 所有权约定

```
调用方传入 key (RespObj 上的零拷贝指针)
  └── respKeyToSds → sdsnewlen (栈上新建 SDS)
        └── kvdbGet/Set/Del (传入 SDS)
              └── kvdb 内部 sdsdup ← 存储层接管所有权
        └── sdsfree ← 调用方释放栈上 SDS
```

**规则**：kvdb 内部自己管理所有 key 的所有权（`sdsdup`），调用方传入的 key 不会被接管，始终由调用方负责释放。这是 kvdb 作为 deep module 的核心接口约定。
