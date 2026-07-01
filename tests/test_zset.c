#include "zset.h"
#include "zskiplist.h"
#include "sds.h"
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <string.h>

/* ================================================================
 *  Part 1: zskiplist 独立测试
 * ================================================================ */

/* 辅助：按下标取 member 名 "m0", "m1", ... */
static sds mk(int n) {
    char tmp[32];
    snprintf(tmp, sizeof(tmp), "m%d", n);
    return sdsnew(tmp);
}

/* 辅助：遍历 L0 收集所有 ele 的序号（解析 "mN" 中的 N） */
static void collectL0(zskiplist *zsl, int *out, int *n_out) {
    *n_out = 0;
    zskiplistNode *x = zsl->header->level[0].forward;
    while (x) {
        int id;
        sscanf(x->ele, "m%d", &id);
        out[(*n_out)++] = id;
        x = x->level[0].forward;
    }
}

/* 辅助：反向遍历 L0 收集 ele 序号 */
static void collectL0Reverse(zskiplist *zsl, int *out, int *n_out) {
    *n_out = 0;
    zskiplistNode *x = zsl->tail;
    while (x) {
        int id;
        sscanf(x->ele, "m%d", &id);
        out[(*n_out)++] = id;
        x = x->backward;
    }
}

/* ---- 基本创建 ---- */
static void test_zsl_create_empty(void)
{
    printf("=== test_zsl_create_empty ===\n");
    zskiplist *zsl = zslnew();
    assert(zsl != NULL);
    assert(zsl->length == 0);
    assert(zsl->level == 1);
    assert(zsl->tail == NULL);
    assert(zsl->header != NULL);
    zslfree(zsl);
    printf("   ✅\n");
}

/* ---- 顺序插入，验证 L0 正向扫描 ---- */
static void test_zsl_insert_order(void)
{
    printf("=== test_zsl_insert_order ===\n");
    zskiplist *zsl = zslnew();

    for (int i = 0; i < 10; i++) {
        zslinsert(zsl, (double)i, mk(i));
    }
    assert(zsl->length == 10);

    int arr[32], n;
    collectL0(zsl, arr, &n);
    assert(n == 10);
    for (int i = 0; i < 10; i++)
        assert(arr[i] == i);

    zslfree(zsl);
    printf("   ✅\n");
}

/* ---- 逆序插入，验证仍按 score 排序 ---- */
static void test_zsl_insert_reverse(void)
{
    printf("=== test_zsl_insert_reverse ===\n");
    zskiplist *zsl = zslnew();

    for (int i = 9; i >= 0; i--) {
        zslinsert(zsl, (double)i, mk(i));
    }
    assert(zsl->length == 10);

    int arr[32], n;
    collectL0(zsl, arr, &n);
    assert(n == 10);
    for (int i = 0; i < 10; i++)
        assert(arr[i] == i);

    zslfree(zsl);
    printf("   ✅\n");
}

/* ---- 精确重复 (score, ele) 被拒绝 ---- */
static void test_zsl_insert_duplicate(void)
{
    printf("=== test_zsl_insert_duplicate ===\n");
    zskiplist *zsl = zslnew();

    zskiplistNode *n1 = zslinsert(zsl, 1.0, mk(0));
    assert(n1 != NULL);
    assert(zsl->length == 1);

    /* 完全相同的 (score, ele) */
    zskiplistNode *n2 = zslinsert(zsl, 1.0, mk(0));
    assert(n2 == n1);                     /* 返回已有节点 */
    assert(zsl->length == 1);             /* 长度不变 */

    zslfree(zsl);
    printf("   ✅\n");
}

/* ---- 相同 score、不同 ele，按字典序排 ---- */
static void test_zsl_insert_same_score(void)
{
    printf("=== test_zsl_insert_same_score ===\n");
    zskiplist *zsl = zslnew();

    /* "b" < "c" < "a" 按字典序：a < b < c */
    zslinsert(zsl, 1.0, sdsnew("c"));
    zslinsert(zsl, 1.0, sdsnew("a"));
    zslinsert(zsl, 1.0, sdsnew("b"));
    assert(zsl->length == 3);

    zskiplistNode *x = zsl->header->level[0].forward;
    assert(strcmp(x->ele, "a") == 0);  x = x->level[0].forward;
    assert(strcmp(x->ele, "b") == 0);  x = x->level[0].forward;
    assert(strcmp(x->ele, "c") == 0);

    zslfree(zsl);
    printf("   ✅\n");
}

