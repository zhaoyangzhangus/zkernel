/* REFACTOR_SYSCALL_VM_OWNER：用户虚拟内存系统调用策略。 */

#include <arch/x86_64/uaccess.h>
#include <kernel/console.h>
#include <kernel/mm.h>
#include <kernel/process.h>
#include <kernel/shared_section.h>
#include <kernel/vfs.h>
#include <kernel/vm.h>
#include <uapi/file.h>
#include <uapi/memory.h>
#include <uapi/cpu_usage.h>
#include <kernel/sched.h>
#include <uapi/mm.h>
#include <uapi/syscall.h>

#include "internal.h"
#include "../process/internal.h"

int64_t syscall_memory_info(uint64_t output_pointer, uint64_t unused1,
                            uint64_t unused2, uint64_t unused3,
                            uint64_t unused4, uint64_t unused5) {
    os_memory_info_t info = {0};
    kstatus_t status;
    (void)unused1;
    (void)unused2;
    (void)unused3;
    (void)unused4;
    (void)unused5;
    if (current_process() == 0) return K_EPERM;
    if (output_pointer == 0U) return K_EINVAL;
    status = page_memory_get_stats(&info.total_pages, &info.free_pages);
    if (status != K_OK) return status;
    info.hdr.size = sizeof(info);
    info.hdr.version = OS_SYSCALL_ABI_VERSION;
    info.page_size = PAGE_SIZE;
    return copy_to_user((void __user *)(uintptr_t)output_pointer,
                        &info, sizeof(info));
}

int64_t syscall_cpu_get_usage(uint64_t pointer,uint64_t a,uint64_t b,uint64_t c,uint64_t d,uint64_t e) {
    (void)a;(void)b;(void)c;(void)d;(void)e;
    if (!current_process()) return K_EPERM;
    if (!pointer) return K_EINVAL;
    os_cpu_usage_t info;
    kstatus_t status=sched_usage_query(&info);
    return status==K_OK ? copy_to_user((void __user *)(uintptr_t)pointer,&info,sizeof(info)) : status;
}

uint32_t translate_vm_protection(uint32_t protection) {
    uint32_t result = VM_PROT_USER;
    if ((protection & OS_VM_READ) != 0) result |= VM_PROT_READ;
    if ((protection & OS_VM_WRITE) != 0) result |= VM_PROT_WRITE;
    if ((protection & OS_VM_EXEC) != 0) result |= VM_PROT_EXEC;
    return result;
}

static uint32_t translate_vm_flags(uint32_t flags) {
    uint32_t result = 0;
    if ((flags & OS_VM_PRIVATE) != 0) result |= VM_MAP_PRIVATE;
    if ((flags & OS_VM_SHARED) != 0) result |= VM_MAP_SHARED;
    if ((flags & OS_VM_FIXED) != 0) result |= VM_MAP_FIXED;
    if ((flags & OS_VM_STACK) != 0) result |= VM_MAP_STACK;
    return result;
}

