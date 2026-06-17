# 代码审查与修复报告

**审查范围**: `src/service.c`, `src/service.h`  
**参考文档**: `docs/service-layer.md`  
**审查日期**: 2026-06-17  
**分支**: `src_datatype`

---

## 修复总览

| # | 严重度 | 类别 | 位置 | 问题 |
|---|--------|------|------|------|
| 1 | 🔴 严重 | 内存泄漏 | `service.c:142-154` | SET 覆盖已有 key 时泄漏旧 ValObj + 新 key sds |
| 2 | 🔴 严重 | UB/崩溃 | `service.c:120-123` 等 | 4 个 handler 缺少 `argv[n].type` 检查 |
| 3 | 🔴 严重 | 架构缺陷 | `service.h:17` → `server.h:28` | `dbnum` 全局共享，SELECT 应 per-connection |
| 4 | 🔴 严重 | 数据损坏 | `service.c:39-41` | `addReplyBulkString` 溢出时 wlen 保留脏值 |
| 5 | 🟡 中等 | 未定义行为 | `service.c:20-21` | `snprintf` 截断未检测，可能泄漏堆内存 |
| 6 | 🟡 中等 | 越界风险 | `service.c:58,65` | `addReplyNull`/`addReplyOK` 缺 wcap 防护 |
| 7 | 🔵 低 | 文档 | `service.h:10` | `SERVICE_ERR` 注释不准确 |

---

## 详细分析

### 1. SET 覆盖已有 key 时内存泄漏

