#include "internal.h"

/* REFACTOR_P7A_INPUT_DRAG_OWNER：保留式拖动和缩放几何策略。 */

/* 拖动捕获属于输入状态。所有修改都集中在此处，使取消操作不会遗留标识符、缩放边缘或保留复制标志。 */
void window_input_reset_drag_locked(void) {
    g_window_server.dragging_identifier = 0U;
    g_window_server.drag_offset_x = 0;
    g_window_server.drag_offset_y = 0;
    g_window_server.resize_edges = 0U;
    g_window_server.drag_blit_valid = false;
    g_window_server.drag_old_x = 0;
    g_window_server.drag_old_y = 0;
    g_window_server.drag_new_x = 0;
    g_window_server.drag_new_y = 0;
    g_window_server.drag_width = 0U;
    g_window_server.drag_height = 0U;
}

void window_input_clear_drag_locked(void) {
    g_window_server.dragging_identifier = 0U;
    g_window_server.drag_offset_x = 0;
    g_window_server.drag_offset_y = 0;
    g_window_server.resize_edges = 0U;
    g_window_server.drag_blit_valid = false;
}

void window_input_begin_drag_locked(uint32_t identifier,
                                    int32_t offset_x,
                                    int32_t offset_y,
                                    uint32_t resize_edges) {
    g_window_server.dragging_identifier = identifier;
    g_window_server.drag_offset_x = offset_x;
    g_window_server.drag_offset_y = offset_y;
    g_window_server.resize_edges = resize_edges;
}

void window_input_set_drag_blit_valid_locked(bool valid) {
    g_window_server.drag_blit_valid = valid;
}

void window_input_record_drag_frame_locked(
    bool blit_valid,
    int32_t old_x, int32_t old_y,
    int32_t new_x, int32_t new_y,
    uint32_t width, uint32_t height) {
    g_window_server.drag_blit_valid = blit_valid;
    g_window_server.drag_old_x = old_x;
    g_window_server.drag_old_y = old_y;
    g_window_server.drag_new_x = new_x;
    g_window_server.drag_new_y = new_y;
    g_window_server.drag_width = width;
    g_window_server.drag_height = height;
}

static __attribute__((unused)) bool window_drag_reuse_available_locked(
    const window_server_window_t *window) {
    return window != 0 &&
           g_window_server.resize_edges == 0U &&
           g_window_server.composite_framebuffer != 0 &&
           g_window_server.composite_framebuffer !=
               g_window_server.framebuffer &&
           g_window_server.scene.count != 0U &&
           g_window_server.scene.children[g_window_server.scene.count - 1U] == window;
}

/*
 * 仅当 old_x/old_y 仍描述 composite_framebuffer 当前保存的像素时，保留内容拖动才有效。
 * 一帧完成合成前可能消费多份 HID 报告，因此不得将中间逻辑位置误当成已呈现内容再次复用。
 */
bool window_drag_reuse_safe_locked(
    const window_server_window_t *window,
    int32_t old_x, int32_t old_y,
    int32_t new_x, int32_t new_y,
    uint32_t width, uint32_t height) {
#if WINDOW_COMPOSITOR_RETAINED_DRAG == 0U
    (void)window;
    (void)old_x;
    (void)old_y;
    (void)new_x;
    (void)new_y;
    (void)width;
    (void)height;
    return false;
#else
    if (!window_drag_reuse_available_locked(window) ||
        g_window_server.dirty ||
        g_window_server.composing ||
        width == 0U || height == 0U ||
        old_x < 0 || old_y < 0 ||
        new_x < 0 || new_y < 0 ||
        !window->compositor_presented_valid ||
        window->compositor_presented_x != old_x ||
        window->compositor_presented_y != old_y ||
        window->compositor_presented_width != width ||
        window->compositor_presented_height != height) {
        return false;
    }

    return (uint64_t)(uint32_t)old_x + width <=
               g_window_server.display_width &&
           (uint64_t)(uint32_t)new_x + width <=
               g_window_server.display_width &&
           (uint64_t)(uint32_t)old_y + height <=
               g_window_server.display_height &&
           (uint64_t)(uint32_t)new_y + height <=
               g_window_server.display_height;
#endif
}