/* VM_MAP(args) 在同一版本化结构的 address 字段中写回最终地址。 */
int64_t syscall_vm_map(uint64_t arguments_pointer, uint64_t unused1,
                          uint64_t unused2, uint64_t unused3,
                          uint64_t unused4, uint64_t unused5) {
    (void)unused1;
    (void)unused2;
    (void)unused3;
    (void)unused4;
    (void)unused5;
    process_t *process = current_process();
    if (process == 0) return K_EPERM;
    os_vm_map_args_t arguments;
    kstatus_t status = copy_from_user(&arguments,
        (const void __user *)(uintptr_t)arguments_pointer, sizeof(arguments));
    if (status != K_OK) return status;
    const uint32_t valid_protection = OS_VM_READ | OS_VM_WRITE | OS_VM_EXEC;
    const uint32_t valid_flags = OS_VM_PRIVATE | OS_VM_SHARED | OS_VM_FIXED | OS_VM_STACK;
    if (!versioned_header_valid(&arguments.hdr, sizeof(arguments)) ||
        (arguments.prot & ~valid_protection) != 0 ||
        (arguments.flags & ~valid_flags) != 0 ||
        ((arguments.flags & OS_VM_PRIVATE) != 0 &&
         (arguments.flags & OS_VM_SHARED) != 0) ||
        ((arguments.flags & OS_VM_STACK) != 0 &&
         (arguments.flags & OS_VM_SHARED) != 0) ||
        (arguments.length & (PAGE_SIZE - 1ULL)) != 0 ||
        (arguments.offset & (PAGE_SIZE - 1ULL)) != 0) return K_EINVAL;

    vm_object_t *mapped_object = 0;
    uint64_t mapped_offset = 0;
    void *file_object = 0;
    shared_section_t *shared_section = 0;
    if (arguments.object != OS_INVALID_HANDLE) {
        uint32_t rights = FILE_RIGHT_READ;
        if (handle_lookup(&process->handles, (handle_t)arguments.object,
                          SHARED_SECTION_RIGHT_MAP, (void **)&shared_section) == K_OK &&
            shared_section->object.type == KOBJECT_TYPE_SHARED_SECTION) {
            if ((arguments.flags & OS_VM_SHARED) == 0 ||
                arguments.offset > shared_section->size ||
                arguments.length > shared_section->size - arguments.offset) {
                object_put(shared_section);
                return K_EINVAL;
            }
            mapped_object = shared_section->vm_object;
            mapped_offset = arguments.offset;
            vm_object_get(mapped_object);
            object_put(shared_section);
            shared_section = 0;
            goto map_object;
        }
        if (shared_section != 0) {
            object_put(shared_section);
            shared_section = 0;
        }
        /* 私有写映射只读取文件页；只有共享写映射才要求文件写权限。 */
        if ((arguments.prot & OS_VM_WRITE) != 0 &&
            (arguments.flags & OS_VM_SHARED) != 0) rights |= FILE_RIGHT_WRITE;
        status = handle_lookup(&process->handles, (handle_t)arguments.object,
                               rights, &file_object);
        if (status != K_OK) return status;
        file_t *file = (file_t *)file_object;
        if (file->object.type != KOBJECT_TYPE_FILE || file->vnode == 0 ||
            arguments.offset >= file->vnode->size) {
            object_put(file_object);
            return K_EINVAL;
        }
        status = vm_object_create_file(file->vnode, vfs_vm_file_ops(),
                                       file->vnode->size, arguments.offset,
                                       (size_t)arguments.length, &mapped_object);
        object_put(file_object);
        if (status != K_OK) return status;
    } else if (arguments.offset != 0U) {
        return K_EINVAL;
    }

map_object:
    vm_space_t *space = process_vm_reference(process);
    vaddr_t address = (vaddr_t)arguments.address;
    status = vm_map_object(space, mapped_object, &address, mapped_offset,
                           (size_t)arguments.length,
                           translate_vm_protection(arguments.prot),
                           translate_vm_flags(arguments.flags));
    if (mapped_object != 0) vm_object_put(mapped_object);
    if (status != K_OK) {
        vm_space_put(space);
        return status;
    }
    arguments.address = (uint64_t)address;
    status = copy_to_user((void __user *)(uintptr_t)arguments_pointer,
                          &arguments, sizeof(arguments));
    if (status != K_OK) {
        /* 用户态看不到返回地址时，映射也不能留在进程中。 */
        (void)vm_unmap(space, address, (size_t)arguments.length);
    }
    vm_space_put(space);
    return status;
}

/* VM_SHARE 创建由句柄持有的匿名共享段，实际页按缺页时分配。 */
int64_t syscall_vm_share(uint64_t arguments_pointer, uint64_t unused1,
                            uint64_t unused2, uint64_t unused3,
                            uint64_t unused4, uint64_t unused5) {
    (void)unused1;
    (void)unused2;
    (void)unused3;
    (void)unused4;
    (void)unused5;
    process_t *process = current_process();
    if (process == 0) return K_EPERM;
    os_vm_share_args_t arguments;
    kstatus_t status = copy_from_user(&arguments,
        (const void __user *)(uintptr_t)arguments_pointer, sizeof(arguments));
    if (status != K_OK) return status;
    if (!versioned_header_valid(&arguments.hdr, sizeof(arguments)) ||
        arguments.flags != 0U || arguments.reserved != 0U ||
        arguments.size == 0U || arguments.size > (uint64_t)SIZE_MAX ||
        (arguments.size & (PAGE_SIZE - 1ULL)) != 0U) return K_EINVAL;

    shared_section_t *section = 0;
    status = shared_section_create(arguments.size, &section);
    if (status != K_OK) return status;
    handle_t handle = OS_INVALID_HANDLE;
    status = handle_create(&process->handles, section,
                           SHARED_SECTION_RIGHT_ALL, &handle);
    if (status == K_OK) {
        arguments.section = handle;
        status = copy_to_user((void __user *)(uintptr_t)arguments_pointer,
                              &arguments, sizeof(arguments));
    }
    if (status != K_OK && handle != OS_INVALID_HANDLE) {
        (void)handle_close(&process->handles, handle);
    }
    object_put(section);
    return status;
}

