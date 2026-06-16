#include "dict.h"
#include "dict_type.h"
#include "sds.h"
#include "val_obj.h"
#include <stdio.h>
#include <assert.h>
#include <string.h>

/* 辅助：创建 ValObj 字符串 */
static ValObj *makeStr(const char *s) {
    ValObj *o = malloc(sizeof(*o));
    o->type = VAL_STRING;
    o->val.str = sdsnew(s);
    return o;
}

/* 辅助：创建 ValObj 整数 */
static ValObj *makeInt(long long v) {
    ValObj *o = malloc(sizeof(*o));
    o->type = VAL_INT;
    o->val.ll = v;
    return o;
}

static void test_add_and_find(void)
{
    printf("=== test_add_and_find ===\n");
    struct dict *d = dictnew(4, &dictTypeSds);  // size = 16

    sds k1 = sdsnew("name");
    ValObj *v1 = makeStr("FlashKV");
    assert(dictAdd(d, k1, v1) == DICT_OK);
    assert(d->ht.used == 1);

    /* 重复 key 插入失败，调用方自己清理 */
    ValObj *dup = makeStr("dup");
    assert(dictAdd(d, k1, dup) == DICT_ERROR);
    valObjFree(dup);  /* dict 未接管，调用方释放 */
    assert(d->ht.used == 1);

    /* 查找 */
    ValObj *found = (ValObj *)dictfind(d, k1);
    assert(found != NULL);
    assert(found->type == VAL_STRING);
    assert(strcmp(found->val.str, "FlashKV") == 0);

    /* 不存在的 key */
    sds missing = sdsnew("nope");
    assert(dictfind(d, missing) == NULL);

    dictfree(d);
    sdsfree(missing);
    printf("   ✅\n");
}

static void test_replace(void)
{
    printf("=== test_replace ===\n");
    struct dict *d = dictnew(4, &dictTypeSds);

    sds k = sdsnew("key");
    ValObj *v1 = makeStr("old");
    dictAdd(d, k, v1);

    /* replace 旧值 */
    ValObj *v2 = makeStr("new");
    dictReplace(d, k, v2);

    ValObj *found = (ValObj *)dictfind(d, k);
    assert(found->type == VAL_STRING);
    assert(strcmp(found->val.str, "new") == 0);

    dictfree(d);
    printf("   ✅\n");
}

static void test_delete(void)
{
    printf("=== test_delete ===\n");
    struct dict *d = dictnew(4, &dictTypeSds);

    sds k1 = sdsnew("alive");
    sds k2 = sdsnew("gone");
    dictAdd(d, k1, makeStr("hello"));
    dictAdd(d, k2, makeStr("bye"));

    assert(d->ht.used == 2);

    /* 删除存在的 key */
    assert(dictDelete(d, k2) == DICT_OK);
    assert(d->ht.used == 1);
    /* 注意：k2 已经在 dictDelete 里被 free 了，不能再用原指针查 */
    sds k2copy = sdsnew("gone");
    assert(dictfind(d, k2copy) == NULL);

    /* 删除不存在的 key（在 free k2copy 之前） */
    assert(dictDelete(d, k2copy) == DICT_ERROR);
    sdsfree(k2copy);

    /* 验证另一个 key 还在 */
    assert(dictfind(d, k1) != NULL);

    dictfree(d);
    printf("   ✅\n");
}

static void test_delete_missing(void)
{
    printf("=== test_delete_missing ===\n");
    struct dict *d = dictnew(4, &dictTypeSds);

    sds k = sdsnew("lonely");
    dictAdd(d, k, makeInt(1));

    /* 删不存在的 key */
    sds missing = sdsnew("nobody");
    assert(dictDelete(d, missing) == DICT_ERROR);
    assert(d->ht.used == 1);
    sdsfree(missing);

    dictfree(d);
    printf("   ✅\n");
}

