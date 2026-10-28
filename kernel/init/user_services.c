/* REFACTOR_P3_USER_SERVICES_OWNER：启动时发布用户服务。 */
#include <arch/x86_64/cpu.h>
#include <arch/x86_64/paging.h>
#include <kernel/device.h>
#include <kernel/elf_loader.h>
#include <kernel/kmem.h>
#include <kernel/debug_stage.h>
#include <kernel/process.h>
#include <kernel/sched.h>
#include <kernel/vfs.h>
#include <uapi/device.h>
#include <uapi/audio.h>
#include <uapi/process.h>
#include <uapi/syscall.h>

#define INIT_IMAGE_SIZE       0x3000U
#define INIT_DATA_ADDRESS     0x00401000ULL
#define INIT_SERVICE_COUNT     6U
#define INIT_DEVICE_ID         0x4C4954454F530001ULL
#define INIT_DEVICE_CLASS      0x0600U
#define INIT_RUNTIME_RESTART_LIMIT 3U

#define INIT_BOOTSTRAP_EXIT_TIMEOUT_NS 15000000000ULL

static uint32_t g_user_init_stage;
static int64_t g_user_init_result;
static device_t g_user_init_device;
static bool g_user_init_device_registered;
static process_t *g_runtime_init_process;
static thread_t *g_runtime_init_thread;
static bool g_runtime_init_started;
static uint32_t g_runtime_init_restart_count;

static kstatus_t user_init_device_reset(device_t *device, uint32_t level) {
    return device != 0 && level < 3U ? K_OK : K_EINVAL;
}

static const device_ops_t g_user_init_device_ops = {
    .reset = user_init_device_reset,
};

static bool init_register_device(void) {
    device_object_init(&g_user_init_device, INIT_DEVICE_ID, INIT_DEVICE_CLASS,
                       &g_user_init_device_ops, 0);
    if (device_register(&g_user_init_device) != K_OK) return false;
    g_user_init_device_registered = true;
    return true;
}


static kstatus_t init_load_path(const char *path, process_t *process,
                                user_elf_image_info_t *info,
                                vaddr_t *stack_pointer) {
    void *image = 0;
    size_t image_size = 0U;
    /* 文件打开、边界检查、读取与回滚由 VFS 统一处理；此处只记录整体读取阶段。 */
    kernel_debug_stage(OS_DEBUG_PHASE_USER, OS_DEBUG_STEP_LOAD, 0x1AU);
    kstatus_t status = vfs_load_file_kernel(path, 1U, INIT_IMAGE_SIZE,
                                            &image, &image_size);
    kernel_debug_stage(OS_DEBUG_PHASE_USER, OS_DEBUG_STEP_LOAD, 0x1DU);
    if (status == K_OK) {
        status = process_load_elf_image(process, image, image_size,
                                        info, stack_pointer);
        if (status == K_OK) process_set_name(process, path);
    }
    kfree(image);
    return status;
}

static bool init_wait_for_exit(process_t *process, thread_t *thread,
                               vm_space_t *runtime_space) {
    paddr_t marker_physical;
    uint8_t stage = 0;
    uint32_t service_count = 0;
    if (x86_translate_page(runtime_space->root_table, INIT_DATA_ADDRESS,
                           &marker_physical, 0) == K_OK) {
        uint8_t *marker_page = (uint8_t *)phys_to_direct(marker_physical);
        if (marker_page != 0) {
            stage = marker_page[0];
            service_count = *(const uint32_t *)(const void *)(marker_page + 0x10U);
            g_user_init_result = *(const int64_t *)(const void *)(marker_page + 0x08U);
        }
    }
    g_user_init_stage = stage;

    uint64_t deadline = x86_read_tsc();
    uint64_t budget =
        x86_timeout_ns_to_tsc(INIT_BOOTSTRAP_EXIT_TIMEOUT_NS);
    deadline = budget > UINT64_MAX - deadline ? UINT64_MAX : deadline + budget;
    for (;;) {
        __asm__ volatile ("sti" : : : "memory");
        schedule();
        sched_finish_switch();
        if (atomic_load_explicit(&thread->state, memory_order_acquire) == THREAD_DEAD &&
            atomic_load_explicit(&process->state, memory_order_acquire) == PROCESS_DEAD) {
            break;
        }
        if ((int64_t)(x86_read_tsc() - deadline) >= 0) break;
        __asm__ volatile ("pause");
    }

    /* 用户线程可能在关中断状态下退出，给调度器几次机会完成 execution ref 回收。 */
    __asm__ volatile ("sti" : : : "memory");
    for (uint32_t attempt = 0; attempt < 4U && process->vm != 0; ++attempt) {
        sched_finish_switch();
        if (process->vm == 0) break;
        schedule();
    }
    /* 若退出发生在 syscall 的关中断窗口，调度器会延后 reaper；这里安全地补做一次幂等释放。 */
    thread_release_execution_ref(thread);

    if (x86_translate_page(runtime_space->root_table, INIT_DATA_ADDRESS,
                           &marker_physical, 0) == K_OK) {
        uint8_t *marker_page = (uint8_t *)phys_to_direct(marker_physical);
        if (marker_page != 0) {
            stage = marker_page[0];
            service_count = *(const uint32_t *)(const void *)(marker_page + 0x10U);
            g_user_init_result = *(const int64_t *)(const void *)(marker_page + 0x08U);
        }
    }
    g_user_init_stage = stage;
    if (g_user_init_result == 0 && thread->exit_code != 0) {
        g_user_init_result = thread->exit_code;
    }
    bool thread_dead = atomic_load_explicit(&thread->state, memory_order_acquire) == THREAD_DEAD;
    bool process_dead = atomic_load_explicit(&process->state, memory_order_acquire) == PROCESS_DEAD;
    bool success = thread_dead && thread->exit_code == 0 && process->thread_count == 0 &&
                   process_dead && process->exit_code == 0 && process->vm == 0 &&
                   stage == 0xA5U && service_count == INIT_SERVICE_COUNT &&
                   g_user_init_result == 0;
    if (!success && g_user_init_result == 0) {
        g_user_init_result = (int64_t)((thread_dead ? 0U : 1U) |
            (thread->exit_code == 0 ? 0U : 2U) |
            (process->thread_count == 0 ? 0U : 4U) |
            (process_dead ? 0U : 8U) |
            (process->exit_code == 0 ? 0U : 16U) |
            (process->vm == 0 ? 0U : 32U) |
            (stage == 0xA5U ? 0U : 64U) |
            (service_count == INIT_SERVICE_COUNT ? 0U : 128U));
    }
    return success;
}

