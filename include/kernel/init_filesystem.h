#pragma once

#include <kernel/bootinfo.h>
#include "nvme_core.h"

typedef struct kernel_init_filesystem_hooks {
    void (*write)(const CHAR8 *text);
    void (*write_u32)(UINT32 value);
    void (*halt)(void);
} kernel_init_filesystem_hooks_t;

/* 挂载启动根目录，并校验 VFS、文件映射、日志和 LiteFS 路径。 */
BOOLEAN prepare_realtest_root(const OS_BOOT_INFO *boot_info);
BOOLEAN kernel_init_filesystem(const OS_BOOT_INFO *boot_info,
                               const nvme_controller_t *active_controller,
                               const kernel_init_filesystem_hooks_t *hooks);
