#include <kernel/display.h>
#include "internal.h"

#define WINDOW_CURSOR_WIDTH 24U
#define WINDOW_CURSOR_HEIGHT 24U
#define WINDOW_CURSOR_HOTSPOT_X 3U
#define WINDOW_CURSOR_HOTSPOT_Y 1U
uint32_t window_damage_tile_columns_locked(void) {
    uint32_t count = (g_window_server.display_width +
                      WINDOW_DAMAGE_TILE_SIZE - 1U) /
                     WINDOW_DAMAGE_TILE_SIZE;
    return count > WINDOW_DAMAGE_MAX_TILES_X ?
           WINDOW_DAMAGE_MAX_TILES_X : count;
}

uint32_t window_damage_tile_rows_locked(void) {
    uint32_t count = (g_window_server.display_height +
                      WINDOW_DAMAGE_TILE_SIZE - 1U) /
                     WINDOW_DAMAGE_TILE_SIZE;
    return count > WINDOW_DAMAGE_MAX_TILES_Y ?
           WINDOW_DAMAGE_MAX_TILES_Y : count;
}

bool window_damage_tile_is_set_locked(uint32_t tile_x,
                                             uint32_t tile_y) {
    uint32_t index = tile_y * WINDOW_DAMAGE_MAX_TILES_X + tile_x;
    return (g_window_server.damage_tiles[index >> 6] &
            (1ULL << (index & 63U))) != 0U;
}

static void window_damage_tile_range_set_locked(
    uint32_t tile_y,
    uint32_t first_x,
    uint32_t last_x) {

    uint32_t first_index;
    uint32_t last_index;
    uint32_t first_word;
    uint32_t last_word;
    uint32_t first_bit;
    uint32_t last_bit;
    uint64_t first_mask;
    uint64_t last_mask;

    if (first_x > last_x ||
        tile_y >= WINDOW_DAMAGE_MAX_TILES_Y ||
        last_x >= WINDOW_DAMAGE_MAX_TILES_X) {
        return;
    }

    first_index =
        tile_y * WINDOW_DAMAGE_MAX_TILES_X + first_x;

    last_index =
        tile_y * WINDOW_DAMAGE_MAX_TILES_X + last_x;

    first_word = first_index >> 6;
    last_word = last_index >> 6;

    first_bit = first_index & 63U;
    last_bit = last_index & 63U;

    first_mask = UINT64_MAX << first_bit;

    last_mask =
        last_bit == 63U ?
            UINT64_MAX :
            ((1ULL << (last_bit + 1U)) - 1ULL);

    if (first_word == last_word) {
        g_window_server.damage_tiles[first_word] |=
            first_mask & last_mask;
        return;
    }

    g_window_server.damage_tiles[first_word] |= first_mask;

    for (uint32_t word = first_word + 1U;
         word < last_word;
         ++word) {
        g_window_server.damage_tiles[word] = UINT64_MAX;
    }

    g_window_server.damage_tiles[last_word] |= last_mask;
}

static void window_damage_tiles_clear_locked(void) {
    for (uint32_t word = 0U; word < WINDOW_DAMAGE_TILE_WORDS; ++word) {
        g_window_server.damage_tiles[word] = 0U;
    }
}

/* 在首个显示模式确定前初始化损坏区域存储。 */
void window_damage_reset_locked(void) {
    g_window_server.dirty = false;
    g_window_server.damage_full = false;
    g_window_server.damage_tiles_active = false;
    g_window_server.damage_count = 0U;
    g_window_server.damage_bounds.x0 = 0U;
    g_window_server.damage_bounds.y0 = 0U;
    g_window_server.damage_bounds.x1 = 0U;
    g_window_server.damage_bounds.y1 = 0U;
    window_damage_tiles_clear_locked();
    atomic_store_explicit(&g_window_dirty_hint, false,
                          memory_order_release);
}

/* 丢弃空的生产者代数，不触碰合成器状态。 */
void window_damage_clear_pending_locked(void) {
    g_window_server.dirty = false;
    atomic_store_explicit(&g_window_dirty_hint, false,
                          memory_order_release);
}

