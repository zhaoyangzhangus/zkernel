#include "ascii_font_internal.h"

/* REFACTOR_P7A_FONT_PARSER_OWNER：TTF 表、cmap、度量值和轮廓解码集中在一个冷路径解析单元中。 */

static uint16_t font_read_u16(const ascii_font_parser_t *parser,
                              uint32_t offset) {
    return (uint16_t)(((uint16_t)parser->file[offset] << 8U) |
                      parser->file[offset + 1U]);
}

static int16_t font_read_i16(const ascii_font_parser_t *parser,
                             uint32_t offset) {
    return (int16_t)font_read_u16(parser, offset);
}

static uint32_t font_read_u32(const ascii_font_parser_t *parser,
                              uint32_t offset) {
    return ((uint32_t)parser->file[offset] << 24U) |
           ((uint32_t)parser->file[offset + 1U] << 16U) |
           ((uint32_t)parser->file[offset + 2U] << 8U) |
           (uint32_t)parser->file[offset + 3U];
}

static bool font_range(const ascii_font_parser_t *parser,
                       uint32_t offset, uint32_t length) {
    return (uint64_t)offset + length <= parser->file_size;
}

static bool font_tag_is(const ascii_font_parser_t *parser, uint32_t offset,
                        char a, char b, char c, char d) {
    return parser->file[offset] == (uint8_t)a &&
           parser->file[offset + 1U] == (uint8_t)b &&
           parser->file[offset + 2U] == (uint8_t)c &&
           parser->file[offset + 3U] == (uint8_t)d;
}

static bool font_find_table(const ascii_font_parser_t *parser,
                            uint32_t directory, uint16_t table_count,
                            char a, char b, char c, char d,
                            ascii_font_table_t *table) {
    for (uint16_t index = 0U; index < table_count; ++index) {
        uint32_t record = directory + (uint32_t)index * 16U;
        if (font_tag_is(parser, record, a, b, c, d)) {
            table->offset = font_read_u32(parser, record + 8U);
            table->length = font_read_u32(parser, record + 12U);
            return font_range(parser, table->offset, table->length);
        }
    }
    return false;
}

static bool font_select_cmap(ascii_font_parser_t *parser) {
    uint32_t offset = parser->cmap.offset;
    uint16_t records;
    uint32_t best_offset = 0U;
    uint32_t best_length = 0U;
    uint16_t best_format = 0U;
    uint32_t best_score = 0U;

    if (parser->cmap.length < 4U) return false;
    records = font_read_u16(parser, offset + 2U);
    if (parser->cmap.length < 4U + (uint32_t)records * 8U) {
        return false;
    }
    for (uint16_t index = 0U; index < records; ++index) {
        uint32_t record = offset + 4U + (uint32_t)index * 8U;
        uint16_t platform = font_read_u16(parser, record);
        uint16_t encoding = font_read_u16(parser, record + 2U);
        uint32_t subtable_offset =
            offset + font_read_u32(parser, record + 4U);
        uint16_t format;
        uint32_t length;
        uint32_t score = 0U;

        if (!font_range(parser, subtable_offset, 2U) ||
            subtable_offset < offset ||
            subtable_offset - offset >= parser->cmap.length) {
            continue;
        }
        format = font_read_u16(parser, subtable_offset);
        if (format == 4U) {
            if (!font_range(parser, subtable_offset, 4U)) continue;
            length = font_read_u16(parser, subtable_offset + 2U);
            if (length < 16U || !font_range(parser, subtable_offset, length)) {
                continue;
            }
            if (platform == 3U && encoding == 1U) score = 40U;
            else if (platform == 3U && encoding == 0U) score = 30U;
            else if (platform == 0U) score = 20U;
        } else if (format == 12U) {
            if (!font_range(parser, subtable_offset, 16U)) continue;
            length = font_read_u32(parser, subtable_offset + 4U);
            if (length < 16U || !font_range(parser, subtable_offset, length)) {
                continue;
            }
            if (platform == 3U && encoding == 10U) score = 50U;
            else if (platform == 0U) score = 25U;
        } else {
            continue;
        }
        if (score > best_score) {
            best_score = score;
            best_offset = subtable_offset;
            best_length = length;
            best_format = format;
        }
    }
    if (best_score == 0U) return false;
    parser->cmap_offset = best_offset;
    parser->cmap_length = best_length;
    parser->cmap_format = best_format;
    return true;
}

