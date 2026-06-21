#ifndef _zskiplist_h
#define _zskiplist_h

#include "sds.h"

#define ZSKIPLIST_MAXLEVEL 32
#define ZSKIPLIST_P 0.25

typedef struct zskiplistNode
{
    double score;                   // 排序分值
    sds ele;                        // 元素（动态字符串）
    struct zskiplistNode *backward; // 后退指针（第1层反向链表）
    struct zskiplistLevel
    {
        struct zskiplistNode *forward; // 前向指针
        unsigned long span;            // 跨度（本层到下一节点跨越的节点数）
    } level[];                         // 柔性数组，每个节点 1~N 层
} zskiplistNode;

typedef struct zskiplist
{
    zskiplistNode *header, *tail; // 头尾节点
    unsigned long length;         // 节点总数
    int level;                    // 当前最大层数
} zskiplist;

/* ---- API ---- */
zskiplist *zslnew(void);
void zslfree(zskiplist *zsl);

zskiplistNode *zslinsert(zskiplist *zsl, double score, sds ele);
int zsldel(zskiplist *zsl, double score, sds ele);

unsigned long zslrank(zskiplist *zsl, double score, sds ele);
zskiplistNode *zslbyrank(zskiplist *zsl, unsigned long rank);

unsigned long zslcount(zskiplist *zsl, double min, double max);
unsigned long zsldelrange(zskiplist *zsl, double min, double max);

#endif
