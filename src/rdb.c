#include "rdb.h"
#include "ttl.h"
#include "io.h"
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include "config.h"

int rdbSave(kvdb *kv, const char *filename)
{
    if (!kv || !filename)
        return ERR;

    struct dict *dict = kvdbGetDict(kv);
    struct dict *expires = kvdbGetExpires(kv);
    if (!dict)
        return ERR;

    /* 临时文件 */
    char tmpfile[256];
    snprintf(tmpfile, sizeof(tmpfile), "temp-%d.rdb", getpid());

    Io *io = newIo(tmpfile, BUF_SIZE, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (!io)
        return ERR;

    if (addIo(io, RDB_MAGIC, sizeof(RDB_MAGIC) - 1) != sizeof(RDB_MAGIC) - 1)
        goto err_no_di;
    {
        uint32_t ver = RDB_VERSION;
        if (addIo(io, (const char *)&ver, sizeof(ver)) != sizeof(ver))
            goto err_no_di;
    }

    /* 2. 获取迭代器，从头遍历到尾部 */
    dictIterator *di = dictGetBegin(dict);
    if (!di)
        goto err_no_di;

    dictEntry *de;
    while ((de = dictGetEntry(di)) != NULL)
    {
        sds key = dictEntryGetKey(de);
        ValObj *val = dictEntryGetVal(dict, de);

        /* 3. 先查 ttl，修改标记位 */
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

        /* 写入 type 字节 */
        if (addIo(io, (const char *)&type, 1) != 1)
        {
            dictFreeIterator(di);
            goto err_no_di;
        }

        /* 4. 写入 key */
        if (dict->type->keyWrite(io, key) == ERR)
        {
            dictFreeIterator(di);
            goto err_no_di;
        }
        /* 5. 写入 expire（若有需要） */
        if (expire > 0)
        {
            uint64_t exp64 = (uint64_t)expire;
            if (addIo(io, (const char *)&exp64, sizeof(exp64)) != sizeof(exp64))
            {
                dictFreeIterator(di);
                goto err_no_di;
            }
        }
        /* 6. 写入 value */
        if (dict->type->valWrite(io, val) == ERR)
        {
            dictFreeIterator(di);
            goto err_no_di;
        }
        dictNext(di); /* 前进 */
    }

    dictFreeIterator(di);

    /* 7. 刷盘 + 原子 rename */
    if (flushIo(io) != OK)
        goto err_no_di;

    freeIo(io); /* flush + fsync + close + free */

    if (rename(tmpfile, filename) != 0)
    {
        unlink(tmpfile);
        return ERR;
    }

    return OK;

err_no_di:
    if (di)
        dictFreeIterator(di);
    if (io)
        freeIo(io);
    unlink(tmpfile);
    return ERR;
}