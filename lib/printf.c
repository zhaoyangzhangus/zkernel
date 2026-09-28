/*
 * printf.c —— 内核格式化输出
 *
 * 实现 printf 家族的一个子集，不依赖任何库：
 *   printf / vprintf / putchar / puts    → COM1 串口
 *   snprintf / vsnprintf                 → 缓冲区
 *   kvprintf                             → 核心，把字符交给任意回调
 *
 * 思路：格式化过程完全不关心最终输出到哪里，只调用 putc_fn 逐字符发出；
 * 于是串口输出和写缓冲区可以共用同一套代码。
 *
 * 与 C 标准的差异（都是内核里更实用的做法）：
 *   - %p 固定输出 16 位十六进制（0x0000000000001234），不是最短表示
 *   - 不认识的转换字符原样输出（如 "%q" 输出 "%q"），方便发现写错的地方
 *   - 不支持 %f / %n
 */
#include "printf.h"
#include "../drivers/serial.h"

#include <stdbool.h>
#include <stdint.h>

/* 64 位二进制最多 64 个数字，加前缀和符号留点余量 */
#define NUM_BUF_SIZE 80

/* ------------------------------------------------------------------ */
/* 输出抽象                                                            */
/* ------------------------------------------------------------------ */
typedef struct {
    putc_fn putc;
    void   *ctx;
    int     count;      /* 已输出的字符数，作为返回值 */
} sink_t;

static void sink_putc(sink_t *s, char c)
{
    s->putc(s->ctx, c);
    s->count++;
}

static void sink_fill(sink_t *s, char c, int n)
{
    while (n-- > 0)
        sink_putc(s, c);
}

/* ------------------------------------------------------------------ */
/* 一条转换规格：%[标志][宽度][.精度][长度]转换字符                     */
/* ------------------------------------------------------------------ */
typedef enum {
    LEN_NONE, LEN_HH, LEN_H, LEN_L, LEN_LL, LEN_Z, LEN_J, LEN_T
} length_t;

typedef struct {
    bool     left;      /* '-' */
    bool     zero;      /* '0' */
    bool     plus;      /* '+' */
    bool     space;     /* ' ' */
    bool     alt;       /* '#' */
    bool     ptr;       /* 由 %p 设置：即使值为 0 也要带 0x 前缀 */
    int      width;     /* -1 表示未指定 */
    int      prec;      /* -1 表示未指定 */
    length_t len;
} spec_t;

/* 按长度修饰符取参数。注意默认实参提升：char/short 都是按 int 传进来的 */
static int64_t fetch_signed(const spec_t *sp, va_list ap)
{
    switch (sp->len) {
    case LEN_HH: return (int64_t)(signed char)va_arg(ap, int);
    case LEN_H:  return (int64_t)(short)va_arg(ap, int);
    case LEN_L:  return (int64_t)va_arg(ap, long);
    case LEN_LL: return (int64_t)va_arg(ap, long long);
    /* LP64 下 ssize_t / intmax_t / ptrdiff_t 都是 long */
    case LEN_Z:
    case LEN_J:
    case LEN_T:  return (int64_t)va_arg(ap, long);
    default:     return (int64_t)va_arg(ap, int);
    }
}

static uint64_t fetch_unsigned(const spec_t *sp, va_list ap)
{
    switch (sp->len) {
    case LEN_HH: return (uint64_t)(unsigned char)va_arg(ap, int);
    case LEN_H:  return (uint64_t)(unsigned short)va_arg(ap, int);
    case LEN_L:  return (uint64_t)va_arg(ap, unsigned long);
    case LEN_LL: return (uint64_t)va_arg(ap, unsigned long long);
    case LEN_Z:
    case LEN_J:
    case LEN_T:  return (uint64_t)va_arg(ap, unsigned long);
    default:     return (uint64_t)va_arg(ap, unsigned int);
    }
}

