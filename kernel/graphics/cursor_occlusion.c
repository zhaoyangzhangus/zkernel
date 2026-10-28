#include "internal.h"

/* REFACTOR_P7D_CURSOR_OCCLUSION_OWNER：光标合成和覆盖测试。 */

/*
 * 仅当此场景发布可能覆盖当前扫描输出中可见的软件光标像素时才返回 true。
 */
bool compositor_snapshot_overwrites_presented_cursor(void) {
    int64_t cursor_left;
    int64_t cursor_top;
    int64_t cursor_right;
    int64_t cursor_bottom;

    if (!g_window_server.presented_pointer_valid) {
        return false;
    }

    cursor_left =
        (int64_t)g_window_server.presented_pointer_x -
        window_cursor_hotspot_x(g_window_server.presented_cursor_shape);

    cursor_top =
        (int64_t)g_window_server.presented_pointer_y -
        window_cursor_hotspot_y(g_window_server.presented_cursor_shape);

    cursor_right =
        cursor_left + WINDOW_CURSOR_WIDTH;

    cursor_bottom =
        cursor_top + WINDOW_CURSOR_HEIGHT;

    /* 拖动发布会提交完整事务边界。 */
    if (g_compositor_snapshot.dragging_identifier != 0U) {
        Rect transaction;

        if (!compositor_snapshot_damage_bounds(
                &g_compositor_snapshot,
                &transaction)) {
            return false;
        }

        return
            cursor_right > (int64_t)transaction.x0 &&
            cursor_bottom > (int64_t)transaction.y0 &&
            cursor_left < (int64_t)transaction.x1 &&
            cursor_top < (int64_t)transaction.y1;
    }

    for (uint32_t index = 0U;
         index < g_compositor_snapshot.damage_count;
         ++index) {

        const Rect *damage =
            &g_compositor_snapshot.damage_rects[index];

        if (damage->x0 >= damage->x1 ||
            damage->y0 >= damage->y1) {
            continue;
        }

        if (cursor_right <= (int64_t)damage->x0 ||
            cursor_bottom <= (int64_t)damage->y0 ||
            cursor_left >= (int64_t)damage->x1 ||
            cursor_top >= (int64_t)damage->y1) {
            continue;
        }

        return true;
    }

    return false;
}

bool compositor_window_intersects_damage_locked(
    const compositor_window_view_t *window) {
    int64_t left;
    int64_t top;
    int64_t right;
    int64_t bottom;

    if (window == 0) return false;

    left = window->x;
    top = window->y;
    right =
        left +
        (int64_t)window_outer_width(window->width, window->flags);
    bottom =
        top +
        (int64_t)window_outer_height(window->height, window->flags);

    return left < (int64_t)g_compositor_snapshot.damage_bounds.x1 &&
           right > (int64_t)g_compositor_snapshot.damage_bounds.x0 &&
           top < (int64_t)g_compositor_snapshot.damage_bounds.y1 &&
           bottom > (int64_t)g_compositor_snapshot.damage_bounds.y0;
}

/*
 * 仅当当前完整损坏矩形位于客户端表面保证不透明的内部区域时才返回 true。
 */
bool compositor_damage_inside_surface_interior(
    const compositor_window_view_t *window) {

    int64_t left;
    int64_t top;
    int64_t right;
    int64_t bottom;

    if (window == 0 ||
        window->width <= WINDOW_CORNER_RADIUS * 2U ||
        window->height <= WINDOW_CORNER_RADIUS * 2U) {
        return false;
    }

    left =
        (int64_t)window->x +
        window_frame_border(window->flags) +
        WINDOW_CORNER_RADIUS;

    top =
        (int64_t)window->y +
        window_client_offset_y(window->flags) +
        WINDOW_CORNER_RADIUS;

    right =
        (int64_t)window->x +
        window_frame_border(window->flags) +
        (int64_t)window->width -
        WINDOW_CORNER_RADIUS;

    bottom =
        (int64_t)window->y +
        window_client_offset_y(window->flags) +
        (int64_t)window->height -
        WINDOW_CORNER_RADIUS;

    return
        (int64_t)g_compositor_snapshot.damage_bounds.x0 >= left &&
        (int64_t)g_compositor_snapshot.damage_bounds.y0 >= top &&
        (int64_t)g_compositor_snapshot.damage_bounds.x1 <= right &&
        (int64_t)g_compositor_snapshot.damage_bounds.y1 <= bottom;
}

/* 回退绘制与空间规划共用不透明覆盖规则，透明表面不能淘汰下层。 */
static bool compositor_window_fully_covers_damage(
    const compositor_window_view_t *window) {
    return window != 0 && (window->flags & OS_WINDOW_VISIBLE) != 0U &&
        compositor_view_fully_covers_snapshot_bounds(
            window, &g_compositor_snapshot.damage_bounds);
}


