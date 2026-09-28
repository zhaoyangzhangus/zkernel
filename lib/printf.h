/*
 * printf.h —— 内核格式化输出
 *
 * 支持的转换：
 *   %d %i    有符号十进制        %u     无符号十进制
 *   %x %X    十六进制（大写）    %o     八进制
 *   %p       指针（固定 16 位十六进制，带 0x 前缀）
 *   %c       字符                %s     字符串
 *   %%       百分号
 *
 * 标志：'-' 左对齐  '0' 零填充  '+' 强制正号  ' ' 正数前留空  '#' 进制前缀
 * 宽度/精度：支持数字、也支持 '*'（从参数取）；缺省精度用 '.' 表示 0
 * 长度修饰：hh h l ll z j t
 * 不支持浮点（%f）——内核里要软浮点或 SSE，暂时不需要。
 */
#ifndef __KERNEL_LIB_PRINTF_H__
#define __KERNEL_LIB_PRINTF_H__

#include <stdarg.h>
#include <stddef.h>

/* 借助编译器的 -Wformat 在编译期检查格式串和参数类型 */
#if defined(__GNUC__) || defined(__clang__)
#  define PRINTF_FORMAT(fmt_idx, arg_idx) __attribute__((format(printf, fmt_idx, arg_idx)))
#else
#  define PRINTF_FORMAT(fmt_idx, arg_idx)
#endif

/* 输出回调：把字符 c 交给具体设备或缓冲区，ctx 由调用方提供 */
typedef void (*putc_fn)(void *ctx, char c);

/* 核心格式化函数，printf / snprintf 都只是它的薄封装 */
int kvprintf(putc_fn out, void *ctx, const char *fmt, va_list ap) PRINTF_FORMAT(3, 0);

/* ---------------- 输出到 COM1 串口（drivers/serial.c）---------------- */
int printf(const char *fmt, ...) PRINTF_FORMAT(1, 2);
int vprintf(const char *fmt, va_list ap) PRINTF_FORMAT(1, 0);
int putchar(int c);
int puts(const char *str);            /* 同 C 标准：末尾自动补一个 '\n' */

/* ---------------- 输出到缓冲区 ---------------- */
/*
 * 语义同 C99：最多写 size-1 个字符并补 '\0'，返回“本该写入”的字符数
 * （不含结尾的 '\0'），因此返回值 >= size 表示被截断了。
 */
int snprintf(char *buf, size_t size, const char *fmt, ...) PRINTF_FORMAT(3, 4);
int vsnprintf(char *buf, size_t size, const char *fmt, va_list ap) PRINTF_FORMAT(3, 0);

#endif /* __KERNEL_LIB_PRINTF_H__ */
