# dict.c 渐进式 Rehash 审查报告

**审查范围**: `src/dict.c`, `src/dict.h`  
**审查重点**: 新增的渐进式 rehash 功能  
**审查日期**: 2026-06-19

---

## 变更概要

将单表 dict 升级为双表渐进式 rehash，核心改动：

| 组件 | 变更 |
|------|------|
| `dictEntry` | 新增 `uint64_t hash` 缓存，rehash 时直接 `& sizemask` |
| `struct dict` | `struct dictht ht` → `ht[2]`；新增 `long rehashidx`（-1 = 未 rehash） |
| `dictAddRaw` | rehash 期间每操作搬 1 桶 + 双表去重 + 新 key 写 ht[1] |
| `dictReplace` | 独立实现插入逻辑（不再委托 dictAddRaw）+ 双表查找 |
| `dictfind` / `dictDelete` | 双表遍历 + 每操作搬 1 桶 |
| `dictRehash` | 触发扩容（ht[1] 初始化为 ht[0] 的 2 倍） |
| `dictRehashStep` | 搬移 `number` 个桶；全部搬完自动 swap 表 |
| `dictnew` / `dictfree` | 适配双表 + rehashidx 初始化/清理 |

---

## 审查结论：✅ 通过

渐进式 rehash 的**核心逻辑正确**，所有 13 个测试（含 5 个新增 rehash 测试）通过，ASan 零泄漏。

---

## 发现的 Bug & 修复

### 🔴 [dict.c:183](src/dict.c#L183) — dictReplace 不触发 rehash

**根因**: 旧版 `dictReplace` 委托 `dictAddRaw`，后者在插入新 key 后检查负载因子并触发 `dictRehash`。重写后 `dictReplace` 有了独立插入逻辑，但**遗漏了负载因子检查**。

```c
// 修复前（dictReplace 插入新 key 分支）
ht->table[idx] = dictEntryNew(hashVal, key, val, ht->table[idx]);
ht->used++;
return DICT_OK;   // ← 没有触发 rehash！
```

**后果**: 若服务层只用 `dictReplace`（如 `setCommand`），表负载因子可无限增长，永不扩容。

**修复**: 在 `dictReplace` 插入新 key 后加入与 `dictAddRaw` 一致的触发逻辑：

```c
ht->used++;
/* 负载因子 > 1 且没在 rehash → 触发扩容 */
if (!dictIsRehashing(d) && d->ht[0].used > d->ht[0].size)
    dictRehash(d);
return DICT_OK;
```

**验证**:
```
=== test_rehash_replace_trigger ===
dictReplace 20 条: rehashidx=3 ht[0].size=16 (expect rehash triggered)  ✅
```

---

## 修复的测试问题

| 测试 | 问题 | 修复 |
|------|------|------|
| `test_replace` | 旧 ValObj 被 dictReplace 覆盖后未释放 → ASan 37 字节泄漏 | `dictfind` 预查旧值 → `dictReplace` → `valObjFree(old)` |
| `test_multiple_keys` | 断言 `d->ht[0].used == N`，但 rehash 后部分 entry 已迁至 ht[1] | 使用 `dictTotalUsed()` = `ht[0].used + ht[1].used` |

---

## 新增测试

5 个 rehash 专项测试覆盖关键路径：

| 测试 | 覆盖场景 |
|------|----------|
| `test_rehash_trigger` | 插入 17 条触发扩容（16→32），验证 rehashidx 和 ht[1].size |
| `test_rehash_find_during` | rehash 进行中查找 20 条，全部可查（跨双表） |
| `test_rehash_delete_during` | rehash 进行中删除，used 正确递减 |
| `test_rehash_replace_trigger` | 验证 dictReplace 也能触发 rehash |
| `test_rehash_complete` | 反复 dictfind 推进 rehash 至完成，验证表 swap 后 ht[0].size=32 |

---

## 设计评审

### ✅ 渐进式迁移

每次 `dictAddRaw` / `dictfind` / `dictDelete` / `dictReplace` 调用 `dictRehashStep(d, 1)`，搬 1 个桶（含整条链）。均摊 O(1) 每操作，避免单次长停顿。

### ✅ Entry hash 缓存

`dictEntry` 新增 `uint64_t hash`，迁移时直接用 `entry->hash & ht[1].sizemask` 定位新桶，无需重新调用 `d->type->hash(key)`。正确且高效。

### ✅ 双表去重

rehash 期间 `dictAddRaw` 先查 ht[0]（旧表）再查 ht[1]（新表），确保不会重复插入。`dictReplace` 同样逻辑——在 ht[0] 找到则原地更新值。

### ✅ Rehash 完成时表交换

```c
d->rehashidx = -1;
dicthtfree(d, d->ht);   // 释放旧 ht[0] 桶数组
d->ht[0] = d->ht[1];    // struct copy
d->ht[1].table = NULL;  // 清零备用表
```

`dicthtfree` 使用 `remaining` 计数器提前退出（`used == 0` 时不遍历桶），正确且高效。

### ✅ 负载因子触发

`used > size`（即 used > size `≧ size + 1`），阈值约为 1.0。合理。

---

## 设计建议（非阻塞）

### 🟡 [dict.c:58](src/dict.c#L58) — size 翻倍无溢出检查

`d->ht[0].size << 1` 在大表（size > 2^63）时可能溢出为 0 或回绕。当前 size=16→32→... 距离溢出遥远，加上防御性断言即可。

### 🟡 [dict.c:66](src/dict.c#L66) — rehashidx + number 可能回绕

`(unsigned long)d->rehashidx + number` 在极端值下可能回绕。但 rehashidx 受 size 约束，实际不会触发。防御性 `MIN(end, d->ht[0].size)` 已存在。

### 🔵 [dict.c:48](src/dict.c#L48) — dicthtInit 中有陈旧注释

```c
dht->table = calloc(n, sizeof(dictEntry *)); // ← 少了这个
```

注释写"少了这个"，但代码已包含。建议删除误导性注释。

---

## 测试结果

```
$ make test_dict && ./test_dict

======== Dict 单元测试 ========
=== test_add_and_find ===          ✅
=== test_replace ===               ✅
=== test_delete ===                ✅
=== test_delete_missing ===        ✅
=== test_delete_no_key_free ===    ✅
=== test_multiple_keys ===         ✅
=== test_valobj_types ===          ✅
=== test_null_safety ===           ✅

--- 渐进式 rehash ---
=== test_rehash_trigger ===        ✅
=== test_rehash_find_during ===    ✅
=== test_rehash_delete_during ===  ✅
=== test_rehash_replace_trigger ===✅
=== test_rehash_complete ===       ✅

======== 🎉 全部测试通过 ========

$ ASan: 0 leaks, 0 errors
$ make all: 0 warnings
```
