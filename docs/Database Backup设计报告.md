# Database Backup 设计报告

## 1. 概述

Database Backup（RDB）是 FlashKV 的全量持久化机制：在指定时刻将内存中的整个数据集生成一个紧凑的二进制文件（`dump.rdb`），保存到磁盘。它是数据库崩溃恢复的基础——重启时从 RDB 文件重建全部键值对，恢复现场。

与 AOF（Append-Only File）的命令日志式持久化不同，RDB 走的是**快照**路线：一次写入全部数据，恢复时直接加载，不需要重放命令。

---

## 2. 总体架构

```
┌─────────────────────────────────────────────────┐
│                    Service                       │
│         SAVE (同步)  /  BGSAVE (fork)            │
└─────────────────┬───────────────────────────────┘
                  │
                  ▼
┌─────────────────────────────────────────────────┐
│                rdbSave / rdbSaveAll              │
│  遍历 kvdb → 写 Header → 遍历 dict → 写 Entry    │
└─────────────────┬───────────────────────────────┘
                  │
                  ▼
┌─────────────────────────────────────────────────┐
│                    Io 层                         │
│   双缓冲区 + 柔性数组 + addIo/readIo 流式读写      │
└─────────────────┬───────────────────────────────┘
                  │
                  ▼
             磁盘文件 (dump.rdb)
```

恢复路径是镜像：

```
磁盘文件 → Io → readIo → rdbLoad → dictAddEntry → kvdb
```

---

## 3. RDB 文件格式

```
┌──────────────────────────────────────────────────────────┐
│ Header                                                   │
│  "FLASHKV"  (7 bytes)   魔数                              │
│  version    (4 bytes)   版本号, uint32_t, little-endian    │
│  dbcount    (4 bytes)   数据库个数, uint32_t, LE            │
├──────────────────────────────────────────────────────────┤
│ Per-DB Section  (重复 dbcount 次)                          │
│  ┌────────────────────────────────────────────────────┐  │
│  │ key_count  (4 bytes)  本库键值对数量, uint32_t, LE     │  │
│  │                                                      │  │
│  │ Per-Entry  (重复 key_count 次)                        │  │
│  │  ┌──────────────────────────────────────────────┐   │  │
│  │  │ 1B: type  (低 7 位 = DataType, bit7 = 过期标记) │   │  │
│  │  │ key  →  [4B len][data]   (sds 格式)             │   │  │
│  │  │ val  →  类型自描述 (见 §3.1)                     │   │  │
│  │  │ [8B: expire_time]       (仅当 bit7=1 时存在)     │   │  │
│  │  └──────────────────────────────────────────────┘   │  │
│  └────────────────────────────────────────────────────┘  │
└──────────────────────────────────────────────────────────┘
```

> **注意**：当前版本 Footer 中的 CRC64 校验和尚未实现，设计笔记中已预留格式位。

### 3.1 各类型 val 编码

| DataType | 编码方式 |
|----------|----------|
| `DATA_STRING` | `[4B len][data]` — sds 自描述格式 |
| `DATA_INT` | `[8B]` — `long long` 定长，直接写入 |
| `DATA_ZSET` | `[4B count]` → 每条 `[8B score][4B member_len][member]` × count |

`DATA_LIST`、`DATA_SET`、`DATA_HASH` 类型预留，当前返回错误。

### 3.2 类型特化，各自自描述

一个重要设计决策：**不对 val 做通用的 `[4B val_len][data]` 包装**。

通用包装要求先把整个值序列化到一块临时缓冲区才能得知长度，对 zset（可能包含百万级 member）来说这会导致巨大的临时内存分配。因此改为每种类型自己描述边界：

- String：4B 长度 + data，读完即止
- Int：固定 8B，无需长度前缀
- ZSet：4B count + 逐条 (score + member)，count 决定了循环次数

这也就是 wire format 中 "类型自描述" 的含义——每种类型自己负责"读到哪里算结束"。

---

## 4. 关键设计决策

### 4.1 全量快照，非增量

每次 SAVE/BGSAVE 都序列化**整个数据集**，不是增量追加。优点：

- 恢复速度快：一次读入，无需重放历史
- 文件紧凑：只存最终数据，无中间态冗余
- 格式简单：不需要处理增量和全量的合并逻辑

代价是写操作开销与数据集大小成正比，不适合高频持久化。高频场景应走 AOF。

