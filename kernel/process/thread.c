#include <arch/x86_64/context.h>
#include <arch/x86_64/cpu.h>
#include <arch/x86_64/paging.h>
#include <arch/x86_64/syscall_internal.h>
#include <arch/x86_64/uaccess.h>
#include <kernel/debug_stage.h>
#include <kernel/console.h>
#include <kernel/process.h>
#include <kernel/sched.h>
#include <kernel/wait.h>
#include "internal.h"

#define USER_CODE_SELECTOR 0x23ULL
#define USER_DATA_SELECTOR 0x1BULL
#define USER_INITIAL_RFLAGS 0x202ULL
#define THREAD_KERNEL_STACK_SIZE (64U * 1024U)

extern void x86_user_thread_start(void);

static bool thread_is_signaled(const void *object) {
    const thread_t *thread = (const thread_t *)object;
    return atomic_load_explicit(&thread->state, memory_order_acquire) == THREAD_DEAD;
}

static int64_t thread_wait_value(const void *object) {
    return ((const thread_t *)object)->exit_code;
}

static void thread_destroy(void *object);

static const object_ops_t g_thread_ops = {
    .destroy = thread_destroy,
    .type_name = "Thread",
    .is_signaled = thread_is_signaled,
    .wait_value = thread_wait_value,
};

static volatile uint32_t g_process_thread_create_stage;
#if OS_DEBUG_SERIAL
static void thread_cancel_diag(thread_t *target, int64_t status) {
    thread_t *caller = sched_current_thread();
    /* 与其他常规记录共用串口事务，避免不同私有锁保护的输出相互穿插。 */
    kernel_serial_printf_serial_only(
        "OS_THREAD_WAIT_CANCEL target_pid=%llx target_tid=%llx "
        "caller_pid=%llx caller_tid=%llx status=%llx\r\n",
        (unsigned long long)(target != 0 && target->process != 0 ? target->process->pid : 0U),
        (unsigned long long)(target != 0 ? target->tid : 0U),
        (unsigned long long)(caller != 0 && caller->process != 0 ? caller->process->pid : 0U),
        (unsigned long long)(caller != 0 ? caller->tid : 0U),
        (unsigned long long)(uint64_t)status);
}
#endif

