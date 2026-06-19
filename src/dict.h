#ifndef _DICT_H
#define _DICT_H

#include <stdint.h>
#define DICT_OK 0
#define DICT_ERROR 1

/*
    "信任程序员，不给不需要的东西付代价"
*/
typedef struct dictEntry dictEntry; /* 节点，不暴露 */
/* 哈希表，采用桶存储的方式。 */
struct dictht
{
    dictEntry **table;  /*存储数据的表*/
    unsigned long size; // 数组大小
    unsigned long sizemask; // size - 1
    unsigned long used; /* 数据个数 */
};
/* 函数指针数组 */
struct dictType{
    uint64_t (*hash)(const void *key);
    int (*keyCompare)(const void *key1, const void *key2);
    void (*keyFree)(void *key);
    void (*valFree)(void *val);
};

/*
    hash表，先写一个可以做sds类型的key-val存储的版本。
    之后在外层可以进行泛型等数据的包装。
*/
struct dict
{
    struct dictType *type;
    struct dictht ht[2];
    long rehashidx;
};

/*
    函数设计目标：核心就是dict模块，不会引入一些其他的模块强加依赖，做到松耦合。
*/

int dictReplace(struct dict *d, void *key, void *val);
int dictAdd(struct dict *d, void *key, void *val);
// dict.size = 2^len;
struct dict *dictnew(unsigned long n, struct dictType *type);
void *dictfind(struct dict *d, const void *key);
void dictfree(struct dict *d);
int dictDelete(struct dict *d, const void *key);

/* ---- rehash 控制（供测试 / 调优使用）---- */
// int  dictRehash(struct dict *d);
// int  dictRehashStep(struct dict *d, unsigned long number);
// int  dictRehashData(struct dict *d, unsigned long number);
#endif