#ifndef OS_USER_CLIENT_CHROME_H
#define OS_USER_CLIENT_CHROME_H

#include <stdint.h>
#include <stdbool.h>
#include <uapi/window_theme.h>
#include "runtime/text.h"

/* 共用的 Chrome 风格客户端装饰，尺寸根据当前字体确定。WINDOW_UPDATE 会将这些度量值发布给
 * 服务器的标题栏命中测试。 */
static inline uint32_t user_client_chrome_height(void) {
    uint32_t height = ui_text_line_height() + 16U;
    return height > 40U ? height : 40U;
}
#define USER_CLIENT_CHROME_HEIGHT       (user_client_chrome_height())
#define USER_CLIENT_CHROME_TITLE_Y      ((USER_CLIENT_CHROME_HEIGHT - ui_text_line_height()) / 2U)
#define USER_CLIENT_CHROME_BACKGROUND   OS_THEME_HEADER
#define USER_CLIENT_CHROME_SEPARATOR    OS_THEME_BORDER
#define USER_CLIENT_CHROME_BORDER       OS_THEME_BORDER
#define USER_CLIENT_CHROME_TEXT         OS_THEME_TEXT
#define USER_CLIENT_CHROME_CLOSE_BG     OS_THEME_HEADER
#define USER_CLIENT_CHROME_CLOSE_FG     OS_THEME_MUTED
#define USER_CLIENT_CHROME_ICON_BG      OS_THEME_FIELD
#define USER_CLIENT_CHROME_ICON_FG      OS_THEME_MUTED
#define USER_CLIENT_CHROME_ICON_ACCENT  OS_THEME_ACCENT
#define USER_CLIENT_CHROME_CARD         OS_THEME_SURFACE
#define USER_CLIENT_CHROME_CARD_BORDER  OS_THEME_BORDER

static inline uint32_t user_client_chrome_blend_xrgb8888(
    uint32_t destination, uint32_t source, uint8_t alpha) {
    uint32_t inverse = 255U - alpha;
    uint32_t red = (((source >> 16U) & 0xFFU) * alpha +
                    ((destination >> 16U) & 0xFFU) * inverse + 127U) / 255U;
    uint32_t green = (((source >> 8U) & 0xFFU) * alpha +
                      ((destination >> 8U) & 0xFFU) * inverse + 127U) / 255U;
    uint32_t blue = ((source & 0xFFU) * alpha +
                     (destination & 0xFFU) * inverse + 127U) / 255U;
    return (destination & 0xFF000000U) | (red << 16U) |
           (green << 8U) | blue;
}

#define USER_CLIENT_CHROME_ICON_TERMINAL 1U
#define USER_CLIENT_CHROME_ICON_NOTE     2U
#define USER_CLIENT_CHROME_ICON_NETWORK  3U
#define USER_CLIENT_CHROME_ICON_IMAGE    4U
#define USER_CLIENT_CHROME_ICON_TASKS    5U

/* 与 Fileman 客户端关闭操作使用的相同 16x16 alpha 光栅图。 */
static const char user_client_close_alpha[16][17] = {
    "FE30000000003EFE", "EFE300000003EFE3",
    "3EFE3000003EFE30", "03EFE30003EFE300",
    "003EFE303EFE3000", "0003EFE6EFE30000",
    "00003EFFFE300000", "000006FFF6000000",
    "00003EFFFE300000", "0003EFE6EFE30000",
    "003EFE303EFE3000", "03EFE30003EFE300",
    "3EFE3000003EFE30", "EFE300000003EFE3",
    "FE30000000003EFE", "E3000000000003EF",
};

static inline void user_client_chrome_pixel(uint32_t *pixels,
                                            uint32_t stride,
                                            uint32_t width,
                                            uint32_t height,
                                            uint32_t x, uint32_t y,
                                            uint32_t color, uint8_t alpha) {
    uint32_t *destination;
    if (pixels == 0 || x >= width || y >= height || alpha == 0U) return;
    destination = &pixels[(uint64_t)y * stride + x];
    if (alpha == 255U) {
        *destination = color;
    } else {
        *destination = user_client_chrome_blend_xrgb8888(*destination,
                                                         color, alpha);
    }
}

