#include "rdb.h"
#include "ttl.h"
#include "io.h"
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdbool.h>
#include "config.h"

/* 将单个 kvdb 的键值对写入已打开的 Io（不含 RDB 头/尾/文件管理）。
 * 由 rdbSave / rdbSaveAll 复用。
 * 返回值：0=成功, -1=失败 */
static int rdbSaveData(Io *io, kvdb *kv)
{
    struct dict *dict = kvdbGetDict(kv);
    struct dict *expires = kvdbGetExpires(kv);
    if (!dict)
        return ERR;

    uint32_t key_count = (uint32_t)dictSize(dict);
    if (addIo(io, (const char *)&key_count, sizeof(key_count)) != sizeof(key_count))
        return ERR;

    dictIterator di = dictGetBegin(dict);

    dictEntry *de;
    while ((de = dictGetEntry(&di)) != NULL)
    {
        ValObj *val = dictEntryGetVal(dict, de);
        uint8_t type = (uint8_t)(val->type & RDB_TYPE_MASK);

        /* kvdb 层：查过期时间 */
        uint64_t expire = 0;
        if (expires)
        {
            tstamp_t *when = keyTtlFind(expires, dictEntryGetKey(de),
                                        dictEntryGetHash(de));
            if (when && *when > 0)
            {
                expire = (uint64_t)(*when);
                type |= RDB_HAS_EXPIRE;
            }
        }

        /* wire format: type → key → val → [expire] */
        if (addIo(io, (const char *)&type, 1) != 1)
            goto err;
        if (dictEntryWrite(io, dict, de) == ERR)
            goto err;
        if (expire > 0)
        {
            if (addIo(io, (const char *)&expire, sizeof(expire)) != sizeof(expire))
                goto err;
        }

        dictNext(&di);
    }

    return OK;
err:
    return ERR;
}

int rdbSave(kvdb *kv, const char *filename)
{
    if (!kv || !filename)
        return ERR;

    char tmpfile[256];
    snprintf(tmpfile, sizeof(tmpfile), "temp-%d.rdb", getpid());

    Io *io = newIo(tmpfile, BUF_SIZE, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (!io)
        return ERR;

    /* RDB 头 */
    if (addIo(io, RDB_MAGIC, sizeof(RDB_MAGIC) - 1) != sizeof(RDB_MAGIC) - 1)
        goto err;
    {
        uint32_t ver = RDB_VERSION;
        if (addIo(io, (const char *)&ver, sizeof(ver)) != sizeof(ver))
            goto err;
    }
    {
        uint32_t dbcount = 1;
        if (addIo(io, (const char *)&dbcount, sizeof(dbcount)) != sizeof(dbcount))
            goto err;
    }

    /* 数据 */
    if (rdbSaveData(io, kv) != OK)
        goto err;

    /* 刷盘 + 原子 rename */
    if (flushIo(io, FLUSH_WRITE) != OK)
        goto err;

    freeIo(io);
    if (rename(tmpfile, filename) != 0)
    {
        unlink(tmpfile);
        return ERR;
    }
    return OK;

err:
    if (io)
        freeIo(io);
    unlink(tmpfile);
    return ERR;
}

int rdbSaveAll(kvdb **kvs, unsigned int dbsize, const char *filename)
{
    if (!kvs || !filename || dbsize == 0)
        return ERR;

    char tmpfile[256];
    snprintf(tmpfile, sizeof(tmpfile), "temp-%d.rdb", getpid());

    Io *io = newIo(tmpfile, BUF_SIZE, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (!io)
        return ERR;

    /* RDB 头 */
    if (addIo(io, RDB_MAGIC, sizeof(RDB_MAGIC) - 1) != sizeof(RDB_MAGIC) - 1)
        goto err;
    {
        uint32_t ver = RDB_VERSION;
        if (addIo(io, (const char *)&ver, sizeof(ver)) != sizeof(ver))
            goto err;
    }
    {
        uint32_t dbcount = dbsize;
        if (addIo(io, (const char *)&dbcount, sizeof(dbcount)) != sizeof(dbcount))
            goto err;
    }

    /* 逐库 */
    for (unsigned int i = 0; i < dbsize; i++)
    {
        if (rdbSaveData(io, kvs[i]) != OK)
            goto err;
    }

    /* 刷盘 + 原子 rename */
    if (flushIo(io, FLUSH_WRITE) != OK)
        goto err;

    freeIo(io);
    if (rename(tmpfile, filename) != 0)
    {
        unlink(tmpfile);
        return ERR;
    }
    return OK;

err:
    if (io)
        freeIo(io);
    unlink(tmpfile);
    return ERR;
}

static int rdbLoadData(Io *io, kvdb *kv)
{
    struct dict *dict = kvdbGetDict(kv);
    // struct dict *expires = kvdbGetExpires(kv);

    uint32_t key_count;
    if (readIo(io, (char *)&key_count, sizeof(key_count)) != OK)
        return ERR;

    for (uint32_t i = 0; i < key_count; i++)
    {
        uint8_t type_byte;
        if (readIo(io, (char *)&type_byte, 1) != OK)
            return ERR;

        int dtype = type_byte & RDB_TYPE_MASK;
        bool has_expire = type_byte & RDB_HAS_EXPIRE;

        dictEntry *de = NULL;
        if (dictEntryRead(io, dict, dtype, &de) == ERR)
            return ERR;

        dictAddEntry(dict, de);

        if (has_expire)
        {
            uint64_t exp64;
            if (readIo(io, (char *)&exp64, sizeof(exp64)) != OK)
                return ERR;
            kvdbExpire(kv, dictEntryGetKey(de), (time_t)exp64);
        }
    }

    return OK;
}

/* 负数是错误，整数是 kv 的长度 */
int rdbLoad(kvdb ***kv, const char *filename)
{
    if (!kv || !filename)
        return ERR;

    *kv = NULL;

    Io *io = newIo(filename, BUF_SIZE, O_RDONLY, 0);
    if (!io)
        return ERR;

    /* RDB 头 */
    char magic[8] = {0};
    if (readIo(io, magic, sizeof(RDB_MAGIC) - 1) != OK)
        goto err;
    if (memcmp(magic, RDB_MAGIC, sizeof(RDB_MAGIC) - 1) != 0)
        goto err;

    uint32_t ver;
    if (readIo(io, (char *)&ver, sizeof(ver)) != OK)
        goto err;
    if (ver != RDB_VERSION)
        goto err;

    uint32_t dbcount;
    if (readIo(io, (char *)&dbcount, sizeof(dbcount)) != OK)
        goto err;

    kvdb **arr = calloc(dbcount, sizeof(kvdb *));
    if (!arr)
        goto err;

    for (uint32_t i = 0; i < dbcount; i++)
    {
        arr[i] = kvdbNew();
        if (!arr[i])
            goto rollback;

        if (rdbLoadData(io, arr[i]) != OK)
            goto rollback;
    }

    freeIo(io);
    *kv = arr;
    return (int)dbcount;

rollback:
    for (uint32_t j = 0; j < dbcount && arr[j]; j++)
        kvdbFree(arr[j]);
    free(arr);
err:
    if (io)
        freeIo(io);
    return ERR; /* -1 */
}