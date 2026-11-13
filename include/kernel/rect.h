#pragma once

#include <stdbool.h>
#include <stdint.h>

/* 半开矩形：[x0, x1) x [y0, y1)。 */
typedef struct Rect {
    int32_t x0;
    int32_t y0;
    int32_t x1;
    int32_t y1;
} Rect;

static inline bool rect_is_empty(Rect rect) {
    return rect.x0 >= rect.x1 || rect.y0 >= rect.y1;
}

/* 窗口外框可越过 32 位坐标范围，比较前不得收窄；空区域和仅贴边均不相交。 */
static inline bool rect_intersects_bounds(Rect rect, int64_t left, int64_t top,
    int64_t right, int64_t bottom) {
    return !rect_is_empty(rect) && left < right && top < bottom &&
           left < rect.x1 && right > rect.x0 &&
           top < rect.y1 && bottom > rect.y0;
}

static inline bool rect_intersects(Rect a, Rect b) {
    return rect_intersects_bounds(a, b.x0, b.y0, b.x1, b.y1);
}

/* 非空矩形完全位于宽坐标范围内；贴合边界有效，空或反向范围无效。 */
static inline bool rect_inside_bounds(Rect rect, int64_t left, int64_t top,
    int64_t right, int64_t bottom) {
    return !rect_is_empty(rect) && left < right && top < bottom &&
           (int64_t)rect.x0 >= left && (int64_t)rect.y0 >= top &&
           (int64_t)rect.x1 <= right && (int64_t)rect.y1 <= bottom;
}

/* 将宽坐标收窄至 Rect 前先执行裁剪。失败时输出保持不变；显示范围必须落在 Rect
 * 的有符号坐标范围内。 */
static inline bool rect_clip_to_extent(int64_t left, int64_t top,
    int64_t right, int64_t bottom, uint32_t width, uint32_t height, Rect *out) {
    if (!out || width > INT32_MAX || height > INT32_MAX) return false;
    if (left < 0) left = 0;
    if (top < 0) top = 0;
    if (right > (int64_t)width) right = width;
    if (bottom > (int64_t)height) bottom = height;
    if (left >= right || top >= bottom) return false;
    *out = (Rect){(int32_t)left, (int32_t)top, (int32_t)right, (int32_t)bottom};
    return true;
}
