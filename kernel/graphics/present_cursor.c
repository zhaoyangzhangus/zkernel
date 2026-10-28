#include <kernel/display.h>
#include <kernel/sched.h>
#include "internal.h"

/* REFACTOR_P7J_PRESENT_CURSOR_OWNER：扫描输出侧光标叠加。 */

/* 保留式合成场景不包含光标；呈现模块负责小型直接叠加，以及恢复上一个指针图像。 */
void window_present_cursor_reset_locked(void) {
    g_window_server.presented_pointer_x = 0U;
    g_window_server.presented_pointer_y = 0U;
    g_window_server.presented_pointer_valid = false;
}

bool window_present_self_test(void) {
    return WINDOW_CURSOR_WIDTH != 0U &&
           WINDOW_CURSOR_HEIGHT != 0U &&
           WINDOW_CURSOR_HOTSPOT_X < WINDOW_CURSOR_WIDTH &&
           WINDOW_CURSOR_HOTSPOT_Y < WINDOW_CURSOR_HEIGHT &&
           g_linux_cursor_argb[0] == 0U;
}

static uint32_t cursor_blend_xrgb(uint32_t background, uint32_t cursor) {
    uint32_t alpha = cursor >> 24;
    uint32_t color = cursor & 0x00FFFFFFU;
    uint32_t inverse;

    if (alpha == 0U) return background;
    if (alpha >= 255U) return color;

    inverse = 255U - alpha;
    return (((((background >> 16) & 0xFFU) * inverse +
              ((color >> 16) & 0xFFU) * alpha + 127U) / 255U) << 16) |
           (((((background >> 8) & 0xFFU) * inverse +
              ((color >> 8) & 0xFFU) * alpha + 127U) / 255U) << 8) |
           (((background & 0xFFU) * inverse +
             (color & 0xFFU) * alpha + 127U) / 255U);
}

/* 包围盒只裁剪一次；恢复背景按行复制，混合光标逐像素直接写最终输出。 */
static void cursor_publish_box(volatile uint32_t *destination,
    int64_t origin_x, int64_t origin_y, bool draw_cursor, uint32_t shape) {
    int64_t left = origin_x < 0 ? 0 : origin_x;
    int64_t top = origin_y < 0 ? 0 : origin_y;
    int64_t right = origin_x + (int64_t)WINDOW_CURSOR_WIDTH;
    int64_t bottom = origin_y + (int64_t)WINDOW_CURSOR_HEIGHT;
    if (right > (int64_t)g_window_server.display_width)
        right = g_window_server.display_width;
    if (bottom > (int64_t)g_window_server.display_height)
        bottom = g_window_server.display_height;
    if (left >= right || top >= bottom) return;

    uint32_t pixels = (uint32_t)(right - left);
    for (int64_t y = top; y < bottom; ++y) {
        uint64_t row_offset = (uint64_t)y *
            g_window_server.display_stride + (uint32_t)left;
        /* 恢复旧光标只需复制背景，避免逐像素调用和重复的显示格式检查。 */
        if (!draw_cursor) {
            display_core_publish_xrgb8888_span(destination + row_offset,
                (const uint32_t *)(g_window_server.composite_framebuffer + row_offset),
                pixels);
            continue;
        }
        uint32_t cursor_row = (uint32_t)(y - origin_y);
        uint32_t first_column = (uint32_t)(left - origin_x);
        for (uint32_t column = 0U; column < pixels; ++column) {
            uint64_t offset = row_offset + column;
            uint32_t background = g_window_server.composite_framebuffer[offset];
            uint32_t cursor = window_cursor_pixel(shape, first_column + column, cursor_row);
            display_core_publish_xrgb8888_pixel(destination + offset,
                cursor_blend_xrgb(background, cursor));
        }
    }
}

/*
 * 将保留画面与光标直接混合到最终输出目标。保留画面不含光标且不被改写，
 * 无需读取 WC 显存，也不依赖隐藏页面或翻页。
 */
bool window_present_cursor_into_buffer(volatile uint32_t *destination,
                                       uint32_t pointer_x,
                                       uint32_t pointer_y, uint32_t shape) {
    int64_t origin_x;
    int64_t origin_y;

    if (destination == 0 ||
        g_window_server.composite_framebuffer == 0 ||
        g_window_server.display_width == 0U ||
        g_window_server.display_height == 0U) {
        return false;
    }

    origin_x = (int64_t)pointer_x - window_cursor_hotspot_x(shape);
    origin_y = (int64_t)pointer_y - window_cursor_hotspot_y(shape);

    cursor_publish_box(destination, origin_x, origin_y, true, shape);

    return true;
}

/* 先发布包围盒的新背景与光标，后面的场景复制只写包围盒之外。
 * 场景由单一合成线程拥有；让出 CPU 后的鼠标移动也用该完整场景重新发布新包围盒。 */
void window_present_cursor_prepare_rect(const Rect *rect) {
    if (rect == 0 || display_core_has_native_scanout(0U)) return;
    if (!g_window_server.presented_pointer_valid) {
        compositor_present_cursor_direct(false);
        return;
    }
    int64_t left = (int64_t)g_window_server.presented_pointer_x -
        window_cursor_hotspot_x(g_window_server.presented_cursor_shape);
    int64_t top = (int64_t)g_window_server.presented_pointer_y -
        window_cursor_hotspot_y(g_window_server.presented_cursor_shape);
    if (left >= rect->x1 || top >= rect->y1 ||
        left + WINDOW_CURSOR_WIDTH <= rect->x0 ||
        top + WINDOW_CURSOR_HEIGHT <= rect->y0) return;
    (void)window_present_cursor_into_buffer(g_window_server.framebuffer,
        g_window_server.presented_pointer_x, g_window_server.presented_pointer_y,
        g_window_server.presented_cursor_shape);
    __asm__ volatile ("sfence" : : : "memory");
}

