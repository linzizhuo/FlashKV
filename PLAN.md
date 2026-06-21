# FlashKV 项目计划

> 轻量级内存 KV 存储，C 实现，参考 Redis 架构。

**最后更新**: 2026-06-21

---

## 一、当前状态

### 已完成模块

| 模块 | 文件 | 状态 |
|------|------|------|
| **SDS** 动态字符串 | `src/sds.c/h` | ✅ 柔性数组 + MurmurHash2 |
| **Dict** 哈希表 | `src/dict.c/h` | ✅ 双表渐进式 rehash + 空桶跳过 + 自动缩容 |
| **DictType** 虚表 | `src/dict_type.c/h` | ✅ key/value 生命周期解耦 |
| **KVDB** 存储封装 | `src/kvdb.c/h` | ✅ 主 dict + expires dict + 惰性删除 (deep module) |
| **ValObj** 值包装 | `src/val_obj.h` | ✅ STRING/INT union + 类型枚举 |
| **Zskiplist** 跳表 | `src/zskiplist.c/h` | ✅ 概率平衡 + span 排名 + 范围查询，~300 行 |
| **ZSet** 有序集合 | `src/zset.c/h` | ✅ dict (member→node, O(1)) + skiplist (score 排序, O(log N))，Redis 同款双索引 |
| **RESP** 协议解析 | `src/resp.c/h` | ✅ 零拷贝递归下降，5 种类型，流式友好 |
| **Service** 服务层 | `src/service.c/h` | ✅ bsearch 命令表 + addReply 追加模式 pipeline |
| **Server** 网络层 | `src/server.c/h` | ✅ epoll 单线程 Reactor |
| **Log** 日志 | `src/log.c/h` | ✅ 级别控制 |
| **TTL** 过期 | `src/ttl.h` | ✅ 惰性删除 + `kvdbActiveExpireCycle` |

### 已实现命令 (21 个)

`PING` `SELECT` `SET` `GET` `DEL` `EXISTS` `EXPIRE` `PEXPIRE` `EXPIREAT` `PEXPIREAT` `TTL` `PTTL` `PERSIST` `ZADD` `ZCARD` `ZRANK` `ZSCORE` `ZRANGE` `ZREM` `ZCOUNT` `ZREMRANGEBYSCORE`

### 测试与工具

| 工具 | 说明 |
|------|------|
| `tests/test_dict.c` | Dict 单元测试 |
| `tests/test_resp.c` | RESP 协议测试 |
| `tests/test_sds.c` | SDS 单元测试 |
| `tests/bench_dict.c` | Dict 微基准 (延迟分布 + rehash 对比) |
| `tests/bench_server.c` | 服务端 TCP 压测 (独立二进制，零 FlashKV 依赖) |
| `scripts/bench_sparse.sh` | 紧凑表 vs 稀疏表 集成压测脚本 |

---

## 二、下一步计划

### 功能侧

| 优先级 | 模块 | 代码量 | 说明 |
|--------|------|--------|------|
| 1 | **INCR / DECR** | ~30 行 | VAL_INT 直接自增，零堆分配 |
| 2 | **MSET / MGET** | ~120 行 | 批量操作 + 原子语义 |
| 3 | **定期过期 (active expire)** | ~100 行 | 定时抽样扫描，CPU 时间预算 |
| 4 | **RDB 持久化** | ~400 行 | 全量快照、序列化格式 |

### 质量侧

| 优先级 | 方向 | 说明 |
|--------|------|------|
| 1 | **dictRehashData 空桶上限** | 防极稀疏表下 `while` 循环耗时不可控 |
| 2 | ~~dictShrink 手动缩容~~ | ~~稀疏表场景给用户控制权回收 TLB/cache~~ → ✅ 自动轮询缩容 (100ms cron, 填充率 <10%) |
| 3 | **jemalloc 对比** | glibc malloc vs jemalloc 性能差异 |
| 4 | **valgrind / ASan** | 内存泄漏 + 越界检测 |
| 5 | **定期过期完整实现** | 补全 `kvdbActiveExpireCycle` 抽样逻辑 |

---

## 三、架构