/* 将生产者持有的损坏区域轮转到不可变的合成器快照中。 */
void window_damage_rotate_locked(void) {
    g_window_server.damage_count = 0U;
    g_window_server.damage_full = false;
    g_window_server.damage_tiles_active = false;
    window_damage_clear_pending_locked();
    g_window_server.damage_bounds.x0 = g_window_server.display_width;
    g_window_server.damage_bounds.y0 = g_window_server.display_height;
    g_window_server.damage_bounds.x1 = 0U;
    g_window_server.damage_bounds.y1 = 0U;
}

static void window_damage_mark_tiles_locked(const Rect *rect) {
    uint32_t columns;
    uint32_t rows;
    uint32_t first_x;
    uint32_t first_y;
    uint32_t last_x;
    uint32_t last_y;
    if (rect == 0 ||
        rect->x0 >= rect->x1 ||
        rect->y0 >= rect->y1) {
        return;
    }

    columns = window_damage_tile_columns_locked();
    rows = window_damage_tile_rows_locked();
    if (columns == 0U || rows == 0U) return;
    first_x = rect->x0 / WINDOW_DAMAGE_TILE_SIZE;
    first_y = rect->y0 / WINDOW_DAMAGE_TILE_SIZE;
    last_x = (rect->x1 - 1U) / WINDOW_DAMAGE_TILE_SIZE;
    last_y = (rect->y1 - 1U) / WINDOW_DAMAGE_TILE_SIZE;
    if (first_x >= columns || first_y >= rows) return;
    if (last_x >= columns) last_x = columns - 1U;
    if (last_y >= rows) last_y = rows - 1U;
    for (uint32_t tile_y = first_y;
         tile_y <= last_y;
         ++tile_y) {
        window_damage_tile_range_set_locked(
            tile_y, first_x, last_x);
    }
}

/*
 * 发布一个新的生产者代数。
 *
 * 此时持有 window_lock，因此 g_window_server.dirty 用作边沿检测器。
 * 仅在 clean -> dirty 时返回 true。
 */
static inline bool window_dirty_edge_locked(void) {
    if (g_window_server.dirty) {
        return false;
    }

    g_window_server.dirty = true;

    /*
    * 代数元数据会在 window_lock 外读取。dirty_hint 的 release 存储是发布新待处理场景代数的时刻。
     */
    (void)atomic_fetch_add_explicit(
        &g_window_dirty_generation,
        1U,
        memory_order_relaxed);

    atomic_store_explicit(
        &g_window_dirty_hint,
        true,
        memory_order_release);

    return true;
}


void window_mark_dirty_locked(void);

static void window_damage_enable_tiles_locked(void) {
    if (g_window_server.damage_tiles_active) return;
    /* 固定位图刻意覆盖常见 4K 级输出。若未来模式超出范围，继续使用现有全屏损坏路径保证正确性，
     * 不要静默裁掉右侧/底部边缘。 */
    if (g_window_server.display_width >
            WINDOW_DAMAGE_MAX_TILES_X * WINDOW_DAMAGE_TILE_SIZE ||
        g_window_server.display_height >
            WINDOW_DAMAGE_MAX_TILES_Y * WINDOW_DAMAGE_TILE_SIZE) {
        window_mark_dirty_locked();
        return;
    }
    window_damage_tiles_clear_locked();
    for (uint32_t index = 0U;
         index < g_window_server.damage_count;
         ++index) {
        window_damage_mark_tiles_locked(&g_window_server.damage_rects[index]);
    }
    g_window_server.damage_count = 0U;
    g_window_server.damage_tiles_active = true;
}

void window_mark_dirty_locked(void) {
    /*
    * 将已损坏的代数升级为全屏损坏，不会产生第二个调度边沿。
     */
    (void)window_dirty_edge_locked();

    g_window_server.damage_full = true;
    g_window_server.damage_tiles_active = false;
    g_window_server.damage_count = 0U;
    g_window_server.damage_bounds.x0 = 0U;
    g_window_server.damage_bounds.y0 = 0U;
    g_window_server.damage_bounds.x1 = g_window_server.display_width;
    g_window_server.damage_bounds.y1 = g_window_server.display_height;
}

