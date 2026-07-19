void activeExpireCycle(int type);
```
type工作模式
    ACTIVE_EXPIRE_CYCLE_FAST 快模式
    ACTIVE_EXPIRE_CYCLE_SLOW 慢模式

慢模式是默认模式，快模式只在上一次慢模式超时时触发。

努力程度，1-10
active_expire_effort 
每次扫描多少个带过期的 key
config_keys_per_loop = ACTIVE_EXPIRE_CYCLE_KEYS_PER_LOOP +ACTIVE_EXPIRE_CYCLE_KEYS_PER_LOOP / 4 * effort

config_cycle_fast_duration = ACTIVE_EXPIRE_CYCLE_FAST_DURATION +
ACTIVE_EXPIRE_CYCLE_FAST_DURATION / 4 * effort
SLOW 周期最多占用 serverCron 调用间隔的 25% 时间
config_cycle_slow_time_perc = ACTIVE_EXPIRE_CYCLE_SLOW_TIME_PERC +
2 * effort
抽样中过期 key 占比超过 10%，认为过期残留严重，继续再扫一轮
config_cycle_acceptable_stale = ACTIVE_EXPIRE_CYCLE_ACCEPTABLE_STALE -
effort

每次轮次
ACTIVE_EXPIRE_MAX_LOOPS

cpu时间使用墙上时钟获取，微观上精度问题几乎不影响宏观。

每次从上一次操作结束的库开始比如第一次0-15循环在0号结束，第二次从0号桶开始
如果超时退出，则从退出的地方开始，比如 1号桶超时，下一次依旧从1号桶开始。
主要结构：

for(0 ~ 15)
    do
    {
        for(0 ~ config_keys_per_loop) 每轮扫多少个过期key


        // 慢模式超时 -> 快模式，快模式超时不变
    }
    while(过期key占比小于config_cycle_acceptable_stale退出)

没超时，快模式->慢模式，慢模式不变
```
随机采样策略：
    委托给dictGetRandomKey