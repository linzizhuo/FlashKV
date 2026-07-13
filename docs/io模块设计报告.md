# Io 模块设计报告

## 1. 概述

Io 模块是 FlashKV 的磁盘 I/O 抽象层，位于序列化逻辑与 `read`/`write` 系统调用之间。它解决两个核心问题：

1. **减少系统调用次数**——用户态缓冲区聚合多次小写入，一次 `write` 落盘
2. **减少 malloc 次数**——所有序列化目标共享同一块缓冲区，消除"每个对象独立序列化到临时 malloc 块"的模式

模块专为**单线程嵌入式 KV** 场景设计，不保证线程安全。

---

## 2. 定位：为什么需要这一层

在引入 Io 层之前，RDB 持久化的思路是：

```
序列化 → malloc 临时缓冲区 → write 到磁盘 → free
```

每一步序列化都要 `malloc` 一块临时内存，写出后再 `free`。海量小对象写入时，频繁的堆分配和系统调用成为瓶颈。

引入 Io 层后：

```
序列化 → addIo(io, data, n) → 缓冲区满时自动 write → 继续追加
         ↑ 所有对象共享同一块缓冲区
```

**省掉了中间 malloc，也间接控制了磁盘 IO 频率**——缓冲区满才 flush，不会每个小对象都触发一次 `write`。

更关键的是，这引出了一个设计决策：**舍弃独立序列化层**。在通用系统中，`serialize()` 生成缓冲区、再由上层决定写文件/发网络/存缓存是有价值的。但 FlashKV 只有一个 I/O 目标——磁盘文件。拆两步只会多出 malloc 和 memcpy，没有任何复用收益。所以序列化函数直接追加到 Io 缓冲区，不再经过中间态。

---

## 3. 结构演进

### 3.1 初版：单缓冲区（半双工）

最初的 Io 只有一个缓冲区 `buf[BUF_SIZE]` 和一个索引 `idx`。写入时追加到 `buf + idx`，flush 时 `write` 全部脏数据。

问题：**读写不能同时进行**。一旦需要读（`rdbLoad`），写缓冲区的脏数据必须先 flush，读完之后想写又要重建写上下文。频繁读写切换的场景下，每次切换都要刷盘，性能极差。

本质原因是**只有一个缓冲区却没有变量标识读写分界**。加 tag 可以做半双工，但每次方向切换必须 flush，无法避免。

### 3.2 终版：双缓冲区（全双工）

```c
typedef struct Io {
    int fd;
    size_t buflen;     /* 单个 buf 大小 */
    size_t write_idx;  /* 写缓冲区已用数据量 */
    size_t read_idx;   /* 读缓冲区已缓存数据量 */
    size_t read_pos;   /* 读缓冲区消费位置 */
    char buf[];        /* 柔性数组：前半写、后半读，各 buflen */
} Io;
```

核心设计：

- **一块 malloc，两个缓冲区**：`buf[0..buflen)` 是写缓冲区，`buf[buflen..2*buflen)` 是读缓冲区。`malloc(sizeof(Io) + buflen * 2)` 一次分配，比两段独立 malloc 少一个指针成员、少一次堆操作
- **读写独立索引**：`write_idx` 管理写侧，`read_idx` + `read_pos` 管理读侧，互不干扰
- **柔性数组省 malloc**：`buf[]` 紧邻结构体尾部，释放时 `free(io)` 一步到位

布局示意图：

```
┌──────────────────┬─────────────────────────────────┬─────────────────────────────────┐
│ fd / buflen /   │ 写缓冲区                         │ 读缓冲区                         │
│ write_idx /     │ buf[0 .. buflen)                 │ buf[buflen .. buflen*2)          │
│ read_idx/pos    │ ← write_idx 已用                 │ ← read_idx 已缓存                │
│                 │                                  │ ← read_pos 已消费               │
└──────────────────┴─────────────────────────────────┴─────────────────────────────────┘
```

---

