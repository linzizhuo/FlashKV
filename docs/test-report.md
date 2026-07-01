# FlashKV 测试报告

**日期**: 2026-07-01
**总测试用例**: 76 个（SDS 3 + RESP 19 + Dict 13 + ZSet/Skiplist 32 + Dict Rehash 5 + 其他 4）
**结果**: ✅ 全部通过

---

## 一、SDS 动态字符串 (3 tests)

| 测试 | 验证内容 |
|------|---------|
| `test_basic_create_and_len` | `sdsnew` 创建 + `sdslen` 长度 |
| `test_binary_safety` | 二进制安全（`\0` 嵌入不截断） |
| `test_sdsfree_null` | `sdsfree` 释放后无 crash |

## 二、RESP 协议解析 (19 tests)

| 测试 | 验证内容 |
|------|---------|
| `test_simple_string` | `+OK\r\n` → RespObj |
| `test_simple_string_incomplete` | 半包返回 `RESP_AGAIN` |
| `test_simple_string_too_long` | 单行过长拒绝 |
| `test_error` | `-ERR ...\r\n` 错误类型 |
| `test_integer` | `:100\r\n` 整数类型，含负数 |
| `test_bulk_string` | `$5\r\nhello\r\n` Bulk String |
| `test_bulk_binary_safe` | Bulk String 含 `\r\n` 嵌入 |
| `test_array` | `*3\r\n...` 数组解析 |
| `test_null_array` | `*-1\r\n` Null Array |
| `test_empty_array` | `*0\r\n` 空数组 |
| `test_array_mixed_types` | 数组中混合 String/Integer |
| `test_nested_array` | 嵌套数组递归解析 |
| `test_array_incomplete` | 数组半包 → `RESP_AGAIN` |
| `test_array_bad_number` | 非法数组长度拒绝 |
| `test_empty_buf` | 空缓冲区 → `RESP_AGAIN` |
| `test_unknown_first_byte` | 未知类型字节拒绝 |
| `test_incomplete_headers` | 各类型头部不完整 → `RESP_AGAIN` |
| `test_redis_hello_command` | 模拟 `HELLO 3` 命令 |
| `test_free_null_safety` | `respFreeObj(NULL)` 不崩溃 |
| `test_nested_array_no_crash_on_deep` | 深度 20 嵌套不崩溃 |

## 三、Dict 哈希表 (13 tests)

### 基本操作 (8 tests)

| 测试 | 验证内容 |
|------|---------|
| `test_add_and_find` | `dictAdd` + `dictfind`，重复 key 拒绝 |
| `test_replace` | `dictReplace` 覆写已存在的 key |
| `test_delete` | `dictDelete` 删除后 `dictfind` 返回 NULL |
| `test_delete_missing` | 删除不存在的 key 返回 `DICT_ERROR` |
| `test_delete_no_key_free` | 删除后不需要调用方释放 key |
| `test_multiple_keys` | 大量 key 插入/查找/删除 |
| `test_valobj_types` | ValObj STRING/INT 类型存储 |
| `test_null_safety` | `dictfree(NULL)` + `dictDelete(NULL)` 防御 |

### 渐进式 Rehash (5 tests)

| 测试 | 验证内容 |
|------|---------|
| `test_rehash_trigger` | 16 条不触发，17 条触发 rehash (ht[1].size=32) |
| `test_rehash_find_during` | rehash 期间跨双表查找 20 条全命中 |
| `test_rehash_delete_during` | rehash 期间删除 10 条后总数正确 |
| `test_rehash_replace_trigger` | `dictReplace` 在 rehash 期间触发扩容 |
| `test_rehash_complete` | 10 次 `dictfind` 后 rehash 完成，双表合并 |

## 四、ZSet / Skiplist (32 tests)

### 跳表独立测试 (18 tests)