/* 插入与批量整理共用面积策略；包含关系直接返回，避免常见重复更新的面积运算。 */
static bool window_damage_try_merge(Rect *a, const Rect *b) {
    if (a->x0 <= b->x0 && a->y0 <= b->y0 &&
        a->x1 >= b->x1 && a->y1 >= b->y1) return true;
    if (b->x0 <= a->x0 && b->y0 <= a->y0 &&
        b->x1 >= a->x1 && b->y1 >= a->y1) {
        *a = *b;
        return true;
    }

    uint32_t left = a->x0 < b->x0 ? a->x0 : b->x0;
    uint32_t top = a->y0 < b->y0 ? a->y0 : b->y0;
    uint32_t right = a->x1 > b->x1 ? a->x1 : b->x1;
    uint32_t bottom = a->y1 > b->y1 ? a->y1 : b->y1;
    uint32_t overlap_left = a->x0 > b->x0 ? a->x0 : b->x0;
    uint32_t overlap_top = a->y0 > b->y0 ? a->y0 : b->y0;
    uint32_t overlap_right = a->x1 < b->x1 ? a->x1 : b->x1;
    uint32_t overlap_bottom = a->y1 < b->y1 ? a->y1 : b->y1;
    uint64_t overlap = 0U;
    if (overlap_left < overlap_right && overlap_top < overlap_bottom) {
        overlap = (uint64_t)(overlap_right - overlap_left) *
            (overlap_bottom - overlap_top);
    }
    uint64_t union_area = (uint64_t)(a->x1 - a->x0) * (a->y1 - a->y0) +
        (uint64_t)(b->x1 - b->x0) * (b->y1 - b->y0) - overlap;
    uint64_t merged_area = (uint64_t)(right - left) * (bottom - top);
    /* 包围盒比真实并集多出的像素不超过 25%，保留原有合并阈值。 */
    if (merged_area > union_area + union_area / 4U) return false;
    *a = (Rect){.x0 = left, .y0 = top, .x1 = right, .y1 = bottom};
    return true;
}

void window_mark_rect_locked(int32_t x, int32_t y,
                                    uint32_t width, uint32_t height) {
    int64_t right = (int64_t)x + width;
    int64_t bottom = (int64_t)y + height;
    uint32_t left;
    uint32_t top;
    if (width == 0U || height == 0U || g_window_server.display_width == 0U ||
        g_window_server.display_height == 0U || right <= 0 || bottom <= 0 ||
        x >= (int32_t)g_window_server.display_width ||
        y >= (int32_t)g_window_server.display_height) return;
    left = x < 0 ? 0U : (uint32_t)x;
    top = y < 0 ? 0U : (uint32_t)y;
    if (right > (int64_t)g_window_server.display_width) {
        right = g_window_server.display_width;
    }
    if (bottom > (int64_t)g_window_server.display_height) {
        bottom = g_window_server.display_height;
    }
    bool first_damage =
        !g_window_server.dirty;

    if (first_damage) {
        (void)window_dirty_edge_locked();
    }

    if (g_window_server.damage_full) {
        return;
    }

    Rect candidate = {
        .x0 = left,
        .y0 = top,
        .x1 = (uint32_t)right,
        .y1 = (uint32_t)bottom,
    };

    /*
    * 一个保守的生产者边界即可让快照捕获通过四次比较排除无关窗口，无需扩展瓦片损坏区域，
    * 也无需在 window_lock 下扫描完整矩形集合。
     */
    if (first_damage) {
        g_window_server.damage_bounds.x0 =
            candidate.x0;
        g_window_server.damage_bounds.y0 =
            candidate.y0;
        g_window_server.damage_bounds.x1 =
            candidate.x1;
        g_window_server.damage_bounds.y1 =
            candidate.y1;
    } else {
        if (candidate.x0 <
            g_window_server.damage_bounds.x0) {
            g_window_server.damage_bounds.x0 =
                candidate.x0;
        }

        if (candidate.y0 <
            g_window_server.damage_bounds.y0) {
            g_window_server.damage_bounds.y0 =
                candidate.y0;
        }

        if (candidate.x1 >
            g_window_server.damage_bounds.x1) {
            g_window_server.damage_bounds.x1 =
                candidate.x1;
        }

        if (candidate.y1 >
            g_window_server.damage_bounds.y1) {
            g_window_server.damage_bounds.y1 =
                candidate.y1;
        }
    }
    if (g_window_server.damage_tiles_active) {
        window_damage_mark_tiles_locked(&candidate);
        return;
    }
    for (uint32_t index = 0U; index < g_window_server.damage_count; ++index) {
        Rect *current = &g_window_server.damage_rects[index];
        if (candidate.x1 < current->x0 || current->x1 < candidate.x0 ||
            candidate.y1 < current->y0 || current->y1 < candidate.y0) continue;
        if (!window_damage_try_merge(current, &candidate)) continue;
        return;
    }
    if (g_window_server.damage_count >= WINDOW_DAMAGE_MAX_RECTS) {
        window_damage_enable_tiles_locked();
        window_damage_mark_tiles_locked(&candidate);
        return;
    }
    g_window_server.damage_rects[g_window_server.damage_count++] = candidate;
}

