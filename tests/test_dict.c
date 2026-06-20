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

/* 辅助：返回 dict 中所有条目的总数（跨 ht[0] + ht[1]） */
static unsigned long dictTotalUsed(struct dict *d) {
    return d->ht[0].used + d->ht[1].used;
}

static void test_add_and_find(void)
{
    printf("=== test_add_and_find ===\n");
    struct dict *d = dictnew(4, &dictTypeSds);  // size = 16

    sds k1 = sdsnew("name");
    ValObj *v1 = makeStr("FlashKV");
    assert(dictAdd(d, k1, v1, NULL) == DICT_OK);
    assert(d->ht[0].used == 1);

    /* 重复 key 插入失败，调用方自己清理 */
    ValObj *dup = makeStr("dup");
    assert(dictAdd(d, k1, dup, NULL) == DICT_ERROR);
    valObjFree(dup);  /* dict 未接管，调用方释放 */
    assert(d->ht[0].used == 1);

    /* 查找 */
    ValObj *found = (ValObj *)dictfind(d, k1, NULL);
    assert(found != NULL);
    assert(found->type == VAL_STRING);
    assert(strcmp(found->val.str, "FlashKV") == 0);

    /* 不存在的 key */
    sds missing = sdsnew("nope");
    assert(dictfind(d, missing, NULL) == NULL);

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
    dictAdd(d, k, v1, NULL);

    /* replace 旧值 — dictReplace 不负责释放旧 val，调用方需自行处理 */
    ValObj *v2 = makeStr("new");
    ValObj *old = dictfind(d, k, NULL);
    dictReplace(d, k, v2, NULL);
    if (old) valObjFree(old);   /* 调用方释放被替换的旧值 */

    ValObj *found = (ValObj *)dictfind(d, k, NULL);
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
    dictAdd(d, k1, makeStr("hello"), NULL);
    dictAdd(d, k2, makeStr("bye"), NULL);

    assert(d->ht[0].used == 2);

    /* 删除存在的 key */
    assert(dictDelete(d, k2, NULL) == DICT_OK);
    assert(d->ht[0].used == 1);
    /* 注意：k2 已经在 dictDelete 里被 free 了，不能再用原指针查 */
    sds k2copy = sdsnew("gone");
    assert(dictfind(d, k2copy, NULL) == NULL);

    /* 删除不存在的 key（在 free k2copy 之前） */
    assert(dictDelete(d, k2copy, NULL) == DICT_ERROR);
    sdsfree(k2copy);

    /* 验证另一个 key 还在 */
    assert(dictfind(d, k1, NULL) != NULL);

    dictfree(d);
    printf("   ✅\n");
}

static void test_delete_missing(void)
{
    printf("=== test_delete_missing ===\n");
    struct dict *d = dictnew(4, &dictTypeSds);

    sds k = sdsnew("lonely");
    dictAdd(d, k, makeInt(1), NULL);

    /* 删不存在的 key */
    sds missing = sdsnew("nobody");
    assert(dictDelete(d, missing, NULL) == DICT_ERROR);
    assert(d->ht[0].used == 1);
    sdsfree(missing);

    dictfree(d);
    printf("   ✅\n");
}