static kstatus_t thread_create_user_frame_internal(
    process_t *process, const arch_trap_frame_t *initial_frame,
    vaddr_t fs_base, bool start, thread_t **out) {
    if (process == 0 || initial_frame == 0 || out == 0 ||
        !x86_validate_user_frame(initial_frame) ||
        !x86_is_canonical(fs_base) || fs_base > X86_64_USER_TOP) {
        return K_EINVAL;
    }
    if (atomic_load_explicit(&process->state, memory_order_acquire) !=
        PROCESS_RUNNING) {
        return K_EBUSY;
    }

    thread_t *thread = (thread_t *)kzalloc(sizeof(thread_t), 0);
    g_process_thread_create_stage = 1U;
    if (thread == 0) return K_ENOMEM;
    process_initialize_object(&thread->object, KOBJECT_TYPE_THREAD,
                              &g_thread_ops);
    thread->tid = process_allocate_task_id();
    thread->process = process;
    object_get(process);
    if (!x86_fp_state_create(&thread->arch)) {
        object_put(process);
        kfree(thread);
        return K_ENOMEM;
    }
    thread->flags = 0U;
    thread->kernel_stack_size = THREAD_KERNEL_STACK_SIZE;
    thread->kernel_stack_base = vmalloc(thread->kernel_stack_size);
    g_process_thread_create_stage = 2U;
    if (thread->kernel_stack_base == 0) {
        x86_fp_state_destroy(&thread->arch);
        object_put(process);
        kfree(thread);
        return K_ENOMEM;
    }
    thread->kernel_stack_top = (vaddr_t)(uintptr_t)thread->kernel_stack_base +
                               thread->kernel_stack_size;
    thread->arch.fs_base = fs_base;
    uint32_t current_cpu = x86_current_cpu_index();
    sched_initialize_new_thread(
        thread, current_cpu,
        (process->flags & PROCESS_FLAG_INIT_CPU_PINNED) != 0U);
    list_init(&thread->process_node);
    list_init(&thread->owned_mutexes);
    thread->pi_blocked_on = 0;

    uintptr_t frame_address = (thread->kernel_stack_top -
                               sizeof(arch_trap_frame_t)) & ~(uintptr_t)15U;
    arch_trap_frame_t *frame = (arch_trap_frame_t *)frame_address;
    *frame = *initial_frame;

    uintptr_t switch_stack = frame_address - sizeof(uint64_t);
    *(uint64_t *)switch_stack = (uint64_t)(uintptr_t)&x86_user_thread_start;
    thread->arch.switch_ctx.rsp = switch_stack;
    thread->arch.switch_ctx.r12 = frame_address;
    thread->arch.switch_ctx.r13 = fs_base;
    thread->arch.switch_ctx.r14 = thread->kernel_stack_top;
    process_thread_lock(process);
    if (atomic_load_explicit(&process->state, memory_order_acquire) !=
        PROCESS_RUNNING) {
        process_thread_unlock(process);
        vfree(thread->kernel_stack_base);
        x86_fp_state_destroy(&thread->arch);
        object_put(process);
        kfree(thread);
        return K_EBUSY;
    }
    /* 线程加入列表后，远端终止者就可能回收这个 NEW 线程。必须在同一加锁事务中先发布其执行所有权。 */
    object_get(thread);
    __atomic_fetch_or(&thread->flags, THREAD_FLAG_EXECUTION_REF,
                      __ATOMIC_RELEASE);
    /* 列表变化后，枚举缓存必须在释放线程锁前失效。 */
    process->enumeration_thread_cursor = 0;
    list_add_tail(&process->threads, &thread->process_node);
    if (process->thread_count == 0U) {
        process->linux_thread_group_leader_tid = thread->tid;
    }
    ++process->thread_count;
    process->flags |= PROCESS_FLAG_EVER_HAD_THREAD;
    process_thread_unlock(process);

    if (start) {
        kstatus_t status = sched_start_thread(thread);
        if (status != K_OK) {
            /* 远端启动请求达到上限时，不得在进程中留下不可访问的 NEW 线程却仍报告 clone 成功。创建者
             * 引用必须保持有效，直到线程终止并摘除。 */
            (void)thread_terminate(thread, status);
            object_put(thread);
            return status;
        }
    }
    *out = thread;
    debug_trace_stage(OS_DEBUG_PHASE_USER_RUNTIME,
                             OS_DEBUG_STEP_CREATE,
                             (uint32_t)thread->tid);
    g_process_thread_create_stage = 3U;
    return K_OK;
}

static kstatus_t thread_create_user_internal(process_t *process, vaddr_t entry,
                                             vaddr_t stack_top, vaddr_t fs_base,
                                             uint64_t argument, bool start,
                                             thread_t **out) {
    if (!x86_is_canonical(entry) || !x86_is_canonical(stack_top) ||
        entry < 0x10000ULL || entry > X86_64_USER_TOP ||
        stack_top < 0x10000ULL || stack_top > X86_64_USER_TOP ||
        (stack_top & 7U) != 0) {
        return K_EINVAL;
    }
    arch_trap_frame_t frame = {0};
    frame.rip = entry;
    frame.cs = USER_CODE_SELECTOR;
    frame.rflags = USER_INITIAL_RFLAGS;
    frame.rsp = stack_top;
    frame.ss = USER_DATA_SELECTOR;
    frame.rdi = argument;
    return thread_create_user_frame_internal(process, &frame, fs_base, start,
                                              out);
}

kstatus_t thread_create_user(process_t *process, vaddr_t entry,
                             vaddr_t stack_top, vaddr_t fs_base,
                             thread_t **out) {
    return thread_create_user_internal(process, entry, stack_top, fs_base,
                                       0, true, out);
}

kstatus_t thread_create_user_suspended(process_t *process, vaddr_t entry,
                                       vaddr_t stack_top, vaddr_t fs_base,
                                       uint64_t argument, thread_t **out) {
    return thread_create_user_internal(process, entry, stack_top, fs_base,
                                       argument, false, out);
}

