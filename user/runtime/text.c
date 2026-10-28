#include <stddef.h>
#include <stdint.h>
#if OS_REALTEST
#include <stdio.h>
#endif

#include <blend2d/blend2d.h>

#include "text.h"
#include "text_metrics.h"
#include <uapi/control_tree.h>
#include <uapi/font.h>
#include <uapi/utf8.h>

typedef struct ui_text_state {
    BLFontDataCore font_data;
    BLFontFaceCore font_face;
    BLFontCore font;
    BLImageCore image;
    BLContextCore context;
    BLFontMetrics metrics;
    uint32_t size;
    uint32_t advance;
    uint32_t line_height;
    uint32_t baseline;
    uint32_t *pixels;
    uint32_t stride;
    uint32_t width;
    uint32_t height;
    bool runtime_ready;
    bool font_data_ready;
    bool font_face_ready;
    bool font_ready;
    bool context_ready;
} ui_text_state_t;

static ui_text_state_t g_text;
static const char g_font_path[] = OS_FONT_DEFAULT_PATH;

static uint32_t ceil_pixel(float value) {
    uint32_t integer;
    if (value <= 0.0f) return 1U;
    integer = (uint32_t)value;
    return value > (float)integer ? integer + 1U : integer;
}

static size_t text_length(const char *text) {
    size_t length = 0U;
    if (text == 0) return 0U;
    while (text[length] != '\0') ++length;
    return length;
}

static void close_context(void) {
    if (!g_text.context_ready) return;
    (void)bl_context_end(&g_text.context);
    (void)bl_context_destroy(&g_text.context);
    (void)bl_image_destroy(&g_text.image);
    g_text.context_ready = false;
    g_text.pixels = 0;
    g_text.stride = 0U;
    g_text.width = 0U;
    g_text.height = 0U;
}

void ui_text_shutdown(void) {
    close_context();
    if (g_text.font_ready) {
        (void)bl_font_destroy(&g_text.font);
        g_text.font_ready = false;
    }
    if (g_text.font_face_ready) {
        (void)bl_font_face_destroy(&g_text.font_face);
        g_text.font_face_ready = false;
    }
    if (g_text.font_data_ready) {
        (void)bl_font_data_destroy(&g_text.font_data);
        g_text.font_data_ready = false;
    }
    if (g_text.runtime_ready) {
        (void)bl_runtime_shutdown();
        g_text.runtime_ready = false;
    }
    g_text.size = 0U;
    g_text.advance = 0U;
    g_text.line_height = 0U;
    g_text.baseline = 0U;
}

static bool update_metrics(ui_text_state_t *text) {
    if (bl_font_get_metrics(&text->font,&text->metrics)!=BL_SUCCESS) return false;
    /* 版式尺寸与系统控件一致；基线仅供遗留 Surface 文本绘制使用。 */
    text->advance=ui_text_cell_advance(text->size);
    text->line_height=ui_text_layout_line_height(text->size);
    text->baseline=ceil_pixel(text->metrics.ascent);
    return true;
}

bool ui_text_init(uint32_t size) {
    ui_text_state_t next = {0};
    if (!ui_text_valid_size(size)) {
        return false;
    }
    if (!g_text.runtime_ready) {
        if (bl_runtime_init() != BL_SUCCESS) return false;
        g_text.runtime_ready = true;
    }
    if (!g_text.font_data_ready) {
        if (bl_font_data_init(&g_text.font_data) != BL_SUCCESS) goto fail;
        g_text.font_data_ready = true;
        if (bl_font_data_create_from_file(&g_text.font_data,
                                          g_font_path,
                                          BL_FILE_READ_NO_FLAGS) != BL_SUCCESS) {
            goto fail;
        }
    }
    if (!g_text.font_face_ready) {
        if (bl_font_face_init(&g_text.font_face) != BL_SUCCESS) goto fail;
        g_text.font_face_ready = true;
        if (bl_font_face_create_from_data(&g_text.font_face, &g_text.font_data,
                                          0U) != BL_SUCCESS) goto fail;
    }
    if (g_text.font_ready && g_text.size == size) return true;
    if (bl_font_init(&next.font) != BL_SUCCESS) goto fail;
    next.size = size;
    if (bl_font_create_from_face(&next.font, &g_text.font_face,
                                 (float)size) != BL_SUCCESS ||
        !update_metrics(&next)) {
        (void)bl_font_destroy(&next.font);
        goto fail;
    }
    /* 仅在创建和度量值计算都成功后提交。缩放失败时保留旧字体及其可用上下文；
     * 缩放成功时先使旧光栅化上下文失效，再释放其字体。 */
    close_context();
    if (g_text.font_ready) (void)bl_font_destroy(&g_text.font);
    (void)bl_font_init_move(&g_text.font, &next.font);
    (void)bl_font_destroy(&next.font);
    g_text.font_ready = true;
    g_text.size = next.size;
    g_text.metrics = next.metrics;
    g_text.advance = next.advance;
    g_text.line_height = next.line_height;
    g_text.baseline = next.baseline;
    return true;

fail:
    if (!g_text.font_ready) ui_text_shutdown();
    return false;
}