void window_drag_move_locked(window_server_window_t *window,
                             int32_t new_x, int32_t new_y) {
    if (window == 0 || (window->x == new_x && window->y == new_y)) return;

    uint32_t width = window_outer_width(window->width, window->flags);
    uint32_t height = window_outer_height(window->height, window->flags);
    int32_t old_x = window->x;
    int32_t old_y = window->y;
    bool reuse = window_drag_reuse_safe_locked(
        window, window->compositor_presented_x, window->compositor_presented_y,
        new_x, new_y, width, height);
    if (reuse) {
        old_x = window->compositor_presented_x;
        old_y = window->compositor_presented_y;
    }
    window_input_record_drag_frame_locked(reuse, old_x, old_y,
                                          new_x, new_y, width, height);
    window->x = new_x;
    window->y = new_y;
    window_enqueue_moved_event_locked(window);
    window_mark_moved_rect_locked(old_x, old_y, new_x, new_y,
                                  width, height, !reuse);
    if (reuse) {
        window_mark_drag_corner_repair_locked(new_x, new_y,
                                              width, height, window->flags);
    }
    window->dirty = true;
}

uint32_t window_resize_edges_locked(const window_server_window_t *window,
                                    uint32_t x, uint32_t y) {
    return window_input_resize_edges_locked(window, x, y);
}

static void window_resize_axis(int64_t *start, int64_t *end,
                               int32_t delta, bool move_start, bool move_end,
                               int64_t minimum_size, int64_t maximum_size) {
    int64_t value, minimum, maximum;
    if (move_start) {
        value = *start + delta;
        minimum = *end - maximum_size;
        maximum = *end - minimum_size;
    } else if (move_end) {
        value = *end + delta;
        minimum = *start + minimum_size;
        maximum = *start + maximum_size;
    } else {
        return;
    }
    if (value < minimum) value = minimum;
    if (value > maximum) value = maximum;
    if (move_start) *start = value;
    else *end = value;
}

bool window_resize_locked(window_server_window_t *window,
                          int32_t delta_x, int32_t delta_y) {
    int32_t old_x;
    int32_t old_y;
    uint32_t old_width;
    uint32_t old_height;
    uint32_t max_width;
    uint32_t max_height;
    uint32_t min_width;
    uint32_t min_height;
    int64_t left;
    int64_t top;
    int64_t right;
    int64_t bottom;
    int64_t extra_width;
    int64_t extra_height;
    uint32_t new_width;
    uint32_t new_height;

    if (window == 0 || g_window_server.resize_edges == 0U ||
        (window->flags & OS_WINDOW_RESIZABLE) == 0U) {
        return false;
    }

    max_width = g_window_server.display_width;
    max_height = g_window_server.display_height;
    if (max_width == 0U || max_height == 0U) return false;
    min_width = max_width < WINDOW_MIN_WIDTH ? max_width : WINDOW_MIN_WIDTH;
    min_height = max_height < WINDOW_MIN_HEIGHT ? max_height : WINDOW_MIN_HEIGHT;

    old_x = window->x;
    old_y = window->y;
    old_width = window->width;
    old_height = window->height;

    left = window->x;
    top = window->y;
    right = left +
            (int64_t)window_outer_width(window->width, window->flags);
    bottom =
        top +
        (int64_t)window_outer_height(window->height, window->flags);

    extra_width = window_frame_extra(window->flags);
    extra_height = extra_width + window_titlebar_height(window->flags);
    window_resize_axis(&left, &right, delta_x,
                       (g_window_server.resize_edges & WINDOW_RESIZE_LEFT) != 0U,
                       (g_window_server.resize_edges & WINDOW_RESIZE_RIGHT) != 0U,
                       (int64_t)min_width + extra_width,
                       (int64_t)max_width + extra_width);
    window_resize_axis(&top, &bottom, delta_y,
                       (g_window_server.resize_edges & WINDOW_RESIZE_TOP) != 0U,
                       (g_window_server.resize_edges & WINDOW_RESIZE_BOTTOM) != 0U,
                       (int64_t)min_height + extra_height,
                       (int64_t)max_height + extra_height);

    new_width = (uint32_t)(right - left - extra_width);
    new_height = (uint32_t)(bottom - top - extra_height);
    if (left == old_x && top == old_y &&
        new_width == old_width && new_height == old_height) {
        return false;
    }

    window_mark_rect_locked(old_x, old_y,
                            window_outer_width(old_width, window->flags),
                            window_outer_height(old_height, window->flags));
    window->x = (int32_t)left;
    window->y = (int32_t)top;
    window->width = new_width;
    window->height = new_height;
    window->dirty = true;
    window->resize_pending = true;
    window_mark_window_locked(window);
    window_enqueue_resize_event_locked(window);
    return true;
}

