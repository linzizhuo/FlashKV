# FlashKV — 兼容 Redis 协议的轻量级内存 KV 存储引擎

![Architecture](./images/Architecture.png)

**~5,600 行 C 语言**，从零手写核心数据结构（跳表、哈希表、SDS 动态字符串、RESP 协议解析），
实现了一个**兼容 Redis 有线协议**的高性能内存键值存储系统。

最新 `redis-benchmark` 纯内存压测（100 万请求, 50 并发, 5 轮均值）：
**SET 80,302 ops/s、GET 80,417 ops/s，分别领先 Redis 7.0.15 约 7% 和 10%，且运行更稳定。**
详见 [Benchmark 报告](docs/benchmark.md)。

---

## 🚀 快速开始

```bash
# 构建
make all

# 启动服务器
./flashkv

# 另一个终端，用 redis-cli 操作（协议兼容！）
redis-cli ping
redis-cli set msg "hello flashkv"
redis-cli get msg
```

---

## 📊 Benchmark 总览

测试环境：Linux 6.17 · GCC -O2 · 单机 loopback · 双方均关闭持久化  
测试工具：`redis-benchmark`（50 并发, 100 万请求/轮, 5 轮取均值）

| 测试 | **FlashKV** | **Redis 7.0.15** | 差异 | FlashKV 波动 |
|------|:-----------:|:----------------:|:----:|:------------:|
| SET | **80,302** ops/s | 74,893 ops/s | **+7.2%** | ±337 |
| GET | **80,417** ops/s | 73,294 ops/s | **+9.7%** | ±793 |

**FlashKV 在纯内存 SET/GET 场景全面反超 Redis 7.0，且波动仅为 Redis 的 1/5~1/6。**
详细数据及历史对比见 [Benchmark 报告](docs/benchmark.md)。

---

## 🏗 系统架构

FlashKV 采用**分层设计**，自底向上共 4 层：

![alt text](./images/System%20Architecture.png)


---

## ⚙ 核心模块详解

### 1️⃣ 跳表 (zskiplist) — 概率平衡的排序索引

- **p=0.25** 概率分层（与 Redis 一致），层数期望值低，内存更紧凑
- **span 跨度**机制：插入/删除时 O(log N) 维护 span，`ZRANK` 直接在搜索路径上累加跨度即可得到排名，零额外成本
- **头节点 64 层**（宏 `ZSKIPLIST_MAXLEVEL`），避免动态高度调整

### 2️⃣ 哈希表 (dict) — 双表渐进式 rehash

- **渐进式 rehash**：扩容时不阻塞服务，将搬迁开销均摊到每次操作上
- **两步策略**：
  - `dictRehashStep` — 搬指定槽位数（含空桶），适合批量预加载
  - `dictRehashData` — 搬指定**非空桶**数，跳过空桶，保证每次调用都有实质进度
- **空桶跳过收益**：减少 46.7% 无效搬迁调用，rehash 在 `used` 次操作内必定完成（而非 `size` 次）
- 在使用过程中，dictRehashData相比于dictRehashStep性能波动更平缓，且可以更快完成rehash
- **dictType 虚函数表**：`hash`、`keyCompare`、`free` 以及 **`valGet` 取值策略**，实现类型与容器解耦
- **`valGet` 双策略**：
  - `dictValGetPtr` — 返回 `entry->val`（存的是指针，指向堆上对象），`dictTypeSds` 用
  - `dictValGetRef` — 返回 `&entry->val`（值**直接 inline** 在 entry 的 val 字段里，零堆分配），`dictTTL` 用——时间戳直接 `(void*)when` 存入，无需 ValObj 包装
- **inline 前提**：64 位平台下 `sizeof(long long) == sizeof(void*) == 8`，指针字段可以直接存整数

**延迟分布对比（100K SET 操作）：**

