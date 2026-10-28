#ifndef OS_KERNEL_DESKTOP_ICON_H
#define OS_KERNEL_DESKTOP_ICON_H

#include <stdint.h>

typedef struct desktop_icon_axis {
    uint64_t begin, end;
    uint32_t step;
} desktop_icon_axis_t;

/* 两个方向独立选择过滤方式，兼顾横向放大、纵向缩小等非等比图标。
 * 放大按像素中心插值，缩小保留精确面积权重，共用下面的预乘混合循环。 */
static inline desktop_icon_axis_t desktop_icon_axis(uint32_t source,
    uint32_t target, uint32_t position) {
    uint64_t start=(uint64_t)position*source;
    if (target<=source)
        return (desktop_icon_axis_t){start,start+source,target};
    int64_t center=(int64_t)((start/target)<<16U)+
        (int64_t)(((start%target)*2U+source)*32768U/target)-32768;
    if (center<0) center=0;
    uint64_t last=(uint64_t)(source-1U)<<16U;
    if ((uint64_t)center>last) center=(int64_t)last;
    return (desktop_icon_axis_t){(uint64_t)center,(uint64_t)center+65536U,65536U};
}

/* 缩小使用面积过滤保留细节，放大使用平滑插值；输出直通 ARGB 供桌面混合。 */
static inline uint32_t desktop_icon_area_pixel(const uint8_t *pixels,
    uint32_t stride, uint32_t left, uint32_t top,
    uint32_t source_width, uint32_t source_height,
    uint32_t width, uint32_t height, uint32_t column, uint32_t row) {
    if (!pixels || !source_width || !source_height || !width || !height ||
        column >= width || row >= height) return 0U;
    desktop_icon_axis_t x=desktop_icon_axis(source_width,width,column);
    desktop_icon_axis_t y=desktop_icon_axis(source_height,height,row);
    uint64_t alpha = 0U, red = 0U, green = 0U, blue = 0U;
    for (uint32_t sy = (uint32_t)(y.begin / y.step); sy <= (y.end - 1U) / y.step; ++sy) {
        uint64_t a = (uint64_t)sy * y.step, b = a + y.step;
        uint64_t wy = (b < y.end ? b : y.end) - (a > y.begin ? a : y.begin);
        for (uint32_t sx = (uint32_t)(x.begin / x.step); sx <= (x.end - 1U) / x.step; ++sx) {
            a = (uint64_t)sx * x.step; b = a + x.step;
            uint64_t wx = (b < x.end ? b : x.end) - (a > x.begin ? a : x.begin);
            const uint8_t *p = pixels + ((uint64_t)top + sy) * stride +
                               ((uint64_t)left + sx) * 4U;
            uint64_t weight = wx * wy * p[3];
            alpha += weight;
            red += weight * p[0]; green += weight * p[1]; blue += weight * p[2];
        }
    }
    if (!alpha) return 0U;
    uint64_t area = (x.end-x.begin)*(y.end-y.begin);
    return (uint32_t)((alpha + area / 2U) / area) << 24U |
           (uint32_t)((red + alpha / 2U) / alpha) << 16U |
           (uint32_t)((green + alpha / 2U) / alpha) << 8U |
           (uint32_t)((blue + alpha / 2U) / alpha);
}

/* Vector coordinates are in 1/256 units on a 48-unit design canvas. */
static inline uint32_t desktop_icon_vector_sample(uint32_t kind, int32_t x, int32_t y) {
    if (kind == 0U) {
        int32_t dx = x - 24 * 256, dy = y - 24 * 256;
        int64_t radius = (int64_t)dx * dx + (int64_t)dy * dy;
        if (radius > (int64_t)(22 * 256) * (22 * 256)) return 0U;
        if (radius <= (int64_t)(9 * 256) * (9 * 256)) return 0xFF4285F4U;
        if (radius <= (int64_t)(11 * 256) * (11 * 256)) return 0xFFFFFFFFU;
        if (dy < -9 * 256 || (dx > 0 && dy * 1000 < dx * -577)) return 0xFFEA4335U;
        if (dx < 0 && dy * 1000 < dx * -577) return 0xFF34A853U;
        return 0xFFFBBC05U;
    }
    if (x < 4 * 256 || y < 4 * 256 || x >= 44 * 256 || y >= 44 * 256) return 0U;
    int32_t cx = x < 13 * 256 ? 13 * 256 : x > 35 * 256 ? 35 * 256 : x;
    int32_t cy = y < 13 * 256 ? 13 * 256 : y > 35 * 256 ? 35 * 256 : y;
    int32_t dx = x - cx, dy = y - cy;
    if ((int64_t)dx * dx + (int64_t)dy * dy > (int64_t)(9 * 256) * (9 * 256)) return 0U;
    if (y >= 34 * 256 && y < 36 * 256 && x >= 10 * 256 && x < 38 * 256) return 0xFF8498B0U;
    if (x >= 12 * 256 && x < 18 * 256 && y >= 25 * 256 && y < 34 * 256) return 0xFF8AB4F8U;
    if (x >= 22 * 256 && x < 28 * 256 && y >= 19 * 256 && y < 34 * 256) return 0xFF6D9FF4U;
    if (x >= 32 * 256 && x < 38 * 256 && y >= 12 * 256 && y < 34 * 256) return 0xFF6DD6A4U;
    uint32_t shade = (uint32_t)(44 * 256 - y) / 512U;
    return 0xFF1E293BU + (shade << 16U) + (shade << 8U) + shade;
}

static inline uint32_t desktop_icon_vector_pixel(uint32_t kind,
    uint32_t size, uint32_t column, uint32_t row) {
    if (!size || column >= size || row >= size) return 0U;
    uint32_t alpha = 0U, red = 0U, green = 0U, blue = 0U;
    for (uint32_t sy = 0U; sy < 4U; ++sy) for (uint32_t sx = 0U; sx < 4U; ++sx) {
        int32_t x = (int32_t)(((uint64_t)column * 8U + sx * 2U + 1U) * 48U * 256U / (size * 8U));
        int32_t y = (int32_t)(((uint64_t)row * 8U + sy * 2U + 1U) * 48U * 256U / (size * 8U));
        uint32_t p = desktop_icon_vector_sample(kind, x, y);
        uint32_t a = p >> 24U;
        alpha += a; red += ((p >> 16U) & 255U) * a;
        green += ((p >> 8U) & 255U) * a; blue += (p & 255U) * a;
    }
    if (!alpha) return 0U;
    return ((alpha + 8U) / 16U) << 24U |
           ((red + alpha / 2U) / alpha) << 16U |
           ((green + alpha / 2U) / alpha) << 8U | ((blue + alpha / 2U) / alpha);
}
#endif
