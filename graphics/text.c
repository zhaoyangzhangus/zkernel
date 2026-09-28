/*
 * text.c —— 从 LiteOS 移植的 12x24 A8 字体绘制
 *
 * 每个 glyph 是 12*24=288 个 A8 coverage 值。alpha=0 完全背景，
 * alpha=255 完全前景，中间值做整数 alpha blend。
 */
#include "text.h"
#include "console_font_a8.h"

#include <stdint.h>

enum {
    GOP_PIXEL_RGBR     = 0,
    GOP_PIXEL_BGRR     = 1,
    GOP_PIXEL_BITMASK  = 2,
    GOP_PIXEL_BLT_ONLY = 3,
};

static uint32_t component_to_mask(uint8_t value, uint32_t mask)
{
    uint32_t shift = 0;
    uint32_t width = 0;
    uint32_t maximum;

    if (mask == 0)
        return 0;

    while (shift < 32U && ((mask >> shift) & 1U) == 0)
        ++shift;
    while (shift + width < 32U &&
           ((mask >> (shift + width)) & 1U) != 0)
        ++width;

    if (width == 0)
        return 0;
    if (width >= 32U)
        return mask;

    maximum = (1U << width) - 1U;
    return ((((uint32_t)value * maximum + 127U) / 255U) << shift) & mask;
}

static uint32_t pack_pixel(const BOOT_INFO *bi, uint32_t color)
{
    uint8_t red   = (uint8_t)(color >> 16);
    uint8_t green = (uint8_t)(color >> 8);
    uint8_t blue  = (uint8_t)color;

    switch (bi->fb_pixel_format) {
    case GOP_PIXEL_RGBR:
        return (uint32_t)red |
               ((uint32_t)green << 8) |
               ((uint32_t)blue << 16);

    case GOP_PIXEL_BGRR:
        return (uint32_t)blue |
               ((uint32_t)green << 8) |
               ((uint32_t)red << 16);

    case GOP_PIXEL_BITMASK:
        return component_to_mask(red, bi->fb_red_mask) |
               component_to_mask(green, bi->fb_green_mask) |
               component_to_mask(blue, bi->fb_blue_mask);

    default:
        return 0;
    }
}

static uint32_t blend_color(uint32_t foreground,
                            uint32_t background,
                            uint8_t alpha)
{
    uint32_t inverse = 255U - alpha;
    uint32_t red = (((foreground >> 16U) & 0xFFU) * alpha +
                    ((background >> 16U) & 0xFFU) * inverse + 127U) / 255U;
    uint32_t green = (((foreground >> 8U) & 0xFFU) * alpha +
                      ((background >> 8U) & 0xFFU) * inverse + 127U) / 255U;
    uint32_t blue = ((foreground & 0xFFU) * alpha +
                     (background & 0xFFU) * inverse + 127U) / 255U;

    return (red << 16U) | (green << 8U) | blue;
}

int fb_text_supported(const BOOT_INFO *bi)
{
    if (bi == 0 ||
        bi->fb_base == 0 ||
        bi->fb_width == 0 ||
        bi->fb_height == 0 ||
        bi->fb_pixels_per_scanline < bi->fb_width) {
        return 0;
    }

    return bi->fb_pixel_format == GOP_PIXEL_RGBR ||
           bi->fb_pixel_format == GOP_PIXEL_BGRR ||
           bi->fb_pixel_format == GOP_PIXEL_BITMASK;
}

void fb_draw_char_a8(const BOOT_INFO *bi,
                     uint32_t x, uint32_t y,
                     uint8_t character,
                     uint32_t foreground,
                     uint32_t background)
{
    const uint8_t *glyph;
    volatile uint32_t *framebuffer;

    if (!fb_text_supported(bi) ||
        x >= bi->fb_width ||
        y >= bi->fb_height) {
        return;
    }

    glyph = liteos_console_font_glyph(character);
    framebuffer = (volatile uint32_t *)(uintptr_t)bi->fb_base;

    foreground &= 0x00FFFFFFU;
    background &= 0x00FFFFFFU;

    for (uint32_t glyph_row = 0;
         glyph_row < LITEOS_CONSOLE_FONT_HEIGHT &&
         y + glyph_row < bi->fb_height;
         ++glyph_row) {
        volatile uint32_t *destination =
            framebuffer +
            (uint64_t)(y + glyph_row) * bi->fb_pixels_per_scanline + x;

        for (uint32_t glyph_column = 0;
             glyph_column < LITEOS_CONSOLE_FONT_WIDTH &&
             x + glyph_column < bi->fb_width;
             ++glyph_column) {
            uint8_t alpha =
                glyph[glyph_row * LITEOS_CONSOLE_FONT_WIDTH + glyph_column];
            uint32_t color;

            if (alpha == 0)
                color = background;
            else if (alpha == 255)
                color = foreground;
            else
                color = blend_color(foreground, background, alpha);

            destination[glyph_column] = pack_pixel(bi, color);
        }
    }
}

void fb_draw_text_a8(const BOOT_INFO *bi,
                     uint32_t x, uint32_t y,
                     const char *text,
                     uint32_t foreground,
                     uint32_t background)
{
    uint32_t origin_x = x;

    if (!fb_text_supported(bi) || text == 0)
        return;

    while (*text != '\0') {
        uint8_t character = (uint8_t)*text++;

        if (character == '\n') {
            x = origin_x;
            y += LITEOS_CONSOLE_FONT_HEIGHT;
            continue;
        }

        if (character == '\r') {
            x = origin_x;
            continue;
        }

        if (character == '\t') {
            x += 4U * LITEOS_CONSOLE_FONT_WIDTH;
            continue;
        }

        if (y >= bi->fb_height)
            break;

        if (x + LITEOS_CONSOLE_FONT_WIDTH > bi->fb_width) {
            x = origin_x;
            y += LITEOS_CONSOLE_FONT_HEIGHT;
            if (y >= bi->fb_height)
                break;
        }

        fb_draw_char_a8(bi, x, y, character, foreground, background);
        x += LITEOS_CONSOLE_FONT_WIDTH;
    }

    __asm__ volatile("sfence" : : : "memory");
}
