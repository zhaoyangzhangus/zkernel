#pragma once
#include <stdint.h>

/* 独立于窗口装饰的客户端内容绘制原语。 */
static inline void user_paint_round_rect(uint32_t *pixels, uint32_t stride,
    uint32_t width, uint32_t height, uint32_t x, uint32_t y,
    uint32_t rect_width, uint32_t rect_height, uint32_t color) {
    static const uint8_t inset[8] = {5U, 3U, 2U, 1U, 1U, 0U, 0U, 0U};
    if (!pixels || stride < width || x >= width || y >= height) return;
    uint32_t rows = rect_height < height - y ? rect_height : height - y;
    for (uint32_t row = 0; row < rows; ++row) {
        uint32_t edge = row < 8U ? row : rect_height - 1U - row;
        uint32_t cut = rect_width >= 12U && rect_height >= 12U && edge < 8U ? inset[edge] : 0U;
        if (cut >= rect_width || cut >= width - x || rect_width - cut <= cut) continue;
        uint32_t count = rect_width - cut * 2U;
        if (count > width - x - cut) count = width - x - cut;
        for (uint32_t column = 0; column < count; ++column)
            pixels[(uint64_t)(y + row) * stride + x + cut + column] = color;
    }
}