static void test_delete_no_key_free(void)
{
    printf("=== test_delete (已删除的 key 不需要调用方 free) ===\n");
    struct dict *d = dictnew(4, &dictTypeSds);

    sds k = sdsnew("temp");
    dictAdd(d, k, makeInt(42), NULL);

    /* dictDelete 会自己 keyFree + valFree，调用方不用再 free */
    assert(dictDelete(d, k, NULL) == DICT_OK);
    assert(d->ht[0].used == 0);

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
        assert(dictAdd(d, k, v, NULL) == DICT_OK);
    }
    assert(dictTotalUsed(d) == (unsigned long)N);

    /* 全部能查到 */
    for (int i = 0; i < N; i++) {
        char buf[32];
        snprintf(buf, sizeof(buf), "key-%d", i);
        sds k = sdsnew(buf);
        ValObj *v = (ValObj *)dictfind(d, k, NULL);
        assert(v != NULL);
        assert(v->type == VAL_INT);
        assert(v->val.ll == i * 10);
        sdsfree(k);
    }

    /* 删一半 */
    for (int i = 0; i < N / 2; i++) {
        char buf[32];
        snprintf(buf, sizeof(buf), "key-%d", i);
        sds k = sdsnew(buf);
        assert(dictDelete(d, k, NULL) == DICT_OK);
        sdsfree(k);
    }
    assert(dictTotalUsed(d) == (unsigned long)(N - N / 2));

    /* 剩下的还在 */
    for (int i = N / 2; i < N; i++) {
        char buf[32];
        snprintf(buf, sizeof(buf), "key-%d", i);
        sds k = sdsnew(buf);
        assert(dictfind(d, k, NULL) != NULL);
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
    dictAdd(d, ks, vs, NULL);

    /* 整数值 */
    sds ki = sdsnew("i");
    ValObj *vi = makeInt(999);
    dictAdd(d, ki, vi, NULL);

    /* 查字符串 */
    ValObj *fs = (ValObj *)dictfind(d, ks, NULL);
    assert(fs->type == VAL_STRING);
    assert(strcmp(fs->val.str, "string-val") == 0);

    /* 查整数 */
    ValObj *fi = (ValObj *)dictfind(d, ki, NULL);
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
    assert(dictDelete(NULL, NULL, NULL) == DICT_ERROR);
    struct dict *d = dictnew(4, &dictTypeSds);
    assert(dictDelete(d, NULL, NULL) == DICT_ERROR);
    dictfree(d);
    printf("   dictDelete 防御 ✅\n");
}

/* ================================================================
 *   渐进式 rehash 专项测试
 * ================================================================ */

static void test_rehash_trigger(void)
{
    printf("=== test_rehash_trigger ===\n");
    struct dict *d = dictnew(4, &dictTypeSds);  // size=16

    /* 插入 16 条：未超阈值，不触发 */
    for (int i = 0; i < 16; i++) {
        char buf[32];
        snprintf(buf, sizeof(buf), "k%d", i);
        dictAdd(d, sdsnew(buf), makeInt(i), NULL);
    }
    assert(d->rehashidx == -1);
    assert(d->ht[1].table == NULL);
    printf("   16 条: rehashidx=%ld (expect -1)  ✅\n", d->rehashidx);

    /* 第 17 条：used(17) > size(16)，触发 rehash */
    dictAdd(d, sdsnew("k16"), makeInt(16), NULL);
    assert(d->rehashidx >= 0);
    assert(d->ht[1].table != NULL);
    assert(d->ht[1].size == 32);  /* 翻倍 */
    printf("   17 条: rehash 触发, ht[1].size=%lu (expect 32)  ✅\n", d->ht[1].size);

    dictfree(d);
    printf("   ✅\n");
}

static void test_rehash_find_during(void)
{
    printf("=== test_rehash_find_during ===\n");
    struct dict *d = dictnew(4, &dictTypeSds);

    /* 插入 20 条（触发 rehash 但未完成） */
    for (int i = 0; i < 20; i++) {
        char buf[32];
        snprintf(buf, sizeof(buf), "k%d", i);
        dictAdd(d, sdsnew(buf), makeInt(i * 10), NULL);
    }
    assert(d->rehashidx >= 0);  /* 还在 rehash 中 */
    printf("   插入 20 条后: rehashidx=%ld, ht[0].used=%lu, ht[1].used=%lu\n",
           d->rehashidx, d->ht[0].used, d->ht[1].used);

    /* 所有 key 都能查到（可能分散在两个表中） */
    for (int i = 0; i < 20; i++) {
        char buf[32];
        snprintf(buf, sizeof(buf), "k%d", i);
        sds k = sdsnew(buf);
        ValObj *v = dictfind(d, k, NULL);
        assert(v != NULL);
        assert(v->type == VAL_INT);
        assert(v->val.ll == (long long)i * 10);
        sdsfree(k);
    }
    printf("   20 条全部可查 (跨双表)  ✅\n");

    dictfree(d);
    printf("   ✅\n");
}

