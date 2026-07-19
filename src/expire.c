#define _POSIX_C_SOURCE 199309L /* clock_gettime, CLOCK_MONOTONIC */

#include "server.h"
#include "config.h"
#include "dict.h"
#include "ttl.h"
#include "sds.h"
#include <time.h>
#include <assert.h>
/* 每次扫描多少个带过期的 key */
#define ACTIVE_EXPIRE_CYCLE_KEYS_PER_LOOP 20
/* FAST 周期每次最多 1000 微秒 */
#define ACTIVE_EXPIRE_CYCLE_FAST_DURATION 1000
/* SLOW 周期最多占用 serverCron 调用间隔的 25% 时间 */
#define ACTIVE_EXPIRE_CYCLE_SLOW_TIME_PERC 25
/* 抽样中过期 key 占比超过 10%，认为过期残留严重，继续再扫一轮 */
#define ACTIVE_EXPIRE_CYCLE_ACCEPTABLE_STALE 10
/* 每次抽样轮次 */
#define ACTIVE_EXPIRE_MAX_LOOPS 16 /* 最多轮数 */
/* 单次 cycle 最多扫描的库数量 */
#define ACTIVE_EXPIRE_MAX_DBS 16

static long long ustime(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}
/* ---- 跨 cycle 保持的模块内部状态 ---- */
static unsigned int current_db = 0;   /* 公用扫描游标 */
static int timelimit_exit = 0;        /* 上次 SLOW 是否超时退出 */
static long long last_fast_cycle = 0; /* 上次 FAST 触发时间 (us) */
/*
    对service的数据库根据SERVER_DBSIZE进行追加计算
    不接受空指针
*/
static void dbNext(unsigned int* dbidx)
{
    assert(dbidx);
    (*dbidx)++;
    if (*dbidx == SERVER_DBSIZE)
        *dbidx = 0;
}

/* SLOW→FAST 升级：调用方在 epoll_wait 前调用。
 * 内部封装了 timelimit_exit 检查 + 频率控制（≥ 2 倍 cron 间隔），
 * 调用方无需关心内部状态。 */
void activeExpireTryFast(void)
{
    unsigned long effort = active_expire_effort - 1;
    long long config_cycle_fast_duration = ACTIVE_EXPIRE_CYCLE_FAST_DURATION +
                                               ACTIVE_EXPIRE_CYCLE_FAST_DURATION / 4 * effort;
    /* SLOW 正常结束，不需要 FAST 补偿 */
    if (!timelimit_exit)
        return;

    /* 频率控制：距上次 FAST 至少 200ms (2 × 100ms cron 间隔) */
    long long now = ustime();
    if (now - last_fast_cycle < config_cycle_fast_duration)
        return;

    activeExpireCycle(ACTIVE_EXPIRE_CYCLE_FAST);
}
/*
 * 主动过期：从 expires dict 中随机抽样，淘汰已过期的 key。
 *
 * 设计要点：
 * - 所有 DB 共用 current_db 游标，上次超时退出后下次继续，保证公平
 * - effort 参数 (1~10) 线性缩放采样量和时间预算
 * - FAST 模式：beforeSleep 前调用，极快 (≤1ms)
 * - SLOW 模式：serverCron 驱动，允许占用 cron 间隔的 25%，更彻底
 */
void activeExpireCycle(int type)
{
    /* ---- 根据 effort 计算参数 ---- */
    unsigned long effort = active_expire_effort - 1,
                  config_keys_per_loop = ACTIVE_EXPIRE_CYCLE_KEYS_PER_LOOP +
                                         ACTIVE_EXPIRE_CYCLE_KEYS_PER_LOOP / 4 * effort,
                  config_cycle_fast_duration = ACTIVE_EXPIRE_CYCLE_FAST_DURATION +
                                               ACTIVE_EXPIRE_CYCLE_FAST_DURATION / 4 * effort,
                  config_cycle_slow_time_perc = ACTIVE_EXPIRE_CYCLE_SLOW_TIME_PERC +
                                                2 * effort,
                  config_cycle_acceptable_stale = ACTIVE_EXPIRE_CYCLE_ACCEPTABLE_STALE -
                                                  effort;

    /* ---- 时间预算 ---- */
    long long start = ustime();
    long long timelimit;
    if (type == ACTIVE_EXPIRE_CYCLE_FAST) {
        timelimit = config_cycle_fast_duration;
        last_fast_cycle = start; // ← 加上这行
    } else {
        timelimit = SERVER_CRON_INTERVAL_US * config_cycle_slow_time_perc / 100;
    }

    timelimit_exit = 0;

    unsigned int dbs_per_call = ACTIVE_EXPIRE_MAX_DBS;
    if (dbs_per_call > service->dbsize)
        dbs_per_call = service->dbsize;

    for (unsigned int i = 0; i < dbs_per_call; i++, dbNext(&current_db))
    {
        kvdb *db = service->kvs[current_db];
        struct dict *expires = kvdbGetExpires(db);
        struct dict *keys = kvdbGetDict(db);

        /* 当前 DB 无过期 key，跳过 */
        if (dictSize(expires) == 0) continue;

        int loop = 0;
        unsigned long expired, sampled;

        do {
            sampled = 0;
            expired = 0;
            time_t now = time(NULL);

            for (unsigned long j = 0; j < config_keys_per_loop; j++) {
                /* 时间预算检查 */
                if ((ustime() - start) > timelimit) {
                    if (type == ACTIVE_EXPIRE_CYCLE_SLOW)
                        timelimit_exit = 1;
                    return;
                }

                /* 随机采样一个带 TTL 的 key */
                dictEntry *de = dictGetRandomKey(expires);
                if (!de) break; /* expires dict 已被清空 */

                sampled++;
                hash_t hash = dictEntryGetHash(de);
                sds key = dictEntryGetKey(de);
                time_t expire_time = (time_t)(*(void **)dictEntryGetVal(expires, de));

                if (now >= expire_time) {
                    /* 先删主 dict，再删 expires（顺序保证 key 指针安全：见 contract） */
                    dictDelete(keys, key, &hash);
                    dictDelete(expires, key, &hash);
                    expired++;
                }

                /* expires 已空则停止本 DB 的采样 */
                if (dictSize(expires) == 0) break;
            }

            loop++;
        } while (expired * 100 > sampled * config_cycle_acceptable_stale
                 && loop < ACTIVE_EXPIRE_MAX_LOOPS);

        /* 完成一个 DB 后检查时间预算 */
        if ((ustime() - start) > timelimit) { 
            if (type == ACTIVE_EXPIRE_CYCLE_SLOW) 
                timelimit_exit = 1;
            return;
        }
    }
}