#ifndef _RDB_H
#define _RDB_H

#include "kvdb.h"


/* 将 kvdb 全量快照写入 dump.rdb。
 * 返回值：0=成功, -1=失败（IO 错误 / OOM / 序列化错误） */
// rdb.h
int rdbSave(kvdb *kv, const char *filename);
#endif