kstatus_t thread_create_user_from_frame(process_t *process,
                                         const arch_trap_frame_t *frame,
                                         vaddr_t fs_base, thread_t **out) {
    return thread_create_user_frame_internal(process, frame, fs_base, false,
                                              out);
}

kstatus_t thread_register_user_stack(thread_t *thread, vaddr_t base,
                                     size_t size) {
    if (thread == 0 || thread->process == 0 ||
        !x86_user_range_valid((const void __user *)(uintptr_t)base, size)) {
        return K_EINVAL;
    }
    process_t *process = thread->process;
    process_thread_lock(process);
    vm_space_t *space = process->vm;
    if (space != 0) vm_space_get(space);
    process_thread_unlock(process);
    if (space == 0) return K_EINVAL;
    bool mapped = vm_range_is_mapped(space, base, size);
    kstatus_t status = K_EINVAL;
    if (mapped) {
        process_thread_lock(process);
        uint32_t flags = __atomic_load_n(&thread->flags, __ATOMIC_ACQUIRE);
        /* 校验可能与线程终止或映像替换并发。只有尚未回收的挂起线程才能接管栈所有权。 */
        if (atomic_load_explicit(&process->state, memory_order_acquire) != PROCESS_RUNNING ||
            atomic_load_explicit(&thread->state, memory_order_acquire) != THREAD_NEW ||
            (flags & THREAD_FLAG_EXECUTION_REF) == 0U ||
            (flags & THREAD_FLAG_EXECUTION_REAPING) != 0U ||
            thread->user_stack_owned || process->vm != space) {
            status = K_EBUSY;
        } else {
            thread->user_stack_base = base;
            thread->user_stack_size = size;
            thread->user_stack_owned = true;
            status = K_OK;
        }
        process_thread_unlock(process);
    }
    vm_space_put(space);
    return status;
}

kstatus_t thread_start(thread_t *thread) {
    if (thread == 0) return K_EINVAL;
    return sched_start_thread(thread);
}

uint32_t process_last_thread_create_stage(void) {
    return g_process_thread_create_stage;
}

kstatus_t thread_terminate(thread_t *thread, int64_t status) {
    if (thread == 0) return K_EINVAL;
    for (;;) {
        unsigned state = atomic_load_explicit(&thread->state,
                                              memory_order_acquire);
        if (state == THREAD_DEAD) return K_OK;
        if (state == THREAD_READY && sched_wait_cleanup_pending(thread)) {
            /*
             * 唤醒已使等待者变为可运行状态，但其栈上订阅仍会保持链接，直到它恢复等待路径。回收内核栈
             * 前必须让它完成清理。
             */
            return K_EBUSY;
        }
        if (state == THREAD_RUNNING) return K_EBUSY;
        if (state == THREAD_BLOCKED) {
            if (!wait_cancel_thread(thread)) return K_EBUSY;
#if OS_DEBUG_SERIAL
            thread_cancel_diag(thread, status);
#endif
            continue;
        }
        if (process_publish_thread_exit(thread, status)) break;
        unsigned observed = atomic_load_explicit(&thread->state,
                                                  memory_order_acquire);
        if (observed == THREAD_DEAD) return K_OK;
        /* 首次状态检查后仍可能发生 READY -> BLOCKED 竞争。 */
        if (observed == THREAD_BLOCKED) continue;
        return K_EBUSY;
    }
    sched_remove(thread);
    process_detach_thread(thread);
    /* WAIT_ONE 必须排在 exec/拆除使用的进程列表摘除之后。 */
    object_notify_signaled(thread);
    thread_release_execution_ref(thread);
    return K_OK;
}

static void thread_destroy(void *object) {
    thread_t *thread = (thread_t *)object;
    sched_remove(thread);
    process_t *process = thread->process;
    process_detach_thread(thread);
    vfree(thread->kernel_stack_base);
    x86_fp_state_destroy(&thread->arch);
    if (process != 0) object_put(process);
    kfree(thread);
}