## 4. API 设计

### 生命周期

| 函数 | 说明 |
|------|------|
| `newIo(path, cap, flags, mode)` | 打开文件，分配 Io 结构体，双缓冲各 `cap` 字节 |
| `freeIo(io)` | flush 脏数据 → `fsync` → `close` → `free` |

### 写路径

| 函数 | 说明 |
|------|------|
| `addIo(io, src, n)` | 追加 n 字节到写缓冲区，必要时自动 flush（详见 §5.1） |
| `getbufIo(io, &buf)` | 返回写缓冲区空闲区域的首地址和剩余大小 |
| `commitIo(io, n)` | 宏：`(io)->write_idx += (n)`，配合 getbufIo 做零拷贝写入 |
| `flushIo(io, FLUSH_WRITE)` | 将写缓冲区脏数据 `write` 到磁盘，`write_idx` 归零 |

**零拷贝路径**：当调用方需要先构造数据再写入时，可以走 `getbufIo` 拿到缓冲区尾部指针，直接在原地构造，最后 `commitIo` 提交。省去一次 `memcpy`。

```
getbufIo(io, &buf)  →  sprintf(buf, ...)  →  commitIo(io, len)
```

### 读路径

| 函数 | 说明 |
|------|------|
| `readIo(io, dst, n)` | 从 Io 读取 n 字节，必要时自动填充读缓冲区（详见 §5.2） |
| `flushIo(io, FLUSH_READ)` | 从 fd 读满读缓冲区，`read_idx` 更新，`read_pos` 归零 |

---

## 5. 核心算法

### 5.1 addIo — 三层分流写入

```
addIo(io, src, n):
  ┌─ n ≤ 写缓冲区剩余空间？
  │    → memcpy 到 write_idx 位置，write_idx += n        ← 快速路径，零系统调用
  │
  ├─ n ≤ buflen？（剩余不够但总容量够）
  │    → flush 写缓冲区 → memcpy 到 buf[0]              ← 一次 write + 一次 memcpy
  │
  └─ n > buflen？（超大块，比整个缓冲区还大）
       → flush 写缓冲区 → write(fd, src, n) 直写磁盘     ← 旁路缓冲区，避免两遍拷贝
```

设计意图：

- **小数据快**：大多数 entry（key、sds 字符串、int）都落在快速路径上，只是一次 `memcpy`
- **中数据稳**：偶尔超了缓冲区剩余但没超总容量，flush 一次腾空即可
- **大数据不亏**：超过 `buflen`（如巨大的 zset），与其 memcpy 进 buf 再 write 出去（两遍拷贝），不如直接 write。此时缓冲区仅作对齐用途，真正的数据走旁路

### 5.2 readIo — 四阶段渐进读取

```
readIo(io, dst, n):
  ┌─ n ≤ 缓冲区已有未消费数据？
  │    → memcpy(dst, rbuf + read_pos, n), read_pos += n  ← 快速路径，零系统调用
  │
  ├─ 缓冲区有残量但不够？
  │    → 先拷走残量，dst += avail, n -= avail
  │
  ├─ n ≤ buflen？（消费完残量后请求量仍在缓冲区容量内）
  │    → flush(FLUSH_READ) 补满缓冲区 → memcpy             ← 一次 read + 一次 memcpy
  │
  └─ n > buflen？（超大请求）
       → 直接 read(fd, dst, n) 填到目标地址                ← 旁路缓冲区
```

与 `addIo` 对称：小请求走缓冲区，大请求直通 fd，不产生多余的拷贝。

### 5.3 flushIo — 读写分流

```
flushIo(io, FLUSH_WRITE):
  write(fd, wbuf, write_idx)  —— 循环处理 EINTR，write_idx 归零

flushIo(io, FLUSH_READ):
  read(fd, rbuf, buflen)       —— 循环处理 EINTR，read_idx = 返回值，read_pos 归零
```