| 策略 | P50 | P99 | Max | σ |
|------|:---:|:---:|:---:|:---:|
| 渐进式 rehash | 221 ns | 2,739 ns | **356 μs** | **2,079 ns** |
| 全量式 rehash（一次性搬完） | 122 ns | 606 ns | 1,675 μs | 7,220 ns |

渐进式将 **1.6ms 的周期性毛刺平摊为每次 ~300ns 的均匀开销**，尾部延迟降低 4.7 倍，抖动降低 3.5 倍。

### 3️⃣ ZSet — dict + skiplist 双索引

- **dict**（member → node）：O(1) 按 member 查询/删除
- **skiplist**（score 排序）：O(log N) 范围查询、排名计算
- 与 Redis 设计一致，成员指针共享，零数据冗余

### 4️⃣ RESP 协议解析器 — 零拷贝递归下降

- 支持 5 种类型：简单字符串 (`+`)、错误 (`-`)、整数 (`:`)、Bulk String (`$`)、数组 (`*`)
- **`RESP_AGAIN`** 半包返回：流式 socket 一次 `read` 可能只拿到半条命令，解析器保留缓冲区状态等待下一轮
- 深度限制 `MAX_PARSE_DEPTH` 防止恶意嵌套
- 整数优化：`RESP_INT` 协议直达 `VAL_INT` 存储，省去 SDS 堆分配

### 5️⃣ 整数优化路径

RESP 协议有原生整数类型 `:100\r\n`。FlashKV 在此路径上做了两层优化：

**第一层：RESP_INT 解析直达 `long long`，避开 SDS**

```
Client  :100\r\n                   ← RESP 整数类型
           ↓
RESP 解析 → integer = 100          ← 解析出 long long，无需字符串
```

对比走普通字符串：

```
Client  $3\r\n100\r\n              ← RESP bulk string
           ↓
RESP 解析 → SDS "100"              ← 堆分配
```

**第二层（TTL 已实现）：利用 `entry->val` 直接 inline 存整数**

64 位平台下 `sizeof(void*) == sizeof(long long)`，所以 `entry->val` 这个指针字段可以直接当整数用。
算是一次特定场景下的优化，但同时也存在局限性，这代表了当前项目不能无缝移植到32位系统上。
```c
// src/dict.c
struct dictEntry {
    hash_t hash;
    void *key;
    void *val;    // ← long long 也能放下（8 字节）
    struct dictEntry *next;
};
```

结合 `dictValGetRef`（返回 `&entry->val`），数值以 `(long long)(intptr_t)val` 的形式**直接 inline 在 entry 里，无需 ValObj 包装**：

```
Dict entry → entry->val = (void*)when    ← 无 malloc，无 ValObj
```

**TTL 过期字典**已经用了这个模式（`dictTTL` 用 `valGetRef`，时间戳直接以 `(void*)` 存入），每设置一条 TTL 就省一个 ValObj 的堆分配。

而**主 dict 的值目前仍走 `ValObj`**——SET 整数时 `service.c:213` 依然 `malloc(sizeof(ValObj))`，整数存在 `ValObj->val.ll` 里。这是因为主 dict 要求统一的值读写接口，直接 inline 还需要在 `dictType` 层区分整数和字符串的取值路径，属于待做优化。

### 6️⃣ SDS 动态字符串

- **多类型自适应 header**：参照 Redis 设计，按字符串长度自动选择 `sdshdr5/8/16/32/64`，短字符串用 1 字节 header、长字符串用 8 字节，避免"一刀切"的内存浪费
- 柔性数组 (`char buf[]`) 实现，二进制安全（含 `\0` 可正常存储）
- O(1) 长度获取（`len` 字段，而非 `strlen` 遍历）
- 内置 MurmurHash2，用于 dict key 的哈希计算

### 7️⃣ TTL 过期机制

- 独立 `expires` 字典，key 指针共享（零内存冗余）
- **惰性删除**：访问时检查是否过期，过期则删除
- **主动过期（active expire）**：SLOW/FAST 双模式定期抽样删除
  - `ACTIVE_EXPIRE_CYCLE_SLOW` — 100ms cron 定时触发，遍历所有 DB，每次限时 1ms
  - `ACTIVE_EXPIRE_CYCLE_FAST` — beforeSleep 触发，仅在 SLOW 超时后激活，限时更短
