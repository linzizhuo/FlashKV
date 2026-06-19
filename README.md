# FlashKV

轻量级内存 KV 存储，参考 Redis 设计。

## 当前进度

### 网络层

- **epoll 服务器** — 单线程事件循环（Reactor 模式）
- **Connection** — 连接对象 + 读/写缓冲区 + 状态机
- **非阻塞 TCP** — 与 Redis 相同的 `epoll_wait + O_NONBLOCK` 模型

### 核心引擎

- **dict** — 哈希表核心：`dictnew` / `dictAdd` / `dictReplace` / `dictfind` / `dictDelete` / `dictfree`
- **dictType** — 虚函数表，支持 `hash`、`keyCompare`、`keyFree`、`valFree`
- **dictTypeSds** — 基于 SDS 字符串的键值类型
- **ValObj** — 值统一包装，支持 STRING / LIST / ZSET / SET / HASH / INT 类型，通过 switch 分发释放

### 基础工具

- **SDS** — 动态字符串（柔性数组 + 二进制安全），含 MurmurHash2
- **log** — 日志模块，级别控制 + stdout/stderr，使用 `LOG_INFO(...)` 宏即可

## 测试

所有核心模块已通过单元测试：

- `test_dict` — 增 / 查 / 改 / 删 / 多 key 批量 / 值类型混合 / NULL 防御
- `test_sds` — 创建 / 长度 / 二进制安全

```bash
make test_dict && ./test_dict
# ======== 🎉 全部测试通过 ========
```

## 使用

```bash
# 构建全部
make all

# 运行测试
make test_dict

# 启动服务器（默认 6379 端口）
./flashkv

# 指定端口 + 日志重定向
./flashkv 6380 > flash.log 2>&1
```

## 下一步

### 功能侧

- **ZSet 跳表** — 有序集合底层，skiplist 实现 `ZADD` / `ZRANGE` / `ZRANK`，支持分值排序 + 字典序二级排序

### 质量侧：Dict 2.0 性能优化

当前吞吐基线：~1.7M SET/s（单线程，含渐进式 rehash）。优化目标 **4~6M SET/s**。

| 优化方向 | 手段 | 预估收益 |
|---------|------|---------|
| 内存分配 | per-dict Slab 分配器替换 `malloc`/`free`，消除 entry 粒度的堆分配 | 2~3× |
| 分支消除 | 拆分 rehash / 非 rehash 快速路径，`unlikely()` 标注冷分支 | +15~20% |
| 缓存友好 | 搬迁时 `__builtin_prefetch` 预取下一个桶，规避 cache miss | +5~10% |

三步聚拢起来：**砍 malloc → 拆路径 → 预取流水线**，纯代码级优化，不动数据结构，一天可完成。

---

## 架构

```
Client ──→ epoll 事件循环 ──→ RESP 解析 [TODO] ──→ dict 引擎
                ↕
        Connection 读/写缓冲区
```
