#pragma once

#include <kernel/bootinfo.h>

/*
 * 早期控制台设置与 kernel_entry() 分开。文本输出由 console.h 提供；此头文件只包含后端生命周期钩子，
 * 因此不会形成第二套串口输出 API。
 */
BOOLEAN kernel_console_init(const OS_BOOT_INFO *info,
                            UINT64 framebuffer_virtual_base);
BOOLEAN kernel_console_init_early(const OS_BOOT_INFO *info);
void kernel_console_disable(void);
void kernel_console_serial_init(void);
