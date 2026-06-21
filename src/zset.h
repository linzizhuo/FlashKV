#ifndef _ZSET_H
#define _ZSET_H

#include "dict.h"
#include "zskiplist.h"

/*
 * ZSet: dict (member → zskiplistNode*) + skiplist (score 排序)
 *
 * 与 Redis t_zset 设计一致：
 *   - dict  提供 O(1)  member 查重/查找
 *   - skiplist 提供 O(log N) 排序/排名/范围
 *   - 两者共享 sds (member 字符串) 所有权，由 skiplist 统一释放
 */

typedef struct zset
{
    struct dict *dict;   /* member (sds) → zskiplistNode * */
    zskiplist *zsl;      /* 按 (score, member) 排序 */
} zset;

/* ---- 生命周期 ---- */
zset *zsetNew(void);
void  zsetFree(zset *zs);

/* ---- 核心操作 ---- */
int  zsetAdd(zset *zs, double score, sds ele);   /* 接管 ele。返回 1=新增, 0=已存在(更新score) */
int  zsetDel(zset *zs, sds ele);                 /* 1=删除成功, 0=member 不存在 */
zskiplistNode *zsetFind(zset *zs, sds ele);      /* O(1) 查找 member，NULL=不存在 */

/* ---- 查询 (透传 skiplist) ---- */
unsigned long zsetRank(zset *zs, sds ele);                     /* 1-based rank，0=不存在 */
zskiplistNode *zsetByRank(zset *zs, unsigned long rank);       /* 1-based */
unsigned long zsetCount(zset *zs, double min, double max);     /* score 区间计数 */
unsigned long zsetDelRange(zset *zs, double min, double max);  /* score 区间删除，返回删除数 */
unsigned long zsetLen(zset *zs);                               /* 基数 = zsl->length */

#endif
