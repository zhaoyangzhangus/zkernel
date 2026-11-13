#include <kernel/kmem.h>
#include "internal.h"

/* 合成器在发布扫描输出前，将保留场景写入此缓冲区。 */
void window_buffer_reset_locked(void) {
    g_window_server.composite_framebuffer = 0;
}

void window_buffer_set_target_locked(volatile uint32_t *target) {
    g_window_server.composite_framebuffer = target;
}

void window_buffer_prepare(void) {
    if (g_window_server.composite_framebuffer != 0) return;

    uint64_t pixels =
        (uint64_t)g_window_server.display_stride *
        g_window_server.display_height;

    /* 场景不需要连续物理页；页面对齐的零填充映射避免堆头偏移，并减少大块分配的碎片要求。 */
    if (pixels != 0U && pixels <= SIZE_MAX / sizeof(uint32_t) &&
        g_window_server.display_stride >= g_window_server.display_width)
        window_buffer_set_target_locked(vmalloc((size_t)pixels * sizeof(uint32_t)));
    if (g_window_server.composite_framebuffer == 0) {
        /* 内存不足或几何无效时，使用直接帧缓冲后备路径。 */
        window_buffer_set_target_locked(g_window_server.framebuffer);
    }
}
