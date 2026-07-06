#ifndef _KVDB_H
#define _KVDB_H

#include "val_obj.h"
#include "zset.h"
#include <time.h>

/* 单个数据库：主 dict + expires dict 的封装。
 *
 * kvdbSet 和 kvdbGetOrCreateZset 接管传入的 key（sds 所有权），
 * 调用方在调用后不得再 sdsfree 该 key。
 * 其余 kvdb 接口（Get/Del/Exists/TTL 等）不接管 key。 */

typedef struct kvdb kvdb;

/* ---- 生命周期 ---- */
kvdb *kvdbNew(void);
void  kvdbFree(kvdb *kv);

/* ---- key-value ---- */
ValObj *kvdbGet(kvdb *kv, const void *key);        /* NULL = 不存在/已过期 */
ValObj *kvdbSet(kvdb *kv, sds key, ValObj *val);             /* 接管 key，返回旧值或 NULL */
int     kvdbDel(kvdb *kv, const void *key);        /* 1=删除成功 0=不存在 */
int     kvdbExists(kvdb *kv, const void *key);     /* 1=存在 0=不存在 */

/* ---- TTL ---- */
int       kvdbExpire(kvdb *kv, const void *key, time_t when); /* 1=成功 0=key不存在 */
long long kvdbTTL(kvdb *kv, const void *key);      /* -2=不存在 -1=无TTL ≥0=剩余秒 */
int       kvdbPersist(kvdb *kv, const void *key);  /* 1=已移除 0=无TTL */

/* ---- 定期淘汰 ---- */
void      kvdbActiveExpireCycle(kvdb *kv);

/* ---- 定期缩容 ---- */
void      kvdbTryResize(kvdb *kv);         /* 填充率 < 10% 时缩主 dict + expires dict */

/* ---- ZSET ---- */
zset *kvdbGetZset(kvdb *kv, const void *key, int *found);
  /* *found: 1=是 zset, 0=key 不存在, -1=类型不匹配; 返回 zset 或 NULL */
zset *kvdbGetOrCreateZset(kvdb *kv, sds key);
  /* 接管 key；不存在则创建并写入，类型不匹配或 OOM 返回 NULL */

#endif
