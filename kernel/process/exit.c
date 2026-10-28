#include <arch/x86_64/cpu.h>
#include <kernel/console.h>
#include <kernel/debug_stage.h>
#include <kernel/process.h>
#include <kernel/linux_abi.h>
#include <kernel/sched.h>
#include <kernel/vm.h>
#include "internal.h"

static void process_exit_diag(process_t *process, thread_t *thread,
                              int64_t status, uint32_t signal) {
#if OS_DEBUG_SERIAL
    /* 常规退出复用整条记录的公共输出，不另设串口锁或绕过日志开关。 */
    kernel_serial_printf_serial_only(
        "OS_PROCESS_EXIT pid=0x%llx tid=0x%llx status=0x%llx signal=0x%llx\r\n",
        (unsigned long long)(process != 0 ? process->pid : 0U),
        (unsigned long long)(thread != 0 ? thread->tid : 0U),
        (unsigned long long)(uint64_t)status,
        (unsigned long long)signal);
#else
    (void)process;
    (void)thread;
    (void)status;
    (void)signal;
#endif
}

static bool process_is_chrome_image(const process_t *process) {
    static const char path[] = "/usr/apps/chrome/chrome";
    if (process == 0) return false;
    for (size_t index = 0U; index < sizeof(path); ++index) {
        if (process->linux_exe[index] != path[index]) return false;
    }
    return true;
}

bool process_publish_thread_exit(thread_t *thread, int64_t status) {
    process_t *process;
    bool published;
    int64_t previous_status;
    bool remote_owner;

    if (thread == 0) return false;
    process = thread->process;
    remote_owner = thread->owner_cpu != x86_current_cpu_index();

    /* 远端调度器确认可能延迟到系统调用退出时才完成。等待期间绝不能持有进程线程锁：目标 CPU 在到达
     * 该边界前也可能正试图获取此锁。
     */
    if (process != 0 && !remote_owner) {
        process_thread_lock(process);
        /* 将结果发布后，再使本地线程对外显示为已死亡。 */
        previous_status = thread->exit_code;
        thread->exit_code = status;
    }
    published = sched_publish_dead(thread);
    if (process != 0 && !remote_owner) {
        /* 非当前线程或已进入终止状态的线程不会发布此状态。 */
        if (!published) thread->exit_code = previous_status;
        process_thread_unlock(process);
    } else if (process != 0 && published) {
        /* 远端状态发布已完成；在所有者锁下提交状态。 */
        process_thread_lock(process);
        thread->exit_code = status;
        process_thread_unlock(process);
    }
    if (!published) {
        thread_t *current = sched_current_thread();
        kernel_serial_write("OS_DIAG_THREAD_EXIT_PUBLISH_FAIL THREAD=");
        kernel_serial_write_u32((uint32_t)thread->tid);
        kernel_serial_write(" STATE=");
        kernel_serial_write_u32(atomic_load_explicit(&thread->state,
                                                     memory_order_acquire));
        kernel_serial_write(" CURRENT=");
        kernel_serial_write_u32(current != 0 ? (uint32_t)current->tid : 0U);
        kernel_serial_write(" PROCESS=");
        kernel_serial_write_u32(process != 0 ?
                                atomic_load_explicit(&process->state,
                                                     memory_order_acquire) : 0U);
        kernel_serial_write(" THREADS=");
        kernel_serial_write_u32(process != 0 ? process->thread_count : 0U);
        kernel_serial_write(" LOCK=");
        kernel_serial_write_u32(process != 0 ?
                                atomic_load_explicit(&process->thread_lock.state,
                                                     memory_order_acquire) : 0U);
        kernel_serial_write("\r\n");
    }
    return published;
}

