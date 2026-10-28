#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "text.h"
#include <uapi/utf8.h>

/* 应用发布依赖字体的度量值，但不负责绘制窗口装饰。 */
static inline uint32_t user_caption_height(void) {
    uint32_t line = ui_text_line_height();
    uint32_t font = ui_text_size();
    if (line >= 112U || font >= 104U) return 128U;
    /* 字号一旦放大便同步增高标题栏，不能等行高越过旧阈值才响应。 */
    uint32_t height = font + 24U;
    if (height < line + 16U) height = line + 16U;
    return height < 40U ? 40U : height;
}

static inline void user_caption_format_title(char title[32], const char *path,
                                              bool modified) {
    uint32_t index = 0U;
    if (modified) { title[index++] = '*'; title[index++] = ' '; }
    if (path != 0) {
        while (index < 31U && *path != '\0') {
            size_t used;
            uint32_t code = os_utf8_next(path, 31U - index, &used);
            if (!used || code == 0xFFFDU) break;
            for (size_t i = 0; i < used; ++i) title[index++] = *path++;
        }
    }
    while (index < 32U) title[index++] = '\0';
}

/* 应用名保留在前，长路径仅按完整 UTF-8 字符截断。 */
static inline void user_caption_format_app_title(char title[32], const char *app,
                                                 const char *path, bool modified) {
    char combined[32];
    user_caption_format_title(combined, app, false);
    uint32_t i = 0;
    while (combined[i]) ++i;
    if (path && path[0] && i + 3U < 31U) {
        combined[i++] = ' '; combined[i++] = '-'; combined[i++] = ' ';
        char tail[32];
        user_caption_format_title(tail, path, false);
        for (uint32_t j = 0; tail[j] && i < 31U;) {
            size_t used;
            uint32_t code = os_utf8_next(tail + j, 31U - i, &used);
            if (!used || code == 0xFFFDU) break;
            for (size_t k = 0; k < used; ++k) combined[i++] = tail[j++];
        }
        combined[i] = '\0';
    }
    user_caption_format_title(title, combined, modified);
}