/* ---- 删除头节点 ---- */
static void test_zsl_delete_head(void)
{
    printf("=== test_zsl_delete_head ===\n");
    zskiplist *zsl = zslnew();

    for (int i = 0; i < 5; i++) zslinsert(zsl, (double)i, mk(i));
    assert(zsl->length == 5);

    int ret = zsldel(zsl, 0.0, sdsnew("m0"));
    assert(ret == 1);
    assert(zsl->length == 4);

    int arr[32], n;
    collectL0(zsl, arr, &n);
    assert(n == 4);
    assert(arr[0] == 1 && arr[1] == 2 && arr[2] == 3 && arr[3] == 4);

    zslfree(zsl);
    printf("   ✅\n");
}

/* ---- 删除尾节点 ---- */
static void test_zsl_delete_tail(void)
{
    printf("=== test_zsl_delete_tail ===\n");
    zskiplist *zsl = zslnew();

    for (int i = 0; i < 5; i++) zslinsert(zsl, (double)i, mk(i));
    assert(zsl->length == 5);

    int ret = zsldel(zsl, 4.0, sdsnew("m4"));
    assert(ret == 1);
    assert(zsl->length == 4);

    /* tail 应该更新为 m3 */
    assert(zsl->tail != NULL);
    assert(strcmp(zsl->tail->ele, "m3") == 0);
    assert(zsl->tail->level[0].forward == NULL);

    zslfree(zsl);
    printf("   ✅\n");
}

/* ---- 删除中间节点 ---- */
static void test_zsl_delete_middle(void)
{
    printf("=== test_zsl_delete_middle ===\n");
    zskiplist *zsl = zslnew();

    for (int i = 0; i < 5; i++) zslinsert(zsl, (double)i, mk(i));

    int ret = zsldel(zsl, 2.0, sdsnew("m2"));
    assert(ret == 1);
    assert(zsl->length == 4);

    int arr[32], n;
    collectL0(zsl, arr, &n);
    assert(n == 4);
    assert(arr[0] == 0 && arr[1] == 1 && arr[2] == 3 && arr[3] == 4);

    zslfree(zsl);
    printf("   ✅\n");
}

/* ---- 删除不存在的节点 ---- */
static void test_zsl_delete_nonexist(void)
{
    printf("=== test_zsl_delete_nonexist ===\n");
    zskiplist *zsl = zslnew();

    zslinsert(zsl, 1.0, mk(1));
    int ret = zsldel(zsl, 99.0, sdsnew("m99"));
    assert(ret == 0);
    assert(zsl->length == 1);

    zslfree(zsl);
    printf("   ✅\n");
}

/* ---- 删完所有节点 ---- */
static void test_zsl_delete_all(void)
{
    printf("=== test_zsl_delete_all ===\n");
    zskiplist *zsl = zslnew();

    for (int i = 0; i < 5; i++) zslinsert(zsl, (double)i, mk(i));
    for (int i = 0; i < 5; i++) {
        char tmp[32]; snprintf(tmp, sizeof(tmp), "m%d", i);
        int ret = zsldel(zsl, (double)i, sdsnew(tmp));
        assert(ret == 1);
    }
    assert(zsl->length == 0);
    assert(zsl->tail == NULL);
    assert(zsl->level == 1);

    zslfree(zsl);
    printf("   ✅\n");
}

/* ---- 删除后重新插入 ---- */
static void test_zsl_delete_reinsert(void)
{
    printf("=== test_zsl_delete_reinsert ===\n");
    zskiplist *zsl = zslnew();

    zslinsert(zsl, 1.0, sdsnew("a"));
    zslinsert(zsl, 2.0, sdsnew("b"));
    zslinsert(zsl, 3.0, sdsnew("c"));
    assert(zsl->length == 3);

    zsldel(zsl, 2.0, sdsnew("b"));
    assert(zsl->length == 2);

    /* 重新插入同 score 不同 ele 或同 ele 不同 score */
    zslinsert(zsl, 2.5, sdsnew("b"));
    assert(zsl->length == 3);

    int arr[32], n;
    collectL0(zsl, arr, &n);
    /* score 顺序: a(1.0) < b(2.5) < c(3.0) */
    zskiplistNode *x = zsl->header->level[0].forward;
    assert(x->score == 1.0); x = x->level[0].forward;
    assert(x->score == 2.5); x = x->level[0].forward;
    assert(x->score == 3.0);

    zslfree(zsl);
    printf("   ✅\n");
}

