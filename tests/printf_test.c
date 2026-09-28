/*
 * printf_test.c —— 在开发机上测试内核 printf 的格式化逻辑
 *
 * 不启动虚拟机：直接用 gcc 把 lib/printf.c 编译成一个普通 Linux 程序，
 * 它唯一的外部依赖 serial_putc() 在这里被重定向到 stdout。
 * 编译方式和运行方式见 Makefile 的 test 目标（make test）。
 *
 * 注意：链接时本程序里的 printf/snprintf 会覆盖 libc 的同名函数（这正是目的），
 * 所以下面的报告输出走的也是内核版本的 printf；本文件只用 libc 的 fputc。
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "../drivers/serial.h"
#include "../lib/printf.h"

/* printf.c 依赖串口驱动，主机上把字符写到 stdout */
void serial_putc(char c)
{
    fputc(c, stdout);
}

static int checks;
static int failures;

/* 期望值、格式串、参数；同时校验返回值和字符串内容 */
#define CHECK(expect, fmt, ...)                                          \
    do {                                                                 \
        char buf[128];                                                   \
        int n = snprintf(buf, sizeof(buf), fmt, __VA_ARGS__);            \
        checks++;                                                        \
        if (strcmp(buf, expect) != 0 || n != (int)strlen(expect)) {      \
            failures++;                                                  \
            printf("FAIL: \"%s\" -> \"%s\"，应为 \"%s\" (ret=%d，应为 %d)\n", \
                   fmt, buf, expect, n, (int)strlen(expect));            \
        }                                                                \
    } while (0)

/* 没有额外参数的格式串 */
#define CHECK0(expect, fmt)                                              \
    do {                                                                 \
        char buf[128];                                                   \
        int n = snprintf(buf, sizeof(buf), fmt);                         \
        checks++;                                                        \
        if (strcmp(buf, expect) != 0 || n != (int)strlen(expect)) {      \
            failures++;                                                  \
            printf("FAIL: \"%s\" -> \"%s\"，应为 \"%s\" (ret=%d)\n",     \
                   fmt, buf, expect, n);                                 \
        }                                                                \
    } while (0)

/* 拼出 "0x" + 零填充 + tail，用来校验零填充和前缀的相对顺序 */
static void build_expected(char *out, int total_len, const char *tail)
{
    int tail_len = (int)strlen(tail);
    int i;

    out[0] = '0';
    out[1] = 'x';
    for (i = 2; i < total_len - tail_len; i++)
        out[i] = '0';
    memcpy(out + total_len - tail_len, tail, (size_t)tail_len + 1);
}

static void test_numbers(void)
{
    CHECK("0", "%d", 0);
    CHECK("42", "%d", 42);
    CHECK("-42", "%d", -42);
    CHECK("+42", "%+d", 42);
    CHECK("-42", "%+d", -42);
    CHECK(" 42", "% d", 42);
    CHECK("2147483647", "%d", 2147483647);
    CHECK("-2147483648", "%d", (-2147483647 - 1));
    CHECK("4294967295", "%u", 4294967295u);
    CHECK("123456789012", "%lld", 123456789012ll);
    CHECK("18446744073709551615", "%llu", 18446744073709551615ull);
    CHECK("-9223372036854775808", "%lld", (-9223372036854775807ll - 1));
    CHECK("-1", "%ld", -1l);
    CHECK("8", "%zu", (size_t)8);

    /* 短类型：默认实参提升，取参数时按 int 取再截断 */
    CHECK("-1", "%hhd", 255);
    CHECK("255", "%hhu", 255);
    CHECK("65535", "%hu", 65535);

    CHECK("2a", "%x", 42);
    CHECK("DEADBEEF", "%X", 0xdeadbeefu);
    CHECK("0x2a", "%#x", 42);
    CHECK("0X2A", "%#X", 42);
    CHECK("0", "%x", 0);
    CHECK("0", "%#x", 0);          /* 值为 0 时 # 不加前缀 */
    CHECK("52", "%o", 42);
    CHECK("052", "%#o", 42);
    CHECK("0", "%#o", 0);
    CHECK("", "%.0o", 0);          /* 精度 0 + 值为 0：什么都不输出 */
    CHECK("0", "%#.0o", 0);        /* 但 # 要求八进制必须有前导 0 */
}

