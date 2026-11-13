#pragma once
#include <stdint.h>

/* 颜色计算不依赖设备初始化、堆或锁，早期控制台与显示核心共用。 */
static inline uint32_t pixel_format_component(uint8_t value, uint32_t mask) {
    if (mask == 0U) return 0U;
    uint32_t shift = 0U, width = 0U;
    while (shift < 32U && ((mask >> shift) & 1U) == 0U) ++shift;
    while (shift + width < 32U && ((mask >> (shift + width)) & 1U) != 0U) ++width;
    uint64_t maximum = (1ULL << width) - 1U;
    return (uint32_t)((((uint64_t)value * maximum + 127U) / 255U) << shift) & mask;
}

/* XRGB 的低字节是蓝色；GOP RGBR 的低字节必须换为红色。 */
static inline uint32_t pixel_xrgb_to_rgbr(uint32_t pixel) {
    return ((pixel & 0xffU) << 16U) | (pixel & 0xff00U) | ((pixel >> 16U) & 0xffU);
}

/* 两个像素先整体翻转字节，再交换半部并移除保留字节；只需整数指令。
 * 源像素顺序保持不变，结果与两次单像素转换完全一致。 */
static inline uint64_t pixel_xrgb_pair_to_rgbr(uint64_t pair) {
    uint64_t reversed = __builtin_bswap64(pair);
    uint64_t ordered = (reversed << 32U) | (reversed >> 32U);
    return (ordered >> 8U) & 0x00FFFFFF00FFFFFFULL;
}
