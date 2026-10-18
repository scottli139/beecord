// 极简断言宏：零三方依赖（D3）。每个 test_*.c 是独立可执行文件，自带 main。
#ifndef TEST_H
#define TEST_H

#include <stdio.h>

static int g_test_count = 0;
static int g_test_failed = 0;

// 断言条件成立；失败打印位置与表达式，继续执行后续断言。
#define CHECK(cond)                                                         \
    do {                                                                    \
        g_test_count++;                                                     \
        if (!(cond)) {                                                      \
            g_test_failed++;                                                \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        }                                                                   \
    } while (0)

// 断言整数相等；失败时打印实际值与期望值。
#define CHECK_EQ_INT(actual, expected)                                       \
    do {                                                                     \
        long a_ = (long)(actual);                                            \
        long e_ = (long)(expected);                                          \
        g_test_count++;                                                      \
        if (a_ != e_) {                                                      \
            g_test_failed++;                                                 \
            fprintf(stderr, "FAIL %s:%d: %s == %ld, want %ld\n", __FILE__,   \
                    __LINE__, #actual, a_, e_);                              \
        }                                                                    \
    } while (0)

// 运行一个测试函数，打印单测结果行。
#define RUN_TEST(fn)                                                        \
    do {                                                                    \
        int fail_before_ = g_test_failed;                                   \
        fn();                                                               \
        printf("%-42s %s\n", #fn,                                           \
               g_test_failed == fail_before_ ? "PASS" : "FAIL");            \
    } while (0)

// 打印总结果；返回值作为 main 退出码（0 = 全绿）。
static int test_report(void)
{
    printf("----\n%d checks, %d failed\n", g_test_count, g_test_failed);
    return g_test_failed == 0 ? 0 : 1;
}

#endif // TEST_H