uint32_t compositor_topmost_damage_cover(void) {
    uint32_t count =
        g_compositor_snapshot.window_count;

    while (count != 0U) {
        uint32_t index =
            count - 1U;

        const compositor_window_view_t *window =
            &g_compositor_snapshot.windows[index];

        if (compositor_window_fully_covers_damage(window)) {
            return index;
        }

        count = index;
    }

    return UINT32_MAX;
}

void compositor_cursor_locked(void) {
    int64_t origin_x;
    int64_t origin_y;

    int64_t cursor_clip_left;
    int64_t cursor_clip_top;
    int64_t cursor_clip_right;
    int64_t cursor_clip_bottom;

    if (g_window_server.composite_framebuffer == 0 ||
        g_window_server.display_width == 0U ||
        g_window_server.display_height == 0U) {
        return;
    }

    origin_x =
        (int64_t)g_compositor_snapshot.pointer_x -
        window_cursor_hotspot_x(g_compositor_snapshot.cursor_shape);

    origin_y =
        (int64_t)g_compositor_snapshot.pointer_y -
        window_cursor_hotspot_y(g_compositor_snapshot.cursor_shape);

    cursor_clip_left = origin_x;
    cursor_clip_top = origin_y;

    cursor_clip_right =
        origin_x + WINDOW_CURSOR_WIDTH;

    cursor_clip_bottom =
        origin_y + WINDOW_CURSOR_HEIGHT;

    if (cursor_clip_left < 0) {
        cursor_clip_left = 0;
    }

    if (cursor_clip_top < 0) {
        cursor_clip_top = 0;
    }

    if (cursor_clip_right >
        (int64_t)g_window_server.display_width) {
        cursor_clip_right =
            g_window_server.display_width;
    }

    if (cursor_clip_bottom >
        (int64_t)g_window_server.display_height) {
        cursor_clip_bottom =
            g_window_server.display_height;
    }

    if (cursor_clip_left <
        (int64_t)g_compositor_snapshot.damage_bounds.x0) {
        cursor_clip_left =
            g_compositor_snapshot.damage_bounds.x0;
    }

    if (cursor_clip_top <
        (int64_t)g_compositor_snapshot.damage_bounds.y0) {
        cursor_clip_top =
            g_compositor_snapshot.damage_bounds.y0;
    }

    if (cursor_clip_right >
        (int64_t)g_compositor_snapshot.damage_bounds.x1) {
        cursor_clip_right =
            g_compositor_snapshot.damage_bounds.x1;
    }

    if (cursor_clip_bottom >
        (int64_t)g_compositor_snapshot.damage_bounds.y1) {
        cursor_clip_bottom =
            g_compositor_snapshot.damage_bounds.y1;
    }

    if (cursor_clip_left >= cursor_clip_right ||
        cursor_clip_top >= cursor_clip_bottom) {
        return;
    }

    for (int64_t y = cursor_clip_top;
         y < cursor_clip_bottom;
         ++y) {

        uint32_t source_row =
            (uint32_t)(y - origin_y);

        uint32_t source_column =
            (uint32_t)(cursor_clip_left - origin_x);

        volatile uint32_t *destination =
            g_window_server.composite_framebuffer +
            (uint64_t)y *
                g_window_server.display_stride +
            (uint32_t)cursor_clip_left;

        uint32_t pixels =
            (uint32_t)(cursor_clip_right - cursor_clip_left);

        for (uint32_t column = 0U;
             column < pixels;
             ++column) {

            uint32_t pixel = window_cursor_pixel(g_compositor_snapshot.cursor_shape,
                                                 source_column + column, source_row);
            uint32_t alpha = pixel >> 24;

            if (alpha == 0U) {
                ++destination;
                continue;
            }

            uint32_t color = pixel & 0x00FFFFFFU;

            if (alpha >= 255U) {
                *destination++ = color;
                continue;
            }

            uint32_t current = *destination;
            uint32_t inverse = 255U - alpha;

            uint32_t red =
                ((((current >> 16) & 0xFFU) * inverse) +
                 (((color >> 16) & 0xFFU) * alpha) +
                 127U) / 255U;

            uint32_t green =
                ((((current >> 8) & 0xFFU) * inverse) +
                 (((color >> 8) & 0xFFU) * alpha) +
                 127U) / 255U;

            uint32_t blue =
                (((current & 0xFFU) * inverse) +
                 ((color & 0xFFU) * alpha) +
                 127U) / 255U;

            *destination++ =
                (red << 16) |
                (green << 8) |
                blue;
        }
    }
}
