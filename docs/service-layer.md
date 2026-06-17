# 服务层接口设计文档

## 1. 架构位置

```
Client ──→ epoll ──→ handleRead ──→ respParse (协议解析)
                                          │
                                          ▼
                                   processCommand (服务层)
                                          │
                                    ┌─────┴─────┐
                                    ▼           ▼
                              命令表查找    addReply* 写响应
                                    │           │
                                    ▼           ▼
                              dict 引擎    c->wbuf ──→ Client
```

服务层是 **RESP 协议解析** 和 **dict 引擎** 之间的胶水层，负责：

- 命令路由（命令名 → handler）
- 参数校验（arity）
- 业务逻辑（GET/SET/DEL/...）
- 结果序列化（RESP 格式写回）

---

## 2. 返回值约定

| 宏 | 值 | 含义 | 触发场景 |
|---|---|---|---|
| `SERVICE_OK` | 0 | 处理完成 | 命令执行成功 / 业务错误已写回客户端 |
| `SERVICE_ERR` | -1 | 内部错误 | argv 为空 / 命令名不是字符串 |
| `SERVICE_AGAIN` | -2 | 数据不完整 | **预留**，由上层 `respParse` 返回 |

关键区分：**业务错误**（如"unknown command"、"index out of range"）走 `addReplyError` 写回客户端，`processCommand` 返回 `SERVICE_OK`。只有**协议层面的违规**（空数组、非字符串命令名）才返回 `SERVICE_ERR`。

---

## 3. 数据结构

### 3.1 `struct service` — 服务层状态

```c
struct service {
    struct dict **db;           // 数据库数组，db[0..dbsize-1]
    unsigned int  dbnum;        // 当前选中库（默认 0）
    unsigned int  dbsize;       // 数据库总数
};
```

每个 `db[i]` 是 `dictnew(4, &dictTypeSds)` 创建的哈希表：
- key: SDS 字符串
- val: `ValObj *`，通过 `valObjFree` 释放

### 3.2 `CmdHandler` — 命令处理函数签名

```c
typedef void (*CmdHandler)(struct Connection *c, struct service *svc,
                           RespObj *argv, int argc);
```

| 参数 | 说明 |
|---|---|
| `c` | 连接对象，handler 通过 `addReply*` 写入 `c->wbuf` |
| `svc` | 服务层状态，`svc->db[svc->dbnum]` 获取当前库 |
| `argv` | RESP 数组元素，`argv[0]` 是命令名，`argv[1..]` 是参数 |
| `argc` | `argv` 数组长度 |

### 3.3 `Command` — 命令表条目

```c
typedef struct {
    const char *name;      // 命令名（大写）
    int         arity;     // 参数个数（不含命令名），-1 变长
    CmdHandler  handler;
} Command;
```

---

## 4. 生命周期 API

### 4.1 `serviceInit`

```c
int serviceInit(struct service *svc, unsigned int dbsize);
```

- 行为：分配 `dbsize` 个 dict，每个初始 16 桶
- 成功返回 `SERVICE_OK`，任一 dict 创建失败则回滚已分配的，返回 `SERVICE_ERR`
- `svc->dbnum` 初始化为 0

### 4.2 `serviceFree`

```c
void serviceFree(struct service *svc);
```

- 行为：释放所有 dict 和 db 数组，置 `svc->db = NULL`
- 可安全传入 NULL

---

## 5. RESP 响应写入 API

所有函数将 RESP 协议字符串直接写入 `c->wbuf`，设置 `c->wlen`。
**调用方负责将 `c->state` 置为 `CONN_STATE_WRITE`**（由上层 `handleRead` 统一设置）。

### 5.1 `addReplyOK`

```c
void addReplyOK(Connection *c);
// 输出: +OK\r\n  (5 bytes)
```

### 5.2 `addReplySimpleString`

```c
void addReplySimpleString(Connection *c, const char *str);
// 输出: +{str}\r\n
// 示例: addReplySimpleString(c, "PONG") → +PONG\r\n
```

### 5.3 `addReplyError`

```c
void addReplyError(Connection *c, const char *msg);
// 输出: -ERR {msg}\r\n
// 示例: addReplyError(c, "unknown command") → -ERR unknown command\r\n
```

### 5.4 `addReplyInteger`

```c
void addReplyInteger(Connection *c, long long val);
// 输出: :{val}\r\n
// 示例: addReplyInteger(c, 42) → :42\r\n
```

### 5.5 `addReplyBulkString`

```c
void addReplyBulkString(Connection *c, const char *str, size_t len);
// 输出: ${len}\r\n{str}\r\n  (二进制安全)
// 示例: addReplyBulkString(c, "hello", 5) → $5\r\nhello\r\n
// 注意: 若 wbuf 剩余空间不足，静默返回（不写入）
```

### 5.6 `addReplyBulkSds`

```c
void addReplyBulkSds(Connection *c, void *s);
// 等价于 addReplyBulkString(c, s, sdslen(s))
```

### 5.7 `addReplyNull`

```c
void addReplyNull(Connection *c);
// 输出: $-1\r\n  (5 bytes)
```

---

## 6. 命令分发

### 6.1 `processCommand`

```c
int processCommand(Connection *c, struct service *svc,
                   RespObj *argv, int argc);
```

**执行流程：**

```
processCommand(c, svc, argv, argc)
  │
  ├─ argc < 1 ──────────────────→ SERVICE_ERR
  ├─ argv[0] 不是 STR ──────────→ SERVICE_ERR
  │
  ├─ 遍历 cmd_table[]
  │    ├─ respStrEqCase 大小写不敏感匹配
  │    ├─ 命中 → 检查 arity
  │    │    ├─ 参数不对 → addReplyError("wrong number...") → SERVICE_OK
  │    │    └─ 正确 → handler(c, svc, argv, argc) → SERVICE_OK
  │    └─ 未命中 → 继续
  │
  └─ 全部未命中 → addReplyError("unknown command") → SERVICE_OK
```

---

## 7. 命令表

### 7.1 当前命令

| 命令 | arity | 说明 |
|---|---|---|
| `PING` | 0 | `+PONG` |
| `GET` | 1 | `$len\r\nval\r\n` 或 `$-1\r\n` |
| `SET` | 2 | `+OK` |
| `EXISTS` | 1 | `:1` 或 `:0` |
| `DEL` | 1 | `:1` 或 `:0` |
| `SELECT` | 1 | `+OK`（切换 svc->dbnum） |

### 7.2 新增命令指南

1. 在 `service.c` 中编写 `static void xxxCommand(...)` 
2. 在 `cmd_table[]` 中添加 `{"XXX", N, xxxCommand},`
3. 命令名用大写，匹配时自动 case-insensitive

---

## 8. 与上层集成（TODO）

`server.c` 的 `handleRead` 需要改造为：

```c
// 伪代码
static void handleRead(Connection *c, struct Server *s) {
    // 1. read → c->rbuf 追加
    // 2. respParse → RespObj obj
    //    - RESP_AGAIN → 等待下一轮
    //    - RESP_ERR   → addReplyError
    //    - 成功 → 3
    // 3. processCommand(c, s->svc, obj.elements, obj.len)
    // 4. respFreeObj(obj)
    // 5. c->state = CONN_STATE_WRITE
}
```

需要调整的结构：
- `struct Server` 新增 `struct service svc;` 字段
- `Connection` 新增 `struct Server *server;` 回指针（或 `handleRead` 多收一个参数）