static void test_rehash_delete_during(void)
{
    printf("=== test_rehash_delete_during ===\n");
    struct dict *d = dictnew(4, &dictTypeSds);

    for (int i = 0; i < 20; i++) {
        char buf[32];
        snprintf(buf, sizeof(buf), "k%d", i);
        dictAdd(d, sdsnew(buf), makeInt(i), NULL);
    }
    assert(d->rehashidx >= 0);
    unsigned long before = dictTotalUsed(d);

    /* 删一半 —— 可能落在 ht[0] 或 ht[1] */
    for (int i = 0; i < 10; i++) {
        char buf[32];
        snprintf(buf, sizeof(buf), "k%d", i);
        sds k = sdsnew(buf);
        assert(dictDelete(d, k, NULL) == DICT_OK);
        sdsfree(k);
    }
    assert(dictTotalUsed(d) == before - 10);
    printf("   删除 10 条后: total=%lu (expect %lu)  ✅\n",
           dictTotalUsed(d), before - 10);

    dictfree(d);
    printf("   ✅\n");
}

static void test_rehash_replace_trigger(void)
{
    printf("=== test_rehash_replace_trigger ===\n");
    struct dict *d = dictnew(4, &dictTypeSds);  // size=16

    /* 只用 dictReplace 插入 20 条，验证其也能触发 rehash */
    for (int i = 0; i < 20; i++) {
        char buf[32];
        snprintf(buf, sizeof(buf), "rk%d", i);
        sds k = sdsnew(buf);
        dictReplace(d, k, makeInt(i), NULL);
    }
    /* 修复后：dictReplace 也应该触发 rehash */
    assert(d->rehashidx >= 0 || d->ht[0].size > 16);
    printf("   dictReplace 20 条: rehashidx=%ld ht[0].size=%lu (expect rehash triggered)  ✅\n",
           d->rehashidx, d->ht[0].size);

    dictfree(d);
    printf("   ✅\n");
}

static void test_rehash_complete(void)
{
    printf("=== test_rehash_complete ===\n");
    struct dict *d = dictnew(4, &dictTypeSds);  // size=16

    /* 插入刚好 16 条 + 1 触发 rehash，然后持续 find 来推进 rehash */
    for (int i = 0; i < 17; i++) {
        char buf[32];
        snprintf(buf, sizeof(buf), "c%d", i);
        dictAdd(d, sdsnew(buf), makeInt(i), NULL);
    }
    assert(d->rehashidx >= 0);
    printf("   触发后 rehashidx=%ld\n", d->rehashidx);

    /* 反复 find 推进 rehash（每次 dictfind 搬 1 个桶） */
    int steps = 0;
    while (d->rehashidx >= 0) {
        sds k = sdsnew("c0");  /* 随便查一个存在的 key */
        assert(dictfind(d, k, NULL) != NULL);
        sdsfree(k);
        steps++;
        if (steps > 100) {
            printf("   ERROR: rehash 未在 100 步内完成!\n");
            assert(0);
        }
    }
    printf("   %d 次 dictfind 后 rehash 完成\n", steps);
    assert(d->ht[1].table == NULL);
    assert(d->ht[0].size == 32);
    assert(dictTotalUsed(d) == 17);
    printf("   ht[0].size=%lu, total=%lu  ✅\n", d->ht[0].size, dictTotalUsed(d));

    dictfree(d);
    printf("   ✅\n");
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

    printf("\n--- 渐进式 rehash ---\n\n");
    test_rehash_trigger();
    test_rehash_find_during();
    test_rehash_delete_during();
    test_rehash_replace_trigger();
    test_rehash_complete();

    printf("\n======== 🎉 全部测试通过 ========\n");
    return 0;
}
