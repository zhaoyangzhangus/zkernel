#include "text_metrics.h"

/* 控件由合成器绘制时，应用只需保存字号，无需加载字体或软件绘图库。 */
static uint32_t g_layout_size;

bool ui_text_init(uint32_t size) {
    if (!ui_text_valid_size(size)) return false;
    g_layout_size = size;
    return true;
}

bool ui_text_adjust(int32_t direction) {
    if (g_layout_size == 0U) return false;
    uint32_t next = ui_text_step_size(g_layout_size, direction);
    if (next == g_layout_size) return false;
    g_layout_size = next;
    return true;
}

void ui_text_shutdown(void) {
    g_layout_size = 0U;
}

uint32_t ui_text_size(void) {
    return g_layout_size;
}

uint32_t ui_text_advance(void) {
    return g_layout_size ? ui_text_cell_advance(g_layout_size) : 0U;
}

uint32_t ui_text_line_height(void) {
    return g_layout_size ? ui_text_layout_line_height(g_layout_size) : 0U;
}

uint32_t ui_text_measure_range(const char *text, size_t length) {
    return g_layout_size ? ui_text_measure_cells(text, length, g_layout_size) : 0U;
}

uint32_t ui_text_measure(const char *text) {
    size_t length = 0U;
    if (text != 0) while (text[length] != '\0') ++length;
    return ui_text_measure_range(text, length);
}
