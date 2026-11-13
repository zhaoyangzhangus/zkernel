#pragma once

#include <kernel/bootinfo.h>

typedef struct kernel_init_early_hooks {
    void (*write)(const CHAR8 *text);
    void (*halt)(void);
} kernel_init_early_hooks_t;

/* 校验加载器状态，并初始化 CPU/早期平台边界。 */
BOOLEAN kernel_init_early(OS_BOOT_INFO *info,
                          const kernel_init_early_hooks_t *hooks);
