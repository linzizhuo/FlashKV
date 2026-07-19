# FlashKV Benchmark 报告

**日期**: 2026-07-19
**测试工具**: `redis-benchmark` (Redis 官方)
**测试环境**: Linux 6.17, GCC -O2, 单机 loopback

---

## 最新结果：纯内存模式（RDB/AOF 全关）

**条件**：100 万请求/轮, 50 并发, 5 轮取均值, 双方均关闭持久化（Redis `--save "" --appendonly no`）

| 测试 | FlashKV | Redis 7.0.15 | 差异 | FlashKV 波动 | Redis 波动 |
|------|---------|-------------|------|-------------|-----------|
| **SET** | **80,302** ops/s | 74,893 ops/s | **+7.2%** | ±337 | ±2,071 |
| **GET** | **80,417** ops/s | 73,294 ops/s | **+9.7%** | ±793 | ±4,036 |

**FlashKV 在纯内存 SET/GET 场景全面反超 Redis 7.0，且运行更稳定。**

### 延迟对比（均值）

| 测试 | 延迟指标 | FlashKV | Redis |
|------|---------|---------|-------|
| SET | avg / P50 / P99 | 0.343ms / 0.311ms / 0.852ms | 0.359ms / 0.322ms / 0.994ms |
| GET | avg / P50 / P99 | 0.344ms / 0.311ms / 0.882ms | 0.368ms / 0.332ms / 1.031ms |

### 5 轮原始数据

**FlashKV**：

| 轮次 | SET (ops/s) | GET (ops/s) |
|------|------------|------------|
| 1 | 80,528 | 79,139 |
| 2 | 80,574 | 80,231 |
| 3 | 79,879 | 80,906 |
| 4 | 79,994 | 81,162 |
| 5 | 80,535 | 80,645 |
| **avg** | **80,302** | **80,417** |

**Redis 7.0.15**：

| 轮次 | SET (ops/s) | GET (ops/s) |
|------|------------|------------|
| 1 | 75,901 | 75,126 |
| 2 | 76,034 | 76,092 |
| 3 | 74,250 | 75,069 |
| 4 | 76,717 | 73,986 |
| 5 | 71,561 | 66,199 |
| **avg** | **74,893** | **73,294** |

> Redis 第 5 轮出现明显衰减（GET 跌至 6.6 万），可能与 jemalloc 碎片整理或内核调度有关，非测试误差。

---

## 说明

FlashKV 是一个精简的 KV 存储引擎，实现了 Redis 协议子集（PING/SET/GET/DEL/EXPIRE/TTL/TYPE），
单线程 epoll 事件循环 + 渐进式 rehash 哈希表。

纯内存场景 FlashKV 反超 Redis 的可能原因：
- **代码路径更短**：FlashKV 没有 cluster、Lua、pub/sub、ACL、module 等子系统，命令处理链路更短
- **数据结构精简**：哈希表实现简洁，无 `robj` 间接层，SDS 直接作为 key/value
- **Redis 7.0 全功能开销**：即使不使用的子系统，其钩子、统计、周期性检查也消耗 CPU

---
## 管道模式（Pipeline）

管道模式下，客户端一次发送多条命令，服务端批量处理后批量返回，减少 RTT。

| 指标 | FlashKV P=1 | FlashKV P=16 | Redis P=1 | Redis P=16 |
|------|------------|-------------|-----------|-----------|
| SET | 73,742 | 1,151,071 (×15.6) | 74,963 | 1,177,653 (×15.7) |
| GET | 71,821 | 1,160,677 (×16.2) | 74,769 | 1,278,627 (×17.1) |

管道 P=16 下吞吐提升 **15~17 倍**，与 Redis 的管道加速比一致。

FlashKV 设置 `MAX_PIPELINE_BATCH=16`，原因：
- **调度公平性**：单次 `handleRead` 处理有限条命令，不影响其他连接的响应
- **内存控制**：防止 wbuf 在未 flush 前膨胀过大
- 该值可根据实际场景调整

---

## 复现方法

```bash
# 1. 安装依赖
sudo apt install redis-server redis-tools

# 2. 构建 FlashKV
make flashkv

# 3. 关闭 Redis（释放 6379 端口）
redis-cli shutdown 2>/dev/null

# 4. 启动 FlashKV
./flashkv &

# 5. 跑 5 轮取均值
for i in 1 2 3 4 5; do
  echo "=== FlashKV run $i ==="
  redis-benchmark -t set,get -n 1000000 -c 50 --csv
  sleep 0.5
done

# 6. 停 FlashKV，启动 Redis（不持久化）
kill $(pgrep flashkv)
redis-server --save "" --appendonly no --daemonize yes

# 7. 同样跑 5 轮
for i in 1 2 3 4 5; do
  echo "=== Redis run $i ==="
  redis-benchmark -t set,get -n 1000000 -c 50 --csv
  sleep 0.5
done
```

---

## 历史数据

### 2026-07-19 (早期 P=1/P=16 数据, 20~50 万请求)

FlashKV 当时略逊于 Redis，差距在 10% 以内：

| 场景 | FlashKV | Redis 7.0 | 差异 |
|------|---------|-----------|:---:|
| SET P=1 | 73,742 | 74,963 | -1.6% |
| GET P=1 | 71,821 | 74,769 | -3.9% |
| SET P=16 | 1,151,071 | 1,177,653 | -2.3% |
| GET P=16 | 1,160,677 | 1,278,627 | -9.2% |

### 2026-07-01 (vs Redis 6.0.16, 腾讯云)

FlashKV 在 6 项中胜出 5 项，但当时 Redis 6.0 性能基线较低，
且机器环境不同（腾讯云 vs 本地），不宜直接对比两组数据的绝对值。

参考：[benchmark-sparse-vs-compact](./benchmark-sparse-vs-compact.md) — 渐进式 rehash 内部基准测试