static inline void user_client_chrome_fill_rect(uint32_t *pixels,
                                                uint32_t stride,
                                                uint32_t width,
                                                uint32_t height,
                                                uint32_t x, uint32_t y,
                                                uint32_t rect_width,
                                                uint32_t rect_height,
                                                uint32_t color) {
    if (pixels == 0 || x >= width || y >= height || rect_width == 0U ||
        rect_height == 0U) return;
    if (rect_width > width - x) rect_width = width - x;
    if (rect_height > height - y) rect_height = height - y;
    for (uint32_t row = 0U; row < rect_height; ++row) {
        uint32_t *destination = pixels + (uint64_t)(y + row) * stride + x;
        for (uint32_t column = 0U; column < rect_width; ++column) {
            destination[column] = color;
        }
    }
}

/* 共用的小型圆角图形原语可保持非 Files 客户端的视觉一致性，同时避免依赖 Fileman 的渲染器。 */
static inline void user_client_chrome_round_rect(uint32_t *pixels,
                                                 uint32_t stride,
                                                 uint32_t width,
                                                 uint32_t height,
                                                 uint32_t x, uint32_t y,
                                                 uint32_t rect_width,
                                                 uint32_t rect_height,
                                                 uint32_t color) {
    static const uint8_t inset[8] = {5U, 3U, 2U, 1U, 1U, 0U, 0U, 0U};
    if (rect_width == 0U || rect_height == 0U) return;
    if (rect_width < 12U || rect_height < 12U) {
        user_client_chrome_fill_rect(pixels, stride, width, height,
                                     x, y, rect_width, rect_height, color);
        return;
    }
    for (uint32_t row = 0U; row < rect_height; ++row) {
        uint32_t edge = row < 8U ? row : rect_height - 1U - row;
        uint32_t cut = edge < 8U ? inset[edge] : 0U;
        if (cut * 2U >= rect_width) continue;
        user_client_chrome_fill_rect(pixels, stride, width, height,
                                     x + cut, y + row,
                                     rect_width - cut * 2U, 1U, color);
    }
}

/* 恢复的边框由服务器绘制；客户端只绘制分隔线。 */
static inline void user_client_chrome_frame(uint32_t *pixels,
                                            uint32_t stride,
                                            uint32_t width,
                                            uint32_t height,
                                            uint32_t header_height) {
    if (pixels == 0 || width < 2U || height < 2U) return;

    if (header_height > 0U && header_height < height) {
        user_client_chrome_fill_rect(pixels, stride, width, height,
                                     0U, header_height - 1U, width, 1U,
                                     USER_CLIENT_CHROME_SEPARATOR);
    }
}

