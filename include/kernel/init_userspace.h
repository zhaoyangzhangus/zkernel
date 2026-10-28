#pragma once

#include <kernel/bootinfo.h>

typedef struct kernel_init_userspace_hooks {
    void (*write)(const CHAR8 *text);
    void (*write_u32)(UINT32 value);
    void (*halt)(void);
} kernel_init_userspace_hooks_t;

/* Start after exclusive boot fixtures; GOP desktop never joins the GPU. */
BOOLEAN userspace_start_device_init(void);
BOOLEAN userspace_finish_device_init(void);
BOOLEAN userspace_device_init_done(void);
BOOLEAN userspace_wait_device_init(void);
void userspace_poll_device_init(void);

/* Publish the initial user services, IPC, window server, and runtime workers. */
BOOLEAN kernel_init_userspace(const kernel_init_userspace_hooks_t *hooks);

/* Complete the single-CPU compositor handoff after kernel_main changes stack. */
BOOLEAN userspace_start_window_worker(
    const kernel_init_userspace_hooks_t *hooks);

/* Run the user ELF/process/scheduler runtime gate after display setup. */
void userspace_run_runtime_self_test(
    const kernel_init_userspace_hooks_t *hooks);
