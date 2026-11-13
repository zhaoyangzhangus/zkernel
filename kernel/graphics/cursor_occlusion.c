#include <kernel/pixel_blend.h>
#include "internal.h"

/* REFACTOR_P7D_CURSOR_OCCLUSION_OWNER：光标合成和覆盖测试。 */

bool compositor_window_intersects_damage_locked(Rect clip,
    const compositor_window_view_t *window) {
    if (window == 0) return false;
    return rect_intersects_bounds(clip, window->x, window->y,
        (int64_t)window->x + window_outer_width(window->width, window->flags),
        (int64_t)window->y + window_outer_height(window->height, window->flags));
}

/*
 * 仅当当前完整损坏矩形位于客户端表面保证不透明的内部区域时才返回 true。
 */
bool compositor_damage_inside_surface_interior(Rect clip,
    const compositor_window_view_t *window) {

    if (window == 0 ||
        window->width <= WINDOW_CORNER_RADIUS * 2U ||
        window->height <= WINDOW_CORNER_RADIUS * 2U) return false;

    const int64_t left = (int64_t)window->x + window_frame_border(window->flags);
    const int64_t top = (int64_t)window->y + window_client_offset_y(window->flags);
    return rect_inside_bounds(clip, left + WINDOW_CORNER_RADIUS,
        top + WINDOW_CORNER_RADIUS,
        left + window->width - WINDOW_CORNER_RADIUS,
        top + window->height - WINDOW_CORNER_RADIUS);
}

/* 回退绘制与空间规划共用不透明覆盖规则，透明表面不能淘汰下层。 */
static bool compositor_window_fully_covers_damage(Rect clip,
    const compositor_window_view_t *window) {
    return window != 0 && (window->flags & OS_WINDOW_VISIBLE) != 0U &&
        compositor_view_fully_covers_snapshot_bounds(
            window, &clip);
}


uint32_t compositor_topmost_damage_cover(Rect clip) {
    uint32_t count =
        g_compositor_snapshot.window_count;

    while (count != 0U) {
        uint32_t index =
            count - 1U;

        const compositor_window_view_t *window =
            &g_compositor_snapshot.windows[index];

        if (compositor_window_fully_covers_damage(clip, window)) {
            return index;
        }

        count = index;
    }

    return UINT32_MAX;
}

void compositor_cursor_locked(Rect clip) {
    int64_t origin_x;
    int64_t origin_y;

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

    /* 先与调用方损坏范围相交，再由公共裁剪函数限制到显示范围。 */
    int64_t left = origin_x > clip.x0 ? origin_x : clip.x0;
    int64_t top = origin_y > clip.y0 ? origin_y : clip.y0;
    int64_t right = origin_x + WINDOW_CURSOR_WIDTH;
    int64_t bottom = origin_y + WINDOW_CURSOR_HEIGHT;
    if (right > clip.x1) right = clip.x1;
    if (bottom > clip.y1) bottom = clip.y1;
    Rect bounds;
    if (!rect_clip_to_extent(left, top, right, bottom,
            g_window_server.display_width, g_window_server.display_height, &bounds)) return;

    uint32_t source_column = (uint32_t)((int64_t)bounds.x0 - origin_x);
    uint32_t pixels = (uint32_t)(bounds.x1 - bounds.x0);
    for (int64_t y = bounds.y0;
         y < bounds.y1;
         ++y) {

        uint32_t source_row =
            (uint32_t)(y - origin_y);

        volatile uint32_t *destination =
            g_window_server.composite_framebuffer +
            (uint64_t)y *
                g_window_server.display_stride +
            (uint32_t)bounds.x0;

        for (uint32_t column = 0U;
             column < pixels;
             ++column) {

            uint32_t pixel = window_cursor_pixel(g_compositor_snapshot.cursor_shape,
                                                 source_column + column, source_row);
            /* 透明像素不写回；其余像素沿用正常输出路径的统一混色与 XRGB 规则。 */
            if (pixel >> 24U) {
                *destination = control_blend_cursor_xrgb(*destination, pixel);
            }
            ++destination;
        }
    }
}
