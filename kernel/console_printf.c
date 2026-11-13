#include <kernel/console.h>

#include <stdarg.h>

#define CONSOLE_PRINTF_BUFFER_SIZE 2048U
#define CONSOLE_PRINTF_MAX_DIGITS 21U

typedef __PTRDIFF_TYPE__ console_ssize_t;

typedef struct console_printf_buffer {
    char *data;
    size_t capacity;
    size_t length;
} console_printf_buffer_t;

static void printf_append_char(console_printf_buffer_t *buffer, char value) {
    if (buffer == 0) return;
    if (buffer->length + 1U < buffer->capacity) {
        buffer->data[buffer->length] = value;
    }
    ++buffer->length;
}

static void printf_append_text(console_printf_buffer_t *buffer,
                               const char *text) {
    if (text == 0) text = "(null)";
    while (*text != 0) printf_append_char(buffer, *text++);
}

static void printf_append_unsigned(console_printf_buffer_t *buffer,
                                   uint64_t value, uint32_t base,
                                   bool uppercase) {
    /* 20 位十进制数字，再加上转换循环所需的一个备用位置。 */
    char digits[CONSOLE_PRINTF_MAX_DIGITS];
    uint32_t count = 0U;
    const char *alphabet = uppercase ? "0123456789ABCDEF" :
                                       "0123456789abcdef";
    if (base < 2U || base > 16U) return;
    do {
        digits[count++] = alphabet[value % base];
        value /= base;
    } while (value != 0U && count < sizeof(digits));
    while (count != 0U) printf_append_char(buffer, digits[--count]);
}

static void printf_append_signed(console_printf_buffer_t *buffer, int64_t value) {
    uint64_t magnitude;
    if (value < 0) {
        printf_append_char(buffer, '-');
        /* 避免 INT64_MIN 导致有符号整数溢出。 */
        magnitude = (uint64_t)(-(value + 1)) + 1U;
    } else {
        magnitude = (uint64_t)value;
    }
    printf_append_unsigned(buffer, magnitude, 10U, false);
}

static uint64_t printf_next_unsigned(va_list arguments, bool size_modifier,
                                     uint32_t long_count) {
    if (size_modifier) return (uint64_t)va_arg(arguments, size_t);
    if (long_count >= 2U) return (uint64_t)va_arg(arguments, unsigned long long);
    if (long_count == 1U) return (uint64_t)va_arg(arguments, unsigned long);
    return (uint64_t)va_arg(arguments, unsigned int);
}

static int64_t printf_next_signed(va_list arguments, bool size_modifier,
                                  uint32_t long_count) {
    if (size_modifier) return (int64_t)va_arg(arguments, console_ssize_t);
    if (long_count >= 2U) return (int64_t)va_arg(arguments, long long);
    if (long_count == 1U) return (int64_t)va_arg(arguments, long);
    return (int64_t)va_arg(arguments, int);
}

static size_t console_vformat(char *output, size_t capacity, const char *format,
                             va_list arguments) {
    console_printf_buffer_t buffer = {output, capacity, 0U};
    if (format == 0) return 0U;

    while (*format != 0) {
        if (*format != '%') {
            printf_append_char(&buffer, *format++);
            continue;
        }
        ++format;
        if (*format == '%') {
            printf_append_char(&buffer, *format++);
            continue;
        }

        bool size_modifier = false;
        uint32_t long_count = 0U;
        if (*format == 'z') {
            size_modifier = true;
            ++format;
        } else {
            while (*format == 'l' && long_count < 2U) {
                ++long_count;
                ++format;
            }
        }

        switch (*format) {
            case 'c':
                printf_append_char(&buffer, (char)va_arg(arguments, int));
                break;
            case 's':
                printf_append_text(&buffer, va_arg(arguments, const char *));
                break;
            case 'd':
            case 'i':
                printf_append_signed(&buffer,
                                     printf_next_signed(arguments,
                                                        size_modifier,
                                                        long_count));
                break;
            case 'u':
                printf_append_unsigned(&buffer,
                                       printf_next_unsigned(arguments,
                                                            size_modifier,
                                                            long_count),
                                       10U, false);
                break;
            case 'x':
            case 'X':
                printf_append_unsigned(&buffer,
                                       printf_next_unsigned(arguments,
                                                            size_modifier,
                                                            long_count),
                                       16U, *format == 'X');
                break;
            case 'p': {
                uint64_t pointer = (uint64_t)(uintptr_t)va_arg(arguments, void *);
                printf_append_text(&buffer, "0x");
                printf_append_unsigned(&buffer, pointer, 16U, false);
                break;
            }
            default:
                printf_append_char(&buffer, '%');
                if (*format != 0) printf_append_char(&buffer, *format);
                break;
        }
        if (*format != 0) ++format;
    }

    if (capacity != 0U) {
        size_t terminator = buffer.length < capacity ? buffer.length : capacity - 1U;
        output[terminator] = 0;
    }
    return buffer.length;
}

typedef void (*console_write_fn)(const char *text);

static int console_vprintf_to(const char *format, va_list arguments,
                              console_write_fn write) {
    char output[CONSOLE_PRINTF_BUFFER_SIZE] = {0};
    size_t length;

    /* 现有的大多数启动诊断都使用 printf("%s", text)。保留单一输出目标的语义，
     * 同时避免不必要的有界复制。 */
    if (format != 0 && format[0] == '%' && format[1] == 's' &&
        format[2] == 0) {
        const char *text;
        text = va_arg(arguments, const char *);
        if (text == 0) text = "(null)";
        length = 0U;
        while (text[length] != 0) ++length;
        write(text);
        return length > (size_t)INT32_MAX ? INT32_MAX : (int)length;
    }

    length = console_vformat(output, sizeof(output), format, arguments);
    write(output);
    return length > (size_t)INT32_MAX ? INT32_MAX : (int)length;
}

static int console_vprintf(const char *format, va_list arguments) {
    return console_vprintf_to(format, arguments, kernel_serial_write);
}

int printk(const char *format, ...) {
    va_list arguments;
    int result;
    va_start(arguments, format);
    result = console_vprintf(format, arguments);
    va_end(arguments);
    return result;
}

int printf(const char *format, ...) {
    va_list arguments;
    int result;
    va_start(arguments, format);
    result = console_vprintf(format, arguments);
    va_end(arguments);
    return result;
}

int kernel_serial_printf_serial_only(const char *format, ...) {
    va_list arguments;
    int result;
    va_start(arguments, format);
    result = console_vprintf_to(format, arguments,
                                kernel_serial_write_serial_only);
    va_end(arguments);
    return result;
}
