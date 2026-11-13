#pragma once

#include <kernel/bootinfo.h>

typedef struct kernel_init_graphics_hooks {
    void (*write)(const CHAR8 *text);
    void (*write_u32)(UINT32 value);
    void (*halt)(void);
} kernel_init_graphics_hooks_t;

/* 在同一个明确边界处运行图形/输入/系统调用启动自检。 */
BOOLEAN kernel_init_graphics(const OS_BOOT_INFO *info,
                             const kernel_init_graphics_hooks_t *hooks);
