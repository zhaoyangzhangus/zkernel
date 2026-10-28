#pragma once
#pragma once
#include "base.h"
#include "list.h"
#include "sched.h"
#include "wait.h"

typedef struct kmutex {
    wait_queue_t waitq;
    thread_t *owner;
    list_head_t owner_node;
    list_head_t pi_waiters;
    bool pi_enabled;
    bool initialized;
    bool exit_deferred; /* 由持锁线程使用，仅在 PI 锁保护下转移所有权。 */
} kmutex_t;

void kmutex_init(kmutex_t *mutex, bool priority_inheritance);
kstatus_t kmutex_try_lock(kmutex_t *mutex);
/* 启动阶段的 idle 线程不能等待本 CPU 上的持锁线程。
 * 必须在 PI 锁保护下检查所有者所属 CPU，避免解引用失效指针。 */
kstatus_t kmutex_try_lock_bootstrap(kmutex_t *mutex);
kstatus_t kmutex_lock(kmutex_t *mutex, uint64_t timeout_ns);
/* 获取期间延迟取消，持锁期间延迟外部终止，直到解锁。
 * 不禁用调度；不同互斥锁的保护区允许嵌套。 */
kstatus_t kmutex_lock_uninterruptible(kmutex_t *mutex, uint64_t timeout_ns);
kstatus_t kmutex_unlock(kmutex_t *mutex);
bool kmutex_pi_self_test(void);