### 4.2 BGSAVE：fork + COW

`BGSAVE` 通过 `fork()` 创建子进程执行写盘，父进程立即返回继续处理请求。这之所以可行，依赖的是操作系统的 **Copy-On-Write（写时复制）** 机制：

- fork 后父子进程共享同一份物理内存页，标记为只读
- 父进程继续修改数据时，内核将受影响的页复制一份，父子各持一份
- 子进程看到的始终是 fork 瞬间的快照，不受父进程后续写入影响

因此 BGSAVE 不需要锁表、不需要暂停服务，是"零阻塞"的快照方案。

### 4.3 迭代器模式：解耦遍历与序列化

RDB 需要遍历 dict 的每条 entry 并写入磁盘。最直接的做法是在 `rdb.c` 中直接操作 `dict->ht[0].table[]`，但这会暴露 dict 内部结构，强耦合。

考察过函数回调方案，但 kvdb → dict → entry 中间封装层多，回调会函数调来调去，复用性差。最终选择**迭代器模式**：

```c
dictIterator di = dictGetBegin(dict);
dictEntry *de;
while ((de = dictGetEntry(&di)) != NULL) {
    // 逐条序列化
    dictNext(&di);
}
```

- 迭代器在**栈上分配**（`dictGetBegin` 返回值，非堆分配），零 malloc 开销
- 迭代器自动处理 rehash 双表遍历，调用方无感知
- 与 C++ STL 的 Iterator 概念一致，降低理解成本

### 4.4 Io 抽象层：双缓冲区 + 柔性数组

#### 4.4.1 为什么舍弃"序列化 → 写文件"两步走

通用做法是：先 `serialize()` 生成一块 `malloc` 缓冲区，再 `write()` 写入磁盘。两步拆开在通用系统中有价值——同一个序列化结果可以写文件、发网络、存缓存。

但在 FlashKV 这种单线程嵌入式 KV 引擎中，永远只有一个目标：Io 缓冲区。拆两步只多了 `malloc` 和 `memcpy`，没有任何复用收益。因此**舍弃独立的序列化步骤**，让各类型的 Write 函数直接追加到 Io 缓冲区。

#### 4.4.2 Io 结构

```c
typedef struct Io {
    int fd;
    size_t buflen;     // 单个 buf 大小
    size_t write_idx;  // 写缓冲区已用
    size_t read_idx;   // 读缓冲区已缓存
    size_t read_pos;   // 读缓冲区消费位置
    char buf[];        // 柔性数组：前半写、后半读，各 buflen
} Io;
```

关键设计点：

- **双缓冲区共用一块内存**：`buf[0..buflen)` 是写缓冲区，`buf[buflen..2*buflen)` 是读缓冲区。只做一次 `malloc(sizeof(Io) + cap*2)`，比两段独立 malloc 少一次堆分配、少一个指针成员
- **分层 flush 策略**（`addIo`）：
  - 数据 ≤ 写缓冲区剩余空间 → 直接 `memcpy`，O(1)
  - 数据 ≤ 缓冲区总大小但剩余不够 → `flush` 后拷入
  - 数据 > 缓冲区总大小 → flush 后 `write()` 直写磁盘，绕过缓冲区，避免两遍拷贝

```c
// addIo 核心逻辑
if (n <= io->buflen - io->write_idx) {
    memcpy(wbuf(io) + io->write_idx, src, n);   // 快速路径
    io->write_idx += n;
} else if (n <= io->buflen) {
    flushIo(io, FLUSH_WRITE);                    // 腾空间
    memcpy(wbuf(io), src, n);
} else {
    flushIo(io, FLUSH_WRITE);
    write(io->fd, src, n);                       // 旁路直写
}
```

- **零拷贝路径**：`getbufIo()` 返回写缓冲区的空闲区域指针，调用方直接在里面构造数据，然后 `commitIo()` 提交。适合需要先算长度再填内容的场景
- **柔性数组省 malloc**：`buf[]` 紧跟在 `Io` 结构体之后，与结构体同一块内存，释放时 `free(io)` 一步到位

### 4.5 跳过列表的迭代器

ZSet 底层是跳表（skiplist）。为支持流式写入——即边遍历边序列化，不给临时缓冲区压力——也为跳表配备了迭代器（`zslIterator`），模式与 dict 迭代器一致：