static void test_width_precision(void)
{
    CHECK("   42", "%5d", 42);
    CHECK("42   ", "%-5d", 42);
    CHECK("00042", "%05d", 42);
    CHECK("  -42", "%5d", -42);
    CHECK("-0042", "%05d", -42);   /* 零要加在符号后面 */
    CHECK("00000", "%05u", 0u);
    CHECK("  042", "%5.3d", 42);
    /* 有精度又带 '0' 标志编译器会警告，这里就是要验证这个行为 */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat"
    CHECK("00042", "%05.5d", 42);  /* 有精度时 '0' 标志被忽略 */
#pragma GCC diagnostic pop
    CHECK("42   ", "%-5.2d", 42);
    CHECK("00042", "%.5d", 42);
    CHECK("", "%.0d", 0);
    CHECK("   ", "%3.0d", 0);
    CHECK("  42", "%*d", 4, 42);
    CHECK("42  ", "%-*d", 4, 42);
    CHECK("42  ", "%*d", -4, 42); /* 负宽度等价于左对齐 */
}

static void test_strings(void)
{
    CHECK("hello", "%s", "hello");
    CHECK("  hi", "%4s", "hi");
    CHECK("hi  ", "%-4s", "hi");
    CHECK("hel", "%.3s", "hello");
    CHECK("he", "%.*s", 2, "hello");

    /* 就是要传 NULL 测这个分支，屏蔽编译器对空指针实参的警告 */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat"
#pragma GCC diagnostic ignored "-Wformat-overflow"
    CHECK("(null)", "%s", (const char *)NULL);
    CHECK(" (null)", "%7s", (const char *)NULL);
#pragma GCC diagnostic pop

    CHECK("A", "%c", 'A');
    CHECK("  A", "%3c", 'A');
    CHECK("A  ", "%-3c", 'A');

    CHECK0("纯文字，没有转换", "纯文字，没有转换");
    CHECK0("%", "%%");
    CHECK0("100%", "100%%");

    CHECK("a=1, b=0xff", "a=%d, b=%#x", 1, 0xff);
}

static void test_pointer_and_padding(void)
{
    char buf[32], expect[32];
    int  n;

    /* %#018llx：前缀 0x 在左，零填充在其后 */
    n = snprintf(buf, sizeof(buf), "%#018llx", 42ull);
    build_expected(expect, 18, "2a");
    checks++;
    if (n != 18 || strcmp(buf, expect) != 0) {
        failures++;
        printf("FAIL: %%#018llx -> \"%s\"，应为 \"%s\" (ret=%d)\n", buf, expect, n);
    }

    /* %p：固定 "0x" + 16 位十六进制 */
    n = snprintf(buf, sizeof(buf), "%p", (void *)(uintptr_t)0x2a);
    build_expected(expect, 18, "2a");
    checks++;
    if (n != 18 || strcmp(buf, expect) != 0) {
        failures++;
        printf("FAIL: %%p -> \"%s\"，应为 \"%s\" (ret=%d)\n", buf, expect, n);
    }

    n = snprintf(buf, sizeof(buf), "%p", (void *)NULL);
    build_expected(expect, 18, "");
    checks++;
    if (n != 18 || strcmp(buf, expect) != 0) {
        failures++;
        printf("FAIL: %%p(NULL) -> \"%s\"，应为 \"%s\" (ret=%d)\n", buf, expect, n);
    }
}

static void test_truncation(void)
{
    char small[8];
    char untouched = 'x';
    int  n;

    /* 返回“本该写入”的长度，内容按 size-1 截断并补 '\0' */
    n = snprintf(small, sizeof(small), "%s", "0123456789");
    checks++;
    if (n != 10 || strcmp(small, "0123456") != 0) {
        failures++;
        printf("FAIL: snprintf 截断 -> \"%s\" (ret=%d，应为 10)\n", small, n);
    }

    /* size = 0 时一个字都不写，但返回值照算 */
    n = snprintf(&untouched, 0, "%d", 12345);
    checks++;
    if (n != 5 || untouched != 'x') {
        failures++;
        printf("FAIL: snprintf(size=0) ret=%d，缓冲区被改写为 '%c'\n", n, untouched);
    }

    /* 刚好放得下（size-1 个字符）也要能正常收尾 */
    n = snprintf(small, 4, "%s", "abc");
    checks++;
    if (n != 3 || strcmp(small, "abc") != 0) {
        failures++;
        printf("FAIL: snprintf 边界 -> \"%s\" (ret=%d)\n", small, n);
    }
}

static void test_unknown_conversion(void)
{
    /* 不认识的转换字符原样输出，方便发现格式串写错 */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat"
    CHECK0("%y", "%y");
    CHECK0("100%!", "100%!");
#pragma GCC diagnostic pop
}

int main(void)
{
    test_numbers();
    test_width_precision();
    test_strings();
    test_pointer_and_padding();
    test_truncation();
    test_unknown_conversion();

    if (failures == 0) {
        printf("printf 测试通过：%d 项\n", checks);
        return 0;
    }
    printf("printf 测试失败：%d/%d 项不通过\n", failures, checks);
    return 1;
}
