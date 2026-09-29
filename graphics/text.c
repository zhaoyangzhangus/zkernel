#include "text.h"
#include "console_font_a8.h"

#include <stdint.h>

enum {
    GOP_PIXEL_RGBR = 0,
    GOP_PIXEL_BGRR = 1,
    GOP_PIXEL_BITMASK = 2,
};

static uint32_t mask_component(uint8_t value, uint32_t mask)
{
    if (mask == 0)
        return 0;

    uint32_t shift = (uint32_t)__builtin_ctz(mask);
    uint32_t maximum = mask >> shift;
    return ((((uint32_t)value * maximum + 127U) / 255U) << shift) & mask;
}

static uint32_t pack_pixel(const BOOT_INFO *bi, uint32_t rgb)
{
    uint32_t r = (rgb >> 16) & 0xFFU;
    uint32_t g = (rgb >> 8) & 0xFFU;
    uint32_t b = rgb & 0xFFU;

    if (bi->fb_pixel_format == GOP_PIXEL_RGBR)
        return r | (g << 8) | (b << 16);
    if (bi->fb_pixel_format == GOP_PIXEL_BGRR)
        return b | (g << 8) | (r << 16);

    return mask_component((uint8_t)r, bi->fb_red_mask) |
           mask_component((uint8_t)g, bi->fb_green_mask) |
           mask_component((uint8_t)b, bi->fb_blue_mask);
}

static uint32_t blend_rgb(uint32_t fg, uint32_t bg, uint32_t a)
{
    uint32_t ia = 255U - a;
    uint32_t r = (((fg >> 16) & 0xFFU) * a +
                  ((bg >> 16) & 0xFFU) * ia + 127U) / 255U;
    uint32_t g = (((fg >> 8) & 0xFFU) * a +
                  ((bg >> 8) & 0xFFU) * ia + 127U) / 255U;
    uint32_t b = ((fg & 0xFFU) * a + (bg & 0xFFU) * ia + 127U) / 255U;
    return (r << 16) | (g << 8) | b;
}

int fb_text_supported(const BOOT_INFO *bi)
{
    return bi != 0 && bi->fb_base != 0 &&
           bi->fb_width != 0 && bi->fb_height != 0 &&
           bi->fb_pixels_per_scanline >= bi->fb_width &&
           bi->fb_pixel_format <= GOP_PIXEL_BITMASK;
}

static void draw_char(const BOOT_INFO *bi, uint32_t x, uint32_t y,
                      uint8_t ch, uint32_t fg, uint32_t bg)
{
    const uint8_t *glyph = liteos_console_font_glyph(ch);
    volatile uint32_t *fb = (volatile uint32_t *)(uintptr_t)bi->fb_base;
    uint32_t w = bi->fb_width - x;
    uint32_t h = bi->fb_height - y;

    if (w > LITEOS_CONSOLE_FONT_WIDTH)
        w = LITEOS_CONSOLE_FONT_WIDTH;
    if (h > LITEOS_CONSOLE_FONT_HEIGHT)
        h = LITEOS_CONSOLE_FONT_HEIGHT;

    fg &= 0x00FFFFFFU;
    bg &= 0x00FFFFFFU;

    for (uint32_t gy = 0; gy < h; ++gy) {
        const uint8_t *src = glyph + gy * LITEOS_CONSOLE_FONT_WIDTH;
        volatile uint32_t *dst =
            fb + (uint64_t)(y + gy) * bi->fb_pixels_per_scanline + x;

        for (uint32_t gx = 0; gx < w; ++gx) {
            uint32_t a = src[gx];
            uint32_t rgb = a == 0 ? bg : (a == 255 ? fg : blend_rgb(fg, bg, a));
            dst[gx] = pack_pixel(bi, rgb);
        }
    }
}

void fb_draw_char_a8(const BOOT_INFO *bi, uint32_t x, uint32_t y,
                     uint8_t ch, uint32_t fg, uint32_t bg)
{
    if (!fb_text_supported(bi) || x >= bi->fb_width || y >= bi->fb_height)
        return;

    draw_char(bi, x, y, ch, fg, bg);
    __asm__ volatile("sfence" : : : "memory");
}

void fb_draw_text_a8(const BOOT_INFO *bi, uint32_t x, uint32_t y,
                     const char *text, uint32_t fg, uint32_t bg)
{
    if (!fb_text_supported(bi) || text == 0)
        return;

    uint32_t start_x = x;

    while (*text && y < bi->fb_height) {
        uint8_t ch = (uint8_t)*text++;

        if (ch == '\n') {
            x = start_x;
            y += LITEOS_CONSOLE_FONT_HEIGHT;
            continue;
        }
        if (ch == '\r') {
            x = start_x;
            continue;
        }
        if (ch == '\t') {
            x += 4U * LITEOS_CONSOLE_FONT_WIDTH;
            continue;
        }

        if (x + LITEOS_CONSOLE_FONT_WIDTH > bi->fb_width) {
            x = start_x;
            y += LITEOS_CONSOLE_FONT_HEIGHT;
            if (y >= bi->fb_height)
                break;
        }

        draw_char(bi, x, y, ch, fg, bg);
        x += LITEOS_CONSOLE_FONT_WIDTH;
    }

    __asm__ volatile("sfence" : : : "memory");
}
