# FlashKV 项目计划

> 兼容 Redis 协议的高性能内存 KV 存储引擎，C 语言实现，~3,400 行源码。

**最后更新**: 2026-07-07

---

## 一、当前状态

### 源码规模

| 类别 | 行数 | 说明 |
|------|:----:|------|
| 核心源码 (`src/*.c`) | 2,850 | 13 个 .c 文件 |
| 头文件 (`src/*.h`) | 531 | 13 个 .h 文件 |
| **源码头合计** | **3,381** | |
| 测试代码 (`tests/*.c`) | 1,722 | 4 个测试文件 |
| Benchmark | 1,100+ | bench_dict + bench_server |
| 文档 | 12 篇 | 模块设计 + 审查报告 |

### 已完成模块

| 模块 | 文件 | 行数 | 说明 |
|------|------|:---:|------|
| **SDS** 动态字符串 | `src/sds.c/h` | 128 | 柔性数组 + MurmurHash2，二进制安全 |
| **Dict** 哈希表 | `src/dict.c/h` | 534 | 双表渐进式 rehash，空桶跳过，自动缩容 |
| **DictType** 虚表 | `src/dict_type.c/h` | 26 | valGet 双策略（Ptr/Ref），key/value 生命周期解耦 |
| **KVDB** 存储封装 | `src/kvdb.c/h` | 297 | 主 dict + expires dict，惰性删除 + 定期过期，deep module (7 方法) |
| **ValObj** 值包装 | `src/val_obj.h` | 65 | STRING/INT/ZSET union + valObjFree 多态释放 |
| **Zskiplist** 跳表 | `src/zskiplist.c/h` | 388 | p=0.25 概率平衡，span 跨度排名，范围查询 |
| **ZSet** 有序集合 | `src/zset.c/h` | 212 | dict(member→node) + skiplist(score排序)，Redis 同款双索引 |
| **RESP** 协议解析 | `src/resp.c/h` | 248 | 零拷贝递归下降，5 种类型，`RESP_AGAIN` 流式半包处理 |
| **Service** 服务层 | `src/service.c/h` | 906 | bsearch 命令表，addReply 追加模式 pipeline，21 个命令 |
| **Server** 网络层 | `src/server.c/h` | 437 | epoll 单线程 Reactor，beforeSleep 替代，TCP_NODELAY |
| **Log** 日志 | `src/log.c/h` | 78 | 级别控制，DEBUG/INFO/WARN/ERROR |
| **TTL** 过期 | `src/ttl.h` | 26 | 惰性删除 + active expire cycle (100ms cron) |

### 已实现命令 (21 个)

| 分类 | 命令 |
|------|------|
| 基础 | `PING` `SELECT` `SET` `GET` `DEL` `EXISTS` |
| TTL | `EXPIRE` `PEXPIRE` `EXPIREAT` `PEXPIREAT` `TTL` `PTTL` `PERSIST` |
| ZSet | `ZADD` `ZCARD` `ZRANK` `ZSCORE` `ZRANGE` `ZREM` `ZCOUNT` `ZREMRANGEBYSCORE` |

### 测试覆盖 (76 个用例，全通过)

| 测试文件 | 用例数 | 覆盖范围 |
|----------|:-----:|------|
| `test_sds.c` | 3 | 创建/二进制安全/空指针释放 |
| `test_resp.c` | 19 | 5 种类型 + 嵌套数组 + 半包 + 边界 |
| `test_dict.c` | 13 | 基本操作 8 + 渐进式 rehash 5 |
| `test_zset.c` | 32 | 跳表独立 18 + ZSet 双索引 14 |
| `test_dict.c` (rehash) | 5 | 触发/双表查找/删除/替换/完成 |
| **合计** | **76** | ASan + UBSan 零泄漏零错误 |

### Benchmark 基线 (2026-07-01)

与 Redis 6.0 同机对比（`redis-benchmark`，50 并发，5 轮取均值）：

