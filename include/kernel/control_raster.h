#pragma once
#include <stdint.h>
#include <uapi/chrome_caption.h>
#include <kernel/rect.h>

/* 控件背景、透明背景与边框共用的逐行轮廓；半径先限制在短边的一半。 */
static inline uint32_t control_round_radius(uint32_t radius, uint32_t width, uint32_t height) {
    if (radius > width / 2U) radius = width / 2U;
    if (radius > height / 2U) radius = height / 2U;
    return radius;
}

static inline uint32_t control_round_inset(uint32_t row, uint32_t width,
    uint32_t height, uint32_t radius) {
    if (row >= height) return 0U;
    radius = control_round_radius(radius, width, height);
    if (!radius) return 0U;
    /* 默认主题沿用系统窗口的固定轮廓，不再为控件另定义一份圆角表。 */
    if (radius == CHROME_FRAME_RADIUS)
        return chrome_frame_row_inset(row, width, height);
    if (row >= height / 2U) row = height - 1U - row;
    if (row >= radius) return 0U;
    uint64_t dy = 2ULL * (radius - row) - 1U;
    uint64_t diameter = 2ULL * radius;
    uint64_t limit = diameter * diameter - dy * dy;
    uint32_t low = 0U, high = radius;
    /* 用像素中心判圆并二分边界，避免大半径时逐像素搜索。 */
    while (low < high) {
        uint32_t mid = low + (high-low)/2U;
        uint64_t dx = 2ULL * (radius-mid) - 1U;
        if (dx * dx <= limit) high = mid;
        else low = mid + 1U;
    }
    return low;
}

/* 合成目标保存的是 XRGB/ARGB 容器，混合只更新颜色通道，不把来源透明度写回目标。 */
static inline uint32_t control_blend_argb(uint32_t under, uint32_t color) {
    uint32_t alpha = color >> 24U, result = under & 0xff000000U;
    if (!alpha) return under;
    if (alpha == 255U) return result | (color & 0x00ffffffU);
    /* 红、蓝各占独立的 16 位通道，乘加与舍入均不会向相邻通道进位。
     * 对 0..65025 的加权和，移位舍入与原来的 (和 + 127) / 255 完全等价。 */
    uint32_t inverse = 255U - alpha;
    uint32_t rb = (color & 0x00ff00ffU) * alpha +
        (under & 0x00ff00ffU) * inverse + 0x00800080U;
    rb += (rb >> 8U) & 0x00ff00ffU;
    uint32_t green = (color & 0x0000ff00U) * alpha +
        (under & 0x0000ff00U) * inverse + 0x00008000U;
    green += (green >> 8U) & 0x0000ff00U;
    return result | ((rb >> 8U) & 0x00ff00ffU) |
        ((green >> 8U) & 0x0000ff00U);
}

typedef struct control_raster_target {
    volatile uint32_t *pixels;
    uint32_t stride;
    Rect clip;
} control_raster_target_t;

/* 调用方保证 clip 的底边位于目标分配范围内；这里再次限制行内跨度和负坐标。 */
static inline void control_raster_fill_alpha(const control_raster_target_t *target,
    int64_t x, int64_t y, uint32_t w, uint32_t h, uint32_t radius, uint32_t color) {
    if (!target || !target->pixels || !target->stride || !(color >> 24U)) return;
    Rect clip = target->clip;
    if (clip.x0 < 0) clip.x0 = 0;
    if (clip.y0 < 0) clip.y0 = 0;
    if (clip.x1 > (int64_t)target->stride) clip.x1 = target->stride > INT32_MAX ? INT32_MAX : (int32_t)target->stride;
    int64_t left=x>clip.x0?x:clip.x0, top=y>clip.y0?y:clip.y0;
    int64_t right=x+w<clip.x1?x+w:clip.x1, bottom=y+h<clip.y1?y+h:clip.y1;
    if (left>=right || top>=bottom) return;
    for (int64_t py=top; py<bottom; ++py) {
        uint32_t inset=control_round_inset((uint32_t)(py-y), w, h, radius);
        int64_t first=left>x+inset?left:x+inset, last=right<x+w-inset?right:x+w-inset;
        for (int64_t px=first; px<last; ++px) {
            uint64_t offset=(uint64_t)py*target->stride+(uint32_t)px;
            target->pixels[offset]=control_blend_argb(target->pixels[offset], color);
        }
    }
}

typedef void (*control_border_span_fn)(void *context, int64_t x, int64_t y,
    uint32_t width, uint32_t height, uint32_t color);

/* 统一生成边环跨度：桌面透明描边和窗口不透明边框共用裁剪、圆角与中段合并。 */
static inline void control_raster_border_spans(Rect clip, int64_t x, int64_t y,
    uint32_t w, uint32_t h, uint32_t radius, uint32_t border, uint32_t color,
    control_border_span_fn emit, void *context) {
    if (!emit || rect_is_empty(clip) || x>=clip.x1 || y>=clip.y1 ||
        x+w<=clip.x0 || y+h<=clip.y0) return;
    if (border > w/2U) border=w/2U;
    if (border > h/2U) border=h/2U;
    if (!border) return;
    radius=control_round_radius(radius,w,h);
    int64_t top=y>clip.y0?y:clip.y0, bottom=y+h<clip.y1?y+h:clip.y1;
    for (int64_t py=top; py<bottom; ++py) {
        uint32_t row=(uint32_t)(py-y), span_height=1U;
        uint32_t corner=radius>border?radius:border;
        if (row>=corner && row<h-corner) {
            int64_t end=y+h-corner<bottom?y+h-corner:bottom;
            span_height=(uint32_t)(end-py);
        }
        uint32_t outer=control_round_inset(row,w,h,radius);
        int64_t left=x+outer,right=x+w-outer;
        if (row<border || row>=h-border) {
            if (left<clip.x0) left=clip.x0;
            if (right>clip.x1) right=clip.x1;
            if (left<right) emit(context,left,py,(uint32_t)(right-left),1U,color);
            continue;
        }
        uint32_t inner=border+control_round_inset(row-border,w-2U*border,
            h-2U*border,radius>border?radius-border:0U);
        if (inner<outer) inner=outer;
        int64_t ends[4]={left,x+inner,x+w-inner,right};
        for (uint32_t side=0; side<2U; ++side) {
            int64_t first=ends[side*2U],last=ends[side*2U+1U];
            if (first<clip.x0) first=clip.x0;
            if (last>clip.x1) last=clip.x1;
            if (first<last) emit(context,first,py,(uint32_t)(last-first),span_height,color);
        }
        py+=span_height-1U;
    }
}
