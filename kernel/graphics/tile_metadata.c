#include "internal.h"
#include <kernel/perf.h>
#include <kernel/telemetry.h>

/* REFACTOR_P7F_TILE_METADATA_OWNER: per-frame tile masks and self-test. */

_Static_assert(WINDOW_SERVER_MAX_WINDOWS <= 64U,
               "tile metadata masks require <= 64 windows");

compositor_tile_meta_t g_compositor_tile_meta[WINDOW_DAMAGE_TILE_COUNT]
    __attribute__((aligned(64)));

static uint32_t g_compositor_tile_columns;
static uint32_t g_compositor_tile_rows;
static bool g_compositor_tile_meta_valid;

static uint32_t tile_count_for_extent(uint32_t extent, uint32_t limit) {
    uint32_t count =
        (extent + WINDOW_DAMAGE_TILE_SIZE - 1U) /
        WINDOW_DAMAGE_TILE_SIZE;
    return count > limit ? limit : count;
}

static bool tile_view_bounds(const compositor_window_view_t *view,
                             int64_t *left, int64_t *top,
                             int64_t *right, int64_t *bottom) {
    if (view == 0 || left == 0 || top == 0 || right == 0 || bottom == 0) {
        return false;
    }

    *left = view->x;
    *top = view->y;
    *right =
        *left +
        (int64_t)window_outer_width(view->width, view->flags);
    *bottom =
        *top +
        (int64_t)window_outer_height(view->height, view->flags);

    return *left < *right && *top < *bottom;
}

static bool tile_intersects(int64_t left, int64_t top,
                            int64_t right, int64_t bottom,
                            int64_t tile_left, int64_t tile_top,
                            int64_t tile_right, int64_t tile_bottom) {
    return left < tile_right && right > tile_left &&
           top < tile_bottom && bottom > tile_top;
}

/* 已知外框时只计算不透明内接区域，避免重复查询窗口尺寸。 */
static bool tile_inset_opaque_bounds(const compositor_window_view_t *view,
    int64_t *left, int64_t *top, int64_t *right, int64_t *bottom) {
    if ((view->flags & OS_WINDOW_PREMULTIPLIED_ALPHA) != 0U) return false;
    uint32_t inset = window_client_decorations(view->flags) ?
        (view->maximized ? 0U : 8U) : WINDOW_CORNER_RADIUS;
    if (*right - *left <= (int64_t)inset * 2 ||
        *bottom - *top <= (int64_t)inset * 2) return false;
    *left += inset;
    *top += inset;
    *right -= inset;
    *bottom -= inset;
    return true;
}

bool compositor_view_opaque_bounds(const compositor_window_view_t *view,
    int64_t *left, int64_t *top, int64_t *right, int64_t *bottom) {
    if (view != 0 && (view->flags & OS_WINDOW_PREMULTIPLIED_ALPHA) != 0U)
        return false;
    return tile_view_bounds(view, left, top, right, bottom) &&
        tile_inset_opaque_bounds(view, left, top, right, bottom);
}

