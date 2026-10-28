#pragma once

#include <kernel/bootinfo.h>

typedef struct kernel_init_memory_hooks {
    void (*write)(const CHAR8 *text);
    void (*halt)(void);
    BOOLEAN (*console_init)(const OS_BOOT_INFO *info,
                            UINT64 framebuffer_virtual_base);
    void (*console_disable)(void);
} kernel_init_memory_hooks_t;

/* Bring up the canonical memory map and publish the memory boundary. */
BOOLEAN kernel_init_memory(OS_BOOT_INFO *info,
                           const kernel_init_memory_hooks_t *hooks,
                           UINT64 *framebuffer_virtual_base);

/* Conservative early GOP mapping, reused after the high-half hand-off. */
BOOLEAN map_framebuffer_wc(const OS_BOOT_INFO *info,
                                  UINT64 *virtual_base);
/* After PCI discovery, safely upgrade a supported, non-overlapping GOP BAR. */
BOOLEAN framebuffer_enable_wc(const OS_BOOT_INFO *info, UINT64 virtual_base);