使用 `FLUSH_READ` / `FLUSH_WRITE` 两个方向常量（来自 `config.h`），明确表达意图。

---

## 6. 在全系统中的位置

Io 模块是纯粹的**基础设施**——它不关心写入的内容是什么，只提供"往 fd 写字节"和"从 fd 读字节"的缓冲抽象。

调用关系：

```
service.c (SAVE/BGSAVE)
    │
    ▼
rdb.c (rdbSave / rdbSaveAll / rdbLoad)
    │
    ├── dictEntryWrite / dictEntryRead  ── dict_type.c
    │       ├── sdsWrite / sdsRead      ── sds.c
    │       └── valObjWrite / valObjRead ── val_obj.h
    │               ├── DATA_STRING → sdsWrite
    │               ├── DATA_INT    → addIo
    │               └── DATA_ZSET   → zsetWrite / zsetRead
    │                       └── zslNodeWrite / zslNodeRead ── zskiplist.c
    │
    ▼
Io (addIo / readIo / flushIo)  ← 所有序列化的最终落点
    │
    ▼
fd → write() / read()
```

**所有序列化函数最终都调用 `addIo` / `readIo`**。Io 层不感知 DataType、不感知 dict entry、不感知 TTL——它的边界画在了"字节"这一级。

---

## 7. 设计取舍

| 决策 | 理由 |
|------|------|
| 读写各一半容量（非动态调整） | 单线程 KV 场景读写不会同时高频发生；动态分配增加复杂度，收益不明确 |
| 柔性数组而非独立 malloc buf | 少一次堆分配，释放时一步到位 |
| 大块数据旁路缓冲区 | 避免两遍拷贝（进 buf 再写），大数据场景 buffering 本就没有边际收益 |
| 单线程，无锁 | RDB 场景要么主线程阻塞写（SAVE），要么子进程独立写（BGSAVE），不存在竞争 |
| `config.h` 中的 `BUF_SIZE` 全局共用 4096 | 4KB 是页大小整数倍，对齐好；对 KV entry 粒度来说足够覆盖多数小对象 |
| 读写方向用 `FLUSH_READ`/`FLUSH_WRITE` 常量而非 enum | 只有两个值，宏省掉类型转换，调用点一目了然 |

### 与 Connection 层的 `wbuf`/`rbuf` 是两套东西

容易混淆的是 `server.h` 中 Connection 也有 `wbuf`/`rbuf`，那是**网络 I/O 的读写缓冲区**，管理的是 socket fd 上的 RESP 协议流。Io 模块管理的是**磁盘 fd**，用于 RDB 文件读写。两者职责正交，不共享缓冲区。

---

## 8. 后续方向

- **写缓冲区批量 flush 策略**：当前 `addIo` 中只有"满了才 flush"，可以考虑定时 flush（类似 AOF 的 `everysec`），防止断电丢失未 flush 数据。不过这更适合 AOF 场景，RDB 在 `freeIo` 时会强制 fsync
- **mmap 替代 read/write**：对于大文件（GB 级 RDB），mmap 可能比反复 read/write 更高效，但引入页错误处理的复杂性
- **压缩 hook**：在 `flushIo` 之前对写缓冲区做一次压缩（LZ4/Zstd），减少磁盘 I/O 量。对 RDB 这种以字符串为主的格式，压缩比通常很高

---

## 附录：相关源码索引

| 模块 | 文件 |
|------|------|
| Io 核心 | [src/io.c](../src/io.c) / [src/io.h](../src/io.h) |
| 配置宏 (BUF_SIZE 等) | [src/config.h](../src/config.h) |
| RDB（Io 的主要消费方） | [src/rdb.c](../src/rdb.c) |
| SDS 序列化 | [src/sds.c](../src/sds.c) |
| ValObj 序列化 | [src/val_obj.h](../src/val_obj.h) |
| ZSet/跳表序列化 | [src/zset.c](../src/zset.c) / [src/zskiplist.c](../src/zskiplist.c) |