bool ascii_font_parser_init(ascii_font_parser_t *parser,
                            const uint8_t *file, size_t file_size) {
    uint16_t table_count;
    uint32_t directory_length;
    uint32_t directory = 12U;
    uint32_t loca_entry_size;

    if (parser == 0 || file == 0) return false;
    *parser = (ascii_font_parser_t){
        .file = file,
        .file_size = file_size,
    };
    if (file_size < 12U || font_read_u32(parser, 0U) != 0x00010000U) {
        return false;
    }
    table_count = font_read_u16(parser, 4U);
    directory_length = (uint32_t)table_count * 16U;
    if (!font_range(parser, directory, directory_length)) return false;

    if (!font_find_table(parser, directory, table_count, 'h', 'e', 'a', 'd',
                         &parser->head) ||
        !font_find_table(parser, directory, table_count, 'h', 'h', 'e', 'a',
                         &parser->hhea) ||
        !font_find_table(parser, directory, table_count, 'h', 'm', 't', 'x',
                         &parser->hmtx) ||
        !font_find_table(parser, directory, table_count, 'm', 'a', 'x', 'p',
                         &parser->maxp) ||
        !font_find_table(parser, directory, table_count, 'l', 'o', 'c', 'a',
                         &parser->loca) ||
        !font_find_table(parser, directory, table_count, 'g', 'l', 'y', 'f',
                         &parser->glyf) ||
        !font_find_table(parser, directory, table_count, 'c', 'm', 'a', 'p',
                         &parser->cmap)) {
        return false;
    }
    if (parser->head.length < 54U || parser->hhea.length < 36U ||
        parser->maxp.length < 6U) {
        return false;
    }
    parser->units_per_em = font_read_u16(parser, parser->head.offset + 18U);
    parser->loca_format =
        (uint16_t)font_read_i16(parser, parser->head.offset + 50U);
    parser->ascender = font_read_i16(parser, parser->hhea.offset + 4U);
    parser->descender = font_read_i16(parser, parser->hhea.offset + 6U);
    parser->glyph_count = font_read_u16(parser, parser->maxp.offset + 4U);
    parser->metric_count = font_read_u16(parser, parser->hhea.offset + 34U);
    if (parser->units_per_em == 0U || parser->glyph_count == 0U ||
        parser->metric_count == 0U ||
        parser->metric_count > parser->glyph_count ||
        (parser->loca_format != 0U && parser->loca_format != 1U) ||
        parser->ascender <= parser->descender) {
        return false;
    }
    loca_entry_size = parser->loca_format == 0U ? 2U : 4U;
    if ((uint64_t)(parser->glyph_count + 1U) * loca_entry_size >
            parser->loca.length ||
        (uint64_t)parser->metric_count * 4U > parser->hmtx.length) {
        return false;
    }
    return font_select_cmap(parser);
}