static inline void user_client_chrome_app_icon(uint32_t *pixels,
                                               uint32_t stride,
                                               uint32_t width,
                                               uint32_t height,
                                               uint32_t x, uint32_t y,
                                               uint32_t kind) {
    user_client_chrome_round_rect(pixels, stride, width, height,
                                  x, y, 24U, 24U,
                                  USER_CLIENT_CHROME_ICON_BG);
    if (kind == USER_CLIENT_CHROME_ICON_TERMINAL) {
        user_client_chrome_round_rect(pixels, stride, width, height,
                                      x + 5U, y + 6U, 14U, 12U,
                                      USER_CLIENT_CHROME_ICON_FG);
        user_client_chrome_fill_rect(pixels, stride, width, height,
                                     x + 8U, y + 9U, 3U, 2U,
                                     USER_CLIENT_CHROME_ICON_BG);
        user_client_chrome_fill_rect(pixels, stride, width, height,
                                     x + 10U, y + 11U, 2U, 2U,
                                     USER_CLIENT_CHROME_ICON_BG);
        user_client_chrome_fill_rect(pixels, stride, width, height,
                                     x + 13U, y + 15U, 4U, 1U,
                                     USER_CLIENT_CHROME_ICON_BG);
    } else if (kind == USER_CLIENT_CHROME_ICON_NOTE) {
        user_client_chrome_round_rect(pixels, stride, width, height,
                                      x + 6U, y + 4U, 12U, 16U,
                                      0x00FFFFFFU);
        user_client_chrome_fill_rect(pixels, stride, width, height,
                                     x + 9U, y + 9U, 6U, 1U,
                                     USER_CLIENT_CHROME_ICON_ACCENT);
        user_client_chrome_fill_rect(pixels, stride, width, height,
                                     x + 9U, y + 12U, 6U, 1U,
                                     USER_CLIENT_CHROME_ICON_ACCENT);
        user_client_chrome_fill_rect(pixels, stride, width, height,
                                     x + 9U, y + 15U, 4U, 1U,
                                     USER_CLIENT_CHROME_ICON_ACCENT);
    } else if (kind == USER_CLIENT_CHROME_ICON_NETWORK) {
        user_client_chrome_fill_rect(pixels, stride, width, height,
                                     x + 11U, y + 8U, 2U, 9U,
                                     USER_CLIENT_CHROME_ICON_FG);
        user_client_chrome_fill_rect(pixels, stride, width, height,
                                     x + 7U, y + 13U, 10U, 2U,
                                     USER_CLIENT_CHROME_ICON_FG);
        user_client_chrome_pixel(pixels, stride, width, height,
                                 x + 10U, y + 5U,
                                 USER_CLIENT_CHROME_ICON_ACCENT, 255U);
        user_client_chrome_pixel(pixels, stride, width, height,
                                 x + 5U, y + 16U,
                                 USER_CLIENT_CHROME_ICON_ACCENT, 255U);
        user_client_chrome_pixel(pixels, stride, width, height,
                                 x + 17U, y + 16U,
                                 USER_CLIENT_CHROME_ICON_ACCENT, 255U);
    } else if (kind == USER_CLIENT_CHROME_ICON_IMAGE) {
        user_client_chrome_round_rect(pixels, stride, width, height,
                                      x + 5U, y + 5U, 14U, 14U,
                                      0x00FFFFFFU);
        user_client_chrome_pixel(pixels, stride, width, height,
                                 x + 16U, y + 8U,
                                 USER_CLIENT_CHROME_ICON_ACCENT, 255U);
        user_client_chrome_fill_rect(pixels, stride, width, height,
                                     x + 7U, y + 15U, 4U, 2U,
                                     USER_CLIENT_CHROME_ICON_ACCENT);
        user_client_chrome_fill_rect(pixels, stride, width, height,
                                     x + 10U, y + 12U, 5U, 5U,
                                     USER_CLIENT_CHROME_ICON_FG);
        user_client_chrome_fill_rect(pixels, stride, width, height,
                                     x + 14U, y + 14U, 4U, 3U,
                                     USER_CLIENT_CHROME_ICON_FG);
    } else if (kind == USER_CLIENT_CHROME_ICON_TASKS) {
        user_client_chrome_round_rect(pixels, stride, width, height,
                                      x + 5U, y + 5U, 14U, 14U,
                                      USER_CLIENT_CHROME_ICON_FG);
        user_client_chrome_fill_rect(pixels, stride, width, height,
                                     x + 8U, y + 14U, 2U, 3U,
                                     USER_CLIENT_CHROME_ICON_ACCENT);
        user_client_chrome_fill_rect(pixels, stride, width, height,
                                     x + 11U, y + 11U, 2U, 6U,
                                     USER_CLIENT_CHROME_ICON_ACCENT);
        user_client_chrome_fill_rect(pixels, stride, width, height,
                                     x + 14U, y + 8U, 2U, 9U,
                                     USER_CLIENT_CHROME_ICON_ACCENT);
    }
}

static inline uint8_t user_client_chrome_nibble(char value) {
    if (value >= '0' && value <= '9') return (uint8_t)(value - '0');
    if (value >= 'A' && value <= 'F') return (uint8_t)(value - 'A' + 10U);
    return 0U;
}

static inline void user_client_chrome_close(uint32_t *pixels,
                                            uint32_t stride,
                                            uint32_t width,
                                            uint32_t height,
                                            uint32_t header_height,
                                            uint32_t background,
                                            uint32_t foreground) {
    /* 兼容入口：标准标题栏控件只由合成器绘制。不要再绘制过时的客户端 X 图标。 */
    (void)pixels; (void)stride; (void)width; (void)height;
    (void)header_height; (void)background; (void)foreground;
}

static inline bool user_client_chrome_close_hit(int32_t pointer_x,
                                                int32_t pointer_y,
                                                uint32_t width,
                                                uint32_t header_height) {
    uint32_t x = width > 36U ? width - 36U : 0U;
    return pointer_x >= (int32_t)x &&
           pointer_x < (int32_t)(x + 36U) &&
           pointer_y >= 0 && pointer_y < (int32_t)header_height;
}

#endif
