#include "internal.h"

/* REFACTOR_P7B_COMPOSITOR_DRAG_OWNER：保留场景拖动时的重叠区域移动。 */

/*
 * WB 保留场景的 qword 存储。
 *
 * 此路径中的私有合成帧缓冲区是普通可缓存 RAM。
 * 显式使用 MOVQ，使未对齐的源/目标 X 坐标有效，而不依赖 C 对齐规则。
 */
static inline void compositor_wb_store64(
    uint32_t *destination,
    uint64_t value) {

    __asm__ volatile (
        "movq %1, %0"
        : "=m"(*(uint64_t *)(void *)destination)
        : "r"(value));
}

/*
 * WB 保留场景的 qword 加载。
 *
 * P6 将 WB -> GOP 发布集中到 display core，但合成器移动保留场景像素时仍需执行 WB 加载，
 * 以支持安全处理重叠区间的反向 memmove。此只读辅助函数保留在本地，不重复实现 P6
 * 从此模块移除的显示内存写入逻辑。
 */
static inline uint64_t compositor_wb_load64(
    const uint32_t *source) {

    uint64_t value;

    __asm__ (
        "movq %1, %0"
        : "=r"(value)
        : "m"(*(const uint64_t *)(const void *)source));

    return value;
}

/*
 * 将重叠的 WB 区间从高地址向低地址复制。
 *
 * 在保持 DF=0 的前提下，REP MOVSQ 只有此方向无法安全完成。内核在此处绝不设置方向标志：
 * 中断/异常可能在任意指令处发生，而常规 x86-64 内核 ABI 要求 DF 始终清零。
 *
 * 先加载完整的 64 字节块，再写入其中任何部分。这样即使源和目标在同一块内重叠并右移一个像素，
 * 操作仍然安全。
 */
static inline void compositor_move_wb_pixels_backward(
    uint32_t *destination,
    const uint32_t *source,
    uint32_t pixels) {

    if (destination == 0 ||
        source == 0 ||
        pixels == 0U) {
        return;
    }

    if ((pixels & 1U) != 0U) {
        --pixels;
        destination[pixels] =
            source[pixels];
    }

    while (pixels >= 16U) {
        uint64_t v0;
        uint64_t v1;
        uint64_t v2;
        uint64_t v3;
        uint64_t v4;
        uint64_t v5;
        uint64_t v6;
        uint64_t v7;

        pixels -= 16U;

        v0 = compositor_wb_load64(source + pixels + 0U);
        v1 = compositor_wb_load64(source + pixels + 2U);
        v2 = compositor_wb_load64(source + pixels + 4U);
        v3 = compositor_wb_load64(source + pixels + 6U);
        v4 = compositor_wb_load64(source + pixels + 8U);
        v5 = compositor_wb_load64(source + pixels + 10U);
        v6 = compositor_wb_load64(source + pixels + 12U);
        v7 = compositor_wb_load64(source + pixels + 14U);

        __asm__ volatile ("" : : : "memory");

        compositor_wb_store64(destination + pixels + 14U, v7);
        compositor_wb_store64(destination + pixels + 12U, v6);
        compositor_wb_store64(destination + pixels + 10U, v5);
        compositor_wb_store64(destination + pixels + 8U, v4);
        compositor_wb_store64(destination + pixels + 6U, v3);
        compositor_wb_store64(destination + pixels + 4U, v2);
        compositor_wb_store64(destination + pixels + 2U, v1);
        compositor_wb_store64(destination + pixels + 0U, v0);
    }

    while (pixels >= 2U) {
        uint64_t value;

        pixels -= 2U;

        value =
            compositor_wb_load64(
                source + pixels);

        compositor_wb_store64(
            destination + pixels,
            value);
    }
}

/*
 * 对一段 XRGB8888 WB 扫描线执行 memmove 语义的复制。
 *
 * 正向/不重叠复制使用现有 REP MOVSQ 快速路径。仅重叠的向右移动需要使用上方的高到低复制辅助函数。
 */
static inline void compositor_move_wb_pixels(
    uint32_t *destination,
    const uint32_t *source,
    uint32_t pixels) {

    uintptr_t destination_address;
    uintptr_t source_address;
    uintptr_t bytes;

    if (destination == 0 ||
        source == 0 ||
        pixels == 0U ||
        destination == source) {
        return;
    }

    destination_address =
        (uintptr_t)destination;

    source_address =
        (uintptr_t)source;

    bytes =
        (uintptr_t)pixels *
        sizeof(uint32_t);

    if (destination_address < source_address ||
        destination_address >= source_address + bytes) {

        compositor_copy_wb_pixels(
            destination,
            source,
            pixels);

        return;
    }

    compositor_move_wb_pixels_backward(
        destination,
        source,
        pixels);
}