uint16_t ascii_font_parser_glyph_for_code(
    const ascii_font_parser_t *parser, uint32_t code) {
    uint32_t base = parser->cmap_offset;

    if (parser->cmap_format == 12U) {
        uint32_t groups = font_read_u32(parser, base + 12U);
        uint32_t left = 0U;
        uint32_t right = groups;
        if (parser->cmap_length < 16U ||
            (uint64_t)groups * 12U + 16U > parser->cmap_length) {
            return 0U;
        }
        while (left < right) {
            uint32_t middle = left + (right - left) / 2U;
            uint32_t group = base + 16U + middle * 12U;
            uint32_t start = font_read_u32(parser, group);
            uint32_t end = font_read_u32(parser, group + 4U);
            if (code < start) right = middle;
            else if (code > end) left = middle + 1U;
            else {
                uint32_t glyph = font_read_u32(parser, group + 8U) +
                    code - start;
                return glyph < parser->glyph_count ? (uint16_t)glyph : 0U;
            }
        }
        return 0U;
    }

    if (parser->cmap_format == 4U && code <= 0xFFFFU) {
        uint16_t segments;
        uint32_t end_base;
        uint32_t start_base;
        uint32_t delta_base;
        uint32_t range_base;

        if (parser->cmap_length < 16U) return 0U;
        segments = (uint16_t)(font_read_u16(parser, base + 6U) / 2U);
        if (segments == 0U ||
            (uint64_t)16U + (uint64_t)segments * 8U >
                parser->cmap_length) {
            return 0U;
        }
        end_base = base + 14U;
        start_base = end_base + (uint32_t)segments * 2U + 2U;
        delta_base = start_base + (uint32_t)segments * 2U;
        range_base = delta_base + (uint32_t)segments * 2U;
        for (uint16_t index = 0U; index < segments; ++index) {
            uint16_t end = font_read_u16(
                parser, end_base + (uint32_t)index * 2U);
            uint16_t start = font_read_u16(
                parser, start_base + (uint32_t)index * 2U);
            int16_t delta;
            uint16_t range;

            if (code > end) continue;
            if (code < start) return 0U;
            delta = font_read_i16(
                parser, delta_base + (uint32_t)index * 2U);
            range = font_read_u16(
                parser, range_base + (uint32_t)index * 2U);
            if (range == 0U) {
                return (uint16_t)((code + delta) & 0xFFFFU);
            }
            {
                uint32_t glyph_offset =
                    range_base + (uint32_t)index * 2U + range +
                    (code - start) * 2U;
                uint16_t glyph;
                if (!font_range(parser, glyph_offset, 2U)) return 0U;
                glyph = font_read_u16(parser, glyph_offset);
                return glyph == 0U ? 0U :
                    (uint16_t)((glyph + delta) & 0xFFFFU);
            }
        }
    }
    return 0U;
}

static bool font_glyph_offsets(const ascii_font_parser_t *parser,
                               uint16_t glyph, uint32_t *start,
                               uint32_t *end) {
    uint32_t start_offset;
    uint32_t end_offset;

    if (glyph >= parser->glyph_count || start == 0 || end == 0) {
        return false;
    }
    if (parser->loca_format == 0U) {
        start_offset = (uint32_t)font_read_u16(
            parser, parser->loca.offset + (uint32_t)glyph * 2U) * 2U;
        end_offset = (uint32_t)font_read_u16(
            parser, parser->loca.offset + (uint32_t)(glyph + 1U) * 2U) * 2U;
    } else {
        start_offset = font_read_u32(
            parser, parser->loca.offset + (uint32_t)glyph * 4U);
        end_offset = font_read_u32(
            parser, parser->loca.offset + (uint32_t)(glyph + 1U) * 4U);
    }
    if (start_offset > end_offset || end_offset > parser->glyf.length) {
        return false;
    }
    *start = start_offset;
    *end = end_offset;
    return true;
}

