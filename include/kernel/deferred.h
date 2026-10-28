#pragma once

#include <kernel/base.h>

/* 硬中断只投递固定大小的工作项，工作函数在可抢占的内核上下文执行。 */
typedef void (*deferred_work_fn_t)(void *argument);

bool deferred_init(void);

/*
 * 启动可被调度器管理的常驻内核态下半部工作线程。
 * deferred_init() 可在启动早期安全调用；此函数只能在 sched_init() 后调用。
 */
bool deferred_start_worker(void);
bool deferred_worker_started(void);

/* 耗时且可睡眠的清理工作不得占用设备/输入下半部。
 * 在 sched_init() 后启动；返回 false 时回调仍由调用方持有。
 * 生产者可在中断中安全调用、不分配内存，也绝不内联执行工作。 */
bool deferred_background_start_worker(void);
bool deferred_background_worker_started(void);
bool deferred_background_schedule(deferred_work_fn_t function, void *argument);

bool deferred_schedule(deferred_work_fn_t function, void *argument);
/*
 * 硬中断生产者。此函数使用中断安全的队列锁，因此短暂的普通生产者不会丢弃唯一的
 * 设备完成唤醒。返回 false 表示输入无效或普通队列已满。
 */
bool deferred_try_schedule(deferred_work_fn_t function, void *argument);
/*
 * 一个合并式紧急工作项，专供 xHCI 的 MSI-X 工作线程使用。
 * 它不是第二个通用队列：对应的排队标志由 xHCI 管理，借此安全合并重复中断。
 */
bool deferred_schedule_critical(deferred_work_fn_t function, void *argument);
uint32_t deferred_run(uint32_t budget);
bool deferred_self_test(void);

/* 仅用于诊断持续停留在 deferred_schedule 中的生产者故障。 */
uint32_t deferred_debug_progress(uint32_t cpu_index);
uint32_t deferred_debug_waiting(uint32_t cpu_index);
uint32_t deferred_debug_lock_owner(void);
uint32_t deferred_debug_lock_state(void);
uint32_t deferred_debug_worker_state(void);
uint32_t deferred_debug_queue_count(void);
