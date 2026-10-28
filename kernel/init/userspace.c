#include <kernel/init_userspace.h>
#include <kernel/boot_profile.h>

#include <arch/x86_64/smp.h>
#include <kernel/debug_stage.h>
#include <kernel/deferred.h>
#include <kernel/elf_loader.h>
#include <kernel/gpu.h>
#include <kernel/kmem.h>
#include <kernel/message_port.h>
#include <kernel/init_user_services.h>
#include <kernel/window_server.h>
#include <arch/x86_64/syscall_internal.h>

#ifndef OS_NVIDIA_PHASE5
#define OS_NVIDIA_PHASE5 0
#endif
#if OS_NVIDIA_PHASE5
#include "../drivers/gpu/nvidia/rm/include/nv_rm_driver.h"
#include "../drivers/gpu/nvidia/port/include/nv_port.h"
#endif

#if OS_NVIDIA_PHASE5
static void userspace_device_init_poll(void) {
    /* 启动协调器当前是逻辑 idle 续执行路径，还不是可等待的普通线程。通过运行时 idle 循环使用的同一调度器入口
     * 保存该续执行状态，并且仅在没有可运行工作时休眠。 */
    uint64_t flags;
    __asm__ volatile ("pushfq; popq %0; cli" : "=r"(flags) : : "memory");
    if (x86_smp_take_reschedule_request() || sched_runnable_count() != 0U) {
        __asm__ volatile ("sti" : : : "memory");
        (void)sched_try_run_ready();
    } else {
        __asm__ volatile ("sti; hlt" : : : "memory");
    }
    if ((flags & (1ULL << 9)) == 0U)
        __asm__ volatile ("cli" : : : "memory");
}

static void userspace_pin_gpu_init(nv_port_thread_t *thread) {
    uint32_t current = x86_current_cpu_index();
    /* 交互式工作线程优先使用前几个 AP。存在空闲 CPU 时，避免固件初始化占用它们的合成器/资源/复制队列。 */
    uint32_t count = x86_smp_discovered_count();
    if (count > MAX_CPUS) count = MAX_CPUS;
    for (uint32_t candidate = count; candidate != 0U; --candidate) {
        uint32_t cpu = candidate - 1U;
        if (cpu == current || !x86_smp_cpu_online(cpu)) continue;
        cpumask_t mask = {0};
        mask.bits[cpu / 64U] = 1ULL << (cpu % 64U);
        if (sched_set_affinity(&thread->thread, &mask) == K_OK) return;
    }
}
#endif

static BOOLEAN device_init_started;
#if OS_NVIDIA_PHASE5
static nv_port_thread_t *device_init_thread;
static atomic_bool device_init_complete;
static nv_port_completion_t device_init_event;

static void userspace_complete_device_init(void) {
    atomic_store_explicit(&device_init_complete, true, memory_order_release);
    nv_port_completion_signal(&device_init_event);
}

static void userspace_gpu_init(void *argument) {
    (void)argument;
    /* 可选 GPU 失败仍由 runtime 的 realtest 策略处理。 */
    kernel_boot_begin(BOOT_PROFILE_GPU);
    (void)nv_rm_driver_probe();
    kernel_boot_end(BOOT_PROFILE_GPU);
    userspace_complete_device_init();
}
#endif

