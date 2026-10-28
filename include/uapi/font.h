#pragma once

#include "abi.h"

/* 系统界面共用的字体资源；应用不能另行选择内置控件字体。 */
#define OS_FONT_DEFAULT_PATH "/EFI/OS/fonts/default.ttf"
#define OS_FONT_FALLBACK_PATH "/EFI/OS/fonts/cjk.ttf"

#define OS_FONT_FIRST           0x20U
#define OS_FONT_LAST            0x7EU
#define OS_FONT_GLYPH_COUNT     (OS_FONT_LAST - OS_FONT_FIRST + 1U)
#define OS_FONT_MAX_WIDTH       64U
#define OS_FONT_MAX_HEIGHT      64U

/*
 * TrueType 源文件由内核管理，并按请求的运行时尺寸返回完整的 ASCII A8 缓存。
 * `pixels` 指向可写用户内存；字形连续排列，每像素一字节且没有填充。
 */
typedef struct os_font_cache_request {
    os_versioned_header_t hdr;
    uint32_t width;
    uint32_t height;
    uint32_t stride;
    uint32_t glyph_count;
    uint64_t pixels;
    uint64_t capacity;
    uint64_t bytes_written;
} os_font_cache_request_t;
