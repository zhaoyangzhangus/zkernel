/* REFACTOR_SYSCALL_HANDLE_OWNER：用户句柄关闭策略。 */

#include <arch/x86_64/uaccess.h>
#include <kernel/process.h>
#include <uapi/abi.h>

#include "internal.h"

int64_t syscall_handle_close(uint64_t handle, uint64_t unused1, uint64_t unused2,
                                uint64_t unused3, uint64_t unused4, uint64_t unused5) {
    (void)unused1;
    (void)unused2;
    (void)unused3;
    (void)unused4;
    (void)unused5;
    process_t *process = current_process();
    int64_t status = process != 0 ? handle_close(&process->handles, (handle_t)handle) : K_EPERM;
    return status;
}

int64_t syscall_handle_dup(uint64_t handle, uint64_t flags,
                           uint64_t output_pointer, uint64_t unused3,
                           uint64_t unused4, uint64_t unused5) {
    (void)unused3;
    (void)unused4;
    (void)unused5;
    process_t *process = current_process();
    handle_t duplicate = OS_INVALID_HANDLE;
    kstatus_t status;

    if (process == 0 || output_pointer == 0U ||
        (flags & ~(uint64_t)OS_HANDLE_FLAG_CLOEXEC) != 0U) return K_EINVAL;
    status = handle_duplicate(&process->handles, (handle_t)handle,
                              (uint32_t)flags, &duplicate);
    if (status == K_OK) {
        status = copy_to_user((void __user *)(uintptr_t)output_pointer,
                              &duplicate, sizeof(duplicate));
    }
    if (status != K_OK && duplicate != OS_INVALID_HANDLE) {
        (void)handle_close(&process->handles, duplicate);
    }
    return status;
}

int64_t syscall_handle_get_flags(uint64_t handle, uint64_t unused1,
                                 uint64_t unused2, uint64_t unused3,
                                 uint64_t unused4, uint64_t unused5) {
    (void)unused1;
    (void)unused2;
    (void)unused3;
    (void)unused4;
    (void)unused5;
    process_t *process = current_process();
    uint32_t flags = 0U;
    kstatus_t status;
    if (process == 0) return K_EPERM;
    status = handle_get_flags(&process->handles, (handle_t)handle, &flags);
    return status == K_OK ? (int64_t)flags : status;
}

int64_t syscall_handle_set_flags(uint64_t handle, uint64_t flags,
                                 uint64_t unused2, uint64_t unused3,
                                 uint64_t unused4, uint64_t unused5) {
    (void)unused2;
    (void)unused3;
    (void)unused4;
    (void)unused5;
    process_t *process = current_process();
    if (process == 0) return K_EPERM;
    if ((flags & ~(uint64_t)OS_HANDLE_FLAG_CLOEXEC) != 0U) return K_EINVAL;
    return handle_set_flags(&process->handles, (handle_t)handle,
                            (uint32_t)flags);
}







/* GPU_CREATE_CTX(args, output_handle)：当前后端允许软件 GPU 上下文。 */
