#include "internal.h"

/* REFACTOR_P7I_SNAPSHOT_DAMAGE_OWNER：不可变损坏区域及 tile 转换。 */

/* 一次拖动对应一个可见事务。 */
void compositor_collapse_drag_damage(compositor_snapshot_t *snapshot) {
    Rect bounds;
    if (snapshot == 0 || snapshot->dragging_identifier == 0U ||
        snapshot->damage_count <= 1U) return;

    /* 规划与发布共用有符号包围盒，忽略空矩形，避免拖动越过屏幕边缘时漏绘。 */
    if (compositor_snapshot_damage_bounds(snapshot, &bounds)) {
        snapshot->damage_rects[0] = bounds;
        snapshot->damage_count = 1U;
    }
}

bool compositor_view_intersects_snapshot_damage(
    const compositor_window_view_t *window,
    const compositor_snapshot_t *snapshot) {

    int64_t left;
    int64_t top;
    int64_t right;
    int64_t bottom;

    if (window == 0 ||
        snapshot == 0 ||
        snapshot->damage_count == 0U) {
        return false;
    }

    left = window->x;
    top = window->y;

    right =
        left +
        (int64_t)window_outer_width(
            window->width,
            window->flags);

    bottom =
        top +
        (int64_t)window_outer_height(
            window->height,
            window->flags);

    if (left >= right || top >= bottom) {
        return false;
    }

    for (uint32_t index = 0U;
         index < snapshot->damage_count;
         ++index) {

        const Rect *damage =
            &snapshot->damage_rects[index];

        if (damage->x0 >= damage->x1 ||
            damage->y0 >= damage->y1) {
            continue;
        }

        if (right <= (int64_t)damage->x0 ||
            bottom <= (int64_t)damage->y0 ||
            left >= (int64_t)damage->x1 ||
            top >= (int64_t)damage->y1) {
            continue;
        }

        return true;
    }

    return false;
}

bool compositor_snapshot_damage_bounds(
    const compositor_snapshot_t *snapshot,
    Rect *bounds) {

    bool have_bounds = false;

    if (snapshot == 0 || bounds == 0) {
        return false;
    }

    for (uint32_t index = 0U;
         index < snapshot->damage_count;
         ++index) {

        const Rect *rect =
            &snapshot->damage_rects[index];

        if (rect->x0 >= rect->x1 ||
            rect->y0 >= rect->y1) {
            continue;
        }

        if (!have_bounds) {
            *bounds = *rect;
            have_bounds = true;
            continue;
        }

        if (rect->x0 < bounds->x0)
            bounds->x0 = rect->x0;
        if (rect->y0 < bounds->y0)
            bounds->y0 = rect->y0;
        if (rect->x1 > bounds->x1)
            bounds->x1 = rect->x1;
        if (rect->y1 > bounds->y1)
            bounds->y1 = rect->y1;
    }

    return have_bounds;
}

uint32_t compositor_snapshot_occlusion_floor(
    const compositor_snapshot_t *snapshot) {

    Rect bounds;
    uint32_t floor = 0U;

    if (snapshot == 0 ||
        !compositor_snapshot_damage_bounds(
            snapshot,
            &bounds)) {
        return 0U;
    }

    for (uint32_t count = snapshot->window_count;
         count != 0U;
         --count) {

        uint32_t index = count - 1U;

        if (compositor_view_fully_covers_snapshot_bounds(
                &snapshot->windows[index],
                &bounds)) {
            floor = index;
            break;
        }
    }

    if (snapshot->dragging_identifier != 0U &&
        floor != 0U) {

        for (uint32_t index = 0U;
             index < floor;
             ++index) {

            if (snapshot->windows[index].identifier ==
                snapshot->dragging_identifier) {
                floor = index;
                break;
            }
        }
    }

    return floor;
}

/* 在窗口锁内只计算一次快照捕获范围，瓦片模式需覆盖向外扩展后的像素。 */
Rect compositor_registry_damage_bounds_locked(void) {
    Rect bounds = g_window_server.damage_bounds;
    if (g_window_server.damage_tiles_active) {
        bounds.x0 = (bounds.x0 / WINDOW_DAMAGE_TILE_SIZE) * WINDOW_DAMAGE_TILE_SIZE;
        bounds.y0 = (bounds.y0 / WINDOW_DAMAGE_TILE_SIZE) * WINDOW_DAMAGE_TILE_SIZE;
        bounds.x1 = ((bounds.x1 + WINDOW_DAMAGE_TILE_SIZE - 1U) /
                     WINDOW_DAMAGE_TILE_SIZE) * WINDOW_DAMAGE_TILE_SIZE;
        bounds.y1 = ((bounds.y1 + WINDOW_DAMAGE_TILE_SIZE - 1U) /
                     WINDOW_DAMAGE_TILE_SIZE) * WINDOW_DAMAGE_TILE_SIZE;
        if (bounds.x1 > (int32_t)g_window_server.display_width)
            bounds.x1 = (int32_t)g_window_server.display_width;
        if (bounds.y1 > (int32_t)g_window_server.display_height)
            bounds.y1 = (int32_t)g_window_server.display_height;
    }
    return bounds;
}