/* 相交跨度只复制左右两段，不覆盖预先发布的光标包围盒。 */
bool window_present_cursor_publish_span(
    volatile uint32_t *destination,
    const uint32_t *source,
    uint32_t x,
    uint32_t y,
    uint32_t pixels) {
    int64_t origin_x;
    int64_t origin_y;
    int64_t span_left;
    int64_t span_right;
    int64_t cursor_left;
    int64_t cursor_right;
    int64_t overlap_left;
    int64_t overlap_right;

    if (destination == 0 || source == 0 || pixels == 0U ||
        !g_window_server.presented_pointer_valid ||
        y >= g_window_server.display_height ||
        x > g_window_server.display_width ||
        pixels > g_window_server.display_width - x) {
        return false;
    }

    origin_x =
        (int64_t)g_window_server.presented_pointer_x -
        window_cursor_hotspot_x(g_window_server.presented_cursor_shape);
    origin_y =
        (int64_t)g_window_server.presented_pointer_y -
        window_cursor_hotspot_y(g_window_server.presented_cursor_shape);
    span_left = x;
    span_right = (int64_t)x + pixels;
    cursor_left = origin_x;
    cursor_right = origin_x + WINDOW_CURSOR_WIDTH;

    overlap_left = span_left > cursor_left ? span_left : cursor_left;
    overlap_right = span_right < cursor_right ? span_right : cursor_right;
    if (overlap_left >= overlap_right ||
        y < (uint32_t)(origin_y < 0 ? 0 : origin_y) ||
        (int64_t)y >= origin_y + WINDOW_CURSOR_HEIGHT) {
        return false;
    }

    if (overlap_left > span_left) {
        uint32_t count = (uint32_t)(overlap_left - span_left);
        display_core_publish_xrgb8888_span(
            destination,
            source,
            count);
    }

    if (overlap_right < span_right) {
        uint32_t source_offset =
            (uint32_t)(overlap_right - span_left);
        uint32_t count = (uint32_t)(span_right - overlap_right);

        display_core_publish_xrgb8888_span(
            destination + source_offset,
            source + source_offset,
            count);
    }

    return true;
}

void compositor_present_cursor_direct(bool force) {
    int64_t left;
    int64_t top;
    int64_t right;
    int64_t bottom;

    if (!window_present_cursor_overlay(force,
                                       &left,
                                       &top,
                                       &right,
                                       &bottom)) {
        return;
    }

}

bool window_present_cursor_overlay(bool force,
                                   int64_t *out_left,
                                   int64_t *out_top,
                                   int64_t *out_right,
                                   int64_t *out_bottom) {
    uint32_t pointer_x;
    uint32_t pointer_y;
    uint32_t shape;
    int64_t old_origin_x;
    int64_t old_origin_y;
    int64_t new_origin_x;
    int64_t new_origin_y;

    /* 原生 GB205 扫描输出会在提交帧副本中包含光标；绝不能将未启用的固件 GOP 映射作为直接
     * 叠加目标写入。 */
    if (display_core_has_native_scanout(0U)) {
        return false;
    }

    if (g_window_server.framebuffer == 0 ||
        g_window_server.composite_framebuffer == 0 ||
        g_window_server.composite_framebuffer ==
            g_window_server.framebuffer ||
        g_window_server.display_width == 0U ||
        g_window_server.display_height == 0U) {
        return false;
    }

    window_lock();
    pointer_x = g_window_server.pointer_x;
    pointer_y = g_window_server.pointer_y;
    shape = window_input_cursor_shape_locked();
    window_unlock();

    if (!force &&
        g_window_server.presented_pointer_valid &&
        g_window_server.presented_pointer_x == pointer_x &&
        g_window_server.presented_pointer_y == pointer_y &&
        g_window_server.presented_cursor_shape == shape) {
        return false;
    }

    sched_preempt_disable();

    if (g_window_server.presented_pointer_valid) {
        old_origin_x =
            (int64_t)g_window_server.presented_pointer_x -
            window_cursor_hotspot_x(g_window_server.presented_cursor_shape);
        old_origin_y =
            (int64_t)g_window_server.presented_pointer_y -
            window_cursor_hotspot_y(g_window_server.presented_cursor_shape);

        cursor_publish_box(g_window_server.framebuffer, old_origin_x,
            old_origin_y, false, g_window_server.presented_cursor_shape);
    }

    new_origin_x =
        (int64_t)pointer_x - window_cursor_hotspot_x(shape);
    new_origin_y =
        (int64_t)pointer_y - window_cursor_hotspot_y(shape);

    cursor_publish_box(g_window_server.framebuffer, new_origin_x,
        new_origin_y, true, shape);

    __asm__ volatile ("sfence" : : : "memory");

    g_window_server.presented_pointer_x = pointer_x;
    g_window_server.presented_pointer_y = pointer_y;
    g_window_server.presented_cursor_shape = shape;
    g_window_server.presented_pointer_valid = true;

    if (out_left != 0) *out_left = new_origin_x;
    if (out_top != 0) *out_top = new_origin_y;
    if (out_right != 0) {
        *out_right = new_origin_x + (int64_t)WINDOW_CURSOR_WIDTH;
    }
    if (out_bottom != 0) {
        *out_bottom = new_origin_y + (int64_t)WINDOW_CURSOR_HEIGHT;
    }

    sched_preempt_enable();
    return true;
}
