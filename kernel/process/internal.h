#pragma once

#include <kernel/mm.h>
#include <kernel/process.h>
#include <kernel/resource.h>
#include <kernel/sched.h>
#include <kernel/wait.h>
#include <kernel/vm.h>

void process_initialize_object(object_header_t *header, object_type_id_t type,
                               const object_ops_t *ops);
void process_detach_thread(thread_t *thread);
bool process_begin_exit(process_t *process, int64_t status, uint32_t signal);
/* 由成功执行 RUNNING -> EXITING 转换的一方调用一次，且进程引用仍有效。级联处理常规进程退出时，
 * 排除当前线程。 */
void process_terminate_siblings(process_t *process, thread_t *exclude, int64_t status);
void process_release_runtime_resources(process_t *process);

/* 退出、创建和 exec 路径共用进程线程列表。锁持有者必须留在当前 CPU，避免其在发布列表/状态时被抢占，
 * 导致同级线程永久自旋。 */
static inline void process_thread_lock(process_t *process) {
    sched_preempt_disable();
    spinlock_lock(&process->thread_lock);
}

static inline void process_thread_unlock(process_t *process) {
    spinlock_unlock(&process->thread_lock);
    sched_preempt_enable();
}

/* 调用方保留 process。阻塞式 VM 操作使用此所有者锁保护的快照；操作本身和最终 put 都不在 thread_lock
 * 下执行。 */
static inline vm_space_t *process_vm_reference(process_t *process) {
    process_thread_lock(process);
    vm_space_t *space = process->vm;
    if (space != 0) vm_space_get(space);
    process_thread_unlock(process);
    return space;
}