```
Client ──→ epoll 事件循环 ──→ RESP 解析 (零拷贝) ──→ processCommand (bsearch)
                ↕                    ↕                       ↕
        Connection 读/写缓冲区   RespObj 栈上数组      addReply* 追加模式
                                                              ↕
                                                         kvdb (deep module)
                                                              ↕
                                                    ┌─────────┴─────────┐
                                                    ▼                   ▼
                                              dict (主存储)        dict (expires)
                                                    │                   │
                                              ValObj / SDS          time_t inline
```

### 分层

| 层 | 职责 | 关键设计 |
|----|------|---------|
| **网络层** | epoll Reactor, 非阻塞 TCP, 连接管理 | Connection 状态机 + 读写缓冲区 |
| **协议层** | RESP 解析/序列化 | 零拷贝递归下降, addReply 追加模式 pipeline |
| **服务层** | 命令路由 + 参数校验 + 业务逻辑 | bsearch 二分命令表, RespObj 零拷贝传参 |
| **存储层** | kvdb → dict + expires + 惰性删除 | deep module (7 方法), key 所有权内管 |
| **基础层** | SDS, log, ValObj | 柔性数组, MurmurHash2, union 值存储 |

### 关键设计决策

1. **C 而非 C++** — 保持代码简洁，数据结构手动管理，避免模板膨胀
2. **kvdb 作为 deep module** — 外部只需 7 个方法，不知道内部双表细节
3. **addReply 追加模式** — 单次 handleRead 处理多条 pipeline 命令，一次写回
4. **空桶跳过 rehash** — `dictRehashData` 每次搬迁保证进度，rehash 更快完成
5. **RESP_INT → VAL_INT 快速路径** — 整数值省去 SDS 堆分配
6. **自动轮询缩容** — 100ms cron 检查填充率 <10% 触发，与 Redis 策略一致；搬迁仍渐进完成不阻塞
7. **ZSet 用跳表而非红黑树/B+树** — 详见下方 [跳表选择理由](#跳表选择理由)

### 跳表选择理由

ZSet 需要一个"有序 + 范围扫描 + O(log N) 排名"的数据结构。候选：红黑树、线索红黑树、B+ 树、跳表。

| 维度 | 跳表 | 红黑树 | 线索红黑树 | B+ 树 |
|------|------|--------|-----------|-------|
| 代码复杂度 | **低** | 高 | 极高 | 高 |
| ZRANK | span 天然支持 | 需额外 size 字段 | 需额外 size 字段 | 需额外计数 |
| 范围查询 | L0 顺扫 | 中序遍历 ❌ | 线索顺扫 ✅ | 叶子链表顺扫 ✅ |
| 内存效率 | 每节点 ~1.33 指针 | 每节点 3 指针 | 每节点 5~6 字段 | ~50% 空闲槽位 |
| 再平衡代价 | 仅动插入节点（概率） | 旋转+变色（级联） | 旋转+变色+修线索（级联） | 分裂/合并（级联） |
| 优化目标 | 通用内存结构 | 通用查找 | 让红黑树能做遍历 | 磁盘 I/O 最小化 |

**结论：**

- **红黑树** — 实现复杂度高，范围查询靠中序遍历不自然，ZRANK 要额外维护子树 size。antirez 原话："写一个 bug-free 的红黑树太难了，跳表几乎没有 bug 的空间。"
- **线索红黑树** — 解决了范围查询，但旋转时维护线索 + size 是指针操作噩梦，节点膨胀到 5~6 个元数据字段。
- **B+ 树** — 为磁盘 I/O 优化（扇出大、树矮），内存场景下节点分裂/合并开销大，内部节点 ~50% 空闲槽位是纯浪费。Redis 单线程也让它"分段加锁"的并发优势无意义。
- **跳表** — 概率平衡（p=0.25）、每节点仅动自己、span 让 ZRANK 天然 O(log N)。不是"学术最优"，但在单线程内存 K/V 的排序+排名+范围扫描场景下，工程上最合适。

### 跳表结构设计细节

**为什么所有层塞进一个节点，而非竖向链表（B+ 树式拆分）？**

`forward` 跳到新节点后，`while` 条件要立即比 score。竖向链表里 `forward` 指向的只是某一层的索引片段，score 存底层数据节点，得沿 `down` 追到底才能拿到——搜索最热路径凭空多 2~3 次指针追逐。数组写法 `forward` 落地即整个节点，score 当场可读，降层只是 `i--`，零额外解引用。

拆分为独立索引节点也省不了内存：高度 3 的节点数组写法 72 字节，拆分后三个独立 malloc（数据+两层索引）结构体总和 80 字节，加上每次 malloc 自带的 16~32 字节元数据反而更胖。且拆分后索引节点必须存 score 副本，否则搜索时还是要 `down` 到底。

**span 为什么必要？**

没有 span，ZRANK 只能从 L0 头开始一个一个数——O(N)。span 把排名的 O(N) 计数摊进搜索路径：搜索过程每走一步顺路累加 span，找到目标时 rank 已算好，O(log N)。插入/删除时顺手维护，查询零额外代价。

**backward 为什么只在 L0？**

只有 L0 是完整数据链表。backward 把 L0 串成双向链表，`ZREVRANGE` 从 tail 往回扫 O(k)，不用从 head 重新搜。高层不需要 backward，因为高层只做索引加速，不参与全量遍历。

**zset 抽象层：dict + skiplist 双索引**

跳表按 `(score, ele)` 排序，只知道 member 不知道 score 时无法高效查找。为此在跳表之上封装了 `zset` 模块（`src/zset.c/h`），与 Redis `t_zset` 设计一致：

```
zset {
    dict       (member → zskiplistNode*)  → O(1)  member 去重/查找
    zskiplist  (按 score 排序)             → O(log N) 排名/范围
}
```

dict 的 keyFree/valFree 均为 NULL，skiplist 统一持有 sds 和 node 内存。释放顺序必须先摘 dict entry 再 free node。

---

## 四、压测基线 (2026-06-21)

### 紧凑表 vs 稀疏表

| 场景 | GET 吞吐 | GET P50 | SET 吞吐 | SET P50 |
|------|---------|---------|---------|---------|
| 紧凑表 (1M slots, 1M keys) | 26,285/s | 17.0 μs | 25,497/s | 31.9 μs |
| 稀疏表 (2M slots, 1M keys) | 23,594/s | 35.1 μs | 23,268/s | 34.7 μs |
| **退化** | **−10.2%** | **+106%** | **−8.7%** | **+8.8%** |

### Pipeline 批量写回

| pipeline | 吞吐 | vs p=1 |
|----------|------|--------|
| 1 | 19,057/s | — |
| 4 | 60,524/s | +218% |
| 8 | 79,906/s | +319% |
| 16 | 127,335/s | +568% |
| 32 | 171,231/s | +798% |
| **64** | **220,665/s** | **+1,058%** |

> 关键修复：服务端 `connNew` 中加 `TCP_NODELAY` 消除 Nagle 算法导致的 ~40ms 小包延迟。
> `MAX_PIPELINE_BATCH=4` 限批 + `handleWrite→handleRead` 尾递归，保证 wbuf 不无限增长。

详见 [`docs/benchmark-sparse-vs-compact.md`](docs/benchmark-sparse-vs-compact.md)

---

## 五、开发日志

| 日期 | 内容 |
|------|------|
| 2026-06-21 | 自动轮询缩容：dictNeedsResize → kvdbTryResize → databasesCron，填充率 <10% 触发 |
| 2026-06-21 | 服务端压测：紧凑表 vs 稀疏表对比，量化不缩容代价 |
| 2026-06-21 | addReply 追加模式 + pipeline 响应缓冲 |
| 2026-06-20 | kvdb 重构：收敛 dict/expires/惰性删除为 deep module |
| 2026-06-19 | Dict 渐进式 rehash 微基准：空桶跳过 vs 全量搬迁 |
| 2026-06-17 | RESP 解析器完成，零拷贝递归下降 |
| 2026-06-16 | 服务层初版：命令表 + processCommand |
| 2026-06-14 | Dict 哈希表实现：双表 + 渐进式 rehash |
| 2026-06-07 | 项目初始化：SDS + 基础框架 |
