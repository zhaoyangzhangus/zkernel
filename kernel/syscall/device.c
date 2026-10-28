/* REFACTOR_SYSCALL_DEVICE_OWNER：设备枚举和控制处理程序。 */

#include <arch/x86_64/uaccess.h>
#include <kernel/device.h>
#include <kernel/process.h>
#include <uapi/device.h>
#include <uapi/syscall.h>

#include "internal.h"

int64_t syscall_device_enumerate(uint64_t arguments_pointer, uint64_t unused1,
                                    uint64_t unused2, uint64_t unused3,
                                    uint64_t unused4, uint64_t unused5) {
    (void)unused1;
    (void)unused2;
    (void)unused3;
    (void)unused4;
    (void)unused5;
    os_device_enumerate_t arguments;
    os_device_info_t info = {0};
    device_t *device = 0;
    kstatus_t status = copy_from_user(&arguments,
        (const void __user *)(uintptr_t)arguments_pointer, sizeof(arguments));
    if (status != K_OK) return status;
    if (!versioned_header_valid(&arguments.hdr, sizeof(arguments)) ||
        arguments.reserved != 0U || arguments.output == 0U ||
        arguments.output_size < sizeof(info)) return K_EINVAL;
    status = device_get_by_index(arguments.index, &device);
    if (status != K_OK) return status;
    info.hdr.size = sizeof(info);
    info.hdr.version = OS_SYSCALL_ABI_VERSION;
    info.device_id = device->device_id;
    info.class_id = device->class_id;
    info.state = atomic_load_explicit(&device->state, memory_order_acquire);
    status = copy_to_user((void __user *)(uintptr_t)arguments.output,
                          &info, sizeof(info));
    object_put(device);
    if (status != K_OK) return status;
    arguments.bytes_returned = sizeof(info);
    return copy_to_user((void __user *)(uintptr_t)arguments_pointer,
                        &arguments, sizeof(arguments));
}

/* DEVICE_OPEN(args)：按稳定 device_id 查找设备并生成带权限的 capability。 */
int64_t syscall_device_open(uint64_t arguments_pointer, uint64_t unused1,
                               uint64_t unused2, uint64_t unused3,
                               uint64_t unused4, uint64_t unused5) {
    (void)unused1;
    (void)unused2;
    (void)unused3;
    (void)unused4;
    (void)unused5;
    process_t *process = current_process();
    if (process == 0) return K_EPERM;

    os_device_open_t arguments;
    kstatus_t status = copy_from_user(&arguments,
        (const void __user *)(uintptr_t)arguments_pointer, sizeof(arguments));
    if (status != K_OK) return status;
    if (!versioned_header_valid(&arguments.hdr, sizeof(arguments)) ||
        arguments.desired_rights == 0U ||
        (arguments.desired_rights & ~OS_DEVICE_RIGHT_ALL) != 0U ||
        arguments.reserved != 0U) return K_EINVAL;

    device_t *device = 0;
    status = device_get_by_id(arguments.device_id, &device);
    if (status != K_OK) return status;
    handle_t handle = 0;
    status = handle_create(&process->handles, device, arguments.desired_rights,
                           &handle);
    if (status == K_OK) {
        arguments.handle = handle;
        status = copy_to_user((void __user *)(uintptr_t)arguments_pointer,
                              &arguments, sizeof(arguments));
    }
    if (status != K_OK && handle != OS_INVALID_HANDLE) {
        (void)handle_close(&process->handles, handle);
    }
    object_put(device);
    return status;
}

/* DEVICE_CONTROL(handle, args)：当前先提供查询、复位和电源状态控制。 */
int64_t syscall_device_control(uint64_t handle, uint64_t arguments_pointer,
                                  uint64_t unused2, uint64_t unused3,
                                  uint64_t unused4, uint64_t unused5) {
    (void)unused2;
    (void)unused3;
    (void)unused4;
    (void)unused5;
    process_t *process = current_process();
    if (process == 0) return K_EPERM;

    os_device_control_t arguments;
    kstatus_t status = copy_from_user(&arguments,
        (const void __user *)(uintptr_t)arguments_pointer, sizeof(arguments));
    if (status != K_OK) return status;
    if (!versioned_header_valid(&arguments.hdr, sizeof(arguments)) ||
        arguments.flags != 0U || arguments.reserved != 0U) return K_EINVAL;
    arguments.bytes_returned = 0;

    uint32_t rights = OS_DEVICE_RIGHT_CONTROL;
    switch (arguments.code) {
    case OS_DEVICE_CONTROL_QUERY:
        rights |= OS_DEVICE_RIGHT_QUERY;
        if (arguments.output == 0U || arguments.output_size < sizeof(os_device_info_t)) {
            return K_EINVAL;
        }
        break;
    case OS_DEVICE_CONTROL_RESET:
        rights |= OS_DEVICE_RIGHT_RESET;
        break;
    case OS_DEVICE_CONTROL_SET_POWER:
        rights |= OS_DEVICE_RIGHT_POWER;
        if (arguments.level_or_state > OS_DEVICE_POWER_SUSPENDED) return K_EINVAL;
        break;
    default:
        return K_ENOSYS;
    }

    void *object = 0;
    status = handle_lookup(&process->handles, (handle_t)handle, rights, &object);
    if (status != K_OK) return status;
    device_t *device = (device_t *)object;
    if (device->object.type != KOBJECT_TYPE_DEVICE) {
        status = K_EINVAL;
    } else if (atomic_load_explicit(&device->state, memory_order_acquire) >=
               DEVICE_REMOVING) {
        status = K_EDEVREMOVED;
    } else if (arguments.code == OS_DEVICE_CONTROL_QUERY) {
        os_device_info_t info = {0};
        info.hdr.size = sizeof(info);
        info.hdr.version = OS_SYSCALL_ABI_VERSION;
        info.device_id = device->device_id;
        info.class_id = device->class_id;
        info.state = atomic_load_explicit(&device->state, memory_order_acquire);
        status = copy_to_user((void __user *)(uintptr_t)arguments.output,
                              &info, sizeof(info));
        if (status == K_OK) arguments.bytes_returned = sizeof(info);
    } else if (arguments.code == OS_DEVICE_CONTROL_RESET) {
        status = device_reset(device, arguments.level_or_state);
    } else if (arguments.level_or_state == OS_DEVICE_POWER_ACTIVE) {
        status = device_resume(device);
    } else {
        status = device_suspend(device);
    }
    object_put(object);
    if (status != K_OK) return status;
    status = copy_to_user((void __user *)(uintptr_t)arguments_pointer,
                          &arguments, sizeof(arguments));
    return status;
}

/* PORT_CREATE(kind, capacity, output_handle)：创建消息端口或完成端口。 */
