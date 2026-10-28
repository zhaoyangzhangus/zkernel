/* REFACTOR_SYSCALL_FILESYSTEM_OWNER：VFS 和文件系统调用处理程序。 */

#include <arch/x86_64/uaccess.h>
#include <kernel/process.h>
#include <kernel/vfs.h>
#include <uapi/file.h>

#include "internal.h"

int64_t syscall_file_open(uint64_t path, uint64_t flags, uint64_t mode,
                             uint64_t output_pointer, uint64_t unused4,
                             uint64_t unused5) {
    (void)unused4;
    (void)unused5;
    process_t *process = current_process();
    if (process == 0) return K_EPERM;
    if ((flags & ~((uint64_t)(OS_FILE_OPEN_READ |
                             OS_FILE_OPEN_WRITE |
                             OS_FILE_OPEN_CREATE |
                             OS_FILE_OPEN_EXCLUSIVE |
                             OS_FILE_OPEN_TRUNCATE |
                             OS_FILE_OPEN_APPEND |
                             OS_FILE_OPEN_DIRECTORY |
                             OS_FILE_OPEN_CLOEXEC))) != 0U) {
        return K_EINVAL;
    }
    file_t *file = 0;
    uint32_t open_flags = (uint32_t)flags & ~OS_FILE_OPEN_CLOEXEC;
    uint32_t handle_flags = (flags & OS_FILE_OPEN_CLOEXEC) != 0U ?
                            HANDLE_FLAG_CLOEXEC : 0U;
    kstatus_t status = vfs_open((const char __user *)(uintptr_t)path,
                                open_flags, (uint32_t)mode, &file);
    if (status != K_OK) return status;
    handle_t handle = 0;
    status = handle_create_with_flags(&process->handles, file, file->rights,
                                      handle_flags, &handle);
    if (status == K_OK) {
        status = copy_to_user((void __user *)(uintptr_t)output_pointer,
                              &handle, sizeof(handle));
    }
    if (status != K_OK && handle != 0) (void)handle_close(&process->handles, handle);
    object_put(file);
    return status;
}

int64_t syscall_file_enumerate(uint64_t arguments_pointer, uint64_t unused1,
                                  uint64_t unused2, uint64_t unused3,
                                  uint64_t unused4, uint64_t unused5) {
    (void)unused1;
    (void)unused2;
    (void)unused3;
    (void)unused4;
    (void)unused5;
    process_t *process = current_process();
    os_file_enumerate_t arguments;
    kstatus_t status;
    if (process == 0) return K_EPERM;
    status = copy_from_user(&arguments,
        (const void __user *)(uintptr_t)arguments_pointer, sizeof(arguments));
    if (status != K_OK) return status;
    if (!versioned_header_valid(&arguments.hdr, sizeof(arguments)) ||
        arguments.path == 0U) return K_EINVAL;
    status = vfs_enumerate((const char __user *)(uintptr_t)arguments.path,
                           arguments.index, &arguments.info);
    if (status != K_OK) return status;
    return copy_to_user((void __user *)(uintptr_t)arguments_pointer,
                        &arguments, sizeof(arguments));
}

int64_t syscall_file_enumerate_batch(uint64_t arguments_pointer, uint64_t unused1,
                                    uint64_t unused2, uint64_t unused3,
                                    uint64_t unused4, uint64_t unused5) {
    (void)unused1; (void)unused2; (void)unused3; (void)unused4; (void)unused5;
    os_file_enumerate_batch_t arguments;
    os_file_info_t entries[OS_FILE_ENUMERATE_BATCH_MAX];
    if (current_process() == 0) return K_EPERM;
    kstatus_t status = copy_from_user(&arguments,
        (const void __user *)(uintptr_t)arguments_pointer, sizeof(arguments));
    if (status != K_OK) return status;
    if (!versioned_header_valid(&arguments.hdr, sizeof(arguments)) ||
        arguments.path == 0U || arguments.entries == 0U || arguments.reserved != 0U ||
        arguments.capacity == 0U || arguments.capacity > OS_FILE_ENUMERATE_BATCH_MAX)
        return K_EINVAL;
    size_t bytes = (size_t)arguments.capacity * sizeof(*entries);
    if (!x86_user_range_valid((void __user *)(uintptr_t)arguments.entries, bytes))
        return K_EACCES;
    arguments.count = 0U;
    status = vfs_enumerate_batch((const char __user *)(uintptr_t)arguments.path,
        arguments.index, entries, arguments.capacity, &arguments.count);
    if (status != K_OK) return status;
    if (arguments.count == 0U || arguments.count > arguments.capacity) return K_EIO;
    status = copy_to_user((void __user *)(uintptr_t)arguments.entries, entries,
                          (size_t)arguments.count * sizeof(*entries));
    if (status != K_OK) return status;
    return copy_to_user((void __user *)(uintptr_t)arguments_pointer, &arguments, sizeof(arguments));
}