```c
zslIterator it = zslGetBegin(zs->zsl);
zskiplistNode *node;
while ((node = zslGetNode(&it)) != NULL) {
    zslNodeWrite(io, node);   // 8B score + sds member
    zslNext(&it);
}
```

### 4.6 原子 rename

写入流程始终先写到临时文件，写完后 `rename()` 覆盖目标文件：

```c
char tmpfile[256];
snprintf(tmpfile, sizeof(tmpfile), "temp-%d.rdb", getpid());
// ... 写入 tmpfile ...
rename(tmpfile, "dump.rdb");   // 原子替换
```

`rename` 在同一文件系统上是原子操作——其他进程要么看到旧文件，要么看到新文件，不会看到写了一半的残缺文件。crash 时最多残留临时文件，下次写入覆盖即可。

---

## 5. 加载（恢复）流程

`rdbLoad` 是 `rdbSave/rdbSaveAll` 的逆过程：

```
1. 打开 RDB 文件
2. 验证魔数 "FLASHKV" 和版本号
3. 读取 dbcount
4. For each DB:
   a. kvdbNew() 创建空库
   b. rdbLoadData() 逐条读取 entry：
      - 读 type byte → 解析 DataType + 过期标记
      - dictEntryRead() 调 keyRead/valRead 反序列化
      - dictAddEntry() 插入 dict
      - 有 expire → kvdbExpire() 设置 TTL
5. 返回 kvdb** 数组 + 数据库个数
```

**当前实现是"懒汉"模式**：反序列化后逐条 `dictAddEntry`，不做批量优化。对百万级数据，这是恢复的主要耗时点，后续可考虑预分配 dict 桶位减少 rehash。

---

## 6. 命令接口

| 命令 | 行为 | 阻塞 |
|------|------|------|
| `SAVE` | 主线程同步执行 `rdbSaveAll` | 是 |
| `BGSAVE` | `fork` 子进程执行，主线程立即返回 `+OK` | 否 |

两个命令最终都调用同一套 `rdbSaveAll()` 逻辑，区别仅在于**执行上下文**（主线程 vs 子进程）。

---

## 7. 设计取舍与后续方向

### 已做的取舍

| 决策 | 理由 |
|------|------|
| 舍弃独立序列化层，直接追加 Io | 单目标场景无复用收益，省掉中间 malloc |
| val 类型特化自描述，不包通用 val_len | 避免 zset 等大结构的临时内存峰值 |
| 跳表顺序插入到空 zset | 反序列化时重建索引慢，但实现简单，够用再优化 |
| 栈上迭代器 | 比堆分配少一次 malloc/free，且 RDB 场景用完即弃 |
| 当前未实现 CRC64 | 单机嵌入式场景，文件完整性由文件系统保障；后续可加 |

### 后续可做

- **CRC64 / 校验和**：Footer 已预留格式位，可在 `flushIo` 前计算追加
- **压缩**：RDB 文件对重复 key/value 未做压缩，可考虑 LZ4/Zstd 在 flush 前压缩写缓冲区
- **增量备份**：当前只支持全量，大规模数据集下写盘耗时长
- **dict 恢复优化**：`rdbLoadData` 可先读 `key_count` 预 `dictExpand`，减少加载过程中的 rehash 开销
- **跳表反序列化优化**：直接重建 skiplist 内部节点而非逐条 `zsetAdd`

---

## 附录：相关源码索引

| 模块 | 文件 |
|------|------|
| RDB 核心 | [src/rdb.c](../src/rdb.c) / [src/rdb.h](../src/rdb.h) |
| Io 抽象层 | [src/io.c](../src/io.c) / [src/io.h](../src/io.h) |
| Dict + 迭代器 | [src/dict.c](../src/dict.c) / [src/dict.h](../src/dict.h) |
| 类型序列化 | [src/dict_type.c](../src/dict_type.c) / [src/val_obj.h](../src/val_obj.h) |
| SDS 读写 | [src/sds.c](../src/sds.c) / [src/sds.h](../src/sds.h) |
| ZSet/跳表 读写 | [src/zset.c](../src/zset.c) / [src/zskiplist.c](../src/zskiplist.c) |
| SAVE/BGSAVE 命令 | [src/service.c](../src/service.c) |
| RDB 配置宏 | [src/config.h](../src/config.h) |
| 单元测试 | [tests/test_rdb.c](../tests/test_rdb.c) |