- 过期字典 value 为 64 位时间戳 inline（`valGet=dictValGetRef`，`valFree=NULL`），避免堆分配，实际一次TTL只需要一次额外堆分配。

### 8️⃣ 多数据库支持

- 16 个独立 db，`SELECT n` 切换
- 每个 db 独立 dict + expires，DB 间隔离

### 9️⃣ IO 模块 — 零拷贝双缓冲区

- **双缓冲区设计**：`struct Io` 内含一个柔性数组 `buf[buflen * 2]`，前半段为写缓冲、后半段为读缓冲，读写不互相污染
- **零拷贝路径**：调用方通过 `getbufIo` 直接获取写缓冲区指针，序列化直接写入 `buf`，最后 `commitIo` 提交偏移量——无需中间拷贝
- **公用缓冲区，全局一次 malloc**：创建 `Io` 时一次性分配 `sizeof(Io) + buflen*2`，整个序列化过程零额外堆分配
- **流式 flush**：缓冲区满时自动 `write` 到 fd，支持大体积快照而不占用过多内存
- **统一读写 API**：`addIo` 追加写入、`readIo` 缓冲读取、`flushIo` 带 `fdatasync` 刷盘

### 🔟 RDB 持久化 — 全量快照备份与恢复

- **二进制格式**：自定义 RDB 协议，含魔术字 (`FLASHKV`)、版本号、数据库计数、逐 key 序列化
- **类型全覆盖**：支持 `STRING`、`INT`、`ZSET` 三种值类型的序列化与反序列化
- **TTL 完整保留**：每条 key 的过期时间随值一同持久化，恢复后过期语义不变
- **多库支持**：`rdbSaveAll` 写入 `SELECTDB` opcode 分隔不同 DB，`rdbLoad` 完整恢复多库
- **两种持久化命令**：
  - `SAVE` — 阻塞当前线程，同步完成写盘
  - `BGSAVE` — `fork()` 子进程异步写盘（利用 COW），主线程立即返回 `+OK`
- **启动自动加载**：编译选项 `RDB_LOAD_ENABLED` 控制，服务启动时若 `dump.rdb` 存在则自动恢复
- **当前限制**：自动快照策略（类似 Redis `save <seconds> <changes>`）尚未实现，仅支持手动触发 SAVE/BGSAVE

---

## ✅ 命令支持

| 命令 | 参数 | 说明 |
|------|:----:|------|
| `PING` | 0 | 连通性检查 |
| `SELECT n` | 1 | 切换数据库 |
| `SET key val` | 2 | 支持字符串 + 整数（VAL_INT 快速路径） |
| `GET key` | 1 | 返回 bulk string / integer |
| `APPEND key val` | 2 | 追加字符串到已有值末尾 |
| `SETRANGE key offset val` | 3 | 覆盖指定偏移处的子串 |
| `SETBIT key offset bit` | 3 | 设置指定偏移处的 bit 值 |
| `DEL key` | 1 | 同时清除 TTL 记录 |
| `EXISTS key` | 1 | 含惰性过期检查 |
| `EXPIRE / PEXPIRE key sec/ms` | 2 | 秒级/毫秒级相对过期 |
| `EXPIREAT / PEXPIREAT key ts` | 2 | 秒级/毫秒级绝对过期 |
| `TTL / PTTL key` | 1 | 返回剩余生存时间 |
| `PERSIST key` | 1 | 移除过期设置 |
| `ZADD key score member` | 3 | 新增/更新 member |
| `ZCARD key` | 1 | 返回有序集合基数 |
| `ZRANK key member` | 2 | 0-based 排名 |
| `ZSCORE key member` | 2 | 返回 score |
| `ZRANGE key start stop [WITHSCORES]` | 3 | 按 rank 范围取值 |
| `ZREM key member` | 2 | O(1) 删除 member |
| `ZCOUNT key min max` | 3 | score 区间计数 |
| `ZREMRANGEBYSCORE key min max` | 3 | score 区间范围删除 |
| `SAVE` | 0 | 同步持久化全量快照到 `dump.rdb` |
| `BGSAVE` | 0 | `fork` 子进程异步写盘，主线程立即返回 |