int64_t syscall_vm_unmap(uint64_t address, uint64_t length, uint64_t unused2,
                            uint64_t unused3, uint64_t unused4, uint64_t unused5) {
    (void)unused2;
    (void)unused3;
    (void)unused4;
    (void)unused5;
    process_t *process = current_process();
    if (process == 0) return K_EPERM;
    vm_space_t *space = process_vm_reference(process);
    kstatus_t status = vm_unmap(space, (vaddr_t)address, (size_t)length);
    vm_space_put(space);
    return status;
}

int64_t syscall_vm_protect(uint64_t address, uint64_t length, uint64_t protection,
                              uint64_t unused3, uint64_t unused4, uint64_t unused5) {
    (void)unused3;
    (void)unused4;
    (void)unused5;
    const uint32_t valid = OS_VM_READ | OS_VM_WRITE | OS_VM_EXEC;
    if ((protection & ~valid) != 0) return K_EINVAL;
    process_t *process = current_process();
    vm_space_t *space = process != 0 ? process_vm_reference(process) : 0;
    kstatus_t status = process != 0 ?
        vm_protect(space, (vaddr_t)address, (size_t)length,
                   translate_vm_protection((uint32_t)protection)) : K_EPERM;
    vm_space_put(space);
    if (status != K_OK) {
        kernel_serial_write("OS_DIAG_VM_PROTECT_FAIL STATUS=");
        kernel_serial_write_u32((uint32_t)(status < 0 ? -status : status));
        kernel_serial_write(" ADDRESS_LO=");
        kernel_serial_write_u32((uint32_t)address);
        kernel_serial_write(" ADDRESS_HI=");
        kernel_serial_write_u32((uint32_t)(address >> 32));
        kernel_serial_write(" LENGTH=");
        kernel_serial_write_u32((uint32_t)length);
        kernel_serial_write(" PROTECTION=");
        kernel_serial_write_u32((uint32_t)protection);
        kernel_serial_write("\r\n");
    }
    return status;
}

int64_t syscall_vm_sync(uint64_t address, uint64_t length, uint64_t flags,
                        uint64_t unused3, uint64_t unused4,
                        uint64_t unused5) {
    (void)unused3;
    (void)unused4;
    (void)unused5;
    process_t *process = current_process();
    if (process == 0) return K_EPERM;
    if (length > (uint64_t)SIZE_MAX || flags > (uint64_t)UINT32_MAX) {
        return K_EINVAL;
    }
    vm_space_t *space = process_vm_reference(process);
    kstatus_t status = vm_sync(space, (vaddr_t)address, (size_t)length,
                              (uint32_t)flags);
    vm_space_put(space);
    return status;
}

int64_t syscall_vm_advise(uint64_t address, uint64_t length, uint64_t advice,
                          uint64_t unused3, uint64_t unused4,
                          uint64_t unused5) {
    (void)unused3;
    (void)unused4;
    (void)unused5;
    process_t *process = current_process();
    if (process == 0) return K_EPERM;
    if (length > (uint64_t)SIZE_MAX || advice > (uint64_t)UINT32_MAX) {
        return K_EINVAL;
    }
    vm_space_t *space = process_vm_reference(process);
    kstatus_t status = vm_advise(space, (vaddr_t)address, (size_t)length,
                                (uint32_t)advice);
    vm_space_put(space);
    return status;
}
