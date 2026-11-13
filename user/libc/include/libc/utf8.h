#pragma once

#include <stddef.h>
#include <stdint.h>
#include <wchar.h>

/* 宽字符和 C11 Unicode 转换前端共用的内部 UTF-8 原语。它们处理 Unicode 标量值；目标字符表示形式
 * 由调用方选择。 */
void __libc_mbstate_reset(mbstate_t *state);
int __libc_unicode_scalar_valid(uint32_t value);
size_t __libc_utf8_decode(uint32_t *value, const char *multibyte,
                          size_t length, mbstate_t *state);
size_t __libc_utf8_encode(char *multibyte, uint32_t value,
                          mbstate_t *state);
