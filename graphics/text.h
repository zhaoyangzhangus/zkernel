/*
 * text.h —— 12x24 A8 ASCII framebuffer renderer
 *
 * 字体数据来自 LiteOS 的 console_font_a8.h。颜色参数使用 0x00RRGGBB。
 */
#ifndef __KERNEL_GRAPHICS_TEXT_H__
#define __KERNEL_GRAPHICS_TEXT_H__

#include <stdint.h>

#include "../boot/bootinfo.h"

#define FB_TEXT_GLYPH_WIDTH  12U
#define FB_TEXT_GLYPH_HEIGHT 24U

int fb_text_supported(const BOOT_INFO *bi);

void fb_draw_char_a8(const BOOT_INFO *bi,
                     uint32_t x, uint32_t y,
                     uint8_t character,
                     uint32_t foreground,
                     uint32_t background);

void fb_draw_text_a8(const BOOT_INFO *bi,
                     uint32_t x, uint32_t y,
                     const char *text,
                     uint32_t foreground,
                     uint32_t background);

#endif /* __KERNEL_GRAPHICS_TEXT_H__ */