void thread_release_execution_ref(thread_t *thread) {
    if (thread == 0) return;
    uint32_t observed = __atomic_load_n(&thread->flags, __ATOMIC_ACQUIRE);
    for (;;) {
        if ((observed & THREAD_FLAG_EXECUTION_REF) == 0U ||
            (observed & THREAD_FLAG_EXECUTION_REAPING) != 0U) return;
        uint32_t desired = observed | THREAD_FLAG_EXECUTION_REAPING;
        if (__atomic_compare_exchange_n(&thread->flags, &observed, desired,
                                        false, __ATOMIC_ACQ_REL,
                                        __ATOMIC_ACQUIRE)) {
            break;
        }
    }

    process_t *process = thread->process;
    /* 调度器仅在死亡线程的执行栈不再活动后调用此函数。回收用户栈时要保留其 VM，以防与最后一个同级
     * 线程的资源拆除并发发生。 */
    /* 栈注册也使用同一所有者锁。注册持锁期间，回收器可以设置执行标志，但必须等该事务完成后才能检查
     * 或清除栈所有权。 */
    if (process != 0) process_thread_lock(process);
    bool owned = thread->user_stack_owned;
    vaddr_t base = thread->user_stack_base;
    size_t size = thread->user_stack_size;
    thread->user_stack_owned = false;
    thread->user_stack_base = 0;
    thread->user_stack_size = 0;
    vm_space_t *space = owned && process != 0 ? process->vm : 0;
    if (space != 0) vm_space_get(space);
    if (process != 0) process_thread_unlock(process);
    if (space != 0) {
        (void)vm_unmap(space, base, size);
        vm_space_put(space);
    }
    if (process != 0 &&
        atomic_load_explicit(&process->state, memory_order_acquire) ==
        PROCESS_DEAD) {
        process_release_runtime_resources(process);
    }
    __atomic_fetch_and(&thread->flags,
                       ~(THREAD_FLAG_EXECUTION_REF |
                         THREAD_FLAG_EXECUTION_REAPING),
                       __ATOMIC_RELEASE);
    object_put(thread);
}

static __noreturn void process_exit_internal(int64_t status,
                                             uint32_t signal) {
    thread_t *thread = sched_current_thread();
    if (thread != 0 && thread->process != 0) {
        process_t *process = thread->process;
        bool first_exit = process_begin_exit(process, status, signal);
        if (first_exit) process_exit_diag(process, thread, status, signal);

        /*
         * 同级线程可能正在另一 CPU 上运行，此时 thread_terminate() 无法同步回收它。先标记所有同级
         * 线程，使其在安全的用户态边界退出；否则繁忙线程可能在保留进程资源的同时，让进程永久停留在
         * EXITING 状态。
         */
        if (first_exit) {
            process_terminate_siblings(process, thread, status);
        }
    }
    thread_exit(status);
}

__noreturn void process_exit(int64_t status) {
    process_exit_internal(status, 0U);
}

__noreturn void process_exit_signal(uint32_t signal) {
    process_exit_internal(-(int64_t)signal, signal);
}

__noreturn void thread_exit(int64_t status) {
    thread_t *thread = sched_current_thread();
    if (thread != 0) {
        process_t *process = thread->process;
        if (process_is_chrome_image(process) &&
            (status != 0 || process->thread_count <= 1U)) {
            (void)kernel_serial_printf_serial_only(
                "OS_CHROME_FINAL_THREAD_EXIT pid=%llx tid=%llx "
                "status=%llx threads=%u\r\n",
                (unsigned long long)process->pid,
                (unsigned long long)thread->tid,
                (unsigned long long)(uint64_t)status,
                process->thread_count);
        }
        linux_thread_exit_cleanup(thread);
        /*
         * 当前上下文切走前，THREAD_DEAD 已对外可见。列表摘除和句柄通知必须与状态发布处于同一事务；
         * 否则定时器可能先暴露死亡线程，而 process->thread_count 仍将其计入。
         */
        sched_preempt_disable();
        debug_trace_stage(OS_DEBUG_PHASE_USER_RUNTIME,
                                 OS_DEBUG_STEP_THREAD_EXIT_ENTER,
                                 (uint32_t)thread->tid);
        if (!process_publish_thread_exit(thread, status)) {
            sched_preempt_enable();
            kernel_debug_stage_fail(OS_DEBUG_PHASE_USER_RUNTIME,
                                     OS_DEBUG_STEP_THREAD_EXIT_PUBLISHED,
                                     K_EBUSY);
            for (;;) __asm__ volatile ("cli; hlt" : : : "memory");
        }
        debug_trace_stage(OS_DEBUG_PHASE_USER_RUNTIME,
                                 OS_DEBUG_STEP_THREAD_EXIT_PUBLISHED,
                                 (uint32_t)status);
        process_detach_thread(thread);
        /*
         * 只有进程列表及可能发生的 PROCESS_DEAD 转换均可见后，才能唤醒句柄等待者。正在 exec 的同级
         * 线程可能会在 WAIT_ONE 返回后立即检查 thread_count。
         */
        object_notify_signaled(thread);
        sched_preempt_enable();
    }
    debug_trace_stage(OS_DEBUG_PHASE_USER_RUNTIME,
                             OS_DEBUG_STEP_THREAD_EXIT_SCHEDULE,
                             thread != 0 ? (uint32_t)thread->tid : 0U);
    schedule();
    for (;;) {
        __asm__ volatile ("sti; hlt" : : : "memory");
        schedule();
    }
}