void window_mark_window_locked(const window_server_window_t *window) {
    if (window == 0) return;
    window_mark_rect_locked(window->x, window->y,
                            window_outer_width(window->width, window->flags),
                            window_outer_height(window->height, window->flags));
}

void window_mark_surface_locked(const window_server_window_t *window,
                                       int32_t x, int32_t y,
                                       uint32_t width, uint32_t height) {
    int64_t left;
    int64_t top;
    int64_t right;
    int64_t bottom;
    if (window == 0 || width == 0U || height == 0U) return;
    left = (int64_t)window->x +
           window_client_offset_x(window->flags) + x;
    top =
        (int64_t)window->y +
        window_client_offset_y(window->flags) +
        y;
    right = left + width;
    bottom = top + height;
    if (right <= 0 || bottom <= 0 ||
        left >= (int64_t)g_window_server.display_width ||
        top >= (int64_t)g_window_server.display_height) return;
    if (left < 0) left = 0;
    if (top < 0) top = 0;
    if (right > (int64_t)g_window_server.display_width) {
        right = g_window_server.display_width;
    }
    if (bottom > (int64_t)g_window_server.display_height) {
        bottom = g_window_server.display_height;
    }
    if (left >= right || top >= bottom) return;
    window_mark_rect_locked((int32_t)left, (int32_t)top,
                            (uint32_t)(right - left),
                            (uint32_t)(bottom - top));
}

/*
 * 窗口移动会产生两个逻辑损坏区域：
 *
 *   1. 旧位置：恢复当前位于窗口下方的内容
 *   2. 新位置：在新坐标处绘制窗口
 *
 * 不要立即将它们转换为一个包围矩形。通用损坏区域合并代码会在包围盒浪费不超过 25% 时
 * 合并附近或重叠的矩形，而较大的移动仍保留为两个独立区域。
 *
 * 拖动最上层窗口时，重叠像素会在保留的合成缓冲区中移动，因此只需栅格化新暴露的条带。
 * 其他调用方可通过 render_new_position=true 请求完整的新矩形。
 */