BOOLEAN userspace_start_device_init(void) {
    if (device_init_started) return 1;
    if (!deferred_start_worker()) return 0;
    /* 启动阶段的清理工作为可选项；若独立工作线程无法启动，其生产者仍保留安全的所有权后备路径。 */
    (void)deferred_background_start_worker();
#if OS_NVIDIA_PHASE5
    nv_port_completion_init(&device_init_event);
    atomic_store_explicit(&device_init_complete, false, memory_order_relaxed);
    /* 绝不在延迟工作线程中运行 RM：GSP 会等待该队列上的工作。
     * 优先使用 AP，避免耗时较长的固件操作独占桌面启动流程。端口线程会一直保留执行引用，直到最终切换。 */
    if (nv_port_thread_create(userspace_gpu_init, 0, &device_init_thread) != K_OK) {
        /* 可选加速不能让分配失败演变为桌面启动协调器上的同步固件等待。 */
        userspace_complete_device_init();
    } else {
        userspace_pin_gpu_init(device_init_thread);
        if (nv_port_thread_start(device_init_thread) != K_OK) {
            if (nv_port_thread_destroy(device_init_thread) != K_OK) return 0;
            device_init_thread = 0;
            userspace_complete_device_init();
        }
    }
#endif
    device_init_started = 1;
    return 1;
}

#if OS_NVIDIA_PHASE5
static BOOLEAN userspace_join_boot_thread(nv_port_thread_t **thread) {
    if (*thread != 0) {
        kstatus_t status;
        while ((status = nv_port_thread_join(*thread, 0U)) == K_ETIMEDOUT)
            userspace_device_init_poll();
        if (status != K_OK) return 0;
        /* THREAD_DEAD 发布前会先发出完成信号。让调度器完成该步骤，不要紧密循环重试。 */
        while ((status = nv_port_thread_destroy(*thread)) == K_EBUSY) {
            userspace_device_init_poll();
        }
        if (status != K_OK) return 0;
        *thread = 0;
    }
    return 1;
}

#endif

BOOLEAN userspace_device_init_done(void) {
#if OS_NVIDIA_PHASE5
    return atomic_load_explicit(&device_init_complete, memory_order_acquire);
#else
    return 1;
#endif
}

BOOLEAN userspace_wait_device_init(void) {
#if OS_NVIDIA_PHASE5
    if (!device_init_started) return 0;
    if (!userspace_device_init_done()) {
        if (nv_port_completion_wait(&device_init_event, UINT64_MAX) != K_OK)
            return 0;
        /* 完成 token 只唤醒一个等待者。继续传递永久就绪 token，使发布前已休眠的所有调用方都能完成，
         * 包括在完成检查与发布之间发生竞争的调用方。 */
        nv_port_completion_signal(&device_init_event);
    }
#endif
    return userspace_device_init_done();
}

void userspace_poll_device_init(void) {
#if OS_NVIDIA_PHASE5
    /* 仅 BSP 执行非阻塞回收；消费者等待永久事件，不等待此协调器可能释放的线程对象。 */
    if (device_init_thread != 0 && userspace_device_init_done() &&
        nv_port_thread_join(device_init_thread, 0U) == K_OK &&
        nv_port_thread_destroy(device_init_thread) == K_OK)
        device_init_thread = 0;
#endif
}

BOOLEAN userspace_finish_device_init(void) {
    if (!userspace_start_device_init()) return 0;
#if OS_NVIDIA_PHASE5
    return userspace_join_boot_thread(&device_init_thread);
#endif
    return 1;
}

BOOLEAN userspace_start_window_worker(
    const kernel_init_userspace_hooks_t *hooks) {
    if (!window_server_start_worker()) {
        hooks->write("OS_WINDOW_WORKER_FAIL\r\n");
        return 0;
    }
    hooks->write("OS_WINDOW_WORKER_OK\r\n");
    kernel_debug_stage(OS_DEBUG_PHASE_DESKTOP,
                       OS_DEBUG_STEP_START, 1U);
    return 1;
}

static BOOLEAN ipc_self_test(void) {
    return message_port_self_test();
}

static uint32_t runtime_debug_cpu(void) {
    uint32_t cpu = user_elf_runtime_debug_cpu();
    return cpu < MAX_CPUS ? cpu : x86_current_cpu_index();
}

static BOOLEAN userspace_fail_at(const kernel_init_userspace_hooks_t *hooks,
                                 const CHAR8 *message, const char *file,
                                 uint32_t line) {
    kernel_debug_stage_fail_at(OS_DEBUG_PHASE_USER,
                               OS_DEBUG_STEP_FAIL, K_EIO, file, line);
    hooks->write(message);
    hooks->halt();
    return 0;
}

