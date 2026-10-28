#pragma once

#include "mutex.h"

/* 调度阶段的持锁线程可以休眠，并延迟退出直到解锁。
 * 无线程的启动模式仅限 BSP，不能与调度阶段的所有权交叠。 */
typedef struct io_mutex {
    kmutex_t scheduled;
    atomic_bool boot_owned;
} io_mutex_t;

void io_mutex_init(io_mutex_t *mutex, bool priority_inheritance);
kstatus_t io_mutex_lock(io_mutex_t *mutex);
kstatus_t io_mutex_unlock(io_mutex_t *mutex);

/* 无返回值的子系统锁接口获取失败时，禁止继续执行未受保护的操作。 */
static inline void io_mutex_lock_required(io_mutex_t *mutex) {
    if (io_mutex_lock(mutex) != K_OK) __builtin_trap();
}

static inline void io_mutex_unlock_required(io_mutex_t *mutex) {
    if (io_mutex_unlock(mutex) != K_OK) __builtin_trap();
}
