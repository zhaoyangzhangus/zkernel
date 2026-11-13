#pragma once

#include <kernel/bootinfo.h>
#include "nvme_core.h"

typedef struct kernel_init_storage_hooks {
    void (*write)(const CHAR8 *text);
    void (*write_u32)(UINT32 value);
    void (*halt)(void);
} kernel_init_storage_hooks_t;

/* 校验存储控制器路径，并返回活动控制器。 */
BOOLEAN kernel_init_storage(const kernel_init_storage_hooks_t *hooks,
                            const nvme_controller_t **active_controller);
