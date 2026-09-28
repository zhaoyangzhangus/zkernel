/*
 * text_test.c —— host 上验证 12x24 A8 framebuffer 绘制。
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../graphics/text.h"

#define WIDTH  40U
#define HEIGHT 32U

static uint32_t framebuffer[WIDTH * HEIGHT];
static int checks;
static int failures;

static void check(int condition, const char *what)
{
    checks++;
    if (!condition) {
        failures++;
        fprintf(stderr, "FAIL: %s\n", what);
    }
}

int main(void)
{
    BOOT_INFO bi = { 0 };
    uint32_t changed = 0;
    uint32_t background = 0x00102030U;

    bi.fb_base = (uint64_t)(uintptr_t)framebuffer;
    bi.fb_width = WIDTH;
    bi.fb_height = HEIGHT;
    bi.fb_pixels_per_scanline = WIDTH;
    bi.fb_pixel_format = 1;

    check(fb_text_supported(&bi), "BGRR framebuffer supported");

    memset(framebuffer, 0, sizeof(framebuffer));
    fb_draw_char_a8(&bi, 0, 0, 'A', 0x00FFFFFFU, background);

    for (uint32_t y = 0; y < FB_TEXT_GLYPH_HEIGHT; ++y) {
        for (uint32_t x = 0; x < FB_TEXT_GLYPH_WIDTH; ++x) {
            if (framebuffer[y * WIDTH + x] != background)
                changed++;
        }
    }

    check(changed != 0, "A glyph contains covered pixels");
    check(framebuffer[0] == background,
          "transparent A8 pixel is filled with background");

    framebuffer[WIDTH * HEIGHT - 1] = 0xDEADBEEFU;
    fb_draw_char_a8(&bi, WIDTH, HEIGHT, 'A', 0x00FFFFFFU, 0);
    check(framebuffer[WIDTH * HEIGHT - 1] == 0xDEADBEEFU,
          "out-of-range glyph is ignored");

    bi.fb_pixel_format = 3;
    check(!fb_text_supported(&bi), "BltOnly framebuffer rejected");

    if (failures == 0) {
        printf("A8 字体测试通过：%d 项\n", checks);
        return 0;
    }

    printf("A8 字体测试失败：%d/%d 项不通过\n", failures, checks);
    return 1;
}