/* 在保留的合成缓冲区内移动最上层拖动窗口的重叠区域。源数据来自最近一次已提交帧，因此
 * 单像素移动时无需重新读取客户端分区并重绘所有像素。 */
void compositor_blit_drag_overlap(void) {
    const compositor_snapshot_t *snapshot = &g_compositor_snapshot;
    int64_t local_left;
    int64_t local_top;
    int64_t local_right;
    int64_t local_bottom;
    uint32_t width;
    uint32_t height;

    if (!snapshot->drag_blit_valid ||
        snapshot->dragging_identifier == 0U ||
        snapshot->drag_width == 0U ||
        snapshot->drag_height == 0U ||
        g_window_server.composite_framebuffer == 0 ||
        g_window_server.composite_framebuffer ==
            g_window_server.framebuffer ||
        snapshot->window_count == 0U ||
        snapshot->windows[snapshot->window_count - 1U].identifier !=
            snapshot->dragging_identifier) {
        return;
    }

    local_left = 0;
    local_top = 0;
    local_right = snapshot->drag_width;
    local_bottom = snapshot->drag_height;

    if (snapshot->drag_old_x < 0 &&
        local_left < -(int64_t)snapshot->drag_old_x) {
        local_left = -(int64_t)snapshot->drag_old_x;
    }
    if (snapshot->drag_new_x < 0 &&
        local_left < -(int64_t)snapshot->drag_new_x) {
        local_left = -(int64_t)snapshot->drag_new_x;
    }
    if (snapshot->drag_old_y < 0 &&
        local_top < -(int64_t)snapshot->drag_old_y) {
        local_top = -(int64_t)snapshot->drag_old_y;
    }
    if (snapshot->drag_new_y < 0 &&
        local_top < -(int64_t)snapshot->drag_new_y) {
        local_top = -(int64_t)snapshot->drag_new_y;
    }

    {
        int64_t old_visible_right =
            (int64_t)g_window_server.display_width -
            snapshot->drag_old_x;
        int64_t new_visible_right =
            (int64_t)g_window_server.display_width -
            snapshot->drag_new_x;
        int64_t old_visible_bottom =
            (int64_t)g_window_server.display_height -
            snapshot->drag_old_y;
        int64_t new_visible_bottom =
            (int64_t)g_window_server.display_height -
            snapshot->drag_new_y;

        if (local_right > old_visible_right) {
            local_right = old_visible_right;
        }
        if (local_right > new_visible_right) {
            local_right = new_visible_right;
        }
        if (local_bottom > old_visible_bottom) {
            local_bottom = old_visible_bottom;
        }
        if (local_bottom > new_visible_bottom) {
            local_bottom = new_visible_bottom;
        }
    }

    if (local_left < 0) local_left = 0;
    if (local_top < 0) local_top = 0;

    if (local_left >= local_right ||
        local_top >= local_bottom) {
        return;
    }

    width = (uint32_t)(local_right - local_left);
    height = (uint32_t)(local_bottom - local_top);

    for (uint32_t row_index = 0U;
         row_index < height;
         ++row_index) {
        uint32_t row =
            snapshot->drag_new_y > snapshot->drag_old_y ?
                height - 1U - row_index :
                row_index;

        int64_t local_y = local_top + (int64_t)row;
        int64_t source_y =
            (int64_t)snapshot->drag_old_y + local_y;
        int64_t destination_y =
            (int64_t)snapshot->drag_new_y + local_y;

        int64_t source_x =
            (int64_t)snapshot->drag_old_x + local_left;
        int64_t destination_x =
            (int64_t)snapshot->drag_new_x + local_left;

        /* 回退路径不会调用此函数，因此保留帧缓冲区是私有 WB RAM，而非可见的扫描输出区域。 */
        uint32_t *scene =
            (uint32_t *)(uintptr_t)
                g_window_server.composite_framebuffer;

        const uint32_t *source =
            scene +
            (uint64_t)source_y *
                g_window_server.display_stride +
            (uint32_t)source_x;

        uint32_t *destination =
            scene +
            (uint64_t)destination_y *
                g_window_server.display_stride +
            (uint32_t)destination_x;

        compositor_move_wb_pixels(
            destination,
            source,
            width);
    }
}
