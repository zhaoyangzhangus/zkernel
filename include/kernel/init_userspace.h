#pragma once

#include <kernel/bootinfo.h>

typedef struct kernel_init_userspace_hooks {
    void (*write)(const CHAR8 *text);
    void (*write_u32)(UINT32 value);
    void (*halt)(void);
} kernel_init_userspace_hooks_t;

/* 独占引导测试夹具结束后再启动；GOP 桌面不会使用 GPU。 */
BOOLEAN userspace_start_device_init(void);
BOOLEAN userspace_finish_device_init(void);
BOOLEAN userspace_device_init_done(void);
BOOLEAN userspace_wait_device_init(void);
void userspace_poll_device_init(void);

/* 发布初始用户服务、IPC、窗口服务器和运行时工作线程。 */
BOOLEAN kernel_init_userspace(const kernel_init_userspace_hooks_t *hooks);

/* kernel_main 切换栈后，完成单 CPU 合成器交接。 */
BOOLEAN userspace_start_window_worker(
    const kernel_init_userspace_hooks_t *hooks);

/* 显示初始化完成后，运行用户 ELF/进程/调度器检查。 */
void userspace_run_runtime_self_test(
    const kernel_init_userspace_hooks_t *hooks);
