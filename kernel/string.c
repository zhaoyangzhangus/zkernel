#include <kernel/string.h>

_Static_assert(sizeof(size_t) == sizeof(void *), "kernel string length ABI");

/* 内核对编译器生成的字节操作提供的独立实现。 */
void *memset(void *destination, int value, size_t length) {
    unsigned char *bytes = (unsigned char *)destination;
    unsigned char byte = (unsigned char)value;
#if defined(__x86_64__)
    if (length >= 64U) {
        unsigned long long words = length / 8U;
        const unsigned long long pattern =
            (unsigned long long)byte * 0x0101010101010101ULL;
        /* 整数型字符串指令不会触碰内核的 FPU 状态。
         * 仅访问完整字，尾部数据始终位于 length 范围内。 */
        __asm__ volatile("cld; rep stosq"
                         : "+D"(bytes), "+c"(words)
                         : "a"(pattern)
                         : "memory", "cc");
        length %= 8U;
    }
#endif
    for (size_t index = 0U; index < length; ++index) bytes[index] = byte;
    return destination;
}

/* 目标位于源地址之前时可以前向复制；此辅助函数不附加 memcpy 的不重叠约束。 */
static void copy_forward(void *destination, const void *source, size_t length) {
    unsigned char *out = (unsigned char *)destination;
    const unsigned char *in = (const unsigned char *)source;
#if defined(__x86_64__)
    if (length >= 64U) {
        /* 按字节计数，任意对齐和尾部统一由字符串指令处理，不触碰 SIMD 状态。 */
        __asm__ volatile("cld; rep movsb"
                         : "+D"(out), "+S"(in), "+c"(length)
                         :
                         : "memory", "cc");
        return;
    }
#endif
    for (size_t index = 0U; index < length; ++index) out[index] = in[index];
}

void *memcpy(void *destination, const void *source, size_t length) {
    copy_forward(destination, source, length);
    return destination;
}

void *memmove(void *destination, const void *source, size_t length) {
    unsigned char *out = (unsigned char *)destination;
    const unsigned char *in = (const unsigned char *)source;
    if (out == in || length == 0U) return destination;
    uintptr_t to = (uintptr_t)out, from = (uintptr_t)in;
    if ((to < from && from - to >= length) ||
        (to >= from && to - from >= length)) {
        return memcpy(destination, source, length);
    }
    if (to < from) {
        copy_forward(destination, source, length);
    } else {
        /* 存储前先加载完整的尾部字：即使只重叠一个字节，也不会破坏下一个字所需的源数据。
         * 固定大小的复制可处理未对齐和别名缓冲区，无需设置 DF 或触碰 SIMD 状态；
         * 剩余前缀按字节处理。 */
        if (length >= 64U) {
            while (length >= sizeof(uint64_t)) {
                uint64_t word;
                length -= sizeof(word);
                __builtin_memcpy(&word, in + length, sizeof(word));
                __builtin_memcpy(out + length, &word, sizeof(word));
            }
        }
        for (size_t index = length; index != 0U; --index) {
            out[index - 1U] = in[index - 1U];
        }
    }
    return destination;
}

int memcmp(const void *left, const void *right, size_t length) {
    const unsigned char *a = (const unsigned char *)left;
    const unsigned char *b = (const unsigned char *)right;
    /* 跳过相同的完整字，且不读取请求范围之外的数据。
     * 复制操作允许缓冲区未对齐或彼此别名；遇到不匹配时仍按字节顺序比较，
     * 不采用主机整数的排序方式。 */
    while (length >= sizeof(uint64_t)) {
        uint64_t word_a, word_b;
        __builtin_memcpy(&word_a, a, sizeof(word_a));
        __builtin_memcpy(&word_b, b, sizeof(word_b));
        if (word_a != word_b) break;
        a += sizeof(uint64_t);
        b += sizeof(uint64_t);
        length -= sizeof(uint64_t);
    }
    for (size_t index = 0U; index < length; ++index) {
        if (a[index] != b[index]) {
            return a[index] < b[index] ? -1 : 1;
        }
    }
    return 0;
}