/* ------------------------------------------------------------------ */
/* 输出一个整数：处理符号、进制前缀、精度、宽度填充                     */
/* ------------------------------------------------------------------ */
static void emit_number(sink_t *s, const spec_t *sp, uint64_t value, bool negative,
                        unsigned base, bool upper)
{
    const char *digit_set = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    char digits[NUM_BUF_SIZE];
    char prefix[2];
    char sign = '\0';
    bool zero_value = (value == 0);
    bool pad_with_zero;
    int  n = 0, prefix_len = 0, sign_len, pad, i;

    /* 1. 生成数字（低位在前）。精度为 0 且值为 0 时不输出任何数字 */
    if (!(sp->prec == 0 && zero_value)) {
        do {
            digits[n++] = digit_set[value % base];
            value /= base;
        } while (value != 0);
    }

    /* 2. 精度只约束数字本身，补前导零，符号和前缀不受影响 */
    while (n < sp->prec)
        digits[n++] = '0';

    /* 3. 符号 */
    if (negative)       sign = '-';
    else if (sp->plus)  sign = '+';
    else if (sp->space) sign = ' ';
    sign_len = (sign != '\0') ? 1 : 0;

    /* 4. 进制前缀。'#' 对 0 不加前缀，但 %p 例外，空指针也要写成 0x0… */
    if (sp->alt && base == 16 && (!zero_value || sp->ptr)) {
        prefix[0] = '0';
        prefix[1] = upper ? 'X' : 'x';
        prefix_len = 2;
    } else if (sp->alt && base == 8 && (n == 0 || digits[n - 1] != '0')) {
        digits[n++] = '0';      /* 八进制靠前导 0 表示 */
    }

    /* 5. 宽度填充：'0' 标志在有精度或左对齐时按标准忽略 */
    pad = sp->width - sign_len - prefix_len - n;
    if (pad < 0)
        pad = 0;
    pad_with_zero = sp->zero && !sp->left && sp->prec < 0;

    if (!sp->left && !pad_with_zero)
        sink_fill(s, ' ', pad);

    if (sign_len != 0)
        sink_putc(s, sign);
    for (i = 0; i < prefix_len; i++)
        sink_putc(s, prefix[i]);

    if (pad_with_zero)
        sink_fill(s, '0', pad);

    for (i = n - 1; i >= 0; i--)
        sink_putc(s, digits[i]);

    if (sp->left)
        sink_fill(s, ' ', pad);
}

static void emit_string(sink_t *s, const spec_t *sp, const char *str)
{
    int len = 0, pad, i;

    if (str == NULL)
        str = "(null)";

    if (sp->prec >= 0) {
        while (len < sp->prec && str[len] != '\0')
            len++;
    } else {
        while (str[len] != '\0')
            len++;
    }

    pad = sp->width - len;
    if (pad < 0)
        pad = 0;

    if (!sp->left)
        sink_fill(s, ' ', pad);
    for (i = 0; i < len; i++)
        sink_putc(s, str[i]);
    if (sp->left)
        sink_fill(s, ' ', pad);
}

static void emit_char(sink_t *s, const spec_t *sp, char c)
{
    int pad = (sp->width > 1) ? sp->width - 1 : 0;

    if (!sp->left)
        sink_fill(s, ' ', pad);
    sink_putc(s, c);
    if (sp->left)
        sink_fill(s, ' ', pad);
}

/* ------------------------------------------------------------------ */
/* 解析 % 后面的一串标志/宽度/精度/长度，返回指向转换字符的位置         */
/* ------------------------------------------------------------------ */
static const char *parse_spec(const char *p, spec_t *sp, va_list ap)
{
    sp->left = sp->zero = sp->plus = sp->space = sp->alt = sp->ptr = false;
    sp->width = -1;
    sp->prec = -1;
    sp->len = LEN_NONE;

    /* 标志可以重复、顺序随意 */
    for (;;) {
        if (*p == '-')      sp->left = true;
        else if (*p == '0') sp->zero = true;
        else if (*p == '+') sp->plus = true;
        else if (*p == ' ') sp->space = true;
        else if (*p == '#') sp->alt = true;
        else                break;
        p++;
    }

    /* 宽度：数字或 '*' */
    if (*p == '*') {
        int w = va_arg(ap, int);

        if (w < 0) {            /* 负宽度等价于左对齐 + 正宽度 */
            sp->left = true;
            w = -w;
        }
        sp->width = w;
        p++;
    } else {
        while (*p >= '0' && *p <= '9')
            sp->width = (sp->width < 0 ? 0 : sp->width) * 10 + (*p++ - '0');
    }

    /* 精度：'.' 后面跟数字或 '*'，不写数字表示 0 */
    if (*p == '.') {
        p++;
        sp->prec = 0;
        if (*p == '*') {
            int pr = va_arg(ap, int);

            if (pr < 0)             /* 负精度当作没写 */
                sp->prec = -1;
            else
                sp->prec = pr;
            p++;
        } else {
            while (*p >= '0' && *p <= '9')
                sp->prec = sp->prec * 10 + (*p++ - '0');
        }
    }

    /* 长度修饰符 */
    switch (*p) {
    case 'h':
        p++;
        if (*p == 'h') { p++; sp->len = LEN_HH; }
        else           {      sp->len = LEN_H;  }
        break;
    case 'l':
        p++;
        if (*p == 'l') { p++; sp->len = LEN_LL; }
        else           {      sp->len = LEN_L;  }
        break;
    case 'z': p++; sp->len = LEN_Z; break;
    case 'j': p++; sp->len = LEN_J; break;
    case 't': p++; sp->len = LEN_T; break;
    default:
        break;
    }

    return p;
}

