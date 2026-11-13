#pragma once

#include <kernel/bootinfo.h>

typedef struct kernel_init_scheduler_hooks {
    void (*write)(const CHAR8 *text);
    void (*write_u32)(UINT32 value);
    void (*halt)(void);
} kernel_init_scheduler_hooks_t;

/* 启动 AP、发布调度器，并运行内核早期检查。 */
BOOLEAN kernel_init_scheduler(const OS_BOOT_INFO *boot_info,
                              const kernel_init_scheduler_hooks_t *hooks);

/* 旧队列 API 的过渡期自检；它不是当前使用的调度器。 */
