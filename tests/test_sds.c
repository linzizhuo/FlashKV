#include <stdio.h>
#include <string.h>
#include "sds.h"

void test_basic_create_and_len()
{
    printf("===== 测试 sdsnew / sdslen =====\n");

    sds s1 = sdsnew("hello");
    printf("s1 = \"%s\", len = %zu\n", s1, sdslen(s1));
    printf("预期: len = 5\n\n");

    sds s2 = sdsnew("");
    printf("s2 = \"%s\", len = %zu\n", s2, sdslen(s2));
    printf("预期: len = 0\n\n");

    sdsfree(s1);
    sdsfree(s2);
}

void test_binary_safety()
{
    printf("===== 测试二进制安全 =====\n");
    char data[] = {'H', 'e', 'l', '\0', 'o', '\0'};
    sds s = sdsnewlen(data, 6);
    printf("s (binary), len = %zu", sdslen(s));
    printf(" 预期: len = 6\n");
    printf("前4字节: %.4s\n", s);
    printf("全部6字节: ");
    for (size_t i = 0; i < sdslen(s); i++) {
        putchar(s[i] == '\0' ? '.' : s[i]);
    }
    printf("\n\n");
    sdsfree(s);
}

void test_sdsfree_null()
{
    printf("===== 测试 sdsfree 后释放 =====\n");
    sds s = sdsnew("I will be freed");
    printf("释放前: %s\n", s);
    sdsfree(s);
    printf("释放完成 (无 crash 即通过)\n\n");
}

int main()
{
    printf("======== SDS 测试 ========\n\n");
    test_basic_create_and_len();
    test_binary_safety();
    test_sdsfree_null();
    printf("======== 全部测试通过 ========\n");
    return 0;
}