bool user_init_bootstrap_self_test(void) {
    process_t *process = 0;
    thread_t *thread = 0;
    vm_space_t *runtime_space = 0;
    user_elf_image_info_t info;
    vaddr_t stack_pointer = 0;
    bool success = false;
    g_user_init_stage = 0;
    g_user_init_result = 0;
    g_user_init_device_registered = false;
    if (!init_register_device() ||
        process_create(0, &process) != K_OK ||
        init_load_path("/EFI/OS/init", process, &info, &stack_pointer) != K_OK) {
        goto cleanup;
    }
    process->flags |= PROCESS_FLAG_INIT_CPU_PINNED;
    if (thread_create_user_suspended(process, info.entry, stack_pointer, 0, 0,
                                     &thread) != K_OK) {
        goto cleanup;
    }
    /* /init 是启动协调线程，先固定在引导 CPU，避免首个用户切换跨 CPU 丢失。 */
    /* /init 固定在引导 CPU，避免服务初始化期间跨 CPU 改变地址空间。 */
    if (thread_start(thread) != K_OK) goto cleanup;
    runtime_space = process->vm;
    vm_space_get(runtime_space);
    success = init_wait_for_exit(process, thread, runtime_space);

cleanup:
    if (runtime_space != 0) vm_space_put(runtime_space);
    if (thread != 0) {
        (void)thread_terminate(thread, K_ECANCELED);
        object_put(thread);
    }
    if (process != 0) {
        if (thread == 0) (void)process_abort(process);
        object_put(process);
    }
    if (g_user_init_device_registered) {
        device_unregister(&g_user_init_device);
        g_user_init_device_registered = false;
    }
    return success;
}

bool user_init_start(void) {
    process_t *process = 0;
    thread_t *thread = 0;
    user_elf_image_info_t info;
    vaddr_t stack_pointer = 0;

    if (g_runtime_init_started) {
        return g_runtime_init_process != 0 && g_runtime_init_thread != 0;
    }
    g_runtime_init_started = true;
    g_user_init_stage = 0;
    g_user_init_result = 0;
    if (!g_user_init_device_registered && !init_register_device()) goto runtime_start_fail;
    kernel_debug_stage(OS_DEBUG_PHASE_USER, OS_DEBUG_STEP_ENTER, 1U);
    if (process_create(0, &process) != K_OK ||
        init_load_path("/EFI/OS/init-runtime", process, &info, &stack_pointer) != K_OK) {
        goto runtime_start_fail;
    }
    kernel_debug_stage(OS_DEBUG_PHASE_USER, OS_DEBUG_STEP_LOAD, 2U);
    process->flags |= PROCESS_FLAG_INIT_CPU_PINNED;
    if (thread_create_user_suspended(process, info.entry, stack_pointer, 0, 0,
                                     &thread) != K_OK) {
        goto runtime_start_fail;
    }
    kernel_debug_stage(OS_DEBUG_PHASE_USER, OS_DEBUG_STEP_CREATE, 3U);
    if (thread_start(thread) != K_OK) {
        if (thread != 0) {
            (void)thread_terminate(thread, K_ECANCELED);
            object_put(thread);
        } else {
            (void)process_abort(process);
        }
        object_put(process);
        g_runtime_init_started = false;
        return false;
    }
    kernel_debug_stage(OS_DEBUG_PHASE_USER, OS_DEBUG_STEP_START, 4U);
    /* 静态引用维持 init 与其地址空间，直到系统关机或用户态自行退出。 */
    g_runtime_init_process = process;
    g_runtime_init_thread = thread;
    return true;

runtime_start_fail:
    if (process != 0) {
        if (thread != 0) {
            (void)thread_terminate(thread, K_ECANCELED);
            object_put(thread);
        } else {
            (void)process_abort(process);
        }
        object_put(process);
    }
    g_runtime_init_started = false;
    return false;
}

void user_init_poll(void) {
    process_t *process = g_runtime_init_process;
    thread_t *thread = g_runtime_init_thread;
    if (!g_runtime_init_started || process == 0 || thread == 0 ||
        atomic_load_explicit(&thread->state, memory_order_acquire) != THREAD_DEAD ||
        atomic_load_explicit(&process->state, memory_order_acquire) != PROCESS_DEAD) {
        return;
    }

    /* 先摘下静态引用，再释放对象，避免重启路径观察到半拆除状态。 */
    g_runtime_init_thread = 0;
    g_runtime_init_process = 0;
    g_runtime_init_started = false;
    object_put(thread);
    object_put(process);
    if (g_runtime_init_restart_count >= INIT_RUNTIME_RESTART_LIMIT) return;
    ++g_runtime_init_restart_count;
    (void)user_init_start();
}

uint32_t user_init_failure_stage(void) {
    return g_user_init_stage;
}

int64_t user_init_failure_result(void) {
    return g_user_init_result;
}