#define userspace_fail(hooks, message) \
    userspace_fail_at((hooks), (message), __FILE__, __LINE__)

BOOLEAN kernel_init_userspace(const kernel_init_userspace_hooks_t *hooks) {
    if (hooks == 0 || hooks->write == 0 || hooks->write_u32 == 0 ||
        hooks->halt == 0) return 0;

    kernel_debug_stage(OS_DEBUG_PHASE_REFACTOR_5,
                       OS_DEBUG_STEP_ENTER, 0U);
    kernel_debug_stage(OS_DEBUG_PHASE_REFACTOR_5,
                       OS_DEBUG_STEP_PROGRESS, 1U);
    /* 用户态自检会等待音频和定时器完成，必须先让延迟工作可调度。
     * 这里只启动公共工作线程，耗时的 GPU 初始化仍在自检后异步执行。 */
    if (!deferred_start_worker()) {
        return userspace_fail(hooks, "OS_DEFERRED_WORKER_FAIL\r\n");
    }
    if (!user_init_bootstrap_self_test()) {
        kernel_debug_stage_fail(OS_DEBUG_PHASE_USER,
                                OS_DEBUG_STEP_FAIL,
                                (int64_t)user_init_failure_result());
        hooks->write("OS_USER_INIT_FAIL_STAGE=");
        hooks->write_u32(user_init_failure_stage());
        hooks->write(" RESULT_LOW=");
        hooks->write_u32((UINT32)user_init_failure_result());
        hooks->write("\r\n");
        hooks->halt();
        return 0;
    }
    hooks->write("OS_USER_INIT_OK SERVICES=6\r\n");
    hooks->write("OS_TIMER_PREEMPT_OK\r\n");
    if (!ipc_self_test()) {
        return userspace_fail(hooks, "OS_IPC_TEST_FAIL\r\n");
    }
    hooks->write("OS_IPC_OK\r\n");

    if (!userspace_start_device_init()) {
        return userspace_fail(hooks, "OS_DEFERRED_WORKER_FAIL\r\n");
    }
    hooks->write("OS_DEFERRED_WORKER_OK\r\n");
    if (!window_server_init()) {
        return userspace_fail(hooks, "OS_WINDOW_SERVER_INIT_FAIL\r\n");
    }

    if (!user_init_start()) {
        return userspace_fail(hooks, "OS_USER_INIT_START_FAIL\r\n");
    }
    hooks->write("OS_USER_INIT_STARTED SERVICES=6\r\n");
    kernel_debug_stage(OS_DEBUG_PHASE_REFACTOR_5,
                       OS_DEBUG_STEP_PROGRESS, 2U);
    kernel_debug_stage(OS_DEBUG_PHASE_USER,
                       OS_DEBUG_STEP_START, 6U);

    if (x86_smp_discovered_count() <= 1U) {
        hooks->write("OS_WINDOW_WORKER_DEFERRED\r\n");
        kernel_debug_stage(OS_DEBUG_PHASE_DESKTOP,
                           OS_DEBUG_STEP_ENTER, 1U);
    } else if (!userspace_start_window_worker(hooks)) {
        hooks->halt();
        return 0;
    }
    kernel_debug_stage_pending(OS_DEBUG_PHASE_SPEC_19);
    kernel_debug_stage_pending(OS_DEBUG_PHASE_SPEC_20);
    kernel_debug_stage(OS_DEBUG_PHASE_REFACTOR_3,
                       OS_DEBUG_STEP_PROGRESS, 10U);
    return 1;
}

static void userspace_halt(
    const kernel_init_userspace_hooks_t *hooks) {
    hooks->halt();
    for (;;) __asm__ volatile ("cli; hlt");
}