/*
 * 将累积的一组 X/Y 移动应用到活动的 Ring0 装饰区捕获。Ring0 仅处理拖动/调整大小；普通
 * 指针移动仍通过 input_motion.c 中常规的逐轴路径路由。
 */
bool window_route_drag_motion_batch_locked(
    const window_motion_batch_t *batch) {
    window_server_window_t *dragged;
    uint32_t old_pointer_x;
    uint32_t old_pointer_y;
    int64_t next_x;
    int64_t next_y;
    int32_t actual_delta_x;
    int32_t actual_delta_y;

    if (batch == 0 || g_window_server.dragging_identifier == 0U) {
        return false;
    }

    dragged = window_scene_find_locked(g_window_server.dragging_identifier);
    if (dragged == 0 ||
        (dragged->flags & OS_WINDOW_VISIBLE) == 0U) {
        window_input_clear_drag_locked();
        return false;
    }

    old_pointer_x = g_window_server.pointer_x;
    old_pointer_y = g_window_server.pointer_y;
    next_x = (int64_t)old_pointer_x + batch->delta_x;
    next_y = (int64_t)old_pointer_y + batch->delta_y;
    if (next_x < 0) next_x = 0;
    if (next_y < 0) next_y = 0;
    if (g_window_server.display_width == 0U) {
        next_x = 0;
    } else if (next_x >= (int64_t)g_window_server.display_width) {
        next_x = (int64_t)g_window_server.display_width - 1;
    }
    if (g_window_server.display_height == 0U) {
        next_y = 0;
    } else if (next_y >= (int64_t)g_window_server.display_height) {
        next_y = (int64_t)g_window_server.display_height - 1;
    }

    window_input_set_pointer_locked((uint32_t)next_x, (uint32_t)next_y);
    if (g_window_server.desktop_hovered_app != DESKTOP_APP_NONE) {
        desktop_mark_app_locked(g_window_server.desktop_hovered_app);
        desktop_set_hovered_app_locked(DESKTOP_APP_NONE);
    }

    actual_delta_x = (int32_t)((int64_t)g_window_server.pointer_x -
                               (int64_t)old_pointer_x);
    actual_delta_y = (int32_t)((int64_t)g_window_server.pointer_y -
                               (int64_t)old_pointer_y);
    if (g_window_server.resize_edges != 0U) {
        window_input_set_drag_blit_valid_locked(false);
        if (actual_delta_x != 0 || actual_delta_y != 0) {
            (void)window_resize_locked(dragged, actual_delta_x, actual_delta_y);
        }
    } else {
        int32_t new_x = (int32_t)g_window_server.pointer_x -
                        g_window_server.drag_offset_x;
        int32_t new_y = (int32_t)g_window_server.pointer_y -
                        g_window_server.drag_offset_y;

        window_drag_move_locked(dragged, new_x, new_y);
    }

    if (old_pointer_x != g_window_server.pointer_x ||
        old_pointer_y != g_window_server.pointer_y) {
        window_mark_moved_cursor_locked(
            old_pointer_x, old_pointer_y,
            g_window_server.pointer_x, g_window_server.pointer_y);
    }
    window_coalesce_damage_locked();
    return true;
}
