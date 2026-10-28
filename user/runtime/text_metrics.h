#pragma once

#include "text.h"
#include <uapi/control_tree.h>
#include <uapi/utf8.h>

/* 控件树布局与旧的应用自绘路径使用同一套尺寸和 UTF-8 单元度量。 */
static inline bool ui_text_valid_size(uint32_t size) {
    return size >= OS_TEXT_MIN_SIZE && size <= OS_TEXT_MAX_SIZE;
}

static inline uint32_t ui_text_step_size(uint32_t current, int32_t direction) {
    if (direction > 0)
        return current >= OS_TEXT_MAX_SIZE - OS_TEXT_SIZE_STEP ?
               OS_TEXT_MAX_SIZE : current + OS_TEXT_SIZE_STEP;
    if (direction < 0)
        return current <= OS_TEXT_MIN_SIZE + OS_TEXT_SIZE_STEP ?
               OS_TEXT_MIN_SIZE : current - OS_TEXT_SIZE_STEP;
    return current;
}

static inline uint32_t ui_text_cell_advance(uint32_t size) {
    return os_control_text_cell_width(' ', size);
}

static inline uint32_t ui_text_layout_line_height(uint32_t size) {
    return size + 2U;
}

static inline uint32_t ui_text_measure_cells(const char *text, size_t length,
                                             uint32_t size) {
    if (text == 0 || length == 0U) return 0U;
    uint64_t width = 0U;
    for (size_t offset = 0U; offset < length;) {
        size_t used;
        uint32_t code = os_utf8_next(text + offset, length - offset, &used);
        offset += used ? used : 1U;
        width += os_control_text_cell_width(code, size);
        if (width > UINT32_MAX) return UINT32_MAX;
    }
    return (uint32_t)width;
}
