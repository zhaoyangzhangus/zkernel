#include "internal.h"

/* REFACTOR_P7H_RENDER_PLAN_OWNER：不可变空间计划编译。 */

compositor_render_span_t
    g_compositor_render_plan[WINDOW_RENDER_PLAN_MAX_SPANS];
uint32_t g_compositor_render_plan_count;
bool g_compositor_render_plan_valid;

void compositor_render_plan_reset(void) {
    g_compositor_render_plan_count = 0U;
    g_compositor_render_plan_valid = false;
}

/* 精确遮挡查询和指令编译共用候选集，空瓦片无需遍历窗口。 */
static uint64_t compositor_render_rect_candidates(
    const compositor_snapshot_t *snapshot, const Rect *rect) {
    if (snapshot == 0 || rect == 0 || snapshot->window_count == 0U ||
        snapshot->window_count > 64U || rect_is_empty(*rect)) {
        return 0U;
    }
    uint64_t candidates = UINT64_MAX >> (64U - snapshot->window_count);
    if (rect->x0 < 0 || rect->y0 < 0) return candidates;
    uint32_t first_x = (uint32_t)rect->x0 / WINDOW_DAMAGE_TILE_SIZE;
    uint32_t first_y = (uint32_t)rect->y0 / WINDOW_DAMAGE_TILE_SIZE;
    uint32_t last_x = (uint32_t)(rect->x1 - 1) / WINDOW_DAMAGE_TILE_SIZE;
    uint32_t last_y = (uint32_t)(rect->y1 - 1) / WINDOW_DAMAGE_TILE_SIZE;
    /* 查询瓦片数不超过逐窗扫描次数，大范围绘制仍使用直接几何回退。 */
    uint64_t tiles = (uint64_t)(last_x - first_x + 1U) * (last_y - first_y + 1U);
    if (tiles > snapshot->window_count) return candidates;
    uint64_t touched = 0U;
    for (uint32_t y = first_y; y <= last_y; ++y) {
        for (uint32_t x = first_x; x <= last_x; ++x) {
            const compositor_tile_meta_t *metadata = compositor_tile_metadata_at(x, y);
            /* 任一瓦片不可用便放弃筛选，不能使用不完整并集漏掉窗口。 */
            if (metadata == 0) return candidates;
            touched |= metadata->touch_mask & candidates;
            if (touched == candidates) return candidates;
        }
    }
    return touched;
}

bool compositor_view_fully_covers_snapshot_bounds(
    const compositor_window_view_t *window,
    const Rect *bounds) {

    int64_t left, top, right, bottom;
    return bounds != 0 &&
        compositor_view_opaque_bounds(window, &left, &top, &right, &bottom) &&
        (int64_t)bounds->x0 >= left && (int64_t)bounds->y0 >= top &&
        (int64_t)bounds->x1 <= right && (int64_t)bounds->y1 <= bottom;
}

static uint32_t compositor_render_plan_exact_floor(
    const compositor_snapshot_t *snapshot,
    const Rect *rect) {

    uint64_t candidates = compositor_render_rect_candidates(snapshot, rect);
    while (candidates != 0U) {
        uint32_t index = 63U - (uint32_t)__builtin_clzll(candidates);
        candidates &= ~(1ULL << index);

        if (compositor_view_fully_covers_snapshot_bounds(
                &snapshot->windows[index],
                rect)) {
            return index;
        }
    }

    return UINT32_MAX;
}

static uint32_t compositor_render_plan_tile_floor(
    const compositor_snapshot_t *snapshot,
    uint32_t tile_x,
    uint32_t tile_y) {

    return snapshot == 0 ? UINT32_MAX :
        compositor_tile_occlusion_floor(tile_x, tile_y, snapshot->window_count);

}

typedef struct compositor_render_active_span {
    uint32_t x0;
    uint32_t x1;
    uint32_t first_window;
    uint32_t plan_index;
} compositor_render_active_span_t;