int64_t syscall_file_seek(uint64_t arguments_pointer, uint64_t unused1,
                             uint64_t unused2, uint64_t unused3,
                             uint64_t unused4, uint64_t unused5) {
    (void)unused1;
    (void)unused2;
    (void)unused3;
    (void)unused4;
    (void)unused5;
    os_file_seek_t arguments;
    void *object = 0;
    file_t *file;
    kstatus_t status;
    process_t *process = current_process();
    if (process == 0) return K_EPERM;
    status = copy_from_user(&arguments,
        (const void __user *)(uintptr_t)arguments_pointer, sizeof(arguments));
    if (status != K_OK) return status;
    if (!versioned_header_valid(&arguments.hdr, sizeof(arguments)) ||
        arguments.reserved != 0U || arguments.whence > OS_FILE_SEEK_END) {
        return K_EINVAL;
    }
    status = handle_lookup(&process->handles, (handle_t)arguments.handle, 0, &object);
    if (status != K_OK) return status;
    file = (file_t *)object;
    status = file->object.type == KOBJECT_TYPE_FILE ?
        vfs_seek(file, arguments.offset, arguments.whence, &arguments.position) : K_EBADF;
    object_put(object);
    if (status != K_OK) return status;
    return copy_to_user((void __user *)(uintptr_t)arguments_pointer,
                        &arguments, sizeof(arguments));
}

int64_t syscall_file_stat(uint64_t arguments_pointer, uint64_t unused1,
                             uint64_t unused2, uint64_t unused3,
                             uint64_t unused4, uint64_t unused5) {
    (void)unused1;
    (void)unused2;
    (void)unused3;
    (void)unused4;
    (void)unused5;
    os_file_stat_t arguments;
    kstatus_t status = copy_from_user(&arguments,
        (const void __user *)(uintptr_t)arguments_pointer, sizeof(arguments));
    if (status != K_OK) return status;
    if (!versioned_header_valid(&arguments.hdr, sizeof(arguments)) ||
        arguments.path == 0U) return K_EINVAL;
    status = vfs_stat((const char __user *)(uintptr_t)arguments.path,
                      &arguments.info);
    if (status != K_OK) return status;
    return copy_to_user((void __user *)(uintptr_t)arguments_pointer,
                        &arguments, sizeof(arguments));
}

int64_t syscall_file_fstat(uint64_t arguments_pointer, uint64_t unused1,
                              uint64_t unused2, uint64_t unused3,
                              uint64_t unused4, uint64_t unused5) {
    (void)unused1;
    (void)unused2;
    (void)unused3;
    (void)unused4;
    (void)unused5;
    os_file_handle_stat_t arguments;
    process_t *process = current_process();
    void *object = 0;
    kstatus_t status;
    if (process == 0) return K_EPERM;
    status = copy_from_user(&arguments,
        (const void __user *)(uintptr_t)arguments_pointer, sizeof(arguments));
    if (status != K_OK) return status;
    if (!versioned_header_valid(&arguments.hdr, sizeof(arguments))) return K_EINVAL;
    status = handle_lookup(&process->handles, (handle_t)arguments.handle,
                           0U, &object);
    if (status != K_OK) return status;
    status = ((object_header_t *)object)->type == KOBJECT_TYPE_FILE ?
             vfs_file_stat((file_t *)object, &arguments.info) : K_EBADF;
    object_put(object);
    if (status != K_OK) return status;
    return copy_to_user((void __user *)(uintptr_t)arguments_pointer,
                        &arguments, sizeof(arguments));
}

