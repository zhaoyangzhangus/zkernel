#pragma once

#include <ascii_font.h>

#define ASCII_FONT_MAX_POINTS   1024U
#define ASCII_FONT_MAX_CONTOURS 64U

typedef struct ascii_font_table {
    uint32_t offset;
    uint32_t length;
} ascii_font_table_t;

typedef struct ascii_font_point {
    int32_t x;
    int32_t y;
    bool on_curve;
} ascii_font_point_t;

/* 私有 TTF 解析器状态。生命周期由缓存/光栅化单元管理。 */
typedef struct ascii_font_parser {
    const uint8_t *file;
    size_t file_size;
    ascii_font_table_t head;
    ascii_font_table_t hhea;
    ascii_font_table_t hmtx;
    ascii_font_table_t maxp;
    ascii_font_table_t loca;
    ascii_font_table_t glyf;
    ascii_font_table_t cmap;
    uint16_t units_per_em;
    int16_t ascender;
    int16_t descender;
    uint16_t glyph_count;
    uint16_t metric_count;
    uint16_t loca_format;
    uint32_t cmap_offset;
    uint32_t cmap_length;
    uint16_t cmap_format;
} ascii_font_parser_t;

bool ascii_font_parser_init(ascii_font_parser_t *parser,
                            const uint8_t *file, size_t file_size);
uint16_t ascii_font_parser_glyph_for_code(
    const ascii_font_parser_t *parser, uint32_t code);
bool ascii_font_parser_decode_simple_glyph(
    const ascii_font_parser_t *parser, uint16_t glyph,
    ascii_font_point_t *points, uint16_t *contour_ends,
    uint32_t *point_count, uint32_t *contour_count);
uint32_t ascii_font_parser_advance_width(
    const ascii_font_parser_t *parser, uint16_t glyph);
int32_t ascii_font_parser_left_side_bearing(
    const ascii_font_parser_t *parser, uint16_t glyph);
