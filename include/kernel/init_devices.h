#pragma once

#include <kernel/bootinfo.h>

typedef struct kernel_init_devices_hooks {
    void (*write)(const CHAR8 *text);
    void (*write_u32)(UINT32 value);
    void (*halt)(void);
} kernel_init_devices_hooks_t;

/* 发现平台设备，并完成确定性的设备自检。 */
BOOLEAN kernel_init_devices(OS_BOOT_INFO *info,
                            const kernel_init_devices_hooks_t *hooks);