int64_t syscall_file_truncate(uint64_t arguments_pointer, uint64_t unused1,
                                 uint64_t unused2, uint64_t unused3,
                                 uint64_t unused4, uint64_t unused5) {
    (void)unused1;
    (void)unused2;
    (void)unused3;
    (void)unused4;
    (void)unused5;
    os_file_truncate_t arguments;
    void *object = 0;
    process_t *process = current_process();
    kstatus_t status;
    if (process == 0) return K_EPERM;
    status = copy_from_user(&arguments,
        (const void __user *)(uintptr_t)arguments_pointer, sizeof(arguments));
    if (status != K_OK) return status;
    if (!versioned_header_valid(&arguments.hdr, sizeof(arguments))) return K_EINVAL;
    status = handle_lookup(&process->handles, (handle_t)arguments.handle,
                           FILE_RIGHT_WRITE, &object);
    if (status != K_OK) return status;
    status = ((file_t *)object)->object.type == KOBJECT_TYPE_FILE ?
        vfs_truncate_kernel((file_t *)object, arguments.size) : K_EBADF;
    object_put(object);
    return status;
}

int64_t syscall_file_remove(uint64_t arguments_pointer, uint64_t unused1,
                               uint64_t unused2, uint64_t unused3,
                               uint64_t unused4, uint64_t unused5) {
    (void)unused1;
    (void)unused2;
    (void)unused3;
    (void)unused4;
    (void)unused5;
    os_file_path_op_t arguments;
    kstatus_t status = copy_from_user(&arguments,
        (const void __user *)(uintptr_t)arguments_pointer, sizeof(arguments));
    if (status != K_OK) return status;
    if (!versioned_header_valid(&arguments.hdr, sizeof(arguments)) ||
        arguments.path == 0U || arguments.mode != 0U || arguments.reserved != 0U) {
        return K_EINVAL;
    }
    return vfs_remove((const char __user *)(uintptr_t)arguments.path);
}

int64_t syscall_file_mkdir(uint64_t arguments_pointer, uint64_t unused1,
                              uint64_t unused2, uint64_t unused3,
                              uint64_t unused4, uint64_t unused5) {
    (void)unused1;
    (void)unused2;
    (void)unused3;
    (void)unused4;
    (void)unused5;
    os_file_path_op_t arguments;
    kstatus_t status = copy_from_user(&arguments,
        (const void __user *)(uintptr_t)arguments_pointer, sizeof(arguments));
    if (status != K_OK) return status;
    if (!versioned_header_valid(&arguments.hdr, sizeof(arguments)) ||
        arguments.path == 0U || arguments.reserved != 0U) return K_EINVAL;
    return vfs_mkdir((const char __user *)(uintptr_t)arguments.path, arguments.mode);
}

int64_t syscall_file_rename(uint64_t arguments_pointer, uint64_t unused1,
                               uint64_t unused2, uint64_t unused3,
                               uint64_t unused4, uint64_t unused5) {
    (void)unused1;
    (void)unused2;
    (void)unused3;
    (void)unused4;
    (void)unused5;
    os_file_rename_t arguments;
    kstatus_t status = copy_from_user(&arguments,
        (const void __user *)(uintptr_t)arguments_pointer, sizeof(arguments));
    if (status != K_OK) return status;
    if (!versioned_header_valid(&arguments.hdr, sizeof(arguments)) ||
        arguments.old_path == 0U || arguments.new_path == 0U) return K_EINVAL;
    return vfs_rename(
        (const char __user *)(uintptr_t)arguments.old_path,
        (const char __user *)(uintptr_t)arguments.new_path);
}