/* ---- zslrank 验证 ---- */
static void test_zsl_rank(void)
{
    printf("=== test_zsl_rank ===\n");
    zskiplist *zsl = zslnew();

    for (int i = 0; i < 10; i++)
        zslinsert(zsl, (double)(i * 10), mk(i));

    /* m5 score=50 应该是第 6 个（1-based） */
    unsigned long r = zslrank(zsl, 50.0, sdsnew("m5"));
    assert(r == 6);

    /* 第一个 */
    r = zslrank(zsl, 0.0, sdsnew("m0"));
    assert(r == 1);

    /* 最后一个 */
    r = zslrank(zsl, 90.0, sdsnew("m9"));
    assert(r == 10);

    /* 不存在 */
    r = zslrank(zsl, 99.0, sdsnew("m99"));
    assert(r == 0);

    zslfree(zsl);
    printf("   ✅\n");
}

/* ---- zslbyrank 验证 ---- */
static void test_zsl_by_rank(void)
{
    printf("=== test_zsl_by_rank ===\n");
    zskiplist *zsl = zslnew();

    for (int i = 0; i < 10; i++)
        zslinsert(zsl, (double)(i * 10), mk(i));

    zskiplistNode *n = zslbyrank(zsl, 1);
    assert(n != NULL && n->score == 0.0);

    n = zslbyrank(zsl, 5);
    assert(n != NULL && n->score == 40.0);

    n = zslbyrank(zsl, 10);
    assert(n != NULL && n->score == 90.0);

    /* 越界 */
    n = zslbyrank(zsl, 0);
    assert(n == NULL);
    n = zslbyrank(zsl, 11);
    assert(n == NULL);

    zslfree(zsl);
    printf("   ✅\n");
}

/* ---- zslcount 验证 ---- */
static void test_zsl_count(void)
{
    printf("=== test_zsl_count ===\n");
    zskiplist *zsl = zslnew();

    for (int i = 0; i < 10; i++)
        zslinsert(zsl, (double)(i * 10), mk(i));

    /* 全范围 */
    assert(zslcount(zsl, 0.0, 90.0) == 10);
    /* 子区间 */
    assert(zslcount(zsl, 20.0, 50.0) == 4);   /* 20,30,40,50 */
    /* 单点 */
    assert(zslcount(zsl, 30.0, 30.0) == 1);
    /* 空区间 */
    assert(zslcount(zsl, 100.0, 200.0) == 0);
    /* min > max */
    assert(zslcount(zsl, 50.0, 20.0) == 0);

    zslfree(zsl);
    printf("   ✅\n");
}

/* ---- zslrange 验证 ---- */
static void test_zsl_range(void)
{
    printf("=== test_zsl_range ===\n");
    zskiplist *zsl = zslnew();

    for (int i = 0; i < 10; i++)
        zslinsert(zsl, (double)(i * 10), mk(i));

    unsigned long count;
    zskiplistNode **arr = zslrange(zsl, 20.0, 50.0, &count);
    assert(arr != NULL);
    assert(count == 4);
    assert(arr[0]->score == 20.0);
    assert(arr[1]->score == 30.0);
    assert(arr[2]->score == 40.0);
    assert(arr[3]->score == 50.0);
    free(arr);

    /* 空范围 */
    arr = zslrange(zsl, 100.0, 200.0, &count);
    assert(arr == NULL && count == 0);

    zslfree(zsl);
    printf("   ✅\n");
}

/* ---- zsldelrange 验证 ---- */
static void test_zsl_delrange(void)
{
    printf("=== test_zsl_delrange ===\n");
    zskiplist *zsl = zslnew();

    for (int i = 0; i < 10; i++)
        zslinsert(zsl, (double)(i * 10), mk(i));

    unsigned long removed = zsldelrange(zsl, 20.0, 50.0);
    assert(removed == 4);   /* 20,30,40,50 */
    assert(zsl->length == 6);

    /* 验证剩余 */
    int arr[32], n;
    collectL0(zsl, arr, &n);
    assert(n == 6);
    assert(arr[0] == 0 && arr[1] == 1);                         /* m0, m1  (0, 10) */
    assert(arr[2] == 6 && arr[3] == 7 && arr[4] == 8 && arr[5] == 9); /* m6-m9 (60-90) */

    zslfree(zsl);
    printf("   ✅\n");
}

