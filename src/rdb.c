#include "rdb.h"
#include "ttl.h"
#include <stdio.h>
#include <time.h>

/* ---- 常量 ---- */
#define RDB_MAGIC "FLASHKV"
#define RDB_VERSION 1

#define RDB_TYPE_MASK 0x7F  /* type 字节低 7 位 = ValType */
#define RDB_HAS_EXPIRE 0x80 /* type 字节高 1 位 = TTL 标记 */

// rdb.h
int rdbSave(kvdb *kv, const char *filename)
{
    struct dict *dict = kvdbGetDict(kv);
    struct dict *expires = kvdbGetExpires(kv);

    /* 临时文件 */
    char tmpfile[256];
    snprintf(tmpfile, sizeof(tmpfile), "temp-%d.rdb", getpid());
    FILE *fp = fopen(tmpfile, "wb");
    if (!fp)
        return RDB_ERR;

    /* 1. 文件头 */
    if (fwrite(RDB_MAGIC, 7, 1, fp) != 1)
        goto err;
    {
        uint32_t ver = RDB_VERSION;
        if (fwrite(&ver, 4, 1, fp) != 1)
            goto err;
    }

    /* 2. 遍历主 dict */
    dictIterator *di = dictGetBegin(dict);
    if (!di)
        goto err;

    dictEntry *de;
    while ((de = dictNext(di)) != NULL)
    {
        sds key = dictEntryGetKey(de);
        ValObj *val = dictEntryGetVal(dict, de);

        /* 查 TTL */
        time_t expire = 0;
        {
            hash_t h = sdsHash(key);
            tstamp_t *when = keyTtlFind(expires, key, h);
            if (when)
                expire = (time_t)(*when);
        }

        /* type 字节: 低 7 位=实际类型, 高 1 位=TTL 标记 */
        uint8_t type = (uint8_t)(val->type & RDB_TYPE_MASK);
        if (expire > 0)
            type |= RDB_HAS_EXPIRE;
        if (fwrite(&type, 1, 1, fp) != 1)
        {
            dictFreeIterator(di);
            goto err;
        }

        /* key */
        if (rdbWriteSds(fp, key) == RDB_ERR)
        {
            dictFreeIterator(di);
            goto err;
        }

        /* expire (如果有 TTL) */
        if (expire > 0)
        {
            uint64_t exp64 = (uint64_t)expire;
            if (fwrite(&exp64, 8, 1, fp) != 1)
            {
                dictFreeIterator(di);
                goto err;
            }
        }
        /* value */
        if (rdbWriteVal(fp, val) == RDB_ERR)
        {
            dictFreeIterator(di);
            goto err;
        }
    }

    dictFreeIterator(di);
    fclose(fp);

    /* 3. 原子 rename */
    if (rename(tmpfile, filename) != 0)
    {
        unlink(tmpfile);
        return RDB_ERR;
    }

    return RDB_OK;
err:
    if (di)
        dictFreeIterator(di);
    if (fp)
        fclose(fp);
    unlink(tmpfile);
    return RDB_ERR;
}