int64_t syscall_file_read(uint64_t handle, uint64_t buffer, uint64_t length,
                             uint64_t output_bytes, uint64_t unused4,
                             uint64_t unused5) {
    (void)unused4;
    (void)unused5;
    process_t *process = current_process();
    if (process == 0 || length > (uint64_t)SIZE_MAX) return K_EINVAL;
    void *object = 0;
    kstatus_t status = handle_lookup(&process->handles, (handle_t)handle,
                                     FILE_RIGHT_READ, &object);
    if (status != K_OK) return status;
    file_t *file = (file_t *)object;
    if (file->object.type != KOBJECT_TYPE_FILE) {
        object_put(file);
        return K_EINVAL;
    }
    uint64_t bytes = 0;
    status = vfs_read(file, (void __user *)(uintptr_t)buffer, (size_t)length, &bytes);
    if (status == K_OK) {
        status = copy_to_user((void __user *)(uintptr_t)output_bytes,
                              &bytes, sizeof(bytes));
    }
    object_put(file);
    return status;
}

int64_t syscall_file_write(uint64_t handle, uint64_t buffer, uint64_t length,
                              uint64_t output_bytes, uint64_t unused4,
                              uint64_t unused5) {
    (void)unused4;
    (void)unused5;
    process_t *process = current_process();
    if (process == 0 || length > (uint64_t)SIZE_MAX) return K_EINVAL;
    void *object = 0;
    kstatus_t status = handle_lookup(&process->handles, (handle_t)handle,
                                     FILE_RIGHT_WRITE, &object);
    if (status != K_OK) return status;
    file_t *file = (file_t *)object;
    if (file->object.type != KOBJECT_TYPE_FILE) {
        object_put(file);
        return K_EINVAL;
    }
    uint64_t bytes = 0;
    status = vfs_write(file, (const void __user *)(uintptr_t)buffer,
                       (size_t)length, &bytes);
    if (status == K_OK) {
        status = copy_to_user((void __user *)(uintptr_t)output_bytes,
                              &bytes, sizeof(bytes));
    }
    object_put(file);
    return status;
}

int64_t syscall_file_fsync(uint64_t handle, uint64_t unused1, uint64_t unused2,
                              uint64_t unused3, uint64_t unused4,
                              uint64_t unused5) {
    (void)unused1;
    (void)unused2;
    (void)unused3;
    (void)unused4;
    (void)unused5;
    process_t *process = current_process();
    if (process == 0) return K_EPERM;
    void *object = 0;
    kstatus_t status = handle_lookup(&process->handles, (handle_t)handle, 0, &object);
    if (status != K_OK) return status;
    file_t *file = (file_t *)object;
    status = file->object.type == KOBJECT_TYPE_FILE ? vfs_fsync(file) : K_EINVAL;
    object_put(file);
    return status;
}

/* IO_SUBMIT(args)：将用户向量复制到内核缓冲区后提交到设备。 */

/* 当前目录由进程持有；访问用户缓冲区和解析磁盘路径时不持有进程锁。 */
int64_t syscall_file_chdir(uint64_t path, uint64_t unused1, uint64_t unused2,
                           uint64_t unused3, uint64_t unused4, uint64_t unused5) {
    (void)unused1; (void)unused2; (void)unused3; (void)unused4; (void)unused5;
    return vfs_change_directory((const char __user *)(uintptr_t)path);
}

int64_t syscall_file_getcwd(uint64_t buffer, uint64_t size, uint64_t unused2,
                            uint64_t unused3, uint64_t unused4, uint64_t unused5) {
    (void)unused2; (void)unused3; (void)unused4; (void)unused5;
    process_t *process = current_process();
    if (process == 0) return K_EPERM;
    if (size == 0U) return K_EINVAL;
    char cwd[PROCESS_CWD_CAPACITY];
    kstatus_t status = process_cwd_copy(process, cwd);
    if (status != K_OK) return status;
    size_t length = 0U;
    while (length + 1U < sizeof(cwd) && cwd[length] != '\0') ++length;
    if (length + 1U > size) return K_ENAMETOOLONG;
    status = copy_to_user((void __user *)(uintptr_t)buffer, cwd, length + 1U);
    return status == K_OK ? (int64_t)(length + 1U) : status;
}