/* ---- backward 指针 ---- */
static void test_zsl_backward(void)
{
    printf("=== test_zsl_backward ===\n");
    zskiplist *zsl = zslnew();

    for (int i = 0; i < 5; i++)
        zslinsert(zsl, (double)i, mk(i));

    /* 正向 */
    int fwd[32], n_fwd;
    collectL0(zsl, fwd, &n_fwd);
    assert(n_fwd == 5);

    /* 反向 */
    int rev[32], n_rev;
    collectL0Reverse(zsl, rev, &n_rev);
    assert(n_rev == 5);
    for (int i = 0; i < 5; i++)
        assert(rev[i] == 4 - i);   /* 4,3,2,1,0 */

    zslfree(zsl);
    printf("   ✅\n");
}

/* ---- free(NULL) 安全 ---- */
static void test_zsl_free_null(void)
{
    printf("=== test_zsl_free_null ===\n");
    zslfree(NULL);  /* 不崩溃 */
    printf("   ✅\n");
}

/* ================================================================
 *  Part 2: zset 双索引 (dict + skiplist) 测试
 * ================================================================ */

/* ---- 创建/释放 ---- */
static void test_zset_create_free(void)
{
    printf("=== test_zset_create_free ===\n");
    zset *zs = zsetNew();
    assert(zs != NULL);
    assert(zs->zsl != NULL);
    assert(zs->dict != NULL);
    assert(zsetLen(zs) == 0);
    zsetFree(zs);
    printf("   ✅\n");
}

/* free(NULL) 安全 */
static void test_zset_free_null(void)
{
    printf("=== test_zset_free_null ===\n");
    zsetFree(NULL);
    printf("   ✅\n");
}

/* ---- 基本 add / find ---- */
static void test_zset_add_find(void)
{
    printf("=== test_zset_add_find ===\n");
    zset *zs = zsetNew();

    int added = zsetAdd(zs, 1.0, sdsnew("alice"));
    assert(added == 1);
    assert(zsetLen(zs) == 1);

    zskiplistNode *n = zsetFind(zs, sdsnew("alice"));
    assert(n != NULL);
    assert(n->score == 1.0);

    /* 不存在的 member */
    n = zsetFind(zs, sdsnew("bob"));
    assert(n == NULL);

    zsetFree(zs);
    printf("   ✅\n");
}

/* ---- 批量 add + rank ---- */
static void test_zset_add_batch(void)
{
    printf("=== test_zset_add_batch ===\n");
    zset *zs = zsetNew();

    /* 插入 100 个元素，score 随机 */
    for (int i = 0; i < 100; i++) {
        char tmp[32]; snprintf(tmp, sizeof(tmp), "member_%d", i);
        int added = zsetAdd(zs, (double)(99 - i), sdsnew(tmp));
        assert(added == 1);
    }
    assert(zsetLen(zs) == 100);

    /* rank 最低分（score=0 是 member_99）*/
    unsigned long r = zsetRank(zs, sdsnew("member_99"));
    assert(r == 1);

    /* rank 最高分（score=99 是 member_0）*/
    r = zsetRank(zs, sdsnew("member_0"));
    assert(r == 100);

    zsetFree(zs);
    printf("   ✅\n");
}

/* ---- 相同 member 不同 score → 更新 ---- */
static void test_zset_add_update_score(void)
{
    printf("=== test_zset_add_update_score ===\n");
    zset *zs = zsetNew();

    int added = zsetAdd(zs, 10.0, sdsnew("x"));
    assert(added == 1);
    assert(zsetLen(zs) == 1);

    /* 同 member，不同 score：更新 */
    added = zsetAdd(zs, 20.0, sdsnew("x"));
    assert(added == 0);             /* 返回 0 = 更新（非新增） */
    assert(zsetLen(zs) == 1);       /* 长度不变 */

    zskiplistNode *n = zsetFind(zs, sdsnew("x"));
    assert(n != NULL);
    assert(n->score == 20.0);       /* score 已更新 */

    /* rank 应该仍是 1（唯一元素） */
    assert(zsetRank(zs, sdsnew("x")) == 1);

    zsetFree(zs);
    printf("   ✅\n");
}

