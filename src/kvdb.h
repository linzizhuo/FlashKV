#ifndef _KVDB_H
#define _KVDB_H

#include "val_obj.h"
#include <time.h>

/* 单个数据库：主 dict + expires dict 的封装。
 *
 * 调用方只通过接口操作，不需要知道内部有两套 dict、hash 计算、
 * 惰性删除、key 所有权管理等细节。
 *
 * kvdb 内部自己管理所有 key 的所有权（sdsdup），
 * 调用方传入的 key 不会被接管，始终由调用方负责释放。 */

typedef struct kvdb kvdb;

/* ---- 生命周期 ---- */
kvdb *kvdbNew(void);
void  kvdbFree(kvdb *kv);

/* ---- key-value ---- */
ValObj *kvdbGet(kvdb *kv, const void *key);        /* NULL = 不存在/已过期 */
ValObj *kvdbSet(kvdb *kv, const void *key, ValObj *val); /* 返回旧值或 NULL */
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

#endif