---

## 🚀 构建与运行

```bash
# 构建全部目标和测试程序
make all

# 启动服务器（默认 6379 端口）
./flashkv

# 用 redis-cli 连接（兼容！）
redis-cli ping
redis-cli set foo bar
redis-cli get foo

# 或直接通过原生 RESP 协议测试
printf '*3\r\n$3\r\nSET\r\n$3\r\nfoo\r\n:100\r\n' | nc localhost 6379
printf '*2\r\n$3\r\nGET\r\n$3\r\nfoo\r\n'             | nc localhost 6379
# → :100
```

### 运行 Benchmark

```bash
# 安装 Redis（用于 redis-benchmark 和对比测试）
sudo apt install redis-server redis-tools

# 构建 FlashKV
make flashkv

# FlashKV 压测（需先停 Redis 释放 6379）
redis-cli shutdown 2>/dev/null
./flashkv &
redis-benchmark -t set,get -n 1000000 -c 50 --csv

# Redis 对比（停 FlashKV 后启动 Redis）
kill $(pgrep flashkv)
redis-server --save "" --appendonly no --daemonize yes
redis-benchmark -t set,get -n 1000000 -c 50 --csv
```

---

## 📁 项目结构

```
src/
├── main.c           # 入口
├── server.c/.h     # epoll 事件循环 + 连接管理 + beforeSleep
├── service.c/.h    # 命令注册 + 参数分发（25+ 命令）
├── kvdb.c/.h       # 存储引擎封装（7 方法接口）
├── expire.c/.h     # 主动过期引擎（SLOW/FAST 双模式）
├── dict.c/.h       # 哈希表核心 + 渐进式 rehash
├── dict_type.c/.h  # 虚函数表 + 类型实例
├── zskiplist.c/.h  # 跳表（p=0.25, span 跨度）
├── zset.c/.h       # ZSet 抽象层（dict + skiplist 双索引）
├── resp.c/.h       # RESP 协议解析器（零拷贝递归下降）
├── sds.c/.h        # 动态字符串（柔性数组 + MurmurHash2）
├── io.c/.h         # 零拷贝 I/O 模块（双缓冲区 + 流式读写）
├── rdb.c/.h        # RDB 持久化（序列化/反序列化 + SAVE/BGSAVE）
├── log.c/.h        # 日志模块
├── config.h        # 全局配置参数（端口、DB 数、RDB 魔数等）
├── val_obj.h       # 值类型统一包装（STRING/LIST/ZSET/SET/HASH/INT）
├── ttl.h           # TTL 过期接口 + 时间工具
├── object.h        # 对象类型定义
spec/               # 模块语义规约（AI 读）
├── expire.md
contract/           # 形式化契约（YAML，AI 遵守）
├── expire.yaml
notes/              # 推导思路（人写，AI 不读）
tests/
├── test_dict.c     # dict 单元测试
├── test_zset.c     # ZSet 单元测试（32 组）
├── test_resp.c     # RESP 协议测试
├── test_sds.c      # SDS 字符串测试
├── test_rdb.c      # RDB 序列化/反序列化往返测试
├── test_io.c       # IO 模块单元测试
├── bench_dict.c    # dict 微基准（rehash 延迟分布）
├── bench_server.c  # 服务端吞吐 + 延迟基准
docs/
├── benchmark.md                   # Benchmark 报告（最新）
├── benchmark-report.md            # 历史 Benchmark 报告
├── benchmark-sparse-vs-compact.md # 稀疏 vs 紧凑 rehash 对比
├── dict.md                        # Dict 设计文档
├── dict-rehash-review.md          # Dict Rehash 审查
├── zskiplist.md                   # 跳表设计文档
├── epoll-server.md                # 网络层设计文档
├── resp.md                        # RESP 协议解析器文档
├── service-layer.md               # 命令层设计文档
├── ttl.md                         # TTL 过期机制文档
├── io模块设计报告.md               # IO 模块设计报告
├── Database Backup设计报告.md      # RDB 持久化设计报告
├── test-report.md                 # 测试报告
└── code-review-report.md          # 代码审查报告
```