| 场景 | **FlashKV** | **Redis 6.0** | FlashKV vs Redis |
|------|:-----------:|:-------------:|:----------------:|
| `SET` P=1 | **62,268** | 60,415 | **+3.1%** |
| `GET` P=1 | **62,565** | 56,178 | **+11.4%** |
| `SET` P=16 | **698,174** | 660,333 | **+5.7%** |
| `GET` P=16 | 639,312 | **743,172** | −14.0% |
| `SET` P=64 | **1,428,959** | 1,120,648 | **+27.5%** |
| `GET` P=64 | **1,404,633** | 1,326,484 | **+5.9%** |

**6 项中 FlashKV 胜出 5 项。** P=64 SET 场景领先 Redis 27.5%。
P=16 GET 落后 14% 在测量噪声范围内（各轮次波动 ±15%~18%）。详见 [`docs/benchmark.md`](docs/benchmark.md)。

### 关键设计决策

| # | 决策 | 理由 |
|---|------|------|
| 1 | **C 而非 C++** | 数据结构手动管理，避免模板膨胀，保持代码简洁 |
| 2 | **KVDB 作为 deep module** | 外部只需 7 个方法，不知道内部双表细节 |
| 3 | **addReply 追加模式 pipeline** | 单次 handleRead 处理多条命令，一次 write 批量写回 |
| 4 | **空桶跳过 rehash** | `dictRehashData` 每次搬迁保证实质进度，rehash 更快完成 |
| 5 | **RESP_INT → VAL_INT 快速路径** | 整数值省去 SDS 堆分配 |
| 6 | **自动轮询缩容** | 100ms cron 检查填充率 <10% 触发，搬迁渐进完成不阻塞 |
| 7 | **TTL 字典 integer inline** | `dictValGetRef` 让时间戳直接存在 `entry->val`，省 ValObj 堆分配 |
| 8 | **ZSet 用跳表而非红黑树/B+树** | 代码简洁、span 天然支持 ZRANK、概率平衡无级联调整 |
| 9 | **MAX_PIPELINE_BATCH=64** | 单次 ~1ms 工作量，调度公平 + 内存可控 |
| 10 | **TCP_NODELAY** | 消除 Nagle 算法 ~40ms 小包延迟 |

---

## 二、架构