static bool font_decode_single_glyph(
    const ascii_font_parser_t *parser, uint16_t glyph,
    ascii_font_point_t *points, uint16_t *contour_ends,
    uint32_t *point_count, uint32_t *contour_count,
    uint32_t point_capacity, uint32_t contour_capacity) {
    uint32_t start_offset;
    uint32_t end_offset;
    uint32_t glyph_offset;
    uint32_t cursor;
    uint16_t contours;
    uint16_t ends[ASCII_FONT_MAX_CONTOURS];
    uint8_t flags[ASCII_FONT_MAX_POINTS];
    int32_t x = 0;
    int32_t y = 0;

    if (points == 0 || contour_ends == 0 || point_count == 0 ||
        contour_count == 0 ||
        !font_glyph_offsets(parser, glyph, &start_offset, &end_offset)) {
        return false;
    }
    if (start_offset == end_offset) {
        *point_count = 0U;
        *contour_count = 0U;
        return true;
    }
    if (end_offset - start_offset < 10U) return false;
    glyph_offset = parser->glyf.offset + start_offset;
    if (!font_range(parser, glyph_offset, end_offset - start_offset)) {
        return false;
    }
    contours = (uint16_t)font_read_i16(parser, glyph_offset);
    if ((int16_t)contours < 0 || contours > ASCII_FONT_MAX_CONTOURS || contours > contour_capacity) {
        return false;
    }
    if (contours == 0U) {
        *point_count = 0U;
        *contour_count = 0U;
        return true;
    }
    if (10U + (uint32_t)contours * 2U > end_offset - start_offset) {
        return false;
    }
    for (uint16_t contour = 0U; contour < contours; ++contour) {
        ends[contour] = font_read_u16(
            parser, glyph_offset + 10U + (uint32_t)contour * 2U);
        if (contour != 0U && ends[contour] <= ends[contour - 1U]) {
            return false;
        }
        contour_ends[contour] = ends[contour];
    }
    *point_count = (uint32_t)ends[contours - 1U] + 1U;
    *contour_count = contours;
    if (*point_count > ASCII_FONT_MAX_POINTS || *point_count > point_capacity) return false;

    cursor = glyph_offset + 10U + (uint32_t)contours * 2U;
    if (!font_range(parser, cursor, 2U)) return false;
    cursor += 2U + font_read_u16(parser, cursor);
    if (!font_range(parser, cursor, 1U)) return false;
    for (uint32_t index = 0U; index < *point_count; ++index) {
        uint8_t flag;
        if (!font_range(parser, cursor, 1U)) return false;
        flag = parser->file[cursor++];
        flags[index] = flag;
        if ((flag & 8U) != 0U) {
            uint8_t repeat;
            if (!font_range(parser, cursor, 1U)) return false;
            repeat = parser->file[cursor++];
            if (index + repeat >= *point_count) return false;
            while (repeat-- != 0U) flags[++index] = flag;
        }
    }
    for (uint32_t index = 0U; index < *point_count; ++index) {
        int32_t delta;
        uint8_t flag = flags[index];
        if ((flag & 2U) != 0U) {
            if (!font_range(parser, cursor, 1U)) return false;
            delta = parser->file[cursor++];
            if ((flag & 16U) == 0U) delta = -delta;
        } else if ((flag & 16U) != 0U) {
            delta = 0;
        } else {
            if (!font_range(parser, cursor, 2U)) return false;
            delta = font_read_i16(parser, cursor);
            cursor += 2U;
        }
        x += delta;
        points[index].x = x;
        points[index].on_curve = (flag & 1U) != 0U;
    }
    for (uint32_t index = 0U; index < *point_count; ++index) {
        int32_t delta;
        uint8_t flag = flags[index];
        if ((flag & 4U) != 0U) {
            if (!font_range(parser, cursor, 1U)) return false;
            delta = parser->file[cursor++];
            if ((flag & 32U) == 0U) delta = -delta;
        } else if ((flag & 32U) != 0U) {
            delta = 0;
        } else {
            if (!font_range(parser, cursor, 2U)) return false;
            delta = font_read_i16(parser, cursor);
            cursor += 2U;
        }
        y += delta;
        points[index].y = y;
    }
    return true;
}

