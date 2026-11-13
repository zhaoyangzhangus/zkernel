#ifndef OS_USER_FONT_RUNTIME_H
#define OS_USER_FONT_RUNTIME_H

#include <stdint.h>
#include <stdbool.h>

#include <uapi/font.h>
#include <uapi/syscall.h>

#define FONT_RUNTIME_DEFAULT_WIDTH   12U
#define FONT_RUNTIME_DEFAULT_HEIGHT  24U
#define FONT_RUNTIME_CACHE_BYTES \
    (OS_FONT_GLYPH_COUNT * OS_FONT_MAX_WIDTH * OS_FONT_MAX_HEIGHT)

/* 这里只保留清零初始化的存储；字形像素在运行时通过 OS_SYS_FONT_CACHE 从内核获取。
 * 映像中不包含任何字体数据。 */
static uint8_t g_font_runtime_cache[FONT_RUNTIME_CACHE_BYTES];
static uint32_t g_font_runtime_width;
static uint32_t g_font_runtime_height;
static uint32_t g_font_runtime_stride;
static bool g_font_runtime_ready;

static inline int64_t font_runtime_syscall_one(uint64_t number,
                                               uint64_t argument) {
    register uint64_t rax __asm__("rax") = number;
    register uint64_t rdi __asm__("rdi") = argument;
    __asm__ volatile ("syscall" : "+a"(rax), "+D"(rdi) :
                      : "rcx", "r11", "memory");
    return (int64_t)rax;
}

static inline bool font_runtime_init(uint32_t width, uint32_t height) {
    os_font_cache_request_t request = {0};

    if (width == 0U || height == 0U ||
        width > OS_FONT_MAX_WIDTH || height > OS_FONT_MAX_HEIGHT) {
        return false;
    }
    if (g_font_runtime_ready && g_font_runtime_width == width &&
        g_font_runtime_height == height) {
        return true;
    }
    request.hdr.size = sizeof(request);
    request.hdr.version = OS_SYSCALL_ABI_VERSION;
    request.width = width;
    request.height = height;
    request.pixels = (uint64_t)(uintptr_t)g_font_runtime_cache;
    request.capacity = sizeof(g_font_runtime_cache);
    if (font_runtime_syscall_one(OS_SYS_FONT_CACHE,
                                 (uint64_t)(uintptr_t)&request) != 0 ||
        request.stride != width || request.glyph_count != OS_FONT_GLYPH_COUNT ||
        request.bytes_written !=
            (uint64_t)width * height * OS_FONT_GLYPH_COUNT) {
        g_font_runtime_ready = false;
        return false;
    }
    g_font_runtime_width = width;
    g_font_runtime_height = height;
    g_font_runtime_stride = request.stride;
    g_font_runtime_ready = true;
    return true;
}

static inline uint32_t font_runtime_width(void) {
    return g_font_runtime_width;
}

static inline uint32_t font_runtime_height(void) {
    return g_font_runtime_height;
}

static inline const uint8_t *font_runtime_glyph(char character) {
    uint32_t code = (uint8_t)character;

    if (!g_font_runtime_ready) return 0;
    if (code < OS_FONT_FIRST || code > OS_FONT_LAST) code = '?';
    return g_font_runtime_cache +
        (uint64_t)(code - OS_FONT_FIRST) * g_font_runtime_stride *
        g_font_runtime_height;
}

static inline uint32_t font_runtime_blend_xrgb8888(uint32_t destination,
                                                   uint32_t source,
                                                   uint8_t alpha) {
    uint32_t a = alpha;
    uint32_t inverse = 255U - a;
    uint32_t dr = (destination >> 16U) & 0xFFU;
    uint32_t dg = (destination >> 8U) & 0xFFU;
    uint32_t db = destination & 0xFFU;
    uint32_t sr = (source >> 16U) & 0xFFU;
    uint32_t sg = (source >> 8U) & 0xFFU;
    uint32_t sb = source & 0xFFU;
    uint32_t r = (sr * a + dr * inverse + 127U) / 255U;
    uint32_t g = (sg * a + dg * inverse + 127U) / 255U;
    uint32_t b = (sb * a + db * inverse + 127U) / 255U;

    return (destination & 0xFF000000U) |
           (r << 16U) | (g << 8U) | b;
}

static inline void font_runtime_draw_glyph(uint32_t *pixels,
                                           uint32_t stride,
                                           uint32_t width,
                                           uint32_t height,
                                           int32_t x, int32_t y,
                                           char character, uint32_t color) {
    const uint8_t *glyph = font_runtime_glyph(character);
    uint32_t glyph_width = font_runtime_width();
    uint32_t glyph_height = font_runtime_height();

    if (pixels == 0 || glyph == 0 || glyph_width == 0U ||
        glyph_height == 0U) return;
    for (uint32_t row = 0U; row < glyph_height; ++row) {
        int32_t destination_y = y + (int32_t)row;
        if (destination_y < 0 || destination_y >= (int32_t)height) continue;
        for (uint32_t column = 0U; column < glyph_width; ++column) {
            int32_t destination_x = x + (int32_t)column;
            uint8_t alpha;
            uint32_t *destination;

            if (destination_x < 0 || destination_x >= (int32_t)width) continue;
            alpha = glyph[(uint64_t)row * glyph_width + column];
            if (alpha == 0U) continue;
            destination = &pixels[(uint64_t)(uint32_t)destination_y * stride +
                                  (uint32_t)destination_x];
            *destination = alpha == 255U ? color :
                font_runtime_blend_xrgb8888(*destination, color, alpha);
        }
    }
}

#define FONT_RUNTIME_WIDTH  (font_runtime_width())
#define FONT_RUNTIME_HEIGHT (font_runtime_height())

#endif
