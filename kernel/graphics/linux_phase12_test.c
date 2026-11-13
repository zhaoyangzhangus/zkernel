/* 第 12 阶段：验证 CPU 回读窗口桥接与进程清理。 */
#include <arch/x86_64/cpu.h>
#include <kernel/console.h>
#include <kernel/elf_loader.h>
#include <kernel/linux_abi.h>
#include <kernel/process.h>
#include <kernel/realtest.h>
#include <kernel/sched.h>
#include <kernel/vm.h>
#include <kernel/window_server.h>
#include <uapi/window.h>

#ifndef OS_PHASE12
#define OS_PHASE12 0
#endif

#define PHASE12_IMAGE_LIMIT (64ULL * 1024ULL * 1024ULL)
#define PHASE12_TIMEOUT_NS 300000000000ULL
#define PHASE12_HBUFFER_ADDRESS 0x07000000ULL
#define PHASE12_WINDOW_X 32
#define PHASE12_WINDOW_Y 32
#define PHASE12_WINDOW_WIDTH 320U
#define PHASE12_WINDOW_HEIGHT 240U

static bool phase12_wait_for_process(process_t *process, thread_t *thread) {
    vm_space_t *runtime_space = process != 0 ? process->vm : 0;
    if (runtime_space == 0) return false;
    vm_space_get(runtime_space);
    uint64_t deadline = x86_read_tsc();
    uint64_t budget = x86_timeout_ns_to_tsc(PHASE12_TIMEOUT_NS);
    deadline = budget > UINT64_MAX - deadline ? UINT64_MAX : deadline + budget;
    for (;;) {
        __asm__ volatile ("sti" : : : "memory");
        schedule();
        sched_finish_switch();
        bool thread_dead = atomic_load_explicit(&thread->state,
                                                memory_order_acquire) == THREAD_DEAD;
        bool process_dead = atomic_load_explicit(&process->state,
                                                 memory_order_acquire) == PROCESS_DEAD;
        if (thread_dead && process_dead) break;
        if ((int64_t)(x86_read_tsc() - deadline) >= 0) {
            (void)thread_terminate(thread, K_ETIMEDOUT);
            break;
        }
        __asm__ volatile ("pause");
    }
    __asm__ volatile ("sti" : : : "memory");
    for (uint32_t attempt = 0U; attempt < 8U && process->vm != 0; ++attempt) {
        sched_finish_switch();
        if (process->vm == 0) break;
        schedule();
    }
    thread_release_execution_ref(thread);
    bool success =
        atomic_load_explicit(&thread->state, memory_order_acquire) == THREAD_DEAD &&
        thread->exit_code == 0 && process->thread_count == 0U &&
        atomic_load_explicit(&process->state, memory_order_acquire) == PROCESS_DEAD &&
        process->exit_code == 0 && process->vm == 0;
    vm_space_put(runtime_space);
    return success;
}

bool linux_phase12_realtest(void) {
#if !OS_PHASE12
    return false;
#else
    process_t *process = 0;
    thread_t *thread = 0;
    window_server_window_t *window = 0;
    user_elf_image_info_t info;
    vaddr_t stack_pointer = 0U;
    uint64_t mapped_address = 0U;
    bool success = false;

    if (process_create(0, &process) != K_OK ||
        process_load_elf_from_vfs(process, "/tests/p12-window",
                                  PHASE12_IMAGE_LIMIT, &info, &stack_pointer) != K_OK) {
        goto cleanup;
    }
    process_set_name(process, "/tests/p12-window");
    if (!info.linux_abi ||
        window_server_create(process, PHASE12_WINDOW_X, PHASE12_WINDOW_Y,
                             PHASE12_WINDOW_WIDTH, PHASE12_WINDOW_HEIGHT,
                             OS_WINDOW_RESIZABLE | OS_WINDOW_CLIENT_DECORATIONS,
                             0x00121C2EU, "P12 OpenGL", &window) != K_OK ||
        window_server_map(window, process, PHASE12_HBUFFER_ADDRESS,
                          &mapped_address) != K_OK ||
        mapped_address != PHASE12_HBUFFER_ADDRESS ||
        window_server_set_owner_address(window, mapped_address) != K_OK) {
        goto cleanup;
    }
    if (thread_create_user_suspended(process, info.entry, stack_pointer, 0,
                                     0, &thread) != K_OK)
        goto cleanup;
    if (thread_start(thread) != K_OK) goto cleanup;

    success = phase12_wait_for_process(process, thread);
    if (success) {
        kernel_serial_write_serial_only("OS_P12_PROCESS_CLEANUP_PASS\r\n");
    } else {
        kernel_serial_write_serial_only("OS_P12_PROCESS_CLEANUP_FAIL\r\n");
    }

cleanup:
    if (thread != 0) {
        if (atomic_load_explicit(&thread->state, memory_order_acquire) != THREAD_DEAD)
            (void)thread_terminate(thread, K_ECANCELED);
        object_put(thread);
    }
    if (window != 0) {
        /* 进程退出负责清理未关闭窗口；正常路径会先通过关闭请求移除窗口。 */
        window_server_put(window);
    }
    if (process != 0) {
        if (thread == 0) (void)process_abort(process);
        object_put(process);
    }
    return success;
#endif
}