| 测试 | 验证内容 |
|------|---------|
| `test_zsl_create_empty` | 创建空跳表：length=0, level=1, tail=NULL |
| `test_zsl_insert_order` | 顺序插入 10 个，L0 正向扫描验证排序 |
| `test_zsl_insert_reverse` | 逆序插入 10 个，仍按 score 排序 |
| `test_zsl_insert_duplicate` | 精确重复 (score, ele) 被拒绝，返回已有节点 |
| `test_zsl_insert_same_score` | 相同 score 不同 ele，按字典序排列 |
| `test_zsl_delete_head` | 删除头节点，L0 链正确更新 |
| `test_zsl_delete_tail` | 删除尾节点，tail 指针正确前移 |
| `test_zsl_delete_middle` | 删除中间节点，前后链正确连接 |
| `test_zsl_delete_nonexist` | 删除不存在的节点返回 0 |
| `test_zsl_delete_all` | 逐一删除全部节点后 length=0, tail=NULL, level=1 |
| `test_zsl_delete_reinsert` | 删除后重新插入，score 顺序正确 |
| `test_zsl_rank` | `zslrank` 首位/末位/中间/不存在 |
| `test_zsl_by_rank` | `zslbyrank` 取首位/中间/末位/越界 |
| `test_zsl_count` | `zslcount` 全范围/子区间/单点/空区间/min>max |
| `test_zsl_range` | `zslrange` 区间查询返回正确节点数组 |
| `test_zsl_delrange` | `zsldelrange` 区间删除后数量+剩余元素验证 |
| `test_zsl_backward` | backward 反向遍历与正向对称 |
| `test_zsl_free_null` | `zslfree(NULL)` 不崩溃 |

### ZSet 双索引测试 (14 tests)

| 测试 | 验证内容 |
|------|---------|
| `test_zset_create_free` | 创建 zset 含 dict + skiplist |
| `test_zset_free_null` | `zsetFree(NULL)` 不崩溃 |
| `test_zset_add_find` | `zsetAdd` 后用 `zsetFind` O(1) 查找 |
| `test_zset_add_batch` | 批量插入 100 个，验证首尾 rank |
| `test_zset_add_update_score` | 同 member 不同 score → 更新，长度不变 |
| `test_zset_add_same_score` | 同 member 同 score → 无操作 |
| `test_zset_del` | `zsetDel` 删除 middle/nonexistent |
| `test_zset_del_all` | 逐一删除全部，length 归零 |
| `test_zset_count` | `zsetCount` 区间计数 |
| `test_zset_range` | `zsetRange` 区间查询 |
| `test_zset_del_range` | `zsetDelRange` 后 dict 和 skiplist 同步删除 |
| `test_zset_by_rank` | `zsetByRank` 正查/越界 |
| `test_zset_rank_nonexist` | 不存在 member 的 rank 返回 0 |
| `test_zset_rank_after_update` | 更新 score 后 rank 正确变化（dict+skiplist 同步） |

---

## 本次测试发现的 Bug

### `zskiplist.c:87` — `zslRankScore` 边界条件错误

**问题**：`(!exclusive && ...)` 写成了 `(exclusive && ...)` 的相反语义。

**影响**：`ZCOUNT` 和 `ZRANGE BYSCORE` 在区间边界值上会漏算元素。

**示例**：对包含 score 0,10,...,90 的跳表执行 `ZCOUNT 0 90`，返回 8 而不是 10。

**修复**：将 `!exclusive` 改为 `exclusive`。

---

## 编译警告

- 零 warning（`-Wall -Wextra`）。`dictRehashStep` 保留作为设计决策参考，标记 `__attribute__((unused))` 抑制警告。

## 内存安全

- ✅ ASan (AddressSanitizer) + UBSan (UndefinedBehaviorSanitizer) 验证通过
- 76 个测试用例全部通过，0 内存错误，0 未定义行为
- 编译参数：`-fsanitize=address -fsanitize=undefined`
