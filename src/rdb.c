#include "rdb.h"
#include "ttl.h"
#include "io.h"
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
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

    dictIterator di = dictGetBegin(dict);

    dictEntry *de;
    while ((de = dictGetEntry(&di)) != NULL)
    {
        sds key = dictEntryGetKey(de);
        ValObj *val = dictEntryGetVal(dict, de);

        uint8_t type = (uint8_t)(val->type & RDB_TYPE_MASK);

        time_t expire = 0;
        if (expires)
        {
            hash_t h = sdsHash(key);
            tstamp_t *when = keyTtlFind(expires, key, h);
            if (when)
            {
                expire = (time_t)(*when);
                if (expire > 0)
                    type |= RDB_HAS_EXPIRE;
            }
        }

        if (addIo(io, (const char *)&type, 1) != 1)
            goto err;
        if (dict->type->keyWrite(io, key) == ERR)
            goto err;
        if (expire > 0)
        {
            uint64_t exp64 = (uint64_t)expire;
            if (addIo(io, (const char *)&exp64, sizeof(exp64)) != sizeof(exp64))
                goto err;
        }
        if (dict->type->valWrite(io, val) == ERR)
            goto err;

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
    if (flushIo(io) != OK)
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
    if (flushIo(io) != OK)
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