void window_mark_moved_rect_locked(
    int32_t old_x,
    int32_t old_y,
    int32_t new_x,
    int32_t new_y,
    uint32_t width,
    uint32_t height,
    bool render_new_position) {

    int64_t old_left;
    int64_t old_top;
    int64_t old_right;
    int64_t old_bottom;

    int64_t new_left;
    int64_t new_top;
    int64_t new_right;
    int64_t new_bottom;

    int64_t overlap_left;
    int64_t overlap_top;
    int64_t overlap_right;
    int64_t overlap_bottom;

    if (width == 0U ||
        height == 0U) {
        return;
    }

    if (old_x == new_x &&
        old_y == new_y) {

        window_mark_rect_locked(
            old_x,
            old_y,
            width,
            height);

        return;
    }

    old_left = old_x;
    old_top = old_y;
    old_right =
        old_left + (int64_t)width;
    old_bottom =
        old_top + (int64_t)height;

    new_left = new_x;
    new_top = new_y;
    new_right =
        new_left + (int64_t)width;
    new_bottom =
        new_top + (int64_t)height;

    overlap_left =
        old_left > new_left ?
        old_left : new_left;

    overlap_top =
        old_top > new_top ?
        old_top : new_top;

    overlap_right =
        old_right < new_right ?
        old_right : new_right;

    overlap_bottom =
        old_bottom < new_bottom ?
        old_bottom : new_bottom;

    /*
    * 没有重叠：保留常规的旧位置/新位置矩形对。
     */
    if (overlap_left >= overlap_right ||
        overlap_top >= overlap_bottom) {

        window_mark_rect_locked(
            old_x,
            old_y,
            width,
            height);

        window_mark_rect_locked(
            new_x,
            new_y,
            width,
            height);

        return;
    }

    /*
    * 旧区域 \ 新区域：
     *
    * 横向暴露条带覆盖旧矩形的完整高度。
     */
    if (new_left > old_left) {
        uint32_t exposed_width =
            (uint32_t)(
                overlap_left -
                old_left);

        if (exposed_width != 0U) {
            window_mark_rect_locked(
                old_x,
                old_y,
                exposed_width,
                height);
        }
    } else if (new_left < old_left) {
        uint32_t exposed_width =
            (uint32_t)(
                old_right -
                overlap_right);

        if (exposed_width != 0U) {
            window_mark_rect_locked(
                (int32_t)overlap_right,
                old_y,
                exposed_width,
                height);
        }
    }

    /*
    * 纵向暴露条带仅覆盖横向重叠部分，因此不会与上方条带重叠。
     */
    {
        uint32_t overlap_width =
            (uint32_t)(
                overlap_right -
                overlap_left);

        if (new_top > old_top) {
            uint32_t exposed_height =
                (uint32_t)(
                    overlap_top -
                    old_top);

            if (overlap_width != 0U &&
                exposed_height != 0U) {

                window_mark_rect_locked(
                    (int32_t)overlap_left,
                    old_y,
                    overlap_width,
                    exposed_height);
            }
        } else if (new_top < old_top) {
            uint32_t exposed_height =
                (uint32_t)(
                    old_bottom -
                    overlap_bottom);

            if (overlap_width != 0U &&
                exposed_height != 0U) {

                window_mark_rect_locked(
                    (int32_t)overlap_left,
                    (int32_t)overlap_bottom,
                    overlap_width,
                    exposed_height);
            }
        }
    }

    if (!render_new_position) {
        /* 新区域 \ 旧区域：重叠部分已由保留内容拖动复制，因此只需在目标位置渲染新暴露的
         * 横向/纵向条带。 */
        if (old_left > new_left) {
            uint32_t exposed_width =
                (uint32_t)(overlap_left - new_left);
            if (exposed_width != 0U) {
                window_mark_rect_locked(new_x, new_y,
                                        exposed_width, height);
            }
        } else if (old_left < new_left) {
            uint32_t exposed_width =
                (uint32_t)(new_right - overlap_right);
            if (exposed_width != 0U) {
                window_mark_rect_locked((int32_t)overlap_right, new_y,
                                        exposed_width, height);
            }
        }

        {
            uint32_t overlap_width =
                (uint32_t)(overlap_right - overlap_left);
            if (old_top > new_top) {
                uint32_t exposed_height =
                    (uint32_t)(overlap_top - new_top);
                if (overlap_width != 0U && exposed_height != 0U) {
                    window_mark_rect_locked((int32_t)overlap_left, new_y,
                                            overlap_width, exposed_height);
                }
            } else if (old_top < new_top) {
                uint32_t exposed_height =
                    (uint32_t)(new_bottom - overlap_bottom);
                if (overlap_width != 0U && exposed_height != 0U) {
                    window_mark_rect_locked((int32_t)overlap_left,
                                            (int32_t)overlap_bottom,
                                            overlap_width, exposed_height);
                }
            }
        }
    }

    if (render_new_position) {
        /* 未移动保留像素时，仍必须渲染最终位置的完整内容。 */
        window_mark_rect_locked(new_x, new_y, width, height);
    }
}

void window_mark_drag_corner_repair_locked(
    int32_t x,
    int32_t y,
    uint32_t outer_width,
    uint32_t outer_height,
    uint32_t flags) {

    uint32_t radius = WINDOW_CORNER_RADIUS;

    (void)flags;

    if (outer_width == 0U || outer_height == 0U) {
        return;
    }

    if (radius > outer_width) radius = outer_width;
    if (radius > outer_height) radius = outer_height;
    if (radius == 0U) return;

    window_mark_rect_locked(x, y, radius, radius);
    window_mark_rect_locked(
        x + (int32_t)(outer_width - radius),
        y, radius, radius);
    window_mark_rect_locked(
        x,
        y + (int32_t)(outer_height - radius),
        radius, radius);
    window_mark_rect_locked(
        x + (int32_t)(outer_width - radius),
        y + (int32_t)(outer_height - radius),
        radius, radius);
}