/* 组合字形直接追加到调用方的有界数组，避免每层递归复制整套轮廓或分配内存。 */
static bool font_decode_outline(const ascii_font_parser_t *parser, uint16_t glyph,
    ascii_font_point_t *points, uint16_t *ends, uint32_t *count, uint32_t *contours,
    uint32_t point_capacity, uint32_t contour_capacity, unsigned depth) {
    uint32_t start, end;
    if (depth > 8U || !font_glyph_offsets(parser, glyph, &start, &end)) return false;
    if (start == end || (end - start >= 10U &&
        font_read_i16(parser, parser->glyf.offset + start) >= 0))
        return font_decode_single_glyph(parser, glyph, points, ends, count, contours,
                                        point_capacity, contour_capacity);
    if (end - start < 10U) return false;
    uint32_t cursor = parser->glyf.offset + start + 10U;
    uint32_t limit = parser->glyf.offset + end;
    *count = 0; *contours = 0;
    uint16_t flags;
    do {
        if (cursor > limit || limit - cursor < 4U) return false;
        flags = font_read_u16(parser, cursor);
        uint16_t child = font_read_u16(parser, cursor + 2U); cursor += 4U;
        unsigned bytes = flags & 1U ? 4U : 2U;
        if (limit - cursor < bytes) return false;
        int32_t a = flags & 1U ? font_read_u16(parser, cursor) : parser->file[cursor];
        int32_t b = flags & 1U ? font_read_u16(parser, cursor + 2U) : parser->file[cursor + 1U];
        if (flags & 2U) { a = flags & 1U ? (int16_t)a : (int8_t)a; b = flags & 1U ? (int16_t)b : (int8_t)b; }
        cursor += bytes;
        int32_t xx=16384, yy=16384, xy=0, yx=0;
        unsigned transforms = !!(flags & 8U) + !!(flags & 64U) + !!(flags & 128U);
        if (transforms > 1U || ((flags & 0x1800U) == 0x1800U)) return false;
        bytes = flags & 128U ? 8U : flags & 64U ? 4U : flags & 8U ? 2U : 0U;
        if (limit - cursor < bytes) return false;
        if (flags & 128U) {
            xx=font_read_i16(parser,cursor);xy=font_read_i16(parser,cursor+2U);
            yx=font_read_i16(parser,cursor+4U);yy=font_read_i16(parser,cursor+6U);
        } else if (flags & 64U) {xx=font_read_i16(parser,cursor);yy=font_read_i16(parser,cursor+2U);}
        else if (flags & 8U) xx=yy=font_read_i16(parser,cursor);
        cursor += bytes;
        uint32_t added=0, added_contours=0;
        if (!font_decode_outline(parser,child,points+*count,ends+*contours,&added,&added_contours,
            point_capacity-*count,contour_capacity-*contours,depth+1U)) return false;
        for(uint32_t i=0;i<added;++i){
            ascii_font_point_t *p=&points[*count+i];int32_t x=p->x,y=p->y;
            int64_t tx=((int64_t)xx*x+(int64_t)xy*y)/16384;
            int64_t ty=((int64_t)yx*x+(int64_t)yy*y)/16384;
            if(tx<INT32_MIN||tx>INT32_MAX||ty<INT32_MIN||ty>INT32_MAX)return false;
            p->x=(int32_t)tx;p->y=(int32_t)ty;
        }
        int64_t dx=a,dy=b;
        if (!(flags & 2U)) {
            if((uint32_t)a>=*count||(uint32_t)b>=added)return false;
            dx=(int64_t)points[a].x-points[*count+b].x;
            dy=(int64_t)points[a].y-points[*count+b].y;
        } else if (flags & 0x800U) {
            dx=((int64_t)xx*a+(int64_t)xy*b)/16384;
            dy=((int64_t)yx*a+(int64_t)yy*b)/16384;
        }
        for(uint32_t i=0;i<added;++i){
            ascii_font_point_t *p=&points[*count+i];int64_t x=p->x+dx,y=p->y+dy;
            if(x<INT32_MIN||x>INT32_MAX||y<INT32_MIN||y>INT32_MAX)return false;
            p->x=(int32_t)x;p->y=(int32_t)y;
        }
        for(uint32_t i=0;i<added_contours;++i)ends[*contours+i]+=(uint16_t)*count;
        *count+=added;*contours+=added_contours;
    } while(flags & 32U);
    return true;
}

bool ascii_font_parser_decode_simple_glyph(const ascii_font_parser_t *parser, uint16_t glyph,
    ascii_font_point_t *points, uint16_t *ends, uint32_t *count, uint32_t *contours) {
    if(!parser||!points||!ends||!count||!contours)return false;
    return font_decode_outline(parser,glyph,points,ends,count,contours,
                               ASCII_FONT_MAX_POINTS,ASCII_FONT_MAX_CONTOURS,0U);
}

uint32_t ascii_font_parser_advance_width(
    const ascii_font_parser_t *parser, uint16_t glyph) {
    uint16_t metric = glyph < parser->metric_count ? glyph :
        (uint16_t)(parser->metric_count - 1U);
    return font_read_u16(parser, parser->hmtx.offset + (uint32_t)metric * 4U);
}

int32_t ascii_font_parser_left_side_bearing(
    const ascii_font_parser_t *parser, uint16_t glyph) {
    uint16_t metric = glyph < parser->metric_count ? glyph :
        (uint16_t)(parser->metric_count - 1U);
    return font_read_i16(parser,
                         parser->hmtx.offset + (uint32_t)metric * 4U + 2U);
}
