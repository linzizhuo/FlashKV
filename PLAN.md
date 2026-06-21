# FlashKV 项目计划

> 轻量级内存 KV 存储，C 实现，参考 Redis 架构。

**最后更新**: 2026-06-21

---

## 一、当前状态

### 已完成模块

| 模块 | 文件 | 状态 |
|------|------|------|
| **SDS** 动态字符串 | `src/sds.c/h` | ✅ 柔性数组 + MurmurHash2 |
| **Dict** 哈希表 | `src/dict.c/h` | ✅ 双表渐进式 rehash + 空桶跳过 |
| **DictType** 虚表 | `src/dict_type.c/h` | ✅ key/value 生命周期解耦 |
| **KVDB** 存储封装 | `src/kvdb.c/h` | ✅ 主 dict + expires dict + 惰性删除 (deep module) |
| **ValObj** 值包装 | `src/val_obj.h` | ✅ STRING/INT union + 类型枚举 |
| **RESP** 协议解析 | `src/resp.c/h` | ✅ 零拷贝递归下降，5 种类型，流式友好 |
| **Service** 服务层 | `src/service.c/h` | ✅ bsearch 命令表 + addReply 追加模式 pipeline |
| **Server** 网络层 | `src/server.c/h` | ✅ epoll 单线程 Reactor |
| **Log** 日志 | `src/log.c/h` | ✅ 级别控制 |
| **TTL** 过期 | `src/ttl.h` | ✅ 惰性删除 + `kvdbActiveExpireCycle` |

### 已实现命令 (13 个)

`PING` `SELECT` `SET` `GET` `DEL` `EXISTS` `EXPIRE` `PEXPIRE` `EXPIREAT` `PEXPIREAT` `TTL` `PTTL` `PERSIST`

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
| 4 | **ZSet 跳表** | ~500 行 | 多级索引、概率平衡、ZRANK 跨度计算 |
| 5 | **RDB 持久化** | ~400 行 | 全量快照、序列化格式 |

### 质量侧

| 优先级 | 方向 | 说明 |
|--------|------|------|
| 1 | **dictRehashData 空桶上限** | 防极稀疏表下 `while` 循环耗时不可控 |
| 2 | **dictShrink 手动缩容** | 稀疏表场景给用户控制权回收 TLB/cache |
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
6. **不缩容** — 与 Redis 默认一致，避免反复扩缩容的 CPU 抖动

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
| 2026-06-21 | 服务端压测：紧凑表 vs 稀疏表对比，量化不缩容代价 |
| 2026-06-21 | addReply 追加模式 + pipeline 响应缓冲 |
| 2026-06-20 | kvdb 重构：收敛 dict/expires/惰性删除为 deep module |
| 2026-06-19 | Dict 渐进式 rehash 微基准：空桶跳过 vs 全量搬迁 |
| 2026-06-17 | RESP 解析器完成，零拷贝递归下降 |
| 2026-06-16 | 服务层初版：命令表 + processCommand |
| 2026-06-14 | Dict 哈希表实现：双表 + 渐进式 rehash |
| 2026-06-07 | 项目初始化：SDS + 基础框架 |
