#include "internal.h"
#include <uapi/window_theme.h>

/* REFACTOR_P7E_RASTER_OWNER：直接帧缓冲填充原语。 */

void compositor_fill_span_wb(
    volatile uint32_t *destination,
    uint32_t pixels,
    uint32_t color) {

    uint64_t pattern;
    uint64_t qwords;
    void *out;

    if (destination == 0 || pixels == 0U) {
        return;
    }

    /* 先将目标地址对齐到 8 字节。 */
    if ((((uintptr_t)destination) & 7U) != 0U) {
        *destination++ = color;
        --pixels;

        if (pixels == 0U) {
            return;
        }
    }

    pattern =
        (uint64_t)color |
        ((uint64_t)color << 32U);

    qwords = pixels >> 1U;

    if (qwords != 0U) {
        out = (void *)(uintptr_t)destination;

        __asm__ volatile (
            "rep stosq"
            : "+D"(out),
              "+c"(qwords)
            : "a"(pattern)
            : "memory");

        destination =
            (volatile uint32_t *)(uintptr_t)out;
    }

    /* 处理剩余的奇数个像素。 */
    if ((pixels & 1U) != 0U) {
        *destination = color;
    }
}

void compositor_fill_locked(Rect clip, int32_t x, int32_t y,
                            uint32_t width, uint32_t height,
                            uint32_t color) {
    compositor_fill_rounded_locked(clip, x, y, width, height, 0U, color);
}

void compositor_fill_rounded_locked(Rect clip, int32_t x, int32_t y,
                                    uint32_t width, uint32_t height,
                                    uint32_t radius, uint32_t color) {
    color=os_theme_resolve(color,g_compositor_snapshot.theme);
    Rect visible;
    int64_t right = (int64_t)x + width;
    int64_t bottom = (int64_t)y + height;
    /* 先按损坏区域求交，再统一裁剪到屏幕；宽坐标相加不能先截断到 32 位。 */
    if (!rect_clip_to_extent(x > clip.x0 ? x : clip.x0,
            y > clip.y0 ? y : clip.y0,
            right < clip.x1 ? right : clip.x1,
            bottom < clip.y1 ? bottom : clip.y1,
            g_window_server.display_width, g_window_server.display_height,
            &visible)) return;
    int32_t left = visible.x0, top = visible.y0;
    right = visible.x1;
    bottom = visible.y1;

    /* 完整覆盖行跨度时像素连续，合并为一次填充；保留长度上限和非连续路径。 */
    if (!radius && left == 0 && right == g_window_server.display_stride) {
        uint64_t pixels = (uint64_t)(bottom - top) * g_window_server.display_stride;
        if (pixels <= UINT32_MAX) {
            compositor_fill_span_wb(g_window_server.composite_framebuffer +
                (uint64_t)(uint32_t)top * g_window_server.display_stride,
                (uint32_t)pixels, color);
            return;
        }
    }

    for (int32_t row = top; row < bottom; ++row) {
        uint32_t inset = 0U;
        int64_t relative_row = (int64_t)row - y;
        int64_t span_left;
        int64_t span_right;

        if (radius == WINDOW_CORNER_RADIUS &&
            relative_row >= 0 &&
            relative_row < (int64_t)height) {
            inset = compositor_corner_inset(
                (uint32_t)relative_row,
                width,
                height);
        }

        span_left = (int64_t)x + inset;
        span_right = (int64_t)x + width - inset;

        if (span_left < left) span_left = left;
        if (span_right > right) span_right = right;
        if (span_left >= span_right) continue;

        volatile uint32_t *destination =
            g_window_server.composite_framebuffer +
            (uint64_t)(uint32_t)row *
                g_window_server.display_stride +
            (uint32_t)span_left;

        compositor_fill_span_wb(
            destination,
            (uint32_t)(span_right - span_left),
            color);
    }
}
