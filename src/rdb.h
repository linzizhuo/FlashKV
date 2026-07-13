#ifndef _RDB_H
#define _RDB_H

#include "kvdb.h"


/* 将 kvdb 全量快照写入 dump.rdb。
 * 返回值：0=成功, -1=失败（IO 错误 / OOM / 序列化错误） */
// rdb.h
int rdbSave(kvdb *kv, const char *filename);

/* 将所有数据库全量快照写入单个 RDB 文件（含 SELECTDB opcode 分隔）。
 * 返回值：0=成功, -1=失败（IO 错误 / OOM / 序列化错误） */
int rdbSaveAll(kvdb **kvs, unsigned int dbsize, const char *filename);
/* 负数是错误，整数是kv的长度 */
int rdbLoad(kvdb ***kv, const char *filename);

#endif