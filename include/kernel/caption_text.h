#pragma once
#include <stdatomic.h>
#include <stdint.h>
#include <uapi/window.h>

static inline uint32_t caption_text_height(uint32_t flags) {
    uint32_t bar=os_window_caption_height(flags);
    uint32_t height=bar>24U ? bar-24U : 16U;
    return height>96U ? 96U : height;
}

/* 标题与正文使用同一字体服务；仅在提交线程生成不可变 A8 快照。 */
typedef struct caption_text {
    atomic_uint references;
    uint32_t width, height;
    uint8_t pixels[];
} caption_text_t;

caption_text_t *caption_text_create(const char *text, uint32_t height);
void caption_text_get(caption_text_t *text);
void caption_text_put(caption_text_t *text);
