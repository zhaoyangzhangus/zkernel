#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <uapi/abi.h>

#define ASCII_FONT_FIRST       0x20U
#define ASCII_FONT_LAST        0x7EU
#define ASCII_FONT_DEFAULT_WIDTH  8U
#define ASCII_FONT_DEFAULT_HEIGHT 16U

/* 字体文件由内核管理，并以 A8 格式提供字形缓存。绘制前设置目标尺寸；
 * 随后的查询将返回该尺寸下每像素一字节的数据。 */
bool ascii_font_load(void);
bool ui_font_load(void);
/* 在提交线程生成独立 A8 字形；不改变全局字号，不分配内存或读取文件。
 * 调用方提供完整目标缓冲区；失败时保持缓冲区不变。 */
bool ui_font_copy_glyph(uint32_t code, uint32_t width, uint32_t height,
                        uint8_t *destination, size_t capacity);
bool ascii_font_set_size(uint32_t width, uint32_t height);
const uint8_t *ascii_font_glyph(uint8_t character);
uint32_t ascii_font_width(void);
uint32_t ascii_font_height(void);
bool ascii_font_copy_cache(uint32_t width, uint32_t height,
                           uint8_t *destination, size_t capacity);