bool ui_text_adjust(int32_t direction) {
    uint32_t current = g_text.size;
    uint32_t next;
    if (!g_text.font_ready || direction == 0) return false;
    next = ui_text_step_size(current, direction);
    if (next == current || !ui_text_init(next)) return false;
#if OS_REALTEST
    (void)fprintf(stderr, "OS_TEXT_ZOOM_SIZE=%u\n", next);
#endif
    return true;
}

uint32_t ui_text_size(void) {
    return g_text.size;
}

uint32_t ui_text_advance(void) {
    return g_text.advance;
}

uint32_t ui_text_line_height(void) {
    return g_text.line_height;
}

static bool open_context(uint32_t *pixels, uint32_t stride, uint32_t width,
                         uint32_t height) {
    if (!g_text.font_ready || pixels == 0 || width == 0U || height == 0U ||
        stride < width) return false;
    if (g_text.context_ready && g_text.pixels == pixels &&
        g_text.stride == stride && g_text.width == width &&
        g_text.height == height) return true;
    close_context();
    if (bl_image_init_as_from_data(
            &g_text.image, (int)width, (int)height, BL_FORMAT_XRGB32, pixels,
            (intptr_t)((uint64_t)stride * sizeof(uint32_t)), BL_DATA_ACCESS_RW,
            0, 0) != BL_SUCCESS) return false;
    if (bl_context_init_as(&g_text.context, &g_text.image, 0) != BL_SUCCESS) {
        (void)bl_image_destroy(&g_text.image);
        return false;
    }
    (void)bl_context_set_comp_op(&g_text.context, BL_COMP_OP_SRC_OVER);
    g_text.context_ready = true;
    g_text.pixels = pixels;
    g_text.stride = stride;
    g_text.width = width;
    g_text.height = height;
    return true;
}

static void draw_text_range(int32_t x, int32_t y, const char *text,
                            size_t length, uint32_t color) {
    int64_t baseline = (int64_t)y + g_text.baseline;
    BLPointI origin;
    if (length == 0U || baseline < INT32_MIN || baseline > INT32_MAX) return;
    origin.x = x;
    origin.y = (int32_t)baseline;
    (void)bl_context_fill_utf8_text_i_rgba32(
        &g_text.context, &origin, &g_text.font, text, length,
        color | 0xFF000000U);
}

void ui_text_draw(uint32_t *pixels, uint32_t stride, uint32_t width,
                      uint32_t height, int32_t x, int32_t y,
                      const char *text, uint32_t color) {
    if (!open_context(pixels, stride, width, height)) return;
    draw_text_range(x, y, text, text_length(text), color);
}

void ui_text_draw_clipped(uint32_t *pixels, uint32_t stride,
                              uint32_t width, uint32_t height, int32_t x,
                              int32_t y, uint32_t right, const char *text,
                              uint32_t color) {
    ui_text_draw_rect(pixels, stride, width, height, x, y,
                         0U, 0U, right, height, text, color);
}

void ui_text_draw_rect(uint32_t *pixels, uint32_t stride,
                           uint32_t width, uint32_t height, int32_t x,
                           int32_t y, uint32_t left, uint32_t top,
                           uint32_t right, uint32_t bottom,
                           const char *text, uint32_t color) {
    BLContextCookie cookie = {0};
    BLRectI clip;
    uint32_t clip_right = right < width ? right : width;
    uint32_t clip_bottom = bottom < height ? bottom : height;
    if (left >= clip_right || top >= clip_bottom ||
        !open_context(pixels, stride, width, height)) {
        return;
    }
    clip.x = (int)left;
    clip.y = (int)top;
    clip.w = (int)(clip_right - left);
    clip.h = (int)(clip_bottom - top);
    if (bl_context_save(&g_text.context, &cookie) == BL_SUCCESS) {
        if (bl_context_clip_to_rect_i(&g_text.context, &clip) == BL_SUCCESS) {
            draw_text_range(x, y, text, text_length(text), color);
        }
        (void)bl_context_restore(&g_text.context, &cookie);
    }
}

uint32_t ui_text_measure_range(const char *text, size_t length) {
    /* 控件布局与合成器共用 UTF-8 单元度量，不再由另一套字体引擎推算命中宽度。 */
    return g_text.font_ready ? ui_text_measure_cells(text, length, g_text.size) : 0U;
}

uint32_t ui_text_measure(const char *text) {
    return ui_text_measure_range(text, text_length(text));
}