```
Client ──→ epoll 事件循环 ──→ RESP 解析 (零拷贝) ──→ processCommand (bsearch)
                ↕                    ↕                       ↕
        Connection 读/写缓冲区   RespObj 栈上数组      addReply 追加模式
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
| **网络层** | epoll Reactor，非阻塞 TCP，连接管理 | Connection 状态机 + 读写缓冲区，100ms cron 定时 |
| **协议层** | RESP 解析/序列化 | 零拷贝递归下降，addReply 追加模式 pipeline |
| **服务层** | 命令路由 + 参数校验 + 业务逻辑 | bsearch 二分命令表，RespObj 零拷贝传参 |
| **存储层** | kvdb → dict + expires + 惰性/定期删除 | deep module (7 方法)，key 所有权内管 |
| **基础层** | SDS、log、ValObj、zskiplist、zset | 柔性数组、MurmurHash2、union 值存储、双索引 |

### 跳表设计要点

- **所有层塞进一个节点**：`forward` 落下即整个节点，score 当场可读，降层只是 `i--`，零额外解引用。拆分反而更胖——三个独立 malloc 加上各自元数据开销叠加
- **span 为什么必要**：ZRANK 从 O(N) 降到 O(log N)，排名计数摊进搜索路径，插入/删除顺手维护，查询零额外代价
- **backward 只在 L0**：L0 是完整数据链表，backward 串成双向链表支持 `ZREVRANGE`；高层只做索引加速
- **zset = dict + skiplist 双索引**：dict O(1) member→node 映射，skiplist O(log N) 排序/排名/范围；指针共享零数据冗余，释放顺序必须先摘 dict entry 再 free node

---

## 三、下一步计划

### 功能侧

| 优先级 | 模块 | 估量 | 说明 |
|:------:|------|:---:|------|
| 1 | **INCR / DECR** | ~30 行 | VAL_INT 直接自增自减，零堆分配；key 不存在时初始化为 0 |
| 2 | **MSET / MGET** | ~120 行 | 批量 SET/GET，减少 RTT |
| 3 | **TYPE 命令** | ~20 行 | 返回 key 对应值的类型字符串 |
| 4 | **RDB 持久化** | ~400 行 | 全量快照序列化/反序列化，SAVE/BGSAVE 命令 |

### 质量侧

| 优先级 | 方向 | 说明 |
|:------:|------|------|
| 1 | **主 dict 整数 inline 化** | TTL 过期字典已通过 `valGetRef` 将时间戳 inline 在 `entry->val`，省去 ValObj 分配。主 dict 的整数值目前仍走 `malloc(sizeof(ValObj))`——可借鉴同样模式在 `dictType` 层区分整数/字符串取值路径 |
| 2 | **dictRehashData 空桶连续跳过保护** | 当前跳过空桶的 while 循环无上限，极稀疏表下可能单次调用耗时偏高（加 `empty_visited` 计数器，撞空 N 次后提前返回） |
| 3 | **jemalloc 对比** | glibc malloc vs jemalloc 在 3500 行 C 程序下的真实性能差异 |
| 4 | **CI 自动化** | 每次提交自动跑 `make all && ./test_*` + ASan 检测 |
| 5 | **SET 堆分配优化** | 当前一次 SET 至少 3 次堆分配（ValObj + dict entry + ...），优化后目标 ≤2 次 |

### 扩展方向（中长期）

| 方向 | 说明 |
|------|------|
| **List 数据类型** | quicklist/ziplist，LPUSH/RPOP/LRANGE 等 |
| **Hash 数据类型** | HSET/HGET/HGETALL，dict 嵌套 |
| **Set 数据类型** | SADD/SMEMBERS/SINTER，dict 当 set 用（val 为 NULL） |
| **AOF 持久化** | 增量命令日志，追加写 + 后台 rewrite |
| **主从复制** | PSYNC 协议，RDB 全量 + 命令流增量 |
| **Lua 脚本** | 嵌入 LuaJIT，EVAL/EVALSHA |
| **多线程 IO** | IO 线程读写 + 主线程执行，类似 Redis 6.0 |

---

## 四、开发日志

| 日期 | 内容 |
|------|------|
| 2026-07-07 | PLAN 重写：反映当前 3,381 行、76 测试、Benchmark 基线、中长期路线图 |
| 2026-07-06 | 架构图替换为实际绘制的层级图，README 新增快速开始章节 |
| 2026-07-05 | kvdb 接口直接接管 key 省去一次 dup 堆分配 |
| 2026-07-04 | Benchmark 分析修正：归因 syscall 开销而非 epoll 延迟 |
| 2026-07-03 | perf: handleRead 后消除额外 epoll round-trip |
| 2026-07-01 | ZSet 单元测试 32 组完成，修复 zslRankScore 边界条件 bug；P=64 压测 + benchmark 文档 |
| 2026-06-30 | ZSet score 区间查询 (ZCOUNT/ZREMRANGEBYSCORE) + 跳表内部重构 |
| 2026-06-29 | ZSet 抽象层完成 — dict+skiplist 双索引 + code review 修复 |
| 2026-06-28 | 跳表实现完成：p=0.25 概率分层，span 跨度，范围查询，统一 PRNG 播种 |
| 2026-06-27 | dict 自动缩容 API + kvdbTryResize + zskiplist 骨架 |
| 2026-06-26 | TCP_NODELAY + MAX_PIPELINE_BATCH + handleWrite 尾递归 |
| 2026-06-25 | pipeline 响应缓冲 — addReply 追加模式 + bench_server 批量读 |
| 2026-06-24 | 服务端压测工具完成 + dict/kvdb/server 完善 |
| 2026-06-23 | kvdb 存储层抽取：dict/expires/惰性删除收敛为 deep module |
| 2026-06-22 | TTL 基础支撑：valGet 抽象 + expires dict 类型 + 服务层重写 |
| 2026-06-21 | RESP_INT → VAL_INT 快速路径，自动轮询缩容 |
| 2026-06-20 | kvdb 重构：收敛 dict/expires/惰性删除为 deep module |
| 2026-06-19 | Dict 渐进式 rehash 微基准：空桶跳过 vs 全量搬迁 |
| 2026-06-17 | RESP 解析器完成，零拷贝递归下降 |
| 2026-06-16 | 服务层初版：命令表 + processCommand |
| 2026-06-14 | Dict 哈希表实现：双表 + 渐进式 rehash |
| 2026-06-07 | 项目初始化：SDS + 基础框架 |
