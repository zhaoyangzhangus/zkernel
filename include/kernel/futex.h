#pragma once

#include <kernel/process.h>

/* futex 操作默认仅对调用方地址空间可见。 */
kstatus_t futex_wait(process_t *process, uint32_t __user *address,
                     uint32_t expected, uint64_t timeout_ns);
kstatus_t futex_wake(process_t *process, uint32_t __user *address,
                     uint32_t maximum, uint32_t *woken);

/* Linux FUTEX_PRIVATE_FLAG 使用（地址空间、虚拟地址）作为键；共享 futex 使用物理字作为键，
 * 因此不同进程中的别名映射也能互相唤醒。 */
kstatus_t futex_wait_keyed(process_t *process, uint32_t __user *address,
                           uint32_t expected, uint64_t timeout_ns,
                           bool private_key);
kstatus_t futex_wake_keyed(process_t *process, uint32_t __user *address,
                           uint32_t maximum, uint32_t *woken,
                           bool private_key);

/* 启动自测诊断接口，正式内核将由统一 trace 计数器替代。 */