static bool compositor_view_intersects_render_rect(
    const compositor_window_view_t *window,
    const Rect *rect) {

    int64_t left;
    int64_t top;
    int64_t right;
    int64_t bottom;

    if (window == 0 || rect == 0 ||
        rect->x0 >= rect->x1 ||
        rect->y0 >= rect->y1) {
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

    return
        right > (int64_t)rect->x0 &&
        bottom > (int64_t)rect->y0 &&
        left < (int64_t)rect->x1 &&
        top < (int64_t)rect->y1;
}

static uint64_t compositor_compile_render_span_mask(
    const compositor_snapshot_t *snapshot,
    const compositor_render_span_t *span) {

    uint64_t mask = 0U;
    uint32_t first;

    if (snapshot == 0 || span == 0 ||
        snapshot->window_count == 0U) {
        return 0U;
    }

    first =
        span->first_window == UINT32_MAX ?
            0U :
            span->first_window;

    if (first >= snapshot->window_count) {
        return 0U;
    }

    uint64_t candidates = compositor_render_rect_candidates(snapshot, &span->rect);
    candidates &= UINT64_MAX << first;
    while (candidates != 0U) {
        uint32_t index = (uint32_t)__builtin_ctzll(candidates);
        candidates &= candidates - 1U;
        if (compositor_view_intersects_render_rect(
                &snapshot->windows[index], &span->rect)) {
            mask |= 1ULL << index;
        }
    }

    return mask;
}

static void compositor_compile_render_plan_commands(
    const compositor_snapshot_t *snapshot) {

    if (snapshot == 0 ||
        !g_compositor_render_plan_valid) {
        return;
    }

    for (uint32_t index = 0U;
         index < g_compositor_render_plan_count;
         ++index) {

        compositor_render_span_t *span =
            &g_compositor_render_plan[index];

        span->window_mask =
            compositor_compile_render_span_mask(
                snapshot,
                span);
    }
}

bool compositor_build_render_plan(
    const compositor_snapshot_t *snapshot) {

    compositor_render_active_span_t active_rows[2][WINDOW_DAMAGE_MAX_TILES_X];
    compositor_render_active_span_t *previous = active_rows[0];
    compositor_render_active_span_t *current = active_rows[1];

    uint32_t columns;
    uint32_t rows;

    g_compositor_render_plan_count = 0U;
    g_compositor_render_plan_valid = false;

    /* 瓦片数据不可用时由调用方执行精确回退绘制。 */
    if (snapshot == 0 || snapshot->damage_count == 0U ||
        !compositor_tile_metadata_valid()) {
        return false;
    }

    columns = window_damage_tile_columns_locked();
    rows = window_damage_tile_rows_locked();

    if (columns == 0U || rows == 0U) {
        return false;
    }

    for (uint32_t damage_index = 0U;
         damage_index < snapshot->damage_count;
         ++damage_index) {

        Rect clipped_damage;
        const Rect *damage = &snapshot->damage_rects[damage_index];
        uint32_t first_tile_x;
        uint32_t last_tile_x;
        uint32_t first_tile_y;
        uint32_t last_tile_y;
        uint32_t previous_count = 0U;

        /* 有符号裁剪必须先于瓦片索引转换，屏外部分不生成绘制指令。 */
        if (!rect_clip_to_extent(damage->x0, damage->y0,
                damage->x1, damage->y1,
                g_window_server.display_width, g_window_server.display_height,
                &clipped_damage)) {
            continue;
        }
        damage = &clipped_damage;

        first_tile_x =
            damage->x0 /
            WINDOW_DAMAGE_TILE_SIZE;

        last_tile_x =
            (damage->x1 - 1U) /
            WINDOW_DAMAGE_TILE_SIZE;

        first_tile_y =
            damage->y0 /
            WINDOW_DAMAGE_TILE_SIZE;

        last_tile_y =
            (damage->y1 - 1U) /
            WINDOW_DAMAGE_TILE_SIZE;

        if (first_tile_x >= columns ||
            first_tile_y >= rows) {
            continue;
        }

        if (last_tile_x >= columns)
            last_tile_x = columns - 1U;

        if (last_tile_y >= rows)
            last_tile_y = rows - 1U;

        for (uint32_t tile_y = first_tile_y;
             tile_y <= last_tile_y;
             ++tile_y) {

            uint32_t row_count = 0U;
            uint32_t previous_cursor = 0U;

            uint32_t tile_x = first_tile_x;

            uint32_t row_top =
                tile_y * WINDOW_DAMAGE_TILE_SIZE;

            uint32_t row_bottom =
                row_top + WINDOW_DAMAGE_TILE_SIZE;

            if (row_top < (uint32_t)damage->y0)
                row_top = (uint32_t)damage->y0;

            if (row_bottom > (uint32_t)damage->y1)
                row_bottom = (uint32_t)damage->y1;

            while (tile_x <= last_tile_x) {
                uint32_t floor =
                    compositor_render_plan_tile_floor(
                        snapshot,
                        tile_x,
                        tile_y);

                uint32_t run_start = tile_x;
                uint32_t run_end = tile_x + 1U;

                if (floor != UINT32_MAX) {
                    while (run_end <= last_tile_x &&
                           compositor_render_plan_tile_floor(
                               snapshot,
                               run_end,
                               tile_y) == floor) {
                        ++run_end;
                    }
                }

                uint32_t left =
                    run_start * WINDOW_DAMAGE_TILE_SIZE;
                uint32_t right =
                    run_end * WINDOW_DAMAGE_TILE_SIZE;

                if (left < (uint32_t)damage->x0)
                    left = (uint32_t)damage->x0;

                if (right > (uint32_t)damage->x1)
                    right = (uint32_t)damage->x1;

                if (left < right && row_top < row_bottom) {
                    if (floor == UINT32_MAX) {
                        Rect exact_rect = {
                            .x0 = left,
                            .y0 = row_top,
                            .x1 = right,
                            .y1 = row_bottom,
                        };

                        floor =
                            compositor_render_plan_exact_floor(
                                snapshot,
                                &exact_rect);
                    }

                    if (row_count != 0U &&
                        current[row_count - 1U].x1 == left &&
                        current[row_count - 1U].first_window == floor) {

                        current[row_count - 1U].x1 = right;
                    } else {
                        if (row_count >= WINDOW_DAMAGE_MAX_TILES_X) {
                            g_compositor_render_plan_count = 0U;
                            return false;
                        }

                        current[row_count++] =
                            (compositor_render_active_span_t){
                                .x0 = left,
                                .x1 = right,
                                .first_window = floor,
                            };
                    }
                }

                tile_x = run_end;
            }

            for (uint32_t row_index = 0U;
                 row_index < row_count;
                 ++row_index) {

                compositor_render_active_span_t *row = &current[row_index];

                uint32_t plan_index = UINT32_MAX;

                while (previous_cursor < previous_count &&
                       previous[previous_cursor].x0 < row->x0) {
                    ++previous_cursor;
                }

                if (previous_cursor < previous_count &&
                    previous[previous_cursor].x0 == row->x0 &&
                    previous[previous_cursor].x1 == row->x1 &&
                    previous[previous_cursor].first_window ==
                        row->first_window) {

                    uint32_t index =
                        previous[previous_cursor].plan_index;

                    if (index < g_compositor_render_plan_count &&
                        g_compositor_render_plan[index].rect.y1 ==
                            (int32_t)row_top) {

                        g_compositor_render_plan[index].rect.y1 =
                            row_bottom;
                        plan_index = index;
                    }
                }

                if (plan_index == UINT32_MAX) {
                    if (g_compositor_render_plan_count >=
                        WINDOW_RENDER_PLAN_MAX_SPANS) {
                        g_compositor_render_plan_count = 0U;
                        return false;
                    }

                    plan_index =
                        g_compositor_render_plan_count++;

                    g_compositor_render_plan[plan_index] =
                        (compositor_render_span_t){
                            .rect = {
                                .x0 = row->x0,
                                .y0 = row_top,
                                .x1 = row->x1,
                                .y1 = row_bottom,
                            },
                            .first_window = row->first_window,
                        };
                }

                /* 行区间直接补齐计划索引，不再复制到另一份中间数组。 */
                row->plan_index = plan_index;
            }

            previous_count = row_count;
            /* 交换行缓冲区，下一行直接读取本行结果，避免逐项复制。 */
            compositor_render_active_span_t *finished = previous;
            previous = current;
            current = finished;
        }
    }

    if (g_compositor_render_plan_count == 0U) {
        return false;
    }

    g_compositor_render_plan_valid = true;

    compositor_compile_render_plan_commands(snapshot);
    return true;
}
