# dict 哈希表核心

## 概述

链地址法哈希表，通过 `dictType` 虚函数表支持泛型键值，采用**头插法**解决冲突。是整个 FlashKV 引擎的数据存储基石——所有命令最终都落到 `dictAdd` / `dictfind` / `dictDelete`。

## 常量

```c
#define DICT_OK    0   // 操作成功
#define DICT_ERROR 1   // 操作失败（key 已存在 / key 不存在 / NULL 参数）
```

## struct dictEntry（不暴露）

```c
struct dictEntry {
    void *key;
    void *val;
    struct dictEntry *next;  // 链表解决冲突（头插法）
};
```

单向链表，无尾指针。头插法意味着同 key 的旧值被链在后方（但 `dictAddRaw` 先检查同 key 是否存在，实际不会重复插入）。

## struct dictht

```c
struct dictht {
    dictEntry **table;      // 桶数组，size 个 slot
    unsigned long size;     // 桶数量 = 2^n
    unsigned long sizemask; // size - 1，用于取模
    unsigned long used;     // 当前存储的键值对数
};
```

**约束：** `size` 必须为 2 的幂，取模运算用 `hash & sizemask` 加速（而非 `hash % size`）。

## struct dictType

```c
struct dictType {
    uint64_t (*hash)(const void *key);           // 哈希函数
    int      (*keyCompare)(const void *k1, const void *k2); // 比较（0 = 相等）
    void     (*keyFree)(void *key);              // 释放 key
    void     (*valFree)(void *val);              // 释放 value
};
```

调用方通过填充此表来自定义键值行为。FlashKV 提供了 SDS 版本的实现：`extern struct dictType dictTypeSds`（`hash=SDS MurmurHash2`, `compare=sdsCompare`, `keyFree=sdsfree`, `valFree=valObjFree`）。

## struct dict

```c
struct dict {
    struct dictType *type;  // 虚函数表
    struct dictht ht;       // 单表（未实现 rehash / 渐进式扩容）
};
```

**注意：** 当前仅为单表设计。Redis 中 `struct dict` 包含 `ht[2]` 用于渐进式 rehash，FlashKV 尚未实现此机制。

## 公共 API

### dictnew

```c
struct dict *dictnew(unsigned long n, struct dictType *type);
```

创建哈希表。`n` 为指数参数——实际 `size = 1 << n`。`n=4` → 16 槽，`n=10` → 1024 槽。`type` 决定键值的 hash/compare/free 行为，可以为 NULL 吗（未检查）。

### dictAdd

```c
int dictAdd(struct dict *d, void *key, void *val);
```

插入键值对。**key 已存在时返回 `DICT_ERROR`**（不覆盖）。成功返回 `DICT_OK`。采用头插法。

**内存语义：** `key` 和 `val` 的所有权转移给 dict。dict 释放时会调用 `type->keyFree(key)` 和 `type->valFree(val)`。调用方不应在 dict 持有期间 free 它们。

### dictReplace

```c
int dictReplace(struct dict *d, void *key, void *val);
```

插入或覆盖。key 不存在时行为同 `dictAdd`；已存在时覆盖旧值（**注意：覆盖时是否释放旧 key 和旧 val？当前实现仅赋值 `entry->val = val`，不调用 `keyFree`/`valFree`——旧值泄漏**）。始终返回 `DICT_OK`。

### dictfind

```c
void *dictfind(struct dict *d, const void *key);
```

查找，返回 `val` 指针（`void *`），未找到返回 `NULL`。对外层调用者而言返回值即 `ValObj*`。

### dictDelete

```c
int dictDelete(struct dict *d, const void *key);
```

删除键值对。成功返回 `DICT_OK`，key 不存在返回 `DICT_ERROR`。会调用 `keyFree` 和 `valFree` 释放键值内存。

### dictfree

```c
void dictfree(struct dict *d);
```

释放整个哈希表。遍历所有桶、所有链表节点，逐对调用 `keyFree` 和 `valFree`。

## 内部函数（不对外暴露）

| 函数 | 职责 |
|------|------|
| `dictEntryNew(key, val, next)` | 分配 `dictEntry`，保存键值，设置 next |
| `dictAddRaw(d, key, existing)` | 查找或插入。key 已存在时写入 `*existing` 返回 `NULL`；可插入时头插返回新 entry |
| `dicthtGetIdx(ht, hashVal)` | `hashVal & ht->sizemask` |
| `dictEntryFree(d, de)` | 调 `keyFree` + `valFree` 后释放 entry 自身 |
| `dicthtfree(d, dht)` | 遍历释放所有 entry + table |

## 已知问题

1. **dictReplace 旧值泄漏** — 覆盖时不调 `valFree`（可能还有 `keyFree`），旧 ValObj 永久泄漏。
2. **无 rehash / 渐进式扩容** — 负载因子上升后 `used/size` 增大，查找退化为 O(n) 链表扫描。当前无扩容触发逻辑，用户必须估算初始 `n`。
3. **dictnew 不接受 `n=0`** — `1ul << 0 = 1`，实际可用，但语义上 "0 表示默认大小" 的直觉会创建单槽表。
4. **dictType 可能为 NULL** — `dictnew` 未校验 `type` 参数，若传入 NULL 则在 `dictAddRaw` 调用 `d->type->hash(key)` 时崩溃。
5. **单线程假设** — 无任何锁机制。多线程访问需要外部同步。
6. **注释残留** — `dict.c` 中包含被注释掉的 `dictHash`、`dictKeyCmp` 等早期代码，略显杂乱。