bool compositor_registry_window_intersects_damage_bounds_locked(
    const window_server_window_t *window, const Rect *bounds) {
    if (window == 0 || bounds == 0) return false;

    int64_t left = window->x;
    int64_t top = window->y;
    int64_t right = left + (int64_t)window_outer_width(window->width, window->flags);
    int64_t bottom = top + (int64_t)window_outer_height(window->height, window->flags);

    return left < (int64_t)bounds->x1 && right > (int64_t)bounds->x0 &&
           top < (int64_t)bounds->y1 && bottom > (int64_t)bounds->y0;
}

/* 在有效行范围内按位图字查找下一次状态，尾字不读取相邻行的位。 */
static uint32_t compositor_snapshot_find_damage_bit(
    const compositor_snapshot_t *snapshot, uint32_t tile_y,
    uint32_t start, uint32_t limit, bool dirty) {
    while (start < limit) {
        uint32_t index = tile_y * WINDOW_DAMAGE_MAX_TILES_X + start;
        uint32_t offset = index & 63U;
        uint32_t count = 64U - offset;
        if (count > limit - start) count = limit - start;
        uint64_t bits = snapshot->damage_tiles[index >> 6];
        if (!dirty) bits = ~bits;
        bits = (bits >> offset) & (UINT64_MAX >> (64U - count));
        if (bits != 0U) return start + (uint32_t)__builtin_ctzll(bits);
        start += count;
    }
    return limit;
}

uint32_t compositor_snapshot_tiles_to_rects(
    compositor_snapshot_t *snapshot,
    uint32_t capacity) {

    typedef struct active_span {
        uint32_t start_x;
        uint32_t end_x;
        uint32_t rect_index;
    } active_span_t;

    active_span_t active_rows[2][(WINDOW_DAMAGE_MAX_TILES_X + 1U) / 2U];
    active_span_t *previous = active_rows[0];
    active_span_t *current = active_rows[1];

    uint32_t columns;
    uint32_t rows;
    uint32_t count = 0U;
    uint32_t previous_count = 0U;

    if (snapshot == 0 || capacity == 0U) return 0U;

    columns = window_damage_tile_columns_locked();
    rows = window_damage_tile_rows_locked();

    for (uint32_t tile_y = 0U;
         tile_y < rows;
         ++tile_y) {

        uint32_t tile_x = 0U;
        uint32_t current_count = 0U;
        uint32_t previous_cursor = 0U;

        while (tile_x < columns) {
            uint32_t start_x;
            uint32_t end_x;
            uint32_t rect_index = UINT32_MAX;

            start_x = compositor_snapshot_find_damage_bit(
                snapshot, tile_y, tile_x, columns, true);
            if (start_x >= columns) break;
            end_x = compositor_snapshot_find_damage_bit(
                snapshot, tile_y, start_x, columns, false);
            tile_x = end_x;

            while (previous_cursor < previous_count &&
                   previous[previous_cursor].start_x < start_x) {
                ++previous_cursor;
            }

            if (previous_cursor < previous_count &&
                previous[previous_cursor].start_x == start_x &&
                previous[previous_cursor].end_x == end_x) {

                uint32_t index =
                    previous[previous_cursor].rect_index;

                if (index < count &&
                    snapshot->damage_rects[index].y1 ==
                        (int32_t)(tile_y * WINDOW_DAMAGE_TILE_SIZE)) {

                    snapshot->damage_rects[index].y1 =
                        (tile_y + 1U) * WINDOW_DAMAGE_TILE_SIZE;

                    if (snapshot->damage_rects[index].y1 >
                        (int32_t)g_window_server.display_height) {
                        snapshot->damage_rects[index].y1 =
                            g_window_server.display_height;
                    }

                    rect_index = index;
                }
            }

            if (rect_index == UINT32_MAX) {
                Rect candidate = {
                    .x0 = start_x * WINDOW_DAMAGE_TILE_SIZE,
                    .y0 = tile_y * WINDOW_DAMAGE_TILE_SIZE,
                    .x1 = end_x * WINDOW_DAMAGE_TILE_SIZE,
                    .y1 = (tile_y + 1U) * WINDOW_DAMAGE_TILE_SIZE,
                };

                if (candidate.x1 >
                    (int32_t)g_window_server.display_width) {
                    candidate.x1 =
                        g_window_server.display_width;
                }

                if (candidate.y1 >
                    (int32_t)g_window_server.display_height) {
                    candidate.y1 =
                        g_window_server.display_height;
                }

                if (count < capacity) {
                    rect_index = count;
                    snapshot->damage_rects[count++] = candidate;
                } else {
                    /* 溢出时保持有界回退路径的保守性。 */
                    Rect *tail =
                        &snapshot->damage_rects[capacity - 1U];

                    if (candidate.x0 < tail->x0)
                        tail->x0 = candidate.x0;
                    if (candidate.y0 < tail->y0)
                        tail->y0 = candidate.y0;
                    if (candidate.x1 > tail->x1)
                        tail->x1 = candidate.x1;
                    if (candidate.y1 > tail->y1)
                        tail->y1 = candidate.y1;
                }
            }

            if (rect_index != UINT32_MAX &&
                current_count <
                    (WINDOW_DAMAGE_MAX_TILES_X + 1U) / 2U) {
                current[current_count++] = (active_span_t){
                    .start_x = start_x,
                    .end_x = end_x,
                    .rect_index = rect_index,
                };
            }
        }

        previous_count = current_count;
        /* 下一行直接读取本行结果，不再逐项复制活动区间。 */
        active_span_t *finished = previous;
        previous = current;
        current = finished;
    }

    return count;
}