static void test_delete_no_key_free(void)
{
    printf("=== test_delete (已删除的 key 不需要调用方 free) ===\n");
    struct dict *d = dictnew(4, &dictTypeSds);

    sds k = sdsnew("temp");
    dictAdd(d, k, makeInt(42));

    /* dictDelete 会自己 keyFree + valFree，调用方不用再 free */
    assert(dictDelete(d, k) == DICT_OK);
    assert(d->ht.used == 0);

    dictfree(d);
    /* 注意：k 已经在删除时被 free 了，不要再 sdsfree(k) */
    printf("   ✅\n");
}

static void test_multiple_keys(void)
{
    printf("=== test_multiple_keys ===\n");
    struct dict *d = dictnew(4, &dictTypeSds);  // 16 slots，容易冲突

    /* 塞入一批 key-val */
    int N = 20;
    for (int i = 0; i < N; i++) {
        char buf[32];
        snprintf(buf, sizeof(buf), "key-%d", i);
        sds k = sdsnew(buf);
        ValObj *v = makeInt(i * 10);
        assert(dictAdd(d, k, v) == DICT_OK);
    }
    assert(d->ht.used == (unsigned long)N);

    /* 全部能查到 */
    for (int i = 0; i < N; i++) {
        char buf[32];
        snprintf(buf, sizeof(buf), "key-%d", i);
        sds k = sdsnew(buf);
        ValObj *v = (ValObj *)dictfind(d, k);
        assert(v != NULL);
        assert(v->type == VAL_INT);
        assert(v->val.ll == i * 10);
        sdsfree(k);   /* 查找用的 key，用完释放（dict 里有自己的副本） */
    }

    /* 删一半 */
    for (int i = 0; i < N / 2; i++) {
        char buf[32];
        snprintf(buf, sizeof(buf), "key-%d", i);
        sds k = sdsnew(buf);
        assert(dictDelete(d, k) == DICT_OK);
        sdsfree(k);   /* 这个 key 是我们临时创建的，不是 dict 里的那个 */
        /* 注意：这里 sdsfree(k) 和 dict 内部的 keyFree 是不同对象 */
    }
    assert(d->ht.used == (unsigned long)(N - N / 2));

    /* 剩下的还在 */
    for (int i = N / 2; i < N; i++) {
        char buf[32];
        snprintf(buf, sizeof(buf), "key-%d", i);
        sds k = sdsnew(buf);
        assert(dictfind(d, k) != NULL);
        sdsfree(k);
    }

    dictfree(d);
    printf("   ✅\n");
}

static void test_valobj_types(void)
{
    printf("=== test_valobj_types ===\n");
    struct dict *d = dictnew(4, &dictTypeSds);

    /* 字符串值 */
    sds ks = sdsnew("s");
    ValObj *vs = makeStr("string-val");
    dictAdd(d, ks, vs);

    /* 整数值 */
    sds ki = sdsnew("i");
    ValObj *vi = makeInt(999);
    dictAdd(d, ki, vi);

    /* 查字符串 */
    ValObj *fs = (ValObj *)dictfind(d, ks);
    assert(fs->type == VAL_STRING);
    assert(strcmp(fs->val.str, "string-val") == 0);

    /* 查整数 */
    ValObj *fi = (ValObj *)dictfind(d, ki);
    assert(fi->type == VAL_INT);
    assert(fi->val.ll == 999);

    dictfree(d);
    printf("   ✅\n");
}

static void test_null_safety(void)
{
    printf("=== test_null_safety ===\n");

    /* dictfind(NULL, ...) — 当前没有防御，但合理调用不会传 NULL */
    /* dictfree(NULL) 有防御 */
    dictfree(NULL);
    printf("   dictfree(NULL) 不崩溃 ✅\n");

    /* dictDelete — 用户已经写了防御 */
    assert(dictDelete(NULL, NULL) == DICT_ERROR);
    struct dict *d = dictnew(4, &dictTypeSds);
    assert(dictDelete(d, NULL) == DICT_ERROR);
    dictfree(d);
    printf("   dictDelete 防御 ✅\n");
}

int main(void)
{
    printf("======== Dict 单元测试 ========\n\n");

    test_add_and_find();
    test_replace();
    test_delete();
    test_delete_missing();
    test_delete_no_key_free();
    test_multiple_keys();
    test_valobj_types();
    test_null_safety();

    printf("\n======== 🎉 全部测试通过 ========\n");
    return 0;
}
