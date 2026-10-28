#include <kernel/display.h>
#include <kernel/console.h>
#include "internal.h"

/* 硬件查询和扫描输出选择逻辑保持在合成器之外。 */
void window_display_reset_locked(void) {
    g_window_server.display_width = 0U;
    g_window_server.display_height = 0U;
    g_window_server.display_stride = 0U;
    g_window_server.display_format = 0U;
    g_window_server.framebuffer = 0;
}

bool window_display_prepare(void) {
    uint32_t width;
    uint32_t height;
    uint32_t stride;
    uint32_t format;

    if (!display_core_query(0U, &width, &height, &stride, &format)) {
        kernel_serial_write(
            "OS_WINDOW_DISPLAY_PREPARE_FAIL QUERY\r\n");
        return false;
    }

    g_window_server.display_width = width;
    g_window_server.display_height = height;
    g_window_server.display_stride = stride;
    g_window_server.display_format = format;
    g_window_server.framebuffer =
        (volatile uint32_t *)(uintptr_t)display_core_framebuffer_virtual();

    if (g_window_server.framebuffer == 0 || width == 0U ||
        height == 0U || stride < width) {
        kernel_serial_write(
            "OS_WINDOW_DISPLAY_PREPARE_FAIL GEOMETRY WIDTH=");
        kernel_serial_write_u32(width);
        kernel_serial_write(" HEIGHT=");
        kernel_serial_write_u32(height);
        kernel_serial_write(" STRIDE=");
        kernel_serial_write_u32(stride);
        kernel_serial_write(" FB_LO=");
        kernel_serial_write_u32(
            (uint32_t)(uintptr_t)g_window_server.framebuffer);
        kernel_serial_write("\r\n");
        return false;
    }

    kernel_serial_printf_serial_only(
        "OS_DISPLAY_GEOMETRY width=%u height=%u stride=%u format=%u "
        "fb_lo=%u fb_hi=%u\r\n",
        width, height, stride, format,
        (uint32_t)(uintptr_t)g_window_server.framebuffer,
        (uint32_t)((uint64_t)(uintptr_t)g_window_server.framebuffer >> 32U));
    return true;
}
