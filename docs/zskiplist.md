# 跳表设计文档

> FlashKV ZSet 底层数据结构

---

## 1. 结构总览

```
Level 2:  [H] ──────────────────→ [55]
Level 1:  [H] ────→ [30] ────────→ [55] ──────→ [78]
Level 0:  [H] ⇄ [10] ⇄ [18] ⇄ [30] ⇄ [42] ⇄ [55] ⇄ [66] ⇄ [78] ⇄ [91]
```

每上一层就是一次"快进"，L0 是完整数据双向链表，高层是单向索引。

---

## 2. 为什么选跳表

| | 跳表 | 红黑树 | 线索红黑树 | B+ 树 |
|---|---|---|---|---|
| 代码量 | ~500 行 | ~800 行 | ~1200 行 | ~1000 行 |
| ZRANK | span 天然 O(log N) | 需额外 size 字段 | 需额外 size 字段 | 需额外计数 |
| 范围查询 | L0 顺扫 | 中序遍历 | 线索顺扫 | 叶子链表扫 |
| 再平衡 | 掷硬币，仅动自己 | 旋转+变色（级联） | 旋转+修线索（级联） | 分裂/合并（级联） |
| 优化目标 | 通用内存 | 通用查找 | 让红黑树能遍历 | 磁盘 I/O |

结论：单线程、内存驻留、需要排序+排名+范围扫描场景下，跳表工程上最合适。

---

## 3. 结构定义

```c
#define ZSKIPLIST_MAXLEVEL 32   // 最大层数
#define ZSKIPLIST_P      0.25   // 晋升概率

typedef struct zskiplistLevel {
    struct zskiplistNode *forward;  // 本层下一节点
    unsigned long span;             // 本层跨过的节点数
} zskiplistLevel;

typedef struct zskiplistNode {
    double score;
    char *ele;                      // SDS 字符串
    struct zskiplistNode *backward; // 后退指针，仅 L0 有效
    zskiplistLevel level[];         // 柔性数组，高度由掷硬币决定
} zskiplistNode;

typedef struct zskiplist {
    zskiplistNode *header, *tail;
    unsigned long length;           // 节点总数
    int level;                      // 当前最高层数（不含 header）
} zskiplist;
```

### 3.1 为什么所有层级塞进一个节点，而非竖向链表

**核心原因：`forward` 跳到新节点后要立即读 score 做 while 条件比较。**

竖向链表里 `forward` 指向的只是某层索引片段，score 存底层数据节点，得沿 `down` 追到底才能拿到——搜索热路径凭空多 2~3 次指针追逐。数组写法 `forward` 落地即整个节点，score 当场可读，降层只是 `i--`，零额外解引用。

拆分为独立索引节点也省不了内存：高度 3 的节点数组写法 72 字节，拆分后 3 次 malloc 结构体总和 80 字节 + malloc 元数据，反而更胖。且拆分后索引节点必须存 score 副本，否则搜索时仍要 down 到底。

### 3.2 为什么 forward 和 span 捆成 struct

C 只允许一个柔性数组成员。`forward` 和 `span` 是每层的绑定信息——改 forward 必然改 span——捆成一个 struct 既是 C 语言限制，也是语义内聚。

### 3.3 span 为什么必要

没有 span，ZRANK 只能从 L0 头一个个数——O(N)。span 把排名计数摊进搜索路径：每走一步顺路累加 span，找到目标时 rank 即算好，O(log N)。插入/删除时顺手维护。

### 3.4 backward 为什么只在 L0

L0 是完整数据链表。backward 把 L0 串成双向链表，ZREVRANGE 从 tail 往回扫 O(k)，不用从 head 重新搜。高层只做索引加速，不需要 backward。

### 3.5 随机高度

```c
int zslRandomLevel(void) {
    int level = 1;
    while ((random() & 0xFFFF) < (ZSKIPLIST_P * 0xFFFF))
        level++;
    return (level < ZSKIPLIST_MAXLEVEL) ? level : ZSKIPLIST_MAXLEVEL;
}
```

| 高度 | 概率 | 100 万节点中 |
|------|------|------------|
| 1 | 75% | 750,000 |
| 2 | 18.75% | 187,500 |
| 3 | 4.69% | 46,900 |
| 4 | 1.17% | 11,700 |
| ≥5 | 0.39% | ~3,900 |

平均高度 1.33 层，每节点平均 ~1.33 个 forward 指针。

---

## 4. 核心算法

### 4.1 搜索（所有操作的前置步骤）

```
从 header 最高层出发:
  for i = header.level-1 down to 0:
      while level[i].forward.score < target:
          x = level[i].forward    // 能走就走
      update[i] = x               // 记录本层前驱，插入/删除用
```

降层不需要判断条件，就是循环 `i--`。时间复杂度 O(log N)。

### 4.2 插入

1. 搜索，记录各层 `update[i]`
2. 掷硬币决定高度 `lvl`
3. 对 i = 0..lvl-1：
   - `new->level[i].forward = update[i]->level[i].forward`
   - `update[i]->level[i].forward = new`
   - 更新 span
4. 更新 backward
5. 如果 lvl > header.level，升级

### 4.3 删除

1. 搜索，记录 `update[i]`
2. 对 i = 0..node->level-1：
   - `update[i]->level[i].forward = node->level[i].forward`
   - 合并 span
3. 更新 backward
4. 如果 node 是最高层唯一节点，降级 header.level

### 4.4 ZRANK

```
rank = 0
从 header 最高层出发:
  for i = header.level-1 down to 0:
      while level[i].forward.score <= target:
          rank += level[i].span
          x = level[i].forward
      if x 命中: return rank
```

搜索过程顺路累加 span，命中即返回。

---

## 5. API

| 函数 | 说明 |
|------|------|
| `zslCreate()` | 创建空跳表 |
| `zslFree(zsl)` | 释放跳表及其所有节点 |
| `zslInsert(zsl, score, ele)` | 插入节点。score 相同按 ele 字典序排；ele 已存在则更新 score |
| `zslDelete(zsl, score, ele)` | 删除节点 |
| `zslRank(zsl, score, ele)` | 返回 1-based rank，未找到返回 0 |
| `zslElementByRank(zsl, rank)` | 按 rank 取节点 |
| `zslCount(zsl, min, max)` | score 在 [min, max] 区间内的节点数 |
| `zslDeleteRange(zsl, min, max)` | 删除 score 范围内的所有节点，返回删除个数 |

### 命名规则

参考 SDS 命名：`{模块前缀小写}{操作名PascalCase}`。前缀 `zsl`，如 `zslInsert`、`zslRank`、`zslDeleteRange`。

---

## 6. 内存估算

| 节点高度 | 概率 | struct 大小 |
|---------|------|-----------|
| 1 | 75% | 8(score)+8(ele)+8(backward)+16(level[0]) = 40B |
| 2 | 18.75% | 40 + 16(level[1]) = 56B |
| 3 | 4.69% | 72B |
| 4 | 1.17% | 88B |

加权平均 ~47 字节/节点（不含 ele 字符串本身）。100 万节点 ~47MB + ele 字符串。

---

## 7. 参考资料

- Redis 7.x `t_zset.c` — `zslInsert`/`zslDelete`/`zslGetRank` 原始实现
- [PLAN.md](../PLAN.md) — 项目整体架构与跳表选择理由