/* ------------------------------------------------------------------ */
/* 核心：逐字符扫描格式串                                              */
/* ------------------------------------------------------------------ */
int kvprintf(putc_fn out, void *ctx, const char *fmt, va_list ap)
{
    sink_t s = { out, ctx, 0 };

    while (*fmt != '\0') {
        spec_t sp;

        if (*fmt != '%') {
            sink_putc(&s, *fmt++);
            continue;
        }

        fmt++;                          /* 跳过 '%' */

        if (*fmt == '%') {              /* "%%" */
            sink_putc(&s, '%');
            fmt++;
            continue;
        }

        fmt = parse_spec(fmt, &sp, ap);

        switch (*fmt) {
        case 'd':
        case 'i': {
            int64_t  v   = fetch_signed(&sp, ap);
            bool     neg = (v < 0);
            /* 先加 1 再取负，避免 -INT64_MIN 溢出 */
            uint64_t mag = neg ? (uint64_t)(-(v + 1)) + 1 : (uint64_t)v;

            emit_number(&s, &sp, mag, neg, 10, false);
            break;
        }
        case 'u':
            emit_number(&s, &sp, fetch_unsigned(&sp, ap), false, 10, false);
            break;
        case 'x':
            emit_number(&s, &sp, fetch_unsigned(&sp, ap), false, 16, false);
            break;
        case 'X':
            emit_number(&s, &sp, fetch_unsigned(&sp, ap), false, 16, true);
            break;
        case 'o':
            emit_number(&s, &sp, fetch_unsigned(&sp, ap), false, 8, false);
            break;
        case 'p': {
            spec_t ps = sp;             /* 固定 16 位、带 0x 前缀，方便对齐查看 */

            ps.alt  = true;
            ps.ptr  = true;
            ps.prec = 16;
            emit_number(&s, &ps, (uint64_t)(uintptr_t)va_arg(ap, void *), false, 16, false);
            break;
        }
        case 'c':
            emit_char(&s, &sp, (char)va_arg(ap, int));
            break;
        case 's':
            emit_string(&s, &sp, va_arg(ap, const char *));
            break;
        default:
            /* 不认识的转换：原样打出来 */
            sink_putc(&s, '%');
            if (*fmt != '\0') {
                sink_putc(&s, *fmt);
                fmt++;
            }
            continue;                   /* fmt 已经自己前进过了 */
        }

        fmt++;
    }

    return s.count;
}

/* ------------------------------------------------------------------ */
/* 串口输出                                                            */
/* ------------------------------------------------------------------ */
static void serial_sink(void *ctx, char c)
{
    (void)ctx;
    serial_putc(c);
}

int vprintf(const char *fmt, va_list ap)
{
    return kvprintf(serial_sink, NULL, fmt, ap);
}

int printf(const char *fmt, ...)
{
    va_list ap;
    int n;

    va_start(ap, fmt);
    n = vprintf(fmt, ap);
    va_end(ap);
    return n;
}

int putchar(int c)
{
    serial_putc((char)c);
    return (int)(unsigned char)c;
}

int puts(const char *str)
{
    while (*str != '\0')
        serial_putc(*str++);
    serial_putc('\n');
    return 0;
}

/* ------------------------------------------------------------------ */
/* 缓冲区输出（内核里拼字符串、格式化日志用）                           */
/* ------------------------------------------------------------------ */
typedef struct {
    char  *buf;
    size_t size;
    size_t pos;     /* 已接收的字符数，可能超过 size（用于计算返回值） */
} buf_sink_t;

static void buffer_sink(void *ctx, char c)
{
    buf_sink_t *b = ctx;

    if (b->size != 0 && b->pos + 1 < b->size)
        b->buf[b->pos] = c;
    b->pos++;
}

int vsnprintf(char *buf, size_t size, const char *fmt, va_list ap)
{
    buf_sink_t b = { buf, size, 0 };
    int n = kvprintf(buffer_sink, &b, fmt, ap);

    if (size != 0)
        buf[(b.pos < size) ? b.pos : size - 1] = '\0';
    return n;
}

int snprintf(char *buf, size_t size, const char *fmt, ...)
{
    va_list ap;
    int n;

    va_start(ap, fmt);
    n = vsnprintf(buf, size, fmt, ap);
    va_end(ap);
    return n;
}