void window_mark_moved_cursor_locked(uint32_t old_x, uint32_t old_y,
                                            uint32_t new_x, uint32_t new_y) {
    if (old_x == new_x && old_y == new_y) {
        return;
    }

    if (g_window_server.composite_framebuffer != 0 &&
        g_window_server.composite_framebuffer !=
            g_window_server.framebuffer &&
        !display_core_has_native_scanout(0U)) {
        /*
         * 保留的 WB 场景有意不包含光标。仅移动指针不会改变场景像素，因此不得调度完整合成
         * 帧。QEMU 和真机均直接更新可见 framebuffer 上的光标。
         */
        return;
    }

    int32_t old_left;
    int32_t old_top;
    int32_t new_left;
    int32_t new_top;

    /* pointer_x/y 鏄?cursor hotspot锛屼笉鏄?sprite 宸︿笂瑙掋€?*/
    uint32_t old_shape = g_window_server.presented_cursor_shape;
    uint32_t new_shape = window_input_cursor_shape_locked();
    old_left = (int32_t)old_x - (int32_t)window_cursor_hotspot_x(old_shape);
    old_top = (int32_t)old_y - (int32_t)window_cursor_hotspot_y(old_shape);
    new_left = (int32_t)new_x - (int32_t)window_cursor_hotspot_x(new_shape);
    new_top = (int32_t)new_y - (int32_t)window_cursor_hotspot_y(new_shape);

    /*
    * 旧、新光标位置是独立的损坏区域。指针跳跃时，不要创建可能覆盖整屏的包围盒。
     */
    window_mark_rect_locked(old_left, old_top,
                            WINDOW_CURSOR_WIDTH, WINDOW_CURSOR_HEIGHT);

    if (old_left != new_left || old_top != new_top) {
        window_mark_rect_locked(new_left, new_top,
                                WINDOW_CURSOR_WIDTH, WINDOW_CURSOR_HEIGHT);
    }
}

/* 涓€娆￠紶鏍囦簨鍔″彧鍏佽涓€娆?framebuffer 鍖哄煙鎻愪氦銆傜劍鐐广€佷换鍔℃爮銆佽鎷栧姩
 * 绐楀彛鍜屽厜鏍囧彲鑳藉垎鍒骇鐢?damage锛涘垎寮€鎻愪氦浼氳 GOP 鎵弿鍒板崐甯х姸鎬併€?*/
void window_coalesce_damage_locked(void) {
    uint32_t first = 0U;

    if (g_window_server.damage_full ||
        g_window_server.damage_tiles_active ||
        g_window_server.damage_count <= 1U) {
        return;
    }

    /*
    * 保持独立的损坏矩形彼此分离。仅当包围矩形相比真实并集多出的像素不超过 25% 时才合并。
    * 矩形数最多为 WINDOW_DAMAGE_MAX_RECTS；每次合并减少一个矩形，重新检查次数有界。
     */
    while (first < g_window_server.damage_count) {
        uint32_t second = first + 1U;

        while (second < g_window_server.damage_count) {
            Rect *a =
                &g_window_server.damage_rects[first];
            const Rect *b =
                &g_window_server.damage_rects[second];

            if (window_damage_try_merge(a, b)) {
                for (uint32_t index = second + 1U;
                     index < g_window_server.damage_count;
                     ++index) {
                    g_window_server.damage_rects[index - 1U] =
                        g_window_server.damage_rects[index];
                }

                --g_window_server.damage_count;

                /* 后面的矩形扩大后，也可能与前面已处理的矩形满足合并条件。
                 * 每次成功都减少一个矩形；最多 16 个矩形，重新扫描次数有界。 */
                first = 0U;
                second = 1U;
                continue;
            }

            ++second;
        }

        ++first;
    }
}

/*
 * 鍦嗚鍗婂緞鍥哄畾涓?WINDOW_CORNER_RADIUS锛屽洜姝や笉鍦ㄥ悎鎴愮儹璺緞涓绠楀钩鏂瑰拰銆? * 姣忎竴椤硅〃绀鸿鎵弿绾夸粠宸︺€佸彸鍚勮鎺夌殑鍍忕礌鏁帮紱涓嬪崐閮ㄥ垎鍙嶅悜绱㈠紩鍗冲彲銆? */
