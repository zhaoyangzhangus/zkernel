#pragma once
#include <stdatomic.h>
#include <stddef.h>
#include <kernel/control_raster.h>

/* 文档最多 64 KiB，字符起点与字形编号均可用 16 位表示；每项仍占 8 字节。 */
typedef struct control_text_cell { uint32_t end; uint16_t glyph,byte_offset; } control_text_cell_t;
typedef struct control_text_glyph { uint32_t code, width, offset; } control_text_glyph_t;
/* 一段文字只保存一次重复字形；位置表支持按损坏区二分定位，无需从头解码。 */
typedef struct control_text_run {
    atomic_uint references;
    uint32_t count, glyph_count, width, height;
    size_t bytes;
    control_text_cell_t *cells;
    control_text_glyph_t *glyphs;
    uint8_t *pixels;
} control_text_run_t;

/* ASCII 字体缓存与 Unicode 字形缓存均以连续覆盖率行混色，目标高字节保持不变。 */
static inline void control_text_blend_span(volatile uint32_t *out,
    const uint8_t *coverage, uint32_t count, uint32_t color) {
    color &= 0x00ffffffU;
    for (uint32_t col=0; col<count; ++col) {
        uint32_t alpha=coverage[col];
        if (alpha) out[col]=control_blend_argb(out[col], color|(alpha<<24U));
    }
}

static inline void control_text_draw(const control_raster_target_t *target,
    const control_text_run_t *run, int64_t x, int64_t y, uint32_t color) {
    Rect clip=target->clip;
    /* 整段文字未进入损坏区时不读取字符索引或字形缓存，避免无效的二分和行扫描。 */
    if (rect_is_empty(clip) || x>=clip.x1 || x+run->width<=clip.x0 ||
        y>=clip.y1 || y+run->height<=clip.y0) return;
    uint32_t first=0, last=run->count;
    while (first<last) {
        uint32_t middle=first+(last-first)/2;
        if (x+run->cells[middle].end<=clip.x0) first=middle+1;
        else last=middle;
    }
    uint32_t row_begin=y<clip.y0 ? (uint32_t)(clip.y0-y) : 0;
    uint32_t row_end=y+run->height>clip.y1 ? (uint32_t)(clip.y1-y) : run->height;
    for (uint32_t i=first;i<run->count;++i) {
        int64_t left=x+(i ? run->cells[i-1].end : 0);
        if (left>=clip.x1) break;
        const control_text_glyph_t *glyph=run->glyphs+run->cells[i].glyph;
        uint32_t begin=left<clip.x0 ? (uint32_t)(clip.x0-left) : 0;
        uint32_t end=left+glyph->width>clip.x1 ? (uint32_t)(clip.x1-left) : glyph->width;
        for (uint32_t row=row_begin;row<row_end;++row) {
            size_t out=(uint64_t)(y+row)*target->stride+(uint32_t)(left+begin);
            const uint8_t *in=run->pixels+glyph->offset+(size_t)row*glyph->width;
            control_text_blend_span(target->pixels+out, in+begin, end-begin, color);
        }
    }
}