**位置**: [src/service.c:142-154](src/service.c#L142)

**根因**: `dictReplace` 在 key 已存在时仅覆写 `entry->val`，不释放旧 `ValObj`；新分配的 `key` sds 未被插入 dict 也未释放。

```c
// dict.c:71-72 — key 已存在分支
if (p == NULL)
    entry->val = val;   // 旧 ValObj 指针被覆盖 → 泄漏
// 新 key sds 经 dictAddRaw 哈希/比较后未插入，上层也未释放 → 泄漏
```

**修复**: `setCommand` 中先用 `dictfind` 预查旧值，`dictReplace` 后释放旧 `ValObj` 和未被使用的新 key sds。

```c
ValObj *old = dictfind(db, key);
dictReplace(db, key, obj);
if (old) {
    valObjFree(old);   // 释放被覆盖的旧 ValObj
    sdsfree(key);      // dict 保留了旧 key，新 key 未被插入，需释放
}
```

**泄漏规模**: 每次对同一 key 重复 SET，泄漏 ~3 笔堆分配（旧 ValObj 结构体 + 旧值 sds + 新 key sds）。

---

### 2. 缺少 `argv[n].type` 类型检查

**位置**: `setCommand`(L120)、`existsCommand`(L160)、`delCommand`(L176)、`selectCommand`(L193)

**根因**: `RespObj` 使用 union 共享 `str`/`integer`/`elements` 成员。只有 `getCommand` 检查了 `argv[1].type != RESP_STR`，其余 4 个 handler 直接读取 `.str` 和 `.len`。

```c
// 攻击场景: 客户端发送 RESP 整数作为 key 参数
// *3\r\n$3\r\nSET\r\n:123\r\n$3\r\nval\r\n
// argv[1].type == RESP_INT → argv[1].str 读到的指针值为 0x7b
// sdsnewlen(0x7b, ...) → 段错误
```

**修复**: 四个 handler 均增加类型检查，`setCommand` 同时对 `argv[1]` 和 `argv[2]` 检查。

---

### 3. `dbnum` 全局共享 → per-connection

**根因**: `dbnum` 放在 `struct service` 上，所有连接共享。客户端 A 执行 `SELECT 2` 后，客户端 B 的后续操作落到 DB 2 而非 DB 0。

Redis 的 SELECT 是连接级别的——每个客户端应持有独立的当前数据库编号。

**修复**:

| 文件 | 改动 |
|------|------|
| `service.h` | `struct service` 移除 `dbnum` 字段 |
| `server.h` | `struct Connection` 新增 `unsigned int dbnum` |
| `server.c` | `connNew` 初始化 `c->dbnum = 0` |
| `service.c` | 所有 `svc->db[svc->dbnum]` → `svc->db[c->dbnum]` |
| `service.c` | `selectCommand` 中 `svc->dbnum = idx` → `c->dbnum = idx` |

---

### 4. `addReplyBulkString` 溢出时静默返回，wlen 保留脏值

**位置**: [src/service.c:36-42](src/service.c#L36)

**根因**: 当 header + payload + CRLF 超出 `c->wcap` 时，函数直接 `return` 不设置 `c->wlen`。`handleWrite` 无条件发送 `c->wlen` 字节，客户端收到新旧数据混合的损坏响应。

```c
// 修复前
if (hdr < 0 || (size_t)hdr + len + 2 > c->wcap) return;

// 修复后
if (hdr < 0 || (size_t)hdr + len + 2 > c->wcap) {
    c->wlen = 0;   // 清空，避免发送脏数据
    return;
}
```

---

### 5. `snprintf` 截断未检测

**位置**: `addReplySimpleString`(L20)、`addReplyError`(L26)、`addReplyInteger`(L32)

**根因**: `snprintf` 在缓冲区不足时返回**期望写入的完整长度**（可能 > `wcap`），直接赋值给 `wlen` 会导致 `handleWrite` 发送缓冲区外的未初始化堆内存。

```c
// 修复前
c->wlen = (size_t)snprintf(c->wbuf, c->wcap, "+%s\r\n", str);

// 修复后
int n = snprintf(c->wbuf, c->wcap, "+%s\r\n", str);
c->wlen = (n > 0 && (size_t)n < c->wcap) ? (size_t)n : 0;
```

当前调用方传入字符串均较短（PONG、OOM 等），实际触发概率低，但防御性修复避免未来引入长字符串时发生数据泄漏。

---

### 6. `addReplyNull` / `addReplyOK` 缺 wcap 防护

**位置**: [src/service.c:56-68](src/service.c#L56)

**根因**: 使用裸 `memcpy(c->wbuf, ..., 5)` 而未检查 `c->wcap >= 5`。`connNew` 始终分配 4096 字节，**当前不会溢出**，但与其他 `addReply*` 函数的防护模式不一致。

**修复**: 增加 `if (c->wcap < 5) { c->wlen = 0; return; }` 守卫。

---

### 7. `SERVICE_ERR` 注释不准确

**位置**: [src/service.h:10](src/service.h#L10)

**修复前**:
```c
#define SERVICE_ERR  -1   /* 协议错误 / 未知命令 / OOM 等 */
```

**修复后**:
```c
#define SERVICE_ERR  -1   /* 协议错误（argv 为空/命令名非 STR） */
```

设计文档明确规定：**业务错误**（如"unknown command"）走 `addReplyError` 写回客户端并返回 `SERVICE_OK`；只有**协议层面违规**才返回 `SERVICE_ERR`。原注释将"未知命令"归类为 `SERVICE_ERR` 会误导调用方错误关闭连接。

---

## 受影响文件清单

| 文件 | 改动类型 |
|------|----------|
| `src/service.c` | 修复内存泄漏、类型检查、buffer 防护、dbnum 迁移 |
| `src/service.h` | 移除 dbnum、修正注释 |
| `src/server.h` | `Connection` 新增 `dbnum` 字段 |
| `src/server.c` | `connNew` 初始化 `dbnum = 0` |

## 验证

```
$ make all
======= 全部构建完成 =======

$ ./test_dict && ./test_resp && ./test_sds
======== 🎉 全部测试通过 ========
======== 🎉 全部测试通过 ========
======== 全部测试通过 ========
```

编译零警告，所有已有测试无回归。