/* ---- 相同 member 相同 score → 无操作 ---- */
static void test_zset_add_same_score(void)
{
    printf("=== test_zset_add_same_score ===\n");
    zset *zs = zsetNew();

    zsetAdd(zs, 5.0, sdsnew("y"));
    int added = zsetAdd(zs, 5.0, sdsnew("y"));
    assert(added == 0);
    assert(zsetLen(zs) == 1);

    zsetFree(zs);
    printf("   ✅\n");
}

/* ---- del ---- */
static void test_zset_del(void)
{
    printf("=== test_zset_del ===\n");
    zset *zs = zsetNew();

    zsetAdd(zs, 1.0, sdsnew("a"));
    zsetAdd(zs, 2.0, sdsnew("b"));
    zsetAdd(zs, 3.0, sdsnew("c"));
    assert(zsetLen(zs) == 3);

    int deleted = zsetDel(zs, sdsnew("b"));
    assert(deleted == 1);
    assert(zsetLen(zs) == 2);

    /* a 和 c 仍在 */
    assert(zsetFind(zs, sdsnew("a")) != NULL);
    assert(zsetFind(zs, sdsnew("b")) == NULL);
    assert(zsetFind(zs, sdsnew("c")) != NULL);

    /* 删除不存在的 */
    deleted = zsetDel(zs, sdsnew("nobody"));
    assert(deleted == 0);
    assert(zsetLen(zs) == 2);

    zsetFree(zs);
    printf("   ✅\n");
}

/* ---- 删除全部 ---- */
static void test_zset_del_all(void)
{
    printf("=== test_zset_del_all ===\n");
    zset *zs = zsetNew();

    for (int i = 0; i < 10; i++) {
        char tmp[32]; snprintf(tmp, sizeof(tmp), "m%d", i);
        zsetAdd(zs, (double)i, sdsnew(tmp));
    }
    assert(zsetLen(zs) == 10);

    for (int i = 0; i < 10; i++) {
        char tmp[32]; snprintf(tmp, sizeof(tmp), "m%d", i);
        int deleted = zsetDel(zs, sdsnew(tmp));
        assert(deleted == 1);
    }
    assert(zsetLen(zs) == 0);

    zsetFree(zs);
    printf("   ✅\n");
}

/* ---- zsetCount score 区间计数 ---- */
static void test_zset_count(void)
{
    printf("=== test_zset_count ===\n");
    zset *zs = zsetNew();

    for (int i = 0; i < 10; i++) {
        char tmp[32]; snprintf(tmp, sizeof(tmp), "m%d", i);
        zsetAdd(zs, (double)(i * 10), sdsnew(tmp));
    }
    assert(zsetCount(zs, 20.0, 60.0) == 5);   /* 20,30,40,50,60 */
    assert(zsetCount(zs, 0.0, 90.0) == 10);
    assert(zsetCount(zs, 100.0, 200.0) == 0);

    zsetFree(zs);
    printf("   ✅\n");
}

/* ---- zsetRange score 区间取节点 ---- */
static void test_zset_range(void)
{
    printf("=== test_zset_range ===\n");
    zset *zs = zsetNew();

    for (int i = 0; i < 10; i++) {
        char tmp[32]; snprintf(tmp, sizeof(tmp), "m%d", i);
        zsetAdd(zs, (double)(i * 10), sdsnew(tmp));
    }

    unsigned long count;
    zskiplistNode **arr = zsetRange(zs, 30.0, 70.0, &count);
    assert(arr != NULL);
    assert(count == 5);   /* 30,40,50,60,70 */
    assert(arr[0]->score == 30.0);
    assert(arr[4]->score == 70.0);
    free(arr);

    zsetFree(zs);
    printf("   ✅\n");
}

