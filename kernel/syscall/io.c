/* REFACTOR_SYSCALL_IO_OWNER：用户 I/O 请求编组和生命周期。 */

#include <arch/x86_64/uaccess.h>
#include <kernel/completion_port.h>
#include <kernel/device.h>
#include <kernel/io.h>
#include <kernel/kmem.h>
#include <kernel/process.h>
#include <uapi/device.h>
#include <uapi/io.h>
#include <uapi/ipc.h>

#include "internal.h"

#define USER_IO_MAX_VECTORS 64U
#define USER_IO_MAX_BYTES   (16ULL * 1024ULL * 1024ULL)

typedef struct user_io_context {
    uint32_t vector_count;
    uint32_t reserved;
    io_vec_t *kernel_vectors;
    void **buffers;
    os_io_vec_t user_vectors[];
} user_io_context_t;

_Static_assert(sizeof(os_io_vec_t) % _Alignof(io_vec_t) == 0,
               "captured vectors must align kernel vectors");
_Static_assert(sizeof(io_vec_t) % _Alignof(void *) == 0,
               "kernel vectors must align buffer pointers");

static void user_io_context_free(user_io_context_t *context) {
    if (context == 0) return;
    for (uint32_t i = 0; i < context->vector_count; ++i) {
        if (context->buffers[i] != 0) kfree(context->buffers[i]);
    }
    kfree(context);
}

static void user_io_context_release(io_request_t *request) {
    if (request == 0 || request->private_data == 0) return;
    user_io_context_t *context = (user_io_context_t *)request->private_data;
    request->private_data = 0;
    request->private_release = 0;
    user_io_context_free(context);
}

static void user_io_complete(io_request_t *request) {
    if (request == 0 || request->private_data == 0) return;
    user_io_context_t *context = (user_io_context_t *)request->private_data;
    if (request->opcode == IO_READ && request->status == K_OK) {
        uint64_t remaining = request->bytes_done;
        uint64_t copied = 0;
        for (uint32_t i = 0; i < context->vector_count && remaining != 0U; ++i) {
            uint64_t length = context->user_vectors[i].length;
            if (length > remaining) length = remaining;
            if (length != 0U && copy_to_user(
                    (void __user *)(uintptr_t)context->user_vectors[i].address,
                    context->buffers[i], (size_t)length) != K_OK) {
                request->status = K_EACCES;
                request->bytes_done = copied;
                break;
            }
            copied += length;
            remaining -= length;
        }
    }
    /* 取消操作发布完成后，提交过程和待处理 DMA 仍可能持有请求引用。
     * 暂存向量需随这些引用保持存活；最终销毁时由 private_release 释放。 */
}

