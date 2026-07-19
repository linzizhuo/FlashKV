# Expire 模块规约

## 功能定位

主动过期删除引擎。与惰性删除（`expireIfNeeded`，访问时触发）互补，
在 cron 中批量随机采样，将过期 key 残留率控制在统计意义上的低水平。

## 接口

```c
void activeExpireCycle(int type);
```

- `type = ACTIVE_EXPIRE_CYCLE_SLOW`：慢模式，时间预算 = cron 间隔 × 25%
- `type = ACTIVE_EXPIRE_CYCLE_FAST`：快模式，时间预算 ≤ 1ms
- 函数被动执行传入的 type，不自行决定模式
- SLOW 超时时设置内部标志；调用方（server.c）读取标志，下次调 FAST。整体行为：慢模式是默认，快模式只在慢模式超时时触发

## 核心策略

随机采样 + 自适应循环：

```
对于每个 DB（游标轮转，公平推进）：
  do:
    随机采样 N 个带 TTL 的 key
    删除其中已过期的
    如果 过期比例 < 阈值 → 退出当前 DB
    如果 循环次数 ≥ 16 或 超时  → 退出当前 DB
    否则 → 继续下一轮采样
  while 还有 key 要清理
```

## 可调参数

| 参数 | 来源 | 说明 |
|------|------|------|
| `active_expire_effort` | config.h，默认 1（1~10） | 努力程度，越高采样越多、阈值越低 |
| 每轮采样数 | `20 + 20/4 × effort` | effort=1 → 25，effort=10 → 70 |
| FAST 时间预算 | `1000 + 1000/4 × effort` μs | effort=1 → 1250μs |
| SLOW 时间预算 | `25 + 2 × effort` % cron 间隔 | effort=1 → 27% |
| 过期比例阈值 | `10 - effort` % | effort=1 → 9%，effort=10 → 0%（全部清完才停） |
| 最大循环轮数 | 16 | 硬上限，防止无限循环 |
| 最大扫描库数 | `ACTIVE_EXPIRE_MAX_DBS`（16） | 单次 cycle 最多遍历的 DB 数量 |

## 随机采样

委托给 `dictGetRandomKey()`，expire 模块不关心 rehash 内部细节。dict 内部策略：

**选表**：按 `used` 计数加权随机。ht[0] 有 7 个 key、ht[1] 有 3 个 → 70% 概率选 ht[0]。
目的不是等概率，而是让每个 key 有相同的被选机会（数学上无偏）。

**选桶 — ht[0]**：只从 `[rehashidx, size)` 区间随机，已迁空的桶不参与。rehashidx 左侧已经空了。
**选桶 — ht[1]**：全表随机（`& sizemask`）。

**选节点**：命中的桶可能是空桶，`do...while` 重试直到命中非空桶。找到非空桶后，
链内用栈缓存 `DICT_RANDOM_BUF_LEN`（16）个指针，一趟遍历取随机节点，链长超标（~10⁻¹⁴ 概率）才走两趟兜底。

**采样偏差**：ht[0] 密集分布、ht[1] 稀疏分布，桶级随机下同一个 key 在两张表被选中的概率不完全相等。
该偏差不修正——过期删除不是统计学调查，偏差只影响个别 key 多活 1~2 个 cycle（100ms 粒度），宏观清理效果不受影响。

**搭便车优化**：`dictGetRandomKey` 每次调用顺手执行 `dictRehashData(d, 1)`，
搬一个非空桶。采样是高频操作（每秒几十上百次），累积起来显著加速 rehash 完成。
没有额外交出——搬一个非空桶是 O(1)，嵌在采样里不可测量。

## 时间计量

- 使用 `clock_gettime(CLOCK_MONOTONIC)` 获取墙上时钟
- 微观层面的 ~μs 级精度偏差可忽略，不影响宏观表现

## DB 游标轮转

- `current_db`：static 变量，跨 cycle 保持
- 每次从上一次的结束位置继续，不会永远卡在 0 号 DB
- 超时退出时游标停在当前 DB，下次从同一 DB 继续
- 遍历完所有 DB 后从 0 重新开始

## 模式跃迁（由调用方 server.c 负责，expire 模块不持有此状态）

```
serverCron 调用 SLOW → 正常结束 → 下次继续 SLOW
serverCron 调用 SLOW → 超时返回 → 紧接一次 FAST（beforeSleep），之后恢复 SLOW
```

## 测试场景

1. 全部过期：1000 个过期 key，SLOW 模式下 ≤ 16 轮清空
2. 无过期：expires dict 存在但无可过期 key，直接返回，零开销
3. 超时退出：大量过期 + FAST 模式，超过 1ms 后强制返回，游标停在当前位置
4. 游标公平性：16 个 DB 全部有过期 key，多轮 cycle 后每个 DB 都被访问
5. rehash 中采样：expires dict 在 rehash 期间，ht[0] 和 ht[1] 都覆盖到
6. effort 效果：effort=10 比 effort=1 清理更激进（更多采样、更低阈值）
