/* 第 11 阶段 Linux personality 真实三角形探测运行程序。 */
#include <arch/x86_64/cpu.h>
#include <kernel/console.h>
#include <kernel/elf_loader.h>
#include <kernel/kmem.h>
#include <kernel/linux_abi.h>
#include <kernel/process.h>
#include <kernel/realtest.h>
#include <kernel/sched.h>
#include <kernel/vfs.h>
#include <kernel/vm.h>

#ifndef OS_PHASE11
#define OS_PHASE11 0
#endif

#define PHASE11_IMAGE_LIMIT (64ULL * 1024ULL * 1024ULL)
#define PHASE11_TIMEOUT_NS 300000000000ULL

static kstatus_t phase11_load_path(const char *path, process_t *process,
                                   user_elf_image_info_t *info,
                                   vaddr_t *stack_pointer)
{
    file_t *file = 0;
    kstatus_t status = vfs_open_kernel(path, VFS_OPEN_READ, 0U, &file);
    if (status != K_OK) return status;
    if (file->vnode == 0 || file->vnode->size == 0U ||
        file->vnode->size > PHASE11_IMAGE_LIMIT) {
        vfs_close(file);
        return K_EINVAL;
    }
    size_t image_size = (size_t)file->vnode->size;
    uint8_t *image = (uint8_t *)kmalloc(image_size, 0U);
    if (image == 0) {
        vfs_close(file);
        return K_ENOMEM;
    }
    uint64_t bytes = 0U;
    status = vfs_read_kernel(file, image, image_size, &bytes);
    vfs_close(file);
    if (status == K_OK && bytes != image_size) status = K_EIO;
    if (status == K_OK)
        status = process_load_elf_image(process, image, image_size,
                                        info, stack_pointer);
    kfree(image);
    return status;
}

static bool phase11_wait_for_process(process_t *process, thread_t *thread)
{
    vm_space_t *runtime_space = process != 0 ? process->vm : 0;
    if (runtime_space == 0) return false;
    vm_space_get(runtime_space);
    uint64_t deadline = x86_read_tsc();
    uint64_t budget = x86_timeout_ns_to_tsc(PHASE11_TIMEOUT_NS);
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

bool linux_phase11_realtest(void)
{
#if !OS_PHASE11
    return false;
#else
    process_t *process = 0;
    thread_t *thread = 0;
    user_elf_image_info_t info;
    vaddr_t stack_pointer = 0U;
    bool success = false;

    if (process_create(0, &process) != K_OK ||
        phase11_load_path("/tests/p11-triangle", process, &info,
                          &stack_pointer) != K_OK) {
        goto cleanup;
    }
    process_set_name(process, "/tests/p11-triangle");
    if (!info.linux_abi ||
        thread_create_user_suspended(process, info.entry, stack_pointer, 0,
                                     0, &thread) != K_OK)
        goto cleanup;
    if (thread_start(thread) != K_OK)
        goto cleanup;

    success = phase11_wait_for_process(process, thread);
    if (success) {
        kernel_serial_write_serial_only("OS_P11_PROCESS_CLEANUP_PASS\r\n");
    } else {
        kernel_serial_write_serial_only("OS_P11_PROCESS_CLEANUP_FAIL\r\n");
    }

cleanup:
    if (thread != 0) {
        if (atomic_load_explicit(&thread->state, memory_order_acquire) != THREAD_DEAD)
            (void)thread_terminate(thread, K_ECANCELED);
        object_put(thread);
    }
    if (process != 0) {
        if (thread == 0) (void)process_abort(process);
        object_put(process);
    }
    return success;
#endif
}
