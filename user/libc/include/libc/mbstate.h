#pragma once

/*
 * 多字节转换状态由运行时持有。将其放在小型头文件中，以便 libc++ 的 __mbstate_t 兼容性探测也能
 * 使用。
 */
#include <stdint.h>

typedef struct mbstate {
    uint32_t codepoint;
    uint32_t minimum;
    uint16_t surrogate;
    unsigned char expected;
} mbstate_t;