static bool tile_metadata_build_into(
    const compositor_snapshot_t *snapshot,
    uint32_t display_width,
    uint32_t display_height,
    compositor_tile_meta_t *metadata,
    uint32_t *out_columns,
    uint32_t *out_rows) {
    if (snapshot == 0 || metadata == 0 || out_columns == 0 ||
        out_rows == 0 || snapshot->window_count > WINDOW_SERVER_MAX_WINDOWS) {
        return false;
    }
    uint32_t columns = tile_count_for_extent(display_width, WINDOW_DAMAGE_MAX_TILES_X);
    uint32_t rows = tile_count_for_extent(display_height, WINDOW_DAMAGE_MAX_TILES_Y);
    if (columns == 0U || rows == 0U) return false;

    for (uint32_t y = 0U; y < rows; ++y) {
        for (uint32_t x = 0U; x < columns; ++x)
            metadata[y * WINDOW_DAMAGE_MAX_TILES_X + x] = (compositor_tile_meta_t){0};
    }

    /* 每个窗口只计算一次外框，再遍历接触瓦片，掩码顺序与快照保持一致。 */
    for (uint32_t window_index = 0U; window_index < snapshot->window_count; ++window_index) {
        const compositor_window_view_t *view = &snapshot->windows[window_index];
        int64_t left, top, right, bottom;
        int64_t opaque_left, opaque_top, opaque_right, opaque_bottom;
        if ((view->flags & OS_WINDOW_VISIBLE) == 0U ||
            !tile_view_bounds(view, &left, &top, &right, &bottom) ||
            !tile_intersects(left, top, right, bottom,
                             0, 0, display_width, display_height)) {
            continue;
        }
        uint32_t first_x = left > 0 ? (uint32_t)(left / WINDOW_DAMAGE_TILE_SIZE) : 0U;
        uint32_t first_y = top > 0 ? (uint32_t)(top / WINDOW_DAMAGE_TILE_SIZE) : 0U;
        int64_t clipped_right = right < (int64_t)display_width ? right : display_width;
        int64_t clipped_bottom = bottom < (int64_t)display_height ? bottom : display_height;
        uint32_t last_x = (uint32_t)((clipped_right - 1) / WINDOW_DAMAGE_TILE_SIZE);
        uint32_t last_y = (uint32_t)((clipped_bottom - 1) / WINDOW_DAMAGE_TILE_SIZE);
        if (last_x >= columns) last_x = columns - 1U;
        if (last_y >= rows) last_y = rows - 1U;
        opaque_left = left;
        opaque_top = top;
        opaque_right = right;
        opaque_bottom = bottom;
        bool opaque = tile_inset_opaque_bounds(view, &opaque_left, &opaque_top,
                                               &opaque_right, &opaque_bottom);
        uint64_t bit = 1ULL << window_index;
        for (uint32_t y = first_y; y <= last_y; ++y) {
            int64_t tile_top = (int64_t)y * WINDOW_DAMAGE_TILE_SIZE;
            int64_t tile_bottom = tile_top + WINDOW_DAMAGE_TILE_SIZE;
            if (tile_bottom > (int64_t)display_height) tile_bottom = display_height;
            /* 同一行共用纵向覆盖结果，透明行只写接触掩码。 */
            bool opaque_row = opaque && opaque_top <= tile_top &&
                              opaque_bottom >= tile_bottom;
            uint32_t row_index = y * WINDOW_DAMAGE_MAX_TILES_X;
            for (uint32_t x = first_x; x <= last_x; ++x) {
                compositor_tile_meta_t *tile = &metadata[row_index + x];
                tile->touch_mask |= bit;
                if (!opaque_row) continue;
                int64_t tile_left = (int64_t)x * WINDOW_DAMAGE_TILE_SIZE;
                int64_t tile_right = tile_left + WINDOW_DAMAGE_TILE_SIZE;
                if (tile_right > (int64_t)display_width) tile_right = display_width;
                if (opaque_left <= tile_left && opaque_right >= tile_right)
                    tile->opaque_mask |= bit;
            }
        }
    }
    *out_columns = columns;
    *out_rows = rows;
    return true;
}

bool compositor_tile_metadata_build(
    const compositor_snapshot_t *snapshot) {
    uint32_t columns = 0U;
    uint32_t rows = 0U;

    g_compositor_tile_meta_valid = false;

    /* 动画帧使用动态裁剪和平移，不消费静态瓦片遮挡数据。
     * 先作废旧结果再跳过构建，结束动画后的普通帧必须重新计算。 */
    if (snapshot != 0 && snapshot->animations) {
        g_compositor_tile_columns = 0U;
        g_compositor_tile_rows = 0U;
        return false;
    }

    if (!tile_metadata_build_into(
            snapshot,
            g_window_server.display_width,
            g_window_server.display_height,
            g_compositor_tile_meta,
            &columns,
            &rows)) {
        g_compositor_tile_columns = 0U;
        g_compositor_tile_rows = 0U;
        return false;
    }

    g_compositor_tile_columns = columns;
    g_compositor_tile_rows = rows;
    g_compositor_tile_meta_valid = true;
    return true;
}

bool compositor_tile_metadata_valid(void) {
    return g_compositor_tile_meta_valid;
}

const compositor_tile_meta_t *compositor_tile_metadata_at(
    uint32_t tile_x,
    uint32_t tile_y) {
    if (!g_compositor_tile_meta_valid ||
        tile_x >= g_compositor_tile_columns ||
        tile_y >= g_compositor_tile_rows) {
        return 0;
    }

    return &g_compositor_tile_meta[
        tile_y * WINDOW_DAMAGE_MAX_TILES_X + tile_x];
}

