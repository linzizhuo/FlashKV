#include "dict.h"
#include "sds.h"
#include <stdio.h>
#include <assert.h>
#include <string.h>

/* sds 用的 dictType 配置 */
struct dictType dictTypeSds = {
    .hash = sdsHash,
    .keyCompare = sdsCompare,
};

int main()
{
    struct dict *d = dictnew(4, &dictTypeSds);  // size = 16
    assert(d != NULL);

    /* -- 测试 dictAdd -- */
    sds key1 = sdsnew("hello");
    sds val1 = sdsnew("world");
    assert(dictAdd(d, key1, val1) == DICT_OK);
    assert(d->ht.used == 1);

    /* 重复 key 应该失败 */
    assert(dictAdd(d, key1, val1) == DICT_ERROR);
    assert(d->ht.used == 1);

    /* -- 测试 dictFind -- */
    void *v = dictfind(d, key1);
    assert(v != NULL);
    assert(strcmp((sds)v, "world") == 0);

    /* 不存在的 key */
    sds missing = sdsnew("nope");
    assert(dictfind(d, missing) == NULL);
    sdsfree(missing);

    /* -- 测试 dictReplace -- */
    sds val2 = sdsnew("galaxy");
    dictReplace(d, key1, val2);
    v = dictfind(d, key1);
    assert(strcmp((sds)v, "galaxy") == 0);

    /* 多个 key */
    sds key2 = sdsnew("foo");
    sds val3 = sdsnew("bar");
    assert(dictAdd(d, key2, val3) == DICT_OK);
    assert(d->ht.used == 2);

    v = dictfind(d, key2);
    assert(strcmp((sds)v, "bar") == 0);

    /* -- 清理 -- */
    dictfree(d);
    sdsfree(key1);
    /* 注意：val1/val2/val3 和 key1/key2 指向的内存是 entry 里的指针，
       但 dictfree 只释放了 entry 结构体，没有释放 key/val 本身。
       因为我们没有设 keyDestructor/valDestructor，所以手动释放 */
    sdsfree(val1);
    sdsfree(val2);
    sdsfree(key2);
    sdsfree(val3);

    printf("✅ 所有测试通过！\n");
    return 0;
}