/* ---- zsetDelRange 验证 ---- */
static void test_zset_del_range(void)
{
    printf("=== test_zset_del_range ===\n");
    zset *zs = zsetNew();

    for (int i = 0; i < 10; i++) {
        char tmp[32]; snprintf(tmp, sizeof(tmp), "m%d", i);
        zsetAdd(zs, (double)(i * 10), sdsnew(tmp));
    }

    unsigned long removed = zsetDelRange(zs, 20.0, 50.0);
    assert(removed == 4);       /* 20,30,40,50 */
    assert(zsetLen(zs) == 6);

    /* dict 和 skiplist 同步：被删的 member 在 dict 中找不到 */
    assert(zsetFind(zs, sdsnew("m2")) == NULL);
    assert(zsetFind(zs, sdsnew("m5")) == NULL);

    /* 没被删的仍然在 */
    assert(zsetFind(zs, sdsnew("m0")) != NULL);
    assert(zsetFind(zs, sdsnew("m6")) != NULL);

    zsetFree(zs);
    printf("   ✅\n");
}

/* ---- zsetByRank ---- */
static void test_zset_by_rank(void)
{
    printf("=== test_zset_by_rank ===\n");
    zset *zs = zsetNew();

    for (int i = 0; i < 5; i++) {
        char tmp[32]; snprintf(tmp, sizeof(tmp), "m%d", i);
        zsetAdd(zs, (double)(i * 10), sdsnew(tmp));
    }

    zskiplistNode *n = zsetByRank(zs, 1);
    assert(n != NULL && n->score == 0.0);

    n = zsetByRank(zs, 3);
    assert(n != NULL && n->score == 20.0);

    n = zsetByRank(zs, 0);
    assert(n == NULL);

    n = zsetByRank(zs, 100);
    assert(n == NULL);

    zsetFree(zs);
    printf("   ✅\n");
}

/* ---- rank 返回 0 表示不存在 ---- */
static void test_zset_rank_nonexist(void)
{
    printf("=== test_zset_rank_nonexist ===\n");
    zset *zs = zsetNew();
    zsetAdd(zs, 1.0, sdsnew("x"));
    assert(zsetRank(zs, sdsnew("y")) == 0);
    zsetFree(zs);
    printf("   ✅\n");
}

/* ---- 更新 score 后 rank 正确变化 ---- */
static void test_zset_rank_after_update(void)
{
    printf("=== test_zset_rank_after_update ===\n");
    zset *zs = zsetNew();

    zsetAdd(zs, 10.0, sdsnew("a"));
    zsetAdd(zs, 20.0, sdsnew("b"));
    zsetAdd(zs, 30.0, sdsnew("c"));

    assert(zsetRank(zs, sdsnew("a")) == 1);
    assert(zsetRank(zs, sdsnew("c")) == 3);

    /* 更新 a 的 score 到 25.0，应该排到 b 和 c 之间 */
    zsetAdd(zs, 25.0, sdsnew("a"));
    assert(zsetLen(zs) == 3);
    assert(zsetRank(zs, sdsnew("a")) == 2);   /* b(10) < a(25) < c(30) */

    /* b 的 rank 没变 */
    assert(zsetRank(zs, sdsnew("b")) == 1);
    assert(zsetRank(zs, sdsnew("c")) == 3);

    zsetFree(zs);
    printf("   ✅\n");
}

/* ================================================================
 *  入口
 * ================================================================ */

int main(void)
{
    printf("======== ZSet / Skiplist 单元测试 ========\n\n");

    printf("--- zskiplist 纯跳表 ---\n");
    test_zsl_create_empty();
    test_zsl_insert_order();
    test_zsl_insert_reverse();
    test_zsl_insert_duplicate();
    test_zsl_insert_same_score();
    test_zsl_delete_head();
    test_zsl_delete_tail();
    test_zsl_delete_middle();
    test_zsl_delete_nonexist();
    test_zsl_delete_all();
    test_zsl_delete_reinsert();
    test_zsl_rank();
    test_zsl_by_rank();
    test_zsl_count();
    test_zsl_range();
    test_zsl_delrange();
    test_zsl_backward();
    test_zsl_free_null();

    printf("\n--- zset 双索引 (dict + skiplist) ---\n");
    test_zset_create_free();
    test_zset_free_null();
    test_zset_add_find();
    test_zset_add_batch();
    test_zset_add_update_score();
    test_zset_add_same_score();
    test_zset_del();
    test_zset_del_all();
    test_zset_count();
    test_zset_range();
    test_zset_del_range();
    test_zset_by_rank();
    test_zset_rank_nonexist();
    test_zset_rank_after_update();

    printf("\n======== ✅ 全部测试通过 ========\n");
    return 0;
}
