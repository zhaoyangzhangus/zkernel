#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 图形 ABI 仅接受由调用方管理的 XRGB8888 缓冲区。Blend2D 管理光栅化状态，
 * 像素数据仍归调用方所有。 */
void gfx_clear(uint32_t *pixels, uint32_t stride, uint32_t width,
                      uint32_t height, uint32_t color);
void gfx_fill_rect(uint32_t *pixels, uint32_t stride, uint32_t width,
                          uint32_t height, int32_t x, int32_t y,
                          uint32_t rect_width, uint32_t rect_height,
                          uint32_t color);
void gfx_gradient_rect(uint32_t *pixels, uint32_t stride, uint32_t width,
                              uint32_t height, int32_t x, int32_t y,
                              uint32_t rect_width, uint32_t rect_height,
                              uint32_t top_color, uint32_t bottom_color);
void gfx_frame(uint32_t *pixels, uint32_t stride, uint32_t width,
                      uint32_t height, uint32_t thickness, uint32_t color);

#ifdef __cplusplus
}
#endif
