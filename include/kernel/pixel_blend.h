#pragma once
#include <stdint.h>

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

/* 光标写入 XRGB 输出；透明像素原样保留背景，其余像素清除未使用的高字节。 */
static inline uint32_t control_blend_cursor_xrgb(uint32_t under, uint32_t cursor) {
    return cursor >> 24U ? control_blend_argb(under & 0x00ffffffU, cursor) : under;
}

/* 预乘表面直接叠加来源分量；非法预乘输入也沿用旧路径的饱和规则。 */
static inline uint32_t pixel_blend_premultiplied(uint32_t under, uint32_t color) {
    uint32_t alpha = color >> 24U;
    if (!alpha) return under;
    if (alpha == 255U) return color;
    uint32_t inverse = 255U - alpha;
    uint32_t rb = (under & 0x00ff00ffU) * inverse + 0x00800080U;
    rb += (rb >> 8U) & 0x00ff00ffU;
    rb = ((rb >> 8U) & 0x00ff00ffU) + (color & 0x00ff00ffU);
    /* 每个 16 位槽内的第九位表示溢出，独立饱和到 255，不向相邻颜色借位。 */
    uint32_t overflow = rb & 0x01000100U;
    rb |= overflow - (overflow >> 8U);
    uint32_t green = ((under >> 8U) & 255U) * inverse + 128U;
    green = ((green + (green >> 8U)) >> 8U) + ((color >> 8U) & 255U);
    if (green > 255U) green = 255U;
    return (rb & 0x00ff00ffU) | (green << 8U);
}