bool compositor_tile_self_test(void) {
    compositor_snapshot_t snapshot;
    compositor_tile_meta_t metadata[2];
    uint32_t columns = 0U;
    uint32_t rows = 0U;
    uint64_t benchmark_start;

    snapshot.window_count = 1U;
    snapshot.windows[0] = (compositor_window_view_t){
        .x = 0,
        .y = 0,
        .width = 128U,
        .height = 64U,
        .flags = OS_WINDOW_VISIBLE | OS_WINDOW_CLIENT_DECORATIONS,
    };

    if (!tile_metadata_build_into(
            &snapshot,
            64U,
            64U,
            metadata,
            &columns,
            &rows) ||
        columns != 1U ||
        rows != 1U ||
        metadata[0].touch_mask != 0x1U ||
        metadata[0].opaque_mask != 0U) {
        return false;
    }

    /* A restored surface touches the corner tile but does not fully occlude
     * it. The same maximized window is square and genuinely covers the tile. */
    snapshot.windows[0].maximized = true;
    if (!tile_metadata_build_into(&snapshot, 64U, 64U, metadata, &columns, &rows) ||
        metadata[0].opaque_mask != 0x1U) return false;
    snapshot.windows[0].maximized = false;

    snapshot.window_count = 2U;
    snapshot.windows[1] = (compositor_window_view_t){
        .x = 32,
        .y = 0,
        .width = 32U,
        .height = 64U,
        .flags = OS_WINDOW_VISIBLE | OS_WINDOW_CLIENT_DECORATIONS,
    };

    if (!tile_metadata_build_into(
            &snapshot,
            128U,
            64U,
            metadata,
            &columns,
            &rows) ||
        columns != 2U ||
        rows != 1U) {
        return false;
    }

    if (metadata[0].touch_mask != 0x3U ||
        metadata[0].opaque_mask != 0U ||
        metadata[1].touch_mask != 0x1U ||
        metadata[1].opaque_mask != 0U) {
        return false;
    }

    for (uint32_t index = 2U; index < 8U; ++index) {
        snapshot.windows[index] = (compositor_window_view_t){
            .x = (int32_t)(index * 4U),
            .y = 0,
            .width = 32U,
            .height = 64U,
            .flags = OS_WINDOW_VISIBLE | OS_WINDOW_CLIENT_DECORATIONS,
        };
    }

    snapshot.window_count = 4U;
    benchmark_start = telemetry_timestamp();
    if (!tile_metadata_build_into(
            &snapshot,
            128U,
            64U,
            metadata,
            &columns,
            &rows) ||
        columns != 2U ||
        rows != 1U) {
        return false;
    }
    kernel_perf_emit_scope("graphics.overlap_4", benchmark_start);

    snapshot.window_count = 8U;
    benchmark_start = telemetry_timestamp();
    if (!tile_metadata_build_into(
            &snapshot,
            128U,
            64U,
            metadata,
            &columns,
            &rows) ||
        columns != 2U ||
        rows != 1U) {
        return false;
    }
    kernel_perf_emit_scope("graphics.overlap_8", benchmark_start);

    return metadata[0].touch_mask == 0xFFU &&
           metadata[0].opaque_mask == 0U &&
           metadata[1].touch_mask == 0x1U &&
           metadata[1].opaque_mask == 0U;
}

/* 规划与回退绘制共用瓦片遮挡下限，透明窗口不能遮住下面的图层。 */
uint32_t compositor_tile_occlusion_floor(uint32_t tile_x, uint32_t tile_y,
                                         uint32_t window_count) {
    const compositor_tile_meta_t *metadata = compositor_tile_metadata_at(tile_x, tile_y);
    if (metadata == 0) return UINT32_MAX;
    uint64_t blockers = metadata->opaque_mask;
    if (blockers == 0U) return UINT32_MAX;
    uint32_t floor = 63U - (uint32_t)__builtin_clzll(blockers);
    return floor < window_count ? floor : UINT32_MAX;
}

uint32_t compositor_occlusion_floor_for_damage(void) {
    Rect bounds;
    if (!compositor_tile_metadata_valid() ||
        !rect_clip_to_extent(g_compositor_snapshot.damage_bounds.x0,
            g_compositor_snapshot.damage_bounds.y0,
            g_compositor_snapshot.damage_bounds.x1,
            g_compositor_snapshot.damage_bounds.y1,
            g_window_server.display_width, g_window_server.display_height, &bounds))
        return UINT32_MAX;

    uint32_t floor = UINT32_MAX;
    for (uint32_t y = (uint32_t)bounds.y0 / WINDOW_DAMAGE_TILE_SIZE;
         y <= (uint32_t)(bounds.y1 - 1) / WINDOW_DAMAGE_TILE_SIZE; ++y) {
        for (uint32_t x = (uint32_t)bounds.x0 / WINDOW_DAMAGE_TILE_SIZE;
             x <= (uint32_t)(bounds.x1 - 1) / WINDOW_DAMAGE_TILE_SIZE; ++x) {
            uint32_t candidate = compositor_tile_occlusion_floor(
                x, y, g_compositor_snapshot.window_count);
            if (candidate == UINT32_MAX) return UINT32_MAX;
            if (candidate < floor) floor = candidate;
        }
    }
    return floor;
}
