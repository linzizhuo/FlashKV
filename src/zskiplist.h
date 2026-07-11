#ifndef _zskiplist_h
#define _zskiplist_h

#include "sds.h"
#include "config.h"
typedef struct zskiplistNode zskiplistNode;

/* 提示：使用的时候栈上分配，可以省去一次malloc喔。 */
typedef struct zslIterator
{
    zskiplistNode *current; /* NULL = 结束 */
} zslIterator;

typedef struct zskiplist
{
    zskiplistNode *header, *tail; // 头尾节点
    unsigned long length;         // 节点总数
    int level;                    // 当前最大层数
} zskiplist;

/* ---- API ---- */
zskiplist *zslnew(void);
void zslfree(zskiplist *zsl);
/* -----------迭代器----------- */
// 

zskiplistNode *zslNext(zslIterator *it);        /* L0 forward，NULL=结束 */
zskiplistNode *zslPrev(zslIterator *it);        /* L0 backward，NULL=结束 */
// 因为是栈上分配，防止滥用，所以禁掉了，使用的时候发现需要free但找不到free就要想想是不是写错了。
// void zslFreeIterator(zslIterator *it);
// zslIterator *zslGetBegin(const zskiplist *zsl); /* 从 L0 第一个有效节点开始 */
// zslIterator *zslGetIterator(const zskiplistNode *node);
#define zslGetBegin(zsl) \
    ((zslIterator){.current = zslNext(&(zslIterator){.current = (zsl)->header})})

#define zslGetIterator(node) \
    ((zslIterator){.current = (zskiplistNode *)(node)})
zskiplistNode *zslGetNode(const zslIterator *it);
/* 访问器 */
double zslNodeScore(const zskiplistNode *node);
sds zslNodeEle(const zskiplistNode *node);
/*
 * 插入 (score, ele) 节点，接管 sds 所有权。
 *
 * 按 (score, ele) 字典序定位。仅检测精确重复项——若 (score, ele) 完全相同则释放
 * 传入的 ele 并返回已有节点。ele 级唯一性由上层 zset 模块 (dict) 保证。
 */
zskiplistNode *zslinsert(zskiplist *zsl, double score, sds ele);
int zsldel(zskiplist *zsl, double score, sds ele);

unsigned long zslrank(zskiplist *zsl, double score, sds ele);
zskiplistNode *zslbyrank(zskiplist *zsl, unsigned long rank);

unsigned long zslcount(zskiplist *zsl, double min, double max);
unsigned long zsldelrange(zskiplist *zsl, double min, double max);

zskiplistNode **zslrange(zskiplist *zsl, double min, double max,
                         unsigned long *count);

/* ---- 序列化（RDB 用）---- */
// size_t zslSerialize(const zskiplist *zsl, void **buf);
// zskiplist *zslDeserialize(const void *buf);

/* buf不能传非空指针，否则会内存泄漏 */
size_t zslNodeSerialize(const zskiplistNode *node, void **buf);

#endif