---

## 📖 参考文档

项目包含详细的模块设计文档，每篇阐述设计决策、实现要点和与 Redis 的对照分析：

| 文档 | 内容 |
|------|------|
| [Benchmark 报告](docs/benchmark.md) | 最新纯内存压测 — FlashKV vs Redis 7.0.15, 含 5 轮原始数据 |
| [Dict 哈希表](docs/dict.md) | 渐进式 rehash 两种策略、空桶跳过、内存分配 |
| [Dict Rehash 审查](docs/dict-rehash-review.md) | Code Review 视角的 rehash 安全分析 |
| [跳表 (zskiplist)](docs/zskiplist.md) | p=0.25 选择依据、span 跨度 ZRANK 原理 |
| [RESP 协议解析](docs/resp.md) | 零拷贝递归下降、半包处理、整数优化路径 |
| [epoll 服务器](docs/epoll-server.md) | Reactor 模式、syscall 优化、beforeSleep 变体 |
| [Service 命令层](docs/service-layer.md) | 命令表分发、参数校验、响应组装 |
| [TTL 过期机制](docs/ttl.md) | 惰性删除 + 主动过期 (SLOW/FAST)、时间精度 |
| [IO 模块设计报告](docs/io模块设计报告.md) | 零拷贝双缓冲区、流式序列化、设计权衡 |
| [RDB 持久化设计报告](docs/Database%20Backup设计报告.md) | 二进制协议、序列化/反序列化、多库支持、SAVE/BGSAVE |
| [Expire 语义规约](spec/expire.md) | expire 模块规格，AI 代码生成输入 |
| [Expire 形式化契约](contract/expire.yaml) | 不变量、函数契约、状态机（YAML） |
| [测试报告](docs/test-report.md) | 各模块测试覆盖情况 |
| [Code Review 报告](docs/code-review-report.md) | 索引越界、未初始化、资源泄漏审计 |

---

## 🧪 测试

```bash
# Dict 渐进式 rehash + 过期机制
make test_dict && ./test_dict

# RESP 协议（5 种类型 + 流式半包）
make test_resp && ./test_resp

# ZSet 单元测试（32 组测试用例）
make test_zset && ./test_zset

# SDS 字符串
make test_sds && ./test_sds

# RDB 序列化/反序列化往返测试
make test_rdb && ./test_rdb

# IO 模块单元测试
make test_io && ./test_io
```

---

## 🔜 下一步优化方向

- **RDB 自动快照策略**：类似 Redis `save <seconds> <changes>`，在 cron 中检查 dirty 计数器，满足条件自动触发 BGSAVE。当前仅支持手动 SAVE/BGSAVE。
- **主 dict 整数 inline 化**：TTL 过期字典已通过 `valGetRef` 将时间戳直接 inline 在 `entry->val` 里（零 ValObj），但主 dict 的整数值目前仍走 `malloc(sizeof(ValObj))`。
- **entry + key 融合分配**：key 是不可变定长数据，SDS 的 `alloc`/`flags` 字段（17B header）对 key 是纯浪费。将 key 以 `[4B len][data]` 格式直接嵌入 `dictEntry` 尾部柔性数组，一次 `malloc` 搞定 entry + key，SET 路径从 3~4 次分配降到 2 次，100 万 key 省 ~13MB。expires 字典的 key 指针共享同一块内存，rehash 搬迁不受影响。
---
