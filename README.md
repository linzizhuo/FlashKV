# FlashKV

轻量级内存 KV 存储，参考 Redis 设计。

## 当前进度

### 网络层

- **epoll 服务器** — 单线程事件循环（Reactor 模式）
- **Connection** — 连接对象 + 读/写缓冲区 + 状态机
- **非阻塞 TCP** — 与 Redis 相同的 `epoll_wait + O_NONBLOCK` 模型

### 协议层

- **RESP 解析器** — 零拷贝递归下降解析，5 种类型（`+` / `-` / `:` / `$` / `*`）
- **流式友好** — `RESP_AGAIN` 半包返回，适配非阻塞读
- **深度限制** — `MAX_PARSE_DEPTH` 防恶意嵌套

### 存储引擎

- **kvdb** — 数据库封装层，将 dict 双表 + hash 计算 + 惰性删除 + key 所有权管理收敛为一个 **deep module**（7 个方法）
- **dict** — 哈希表核心：`dictnew` / `dictAdd` / `dictReplace` / `dictfind` / `dictDelete` / `dictfree`
- **dictType** — 虚函数表，支持 `hash`、`keyCompare`、`keyFree`、`valFree`、**`valGet`**（取值策略），实现类型与容器解耦
- **dictTypeSds** — 主存储类型（key→ValObj，`valGet=dictValGetPtr`）
- **dictTTL** — 过期字典类型（key→时间戳 inline，`valGet=dictValGetRef`，`valFree=NULL`）
- **ValObj** — 值统一包装，`enum ValType` + `union`，支持 STRING / LIST / ZSET / SET / HASH / INT
- **渐进式 rehash** — 双表扩容 + `rehashidx` 游标 + 空桶跳过，单次操作 O(1) 均摊
- **整数优化** — `RESP_INT` 协议直达 `VAL_INT` 存储，省去整数值的 SDS 堆分配
- **TTL 过期** — 独立 expires 字典 + 惰性删除 + `hash` 复用

### 基础工具

- **SDS** — 动态字符串（柔性数组 + 二进制安全），含 MurmurHash2
- **log** — 日志模块，级别控制 + stdout/stderr

### 命令

| 命令 | 参数 | 实现 |
|------|------|------|
| `PING` | 0 | ✅ |
| `SELECT n` | 1 | ✅ 多数据库 |
| `SET key val` | 2 | ✅ 字符串 + 整数值 |
| `GET key` | 1 | ✅ bulk string / integer |
| `DEL key` | 1 | ✅ 同时清除 TTL |
| `EXISTS key` | 1 | ✅ 含惰性过期 |
| `EXPIRE key sec` | 2 | ✅ 秒级相对 |
| `PEXPIRE key ms` | 2 | ✅ 毫秒级相对 |
| `EXPIREAT key ts` | 2 | ✅ 秒级绝对 |
| `PEXPIREAT key ts` | 2 | ✅ 毫秒级绝对 |
| `TTL key` | 1 | ✅ -2/-1/剩余秒 |
| `PTTL key` | 1 | ✅ -2/-1/剩余毫秒 |
| `PERSIST key` | 1 | ✅ 移除过期 |

## 测试

```bash
make test_dict && ./test_dict
# ======== 🎉 全部测试通过 ========

make test_resp && ./test_resp
# 协议正确性：简单字符串 / 错误 / 整数 / Bulk / Null / 数组嵌套 / 流式半包
```

## 使用

```bash
# 构建全部
make all

# 启动服务器（默认 6379 端口）
./flashkv

# 原生 RESP 测试（整数 SET/GET）
printf '*3\r\n$3\r\nSET\r\n$3\r\nfoo\r\n:100\r\n' | nc localhost 6379
printf '*2\r\n$3\r\nGET\r\n$3\r\nfoo\r\n'             | nc localhost 6379
# → :100
```

## Dict 设计要点

### 渐进式 rehash

rehash 期间维护两张哈希表 `ht[0]` / `ht[1]`，`rehashidx` 记录搬迁进度。每次读写操作顺带搬 1 个非空桶（`dictRehashData`），将扩容开销均摊到每次请求上，避免单次操作卡顿。

### 空桶跳过

`dictRehashStep` 搬 N 个槽位（含空桶），适合批量预加载；`dictRehashData` 搬 N 个非空桶，跳过空桶保证每次调用都有实际搬迁进度。在稀疏表场景下后者显著减少无效迭代。

### 内存分配

热点路径 `dictEntryNew` 每次插入调 `malloc`。现代分配器（jemalloc）对定长对象有 per-size-class 缓存，相当于在分配器层做了 freelist，无需手写 Slab。编译链 `-ljemalloc` 即可替代 glibc malloc，零代码成本。

## 下一步计划

### 功能侧

| 优先级 | 模块 | 预计代码量 | 技术看点 |
|--------|------|-----------|---------|
| 1 | **INCR / DECR** | ~30 行 | VAL_INT 直接自增，零堆分配 |
| 2 | **MSET / MGET** | ~120 行 | 批量操作均摊 rehash、原子语义 |
| 3 | **定期过期** | ~100 行 | 后台抽样扫描、CPU 时间预算 |
| 4 | **ZSet 跳表** | ~500 行 | 多级索引、概率平衡、`ZRANK` 跨度计算 |
| 5 | **RDB 持久化** | ~400 行 | 全量快照、序列化格式、COW 思想 |

### 质量侧

- **Benchmark 套件** — 与 Redis 同场景对比 SET/GET 吞吐 + rehash 行为
- **jemalloc 对比** — glibc malloc vs jemalloc 性能差异数据
- **冷热路径拆分** — 稳态（不 rehash）走快速路径，`unlikely()` 标注冷分支

---

## 架构

```
Client ──→ epoll 事件循环 ──→ RESP 解析 ──→ 命令分发 ──→ service（协议）
                ↕                    ↕            ↕              ↕
        Connection 读/写缓冲区   RespObj 零拷贝   Command 表    kvdb（存储）
                                                                  ↕
                                                            dict + expires
                                                                  ↕
                                                            ValObj / SDS
```
