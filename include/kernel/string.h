#pragma once
#include <kernel/base.h>

/* 独立运行实现；不依赖宿主 CRT 或 SIMD。 */
void *memset(void *destination, int value, size_t length);
void *memcpy(void *destination, const void *source, size_t length);
void *memmove(void *destination, const void *source, size_t length);
int memcmp(const void *left, const void *right, size_t length);
