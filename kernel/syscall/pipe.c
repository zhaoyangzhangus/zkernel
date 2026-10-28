#include <arch/x86_64/uaccess.h>
#include <kernel/mm.h>
#include <kernel/pipe.h>
#include <kernel/process.h>
#include <uapi/pipe.h>

#include "internal.h"

int64_t syscall_pipe_create(uint64_t arguments_pointer, uint64_t unused1,
                            uint64_t unused2, uint64_t unused3,
                            uint64_t unused4, uint64_t unused5) {
    os_pipe_create_t arguments;
    pipe_endpoint_t *reader = 0;
    pipe_endpoint_t *writer = 0;
    process_t *process = current_process();
    handle_t read_handle = 0;
    handle_t write_handle = 0;
    kstatus_t status;

    (void)unused1;
    (void)unused2;
    (void)unused3;
    (void)unused4;
    (void)unused5;
    if (process == 0) return K_EPERM;
    status = copy_from_user(&arguments,
                            (const void __user *)(uintptr_t)arguments_pointer,
                            sizeof(arguments));
    if (status != K_OK) return status;
    if (!versioned_header_valid(&arguments.hdr, sizeof(arguments)) ||
        arguments.reserved != 0U ||
        (arguments.flags & ~OS_PIPE_FLAG_MASK) != 0U) return K_EINVAL;

    status = pipe_create(arguments.flags, &reader, &writer);
    if (status != K_OK) return status;
    uint32_t handle_flags = (arguments.flags & OS_PIPE_FLAG_CLOEXEC) != 0U ?
                            HANDLE_FLAG_CLOEXEC : 0U;
    status = handle_create_with_flags(&process->handles, reader,
                                      PIPE_RIGHT_READ | PIPE_RIGHT_WAIT,
                                      handle_flags, &read_handle);
    if (status == K_OK) {
        status = handle_create_with_flags(&process->handles, writer,
                                          PIPE_RIGHT_WRITE | PIPE_RIGHT_WAIT,
                                          handle_flags, &write_handle);
    }
    if (status == K_OK) {
        arguments.read_handle = read_handle;
        arguments.write_handle = write_handle;
        status = copy_to_user((void __user *)(uintptr_t)arguments_pointer,
                              &arguments, sizeof(arguments));
    }
    if (status != K_OK) {
        if (write_handle != 0) (void)handle_close(&process->handles, write_handle);
        if (read_handle != 0) (void)handle_close(&process->handles, read_handle);
    }
    object_put(reader);
    object_put(writer);
    return status;
}

static int64_t syscall_pipe_transfer(bool write, uint64_t handle,
                                     uint64_t buffer, uint64_t length,
                                     uint64_t timeout_ns, uint64_t output_bytes) {
    process_t *process = current_process();
    pipe_endpoint_t *endpoint = 0;
    /* 小型控制/输出记录无需经过分配器往返。将暂存区保留在系统调用栈上，
     * 使休眠中的 I/O 不会共享或覆盖该暂存区。 */
    uint8_t inline_buffer[256];
    uint8_t *temporary = 0;
    uint64_t bytes = 0U;
    size_t transfer_length;
    kstatus_t status;

    if (process == 0 || length > (uint64_t)SIZE_MAX) return K_EINVAL;
    status = handle_lookup(&process->handles, (handle_t)handle,
                           write ? PIPE_RIGHT_WRITE : PIPE_RIGHT_READ,
                           (void **)&endpoint);
    if (status != K_OK) return status;
    if (!pipe_is_endpoint(endpoint) ||
        (write ? pipe_endpoint_is_read(endpoint) :
                 !pipe_endpoint_is_read(endpoint))) {
        object_put(endpoint);
        return K_EBADF;
    }
    transfer_length = (size_t)length;
    if (transfer_length > OS_PIPE_DEFAULT_SIZE) {
        transfer_length = OS_PIPE_DEFAULT_SIZE;
    }
    if (transfer_length != 0U) {
        temporary = transfer_length <= sizeof(inline_buffer) ? inline_buffer :
                    (uint8_t *)kmalloc(transfer_length, 0);
        if (temporary == 0) {
            object_put(endpoint);
            return K_ENOMEM;
        }
        if (write) {
            status = copy_from_user(temporary,
                                    (const void __user *)(uintptr_t)buffer,
                                    transfer_length);
            if (status != K_OK) {
                if (temporary != inline_buffer) kfree(temporary);
                object_put(endpoint);
                return status;
            }
        }
    }
    if (write) {
        status = pipe_write(endpoint, temporary, transfer_length, timeout_ns,
                            &bytes);
    } else {
        status = pipe_read(endpoint, temporary, transfer_length, timeout_ns,
                           &bytes);
    }
    if (status == K_OK && !write && bytes != 0U) {
        status = copy_to_user((void __user *)(uintptr_t)buffer, temporary,
                              (size_t)bytes);
    }
    if (status == K_OK) {
        status = copy_to_user((void __user *)(uintptr_t)output_bytes, &bytes,
                              sizeof(bytes));
    }
    if (temporary != inline_buffer) kfree(temporary);
    object_put(endpoint);
    return status;
}

int64_t syscall_pipe_read(uint64_t handle, uint64_t buffer, uint64_t length,
                          uint64_t timeout_ns, uint64_t output_bytes,
                          uint64_t unused5) {
    (void)unused5;
    return syscall_pipe_transfer(false, handle, buffer, length, timeout_ns,
                                 output_bytes);
}

int64_t syscall_pipe_write(uint64_t handle, uint64_t buffer, uint64_t length,
                           uint64_t timeout_ns, uint64_t output_bytes,
                           uint64_t unused5) {
    (void)unused5;
    return syscall_pipe_transfer(true, handle, buffer, length, timeout_ns,
                                 output_bytes);
}
