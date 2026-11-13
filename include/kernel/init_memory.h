#pragma once

#include <kernel/bootinfo.h>

typedef struct kernel_init_memory_hooks {
    void (*write)(const CHAR8 *text);
    void (*halt)(void);
    BOOLEAN (*console_init)(const OS_BOOT_INFO *info,
                            UINT64 framebuffer_virtual_base);
    void (*console_disable)(void);
} kernel_init_memory_hooks_t;

/* 初始化规范内存映射，并发布内存边界。 */
BOOLEAN kernel_init_memory(OS_BOOT_INFO *info,
                           const kernel_init_memory_hooks_t *hooks,
                           UINT64 *framebuffer_virtual_base);

/* 早期采用保守的 GOP 映射，并在切换到高半区后复用。 */
BOOLEAN map_framebuffer_wc(const OS_BOOT_INFO *info,
                                  UINT64 *virtual_base);
/* PCI 发现完成后，安全升级受支持且不重叠的 GOP BAR。 */
BOOLEAN framebuffer_enable_wc(const OS_BOOT_INFO *info, UINT64 virtual_base);
