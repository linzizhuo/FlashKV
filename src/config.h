#ifndef _CONFIG_H
#define _CONFIG_H
/*
    统一所有模块的参数，编号，返回值等
*/

enum DataType
{
    DATA_STRING = 0,
    DATA_LIST = 1,
    DATA_ZSET = 2,
    DATA_SET = 3,
    DATA_HASH = 4,
    DATA_INT = 5,
};
// 调优参数
#define DICT_RANDOM_BUF_LEN 16 /* dictGetRandomKey 栈缓存容量，>此长度走兜底 */

// 魔数和版本号
#define RDB_MAGIC "FLASHKV"
#define RDB_VERSION 1
/* RDB type 字节编码（与 DataType 在同一文件，物理上保证一致） */
#define RDB_TYPE_MASK 0x7F /* 低 7 位 = DataType */
#define RDB_HAS_EXPIRE 0x80 /* 高 1 位 = TTL 标记 */

/* 返回值 */
#define OK 0
#define ERR -1
#define AGAIN -2 // 数据不完整，需要继续读，流式读取很有用。

#define MAX_LINE_LEN (16 * 1024)         // 单行最大 16KB
#define MAX_BULK_LEN (512 * 1024 * 1024) // Bulk String 最大 512MB
#define MAX_PARSE_DEPTH 1024             // 数组嵌套递归最大深度

#define MAX_EVENTS 1024
#define BUF_SIZE 4096
#define MAX_PIPELINE_BATCH 64 /* handleRead 单轮最多处理命令数，~1ms 调度粒度 */

#define ZSKIPLIST_MAXLEVEL 32
#define ZSKIPLIST_P 0.25

#define DICT_HT_INITIAL_SIZE 4
/* ---- 定期抽样删除过期 key ---- */

#define ACTIVE_EXPIRE_LOOKUPS 20      /* 每轮采样数 */
#define ACTIVE_EXPIRE_MAX_LOOPS 16    /* 最多轮数 */
#define ACTIVE_EXPIRE_THRESHOLD 5     /* 过期数 < 此值退出 (LOOKUPS * 0.25) */
#define ACTIVE_EXPIRE_TIME_LIMIT 1000 /* 单次最大耗时 (us)，避免阻塞事件循环 */

#define SERVER_PORT 6379
#endif