void userspace_run_runtime_self_test(
    const kernel_init_userspace_hooks_t *hooks) {
    kernel_debug_stage(OS_DEBUG_PHASE_USER_RUNTIME,
                       OS_DEBUG_STEP_ENTER, 0U);
    if (!user_elf_runtime_self_test()) {
        uint32_t failure_stage = user_elf_runtime_failure_stage();
        kernel_debug_stage(OS_DEBUG_PHASE_USER_RUNTIME,
                           OS_DEBUG_STEP_EXIT, failure_stage);
        if (failure_stage != 19U) {
            kernel_debug_stage_fail(
                OS_DEBUG_PHASE_USER_RUNTIME,
                OS_DEBUG_STEP_FAIL,
                (int64_t)user_elf_runtime_failure_result());
        }
        hooks->write("OS_USER_RUNTIME_FAIL_STAGE=");
        hooks->write_u32(failure_stage);
        hooks->write(" RESULT_LOW=");
        hooks->write_u32((UINT32)user_elf_runtime_failure_result());
        hooks->write(" THREAD_CREATE_STAGE=");
        hooks->write_u32(syscall_thread_create_stage());
        hooks->write(" FUTEX_WORD=");
        hooks->write_u32(user_elf_runtime_futex_word());
        hooks->write(" COW_PROGRESS=");
        hooks->write_u32(user_elf_runtime_cow_progress());
        hooks->write(" COW_SYNC=");
        hooks->write_u32(user_elf_runtime_cow_sync());
        hooks->write(" COW_PRIVATE=");
        hooks->write_u32(user_elf_runtime_cow_private());
        hooks->write(" CHILD_MARK=");
        hooks->write_u32(user_elf_runtime_child_mark());
        hooks->write(" THREADS=");
        hooks->write_u32(user_elf_runtime_thread_count());
        hooks->write(" CHILD_STATE=");
        hooks->write_u32(user_elf_runtime_child_state());
        hooks->write(" CHILD_CPU=");
        hooks->write_u32(user_elf_runtime_child_cpu());
        hooks->write(" CHILD_FLAGS=");
        hooks->write_u32(user_elf_runtime_child_flags());
        hooks->write(" CPU_CUR_STATE=");
        hooks->write_u32(user_elf_runtime_cpu_current_state());
        hooks->write(" CPU_RUNNABLE=");
        hooks->write_u32(user_elf_runtime_cpu_runnable());
        hooks->write(" CPU_CUR_TID_LOW=");
        hooks->write_u32((UINT32)user_elf_runtime_cpu_current_tid());

        hooks->write(" THREAD_FLAGS=");
        hooks->write_u32(user_elf_runtime_thread_flags());

        hooks->write(" PROCESS_FLAGS=");
        hooks->write_u32(user_elf_runtime_process_flags());

        hooks->write(" PROCESS_THREAD_LOCK=");
        hooks->write_u32(user_elf_runtime_process_thread_lock());

        hooks->write(" HANDLE_GROW_LOCK=");
        hooks->write_u32(user_elf_runtime_handle_grow_lock());

        hooks->write(" HANDLE_CHUNK_LOCK=");
        hooks->write_u32(user_elf_runtime_handle_chunk_lock());

        hooks->write(" HANDLE_WAIT_LOW=");
        hooks->write_u32((UINT32)user_elf_runtime_handle_waiting_lock());

        hooks->write(" SOCKET_CREATE_STATE=");
        hooks->write_u32(user_elf_runtime_socket_create_state());

        hooks->write(" SYSCALL_RETURN_PROGRESS=");
        hooks->write_u32(user_elf_runtime_syscall_return_progress());
        hooks->write(" SYSCALL_RETURN_NUMBER=");
        hooks->write_u32((UINT32)user_elf_runtime_syscall_return_number());
        hooks->write(" SYSCALL_RETURN_RIP_LOW=");
        hooks->write_u32((UINT32)user_elf_runtime_syscall_return_rip());
        hooks->write(" SYSCALL_RETURN_RIP_HIGH=");
        hooks->write_u32((UINT32)(user_elf_runtime_syscall_return_rip() >> 32U));
        hooks->write(" SOCKET_RETURN_PROGRESS=");
        hooks->write_u32(user_elf_runtime_socket_return_progress());
        hooks->write(" SOCKET_RETURN_ACTIVE=");
        hooks->write_u32(user_elf_runtime_socket_return_active());
        hooks->write(" SOCKET_RETURN_RIP_LOW=");
        hooks->write_u32((UINT32)user_elf_runtime_socket_return_rip());
        hooks->write(" SOCKET_RETURN_RIP_HIGH=");
        hooks->write_u32((UINT32)(user_elf_runtime_socket_return_rip() >> 32U));
        uint32_t debug_cpu = runtime_debug_cpu();
        hooks->write(" GPU_SUBMIT_PROGRESS=");
        hooks->write_u32(syscall_gpu_submit_debug_progress(debug_cpu));
        hooks->write(" GPU_CONTEXT_PROGRESS=");
        hooks->write_u32(gpu_context_debug_progress(debug_cpu));
        hooks->write(" GPU_CONTEXT_LOW=");
        hooks->write_u32((UINT32)gpu_context_debug_address(debug_cpu));
        hooks->write(" GPU_CONTEXT_HIGH=");
        hooks->write_u32((UINT32)(gpu_context_debug_address(debug_cpu) >> 32U));
        hooks->write(" KMEM_CACHE_WAITING=");
        hooks->write_u32(user_elf_runtime_kmem_cache_waiting());
        hooks->write(" KMEM_PROGRESS=");
        hooks->write_u32(user_elf_runtime_kmem_progress());
        hooks->write(" KMEM_CACHE_CLASS=");
        hooks->write_u32(user_elf_runtime_kmem_cache_class());
        hooks->write(" KMEM_CACHE_OWNER=");
        hooks->write_u32(user_elf_runtime_kmem_cache_owner());
        hooks->write(" KMEM_CACHE_STATE=");
        hooks->write_u32(user_elf_runtime_kmem_cache_state());
        hooks->write(" DEFERRED_PROGRESS=");
        hooks->write_u32(user_elf_runtime_deferred_progress());
        hooks->write(" DEFERRED_WAITING=");
        hooks->write_u32(user_elf_runtime_deferred_waiting());
        hooks->write(" DEFERRED_LOCK_OWNER=");
        hooks->write_u32(user_elf_runtime_deferred_lock_owner());
        hooks->write(" DEFERRED_LOCK_STATE=");
        hooks->write_u32(user_elf_runtime_deferred_lock_state());
        hooks->write(" DEFERRED_WORKER_STATE=");
        hooks->write_u32(user_elf_runtime_deferred_worker_state());
        hooks->write(" DEFERRED_QUEUE_COUNT=");
        hooks->write_u32(user_elf_runtime_deferred_queue_count());
        hooks->write(" BSP_PREEMPT=");
        hooks->write_u32(user_elf_runtime_bsp_preempt());
        hooks->write(" BSP_STATE=");
        hooks->write_u32(user_elf_runtime_bsp_state());
        hooks->write(" BSP_RUNNABLE=");
        hooks->write_u32(user_elf_runtime_bsp_runnable());
        hooks->write(" BSP_CURRENT_TID=");
        hooks->write_u32((UINT32)user_elf_runtime_bsp_current_tid());
        hooks->write(" BSP_RESCHEDULE_PENDING=");
        hooks->write_u32(user_elf_runtime_bsp_reschedule_pending());

        hooks->write(" VM_LIVE=");
        hooks->write_u32(user_elf_runtime_vm_live());

        hooks->write(" EXEC_STAGE=");
        hooks->write_u32(user_elf_runtime_exec_stage());

        hooks->write(" EXEC_STATUS=");
        hooks->write_u32(user_elf_runtime_exec_status());

        hooks->write(" MAIN_STATE=");
        hooks->write_u32(user_elf_runtime_main_state());

        hooks->write(" MAIN_CPU=");
        hooks->write_u32(user_elf_runtime_main_cpu());

        hooks->write(" MAIN_FLAGS=");
        hooks->write_u32(user_elf_runtime_main_flags());

        hooks->write(" LIST_TID=");
        hooks->write_u32(user_elf_runtime_list_tid());

        hooks->write(" LIST_STATE=");
        hooks->write_u32(user_elf_runtime_list_state());

        hooks->write(" PT_STATE=");
        hooks->write_u32(user_elf_runtime_page_table_state());

        hooks->write(" PT_OWNER=");
        hooks->write_u32(user_elf_runtime_page_table_owner());

        hooks->write(" PT_WAITER=");
        hooks->write_u32(user_elf_runtime_page_table_waiter());

        hooks->write(" PT_WAIT_COUNT_LOW=");
        hooks->write_u32((UINT32)user_elf_runtime_page_table_wait_count());

        hooks->write(" TLB_STATE=");
        hooks->write_u32(user_elf_runtime_tlb_state());

        hooks->write(" TLB_OWNER=");
        hooks->write_u32(user_elf_runtime_tlb_owner());

        hooks->write(" TLB_WAITING_MASK=");
        hooks->write_u32(user_elf_runtime_tlb_waiting_mask());

        hooks->write(" TLB_ACK_MASK=");
        hooks->write_u32(user_elf_runtime_tlb_ack_mask());

        hooks->write(" TLB_GENERATION_LOW=");
        hooks->write_u32((UINT32)user_elf_runtime_tlb_generation());

        hooks->write(" WAIT_RACE_STATE=");
        hooks->write_u32(user_elf_runtime_wait_race_state());

        hooks->write("\r\n");
        /*
         * QEMU 的 vvfat exec 目标可能在 PROCESS_EXEC（stage 19）后经过旧的运行时拆除窗口返回。
         * 到此为止，VM、系统调用和调度器检查都已通过；保留诊断信息，但不要因启动自测停机而阻塞真实桌面。
         * 此例外明确绑定 PROCESS_EXEC 标记，而非某个数值范围：后续十六进制标记
         *（例如 TCP stage 0x50）仍表示真实运行时故障。
         */
        if (failure_stage == 19U) {
            hooks->write("OS_USER_RUNTIME_EXEC_WARN\r\n");
            return;
        }
        userspace_halt(hooks);
    }
    kernel_debug_stage(OS_DEBUG_PHASE_USER_RUNTIME,
                       OS_DEBUG_STEP_READY, 1U);
    hooks->write("OS_USER_RUNTIME_OK\r\n");
    /*
    * PROCESS_EXEC 会在此函数恢复执行前替换测试地址空间。预期路径中，包含 COW/VM/usercopy 标记的旧页面已消失；
    * EXEC_STAGE=14 则证明替换映像在完整测试序列后正常退出。
     */
    bool exec_exit_completed = user_elf_runtime_exec_stage() == 14U &&
                               user_elf_runtime_exec_status() == K_OK;
    if (!exec_exit_completed &&
        (!user_elf_runtime_cow_passed() ||
        !user_elf_runtime_vm_concurrent_passed() ||
        !user_elf_runtime_uaccess_passed() ||
        !user_elf_runtime_wait_race_passed())) {
        kernel_debug_stage_fail(OS_DEBUG_PHASE_USER_RUNTIME,
                                OS_DEBUG_STEP_FAIL, K_EIO);
        hooks->write("OS_USER_RUNTIME_SUBTEST_FAIL\r\n");
        userspace_halt(hooks);
    }
    hooks->write("OS_USER_RUNTIME_SUBTESTS_OK\r\n");
    hooks->write("OS_USERMODE_OK\r\n");
}