int64_t syscall_io_submit(uint64_t arguments_pointer, uint64_t unused1,
                             uint64_t unused2, uint64_t unused3,
                             uint64_t unused4, uint64_t unused5) {
    (void)unused1;
    (void)unused2;
    (void)unused3;
    (void)unused4;
    (void)unused5;
    process_t *process = current_process();
    os_io_submit_t arguments;
    user_io_context_t *context = 0;
    io_request_t *request = 0;
    void *device_object = 0;
    void *port_object = 0;
    kstatus_t status;
    uint64_t total = 0;
    if (process == 0) return K_EPERM;

    status = copy_from_user(&arguments,
        (const void __user *)(uintptr_t)arguments_pointer, sizeof(arguments));
    if (status != K_OK) return status;
    if (!versioned_header_valid(&arguments.hdr, sizeof(arguments)) ||
        arguments.target == OS_INVALID_HANDLE ||
        arguments.completion_port == OS_INVALID_HANDLE ||
        arguments.vector_count > USER_IO_MAX_VECTORS ||
        arguments.opcode < OS_IO_READ || arguments.opcode > OS_IO_IOCTL ||
        (arguments.vector_count != 0U && arguments.vectors == 0U)) {
        return K_EINVAL;
    }
    if ((arguments.opcode == OS_IO_READ || arguments.opcode == OS_IO_WRITE) &&
        arguments.vector_count == 0U) return K_EINVAL;
    if (arguments.vector_count != 0U &&
        !x86_user_range_valid((const void __user *)(uintptr_t)arguments.vectors,
                              (size_t)arguments.vector_count * sizeof(os_io_vec_t))) {
        return K_EINVAL;
    }

    status = handle_lookup(&process->handles, (handle_t)arguments.target,
                           OS_DEVICE_RIGHT_CONTROL, &device_object);
    if (status != K_OK) return status;
    status = handle_lookup(&process->handles, (handle_t)arguments.completion_port,
                           COMPLETION_PORT_RIGHT_WRITE, &port_object);
    if (status != K_OK) goto cleanup;
    if (((device_t *)device_object)->object.type != KOBJECT_TYPE_DEVICE ||
        ((completion_port_t *)port_object)->object.type !=
            KOBJECT_TYPE_COMPLETION_PORT) {
        status = K_EINVAL;
        goto cleanup;
    }
    if (atomic_load_explicit(&((device_t *)device_object)->state,
                             memory_order_acquire) >= DEVICE_REMOVING) {
        status = K_EDEVREMOVED;
        goto cleanup;
    }

    /* vector_count was capped above: one bounded allocation contains all
     * three arrays, without reserving 64 entries for a small request. */
    size_t context_bytes = sizeof(*context) + (size_t)arguments.vector_count *
        (sizeof(os_io_vec_t) + sizeof(io_vec_t) + sizeof(void *));
    context = arguments.vector_count != 0U ?
        (user_io_context_t *)kmalloc(context_bytes, 0) : 0;
    if (arguments.vector_count != 0U && context == 0) {
        status = K_ENOMEM;
        goto cleanup;
    }
    if (context != 0) context->vector_count = arguments.vector_count;
    if (context != 0) {
        context->kernel_vectors = (io_vec_t *)(context->user_vectors +
                                             arguments.vector_count);
        context->buffers = (void **)(context->kernel_vectors +
                                    arguments.vector_count);
    }
    /* Only active slots are visible to submission and cleanup. Descriptors
     * are fully overwritten by copy_from_user; no inactive slots are allocated.
     * Initialize ownership before that copy can fail. */
    for (uint32_t i = 0; i < arguments.vector_count; ++i) {
        context->kernel_vectors[i] = (io_vec_t){0};
        context->buffers[i] = 0;
    }
    if (arguments.vector_count != 0U && copy_from_user(
            context->user_vectors,
            (const void __user *)(uintptr_t)arguments.vectors,
            (size_t)arguments.vector_count * sizeof(os_io_vec_t)) != K_OK) {
        status = K_EACCES;
        goto cleanup;
    }
    /* Validate the complete captured vector list before allocating payloads
     * or reading user data. A bad late vector must not consume a large
     * valid prefix's staging memory and copy work. */
    for (uint32_t i = 0; i < arguments.vector_count; ++i) {
        const os_io_vec_t *user_vector = &context->user_vectors[i];
        if (user_vector->length > USER_IO_MAX_BYTES ||
            user_vector->length > (uint64_t)SIZE_MAX ||
            (user_vector->length != 0U &&
             !x86_user_range_valid((const void __user *)(uintptr_t)user_vector->address,
                                   (size_t)user_vector->length)) ||
            total > USER_IO_MAX_BYTES - user_vector->length) {
            status = K_EINVAL;
            goto cleanup;
        }
        total += user_vector->length;
    }
    for (uint32_t i = 0; i < arguments.vector_count; ++i) {
        const os_io_vec_t *user_vector = &context->user_vectors[i];
        context->kernel_vectors[i].length = (size_t)user_vector->length;
        if (user_vector->length == 0U) continue;
        /* Writes fill every staged byte before submission; reads and
         * control requests retain blank buffers for short device output. */
        context->buffers[i] = arguments.opcode == OS_IO_WRITE ?
            kmalloc((size_t)user_vector->length, 0) :
            kzalloc((size_t)user_vector->length, 0);
        if (context->buffers[i] == 0) {
            status = K_ENOMEM;
            goto cleanup;
        }
        context->kernel_vectors[i].base = context->buffers[i];
        if (arguments.opcode == OS_IO_WRITE && copy_from_user(
                context->buffers[i],
                (const void __user *)(uintptr_t)user_vector->address,
                (size_t)user_vector->length) != K_OK) {
            status = K_EACCES;
            goto cleanup;
        }
    }
    if (total == 0U && arguments.opcode != OS_IO_FLUSH &&
        arguments.opcode != OS_IO_IOCTL) {
        status = K_EINVAL;
        goto cleanup;
    }

    /* No request object exists on staging failure. The initializer clears
     * it once and installs its first reference before cleanup can put it. */
    request = (io_request_t *)kmalloc(sizeof(*request), 0);
    if (request == 0) {
        status = K_ENOMEM;
        goto cleanup;
    }
    io_request_init(request, arguments.opcode, (device_t *)device_object,
                    process, context != 0 ? context->kernel_vectors : 0,
                    arguments.vector_count);
    request->offset = arguments.offset;
    request->internal_flags = IOREQ_INTERNAL_DYNAMIC | IOREQ_INTERNAL_PROCESS_REF;
    object_get(process);
    request->private_data = context;
    request->private_release = user_io_context_release;
    request->complete = user_io_complete;
    context = 0; /* the request owns the context after submission. */
    status = io_request_set_completion_port(
        request, (completion_port_t *)port_object, arguments.user_key);
    if (status != K_OK) goto cleanup;
    status = io_request_register_user(request);
    if (status != K_OK) goto cleanup;
    /* Registration owns a separate reference; retain ours until submit returns. */
    uint64_t request_id = request->request_id;
    status = io_submit(request);
    object_put(request);
    request = 0;
    if (status == K_OK) {
        object_put(port_object);
        object_put(device_object);
        return (int64_t)request_id;
    }

cleanup:
    if (request != 0) {
        if ((request->internal_flags & IOREQ_INTERNAL_USER_REQUEST) != 0U &&
            !io_request_is_terminal(request)) {
            /* Remove an unsubmitted request from the user registry. */
            (void)io_cancel(request);
        }
        object_put(request);
    }
    user_io_context_free(context);
    if (port_object != 0) object_put(port_object);
    if (device_object != 0) object_put(device_object);
    return status;
}

/* IO_CANCEL(request_id): cancel only a request owned by the current process. */
int64_t syscall_io_cancel(uint64_t request_id, uint64_t unused1,
                             uint64_t unused2, uint64_t unused3,
                             uint64_t unused4, uint64_t unused5) {
    (void)unused1;
    (void)unused2;
    (void)unused3;
    (void)unused4;
    (void)unused5;
    process_t *process = current_process();
    io_request_t *request = 0;
    if (process == 0) return K_EPERM;
    kstatus_t status = io_request_lookup_user(process, request_id, &request);
    if (status != K_OK) return status;
    status = io_cancel(request);
    object_put(request);
    return status;
}
