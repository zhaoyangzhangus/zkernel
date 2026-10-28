#include <arch/x86_64/uaccess.h>
#include <kernel/process.h>
#include <kernel/string.h>
#include <kernel/vfs.h>
#include "internal.h"

/* REFACTOR_FS_VFS_USER_API_OWNER：用户指针到内核路径的适配层。 */
static bool vfs_copy_path_from_user(const char __user *source,
                                    char destination[VFS_PATH_LIMIT]) {
    if (source == 0) return false;
    for (size_t i = 0; i + 1U < VFS_PATH_LIMIT;) {
        uintptr_t address = (uintptr_t)source;
        if (i > UINTPTR_MAX - address) return false;
        address += i;
        /* 绝不跨页复制：当前页中的终止 NUL 不应要求下一用户页也可访问。 */
        size_t chunk = PAGE_SIZE - (address & (PAGE_SIZE - 1U));
        size_t remaining = VFS_PATH_LIMIT - 1U - i;
        if (chunk > remaining) chunk = remaining;
        if (copy_from_user(destination + i, (const void __user *)address,
                           chunk) != K_OK) return false;
        for (size_t j = 0; j < chunk; ++j) {
            if (destination[i + j] == 0) return i + j != 0;
        }
        i += chunk;
    }
    destination[VFS_PATH_LIMIT - 1U] = 0;
    return false;
}

/* 相对路径使用一次工作目录快照，按真实路径顺序展开链接及“..”。 */
static kstatus_t vfs_user_path(const char __user *source, const char *cwd_snapshot,
                               bool follow_final, bool allow_missing,
                               bool resolve_absolute, char output[VFS_PATH_LIMIT]) {
    char input[VFS_PATH_LIMIT];
    char local_cwd[VFS_PATH_LIMIT];
    if (!vfs_copy_path_from_user(source, input)) return K_EACCES;
    size_t input_length = vfs_string_length(input);
    if (input[0] == '/') {
        memcpy(output, input, input_length + 1U);
        return (resolve_absolute || vfs_path_needs_resolution(input)) ?
            vfs_resolve_path_kernel(output, follow_final, allow_missing, output) : K_OK;
    }
    if (cwd_snapshot == 0) {
        process_t *process = current_process();
        if (process == 0) return K_EPERM;
        kstatus_t status = process_cwd_copy(process, local_cwd);
        if (status != K_OK) return status;
        cwd_snapshot = local_cwd;
    }
    size_t base = vfs_string_length(cwd_snapshot);
    if (base == 0U || cwd_snapshot[0] != '/') return K_EINVAL;
    size_t separator = base > 1U ? 1U : 0U;
    if (base + separator + input_length >= VFS_PATH_LIMIT) return K_ENAMETOOLONG;
    memcpy(output, cwd_snapshot, base);
    if (separator) output[base++] = '/';
    memcpy(output + base, input, input_length + 1U);
    return vfs_resolve_path_kernel(output, follow_final, allow_missing, output);
}

kstatus_t vfs_resolve_user_path(const char __user *path, bool follow_final,
                                bool allow_missing_leaf, char output[VFS_PATH_LIMIT]) {
    if (output == 0) return K_EINVAL;
    return vfs_user_path(path, 0, follow_final, allow_missing_leaf, false, output);
}

kstatus_t vfs_change_directory(const char __user *path) {
    process_t *process = current_process();
    if (process == 0) return K_EPERM;
    char resolved[VFS_PATH_LIMIT];
    os_file_info_t info;
    kstatus_t status = vfs_user_path(path, 0, true, false, true, resolved);
    if (status != K_OK) return status;
    status = vfs_stat_kernel(resolved, &info);
    if (status != K_OK) return status;
    if (info.type != OS_FILE_TYPE_DIRECTORY) return K_ENOTDIR;
    return process_cwd_set(process, resolved);
}

kstatus_t vfs_open(const char __user *path, uint32_t flags, uint32_t mode,
                   file_t **out) {
    char kernel_path[VFS_PATH_LIMIT];
    bool exclusive = (flags & (VFS_OPEN_CREATE | VFS_OPEN_EXCLUSIVE)) ==
                     (VFS_OPEN_CREATE | VFS_OPEN_EXCLUSIVE);
    kstatus_t status = vfs_user_path(path, 0, !exclusive,
                                     (flags & VFS_OPEN_CREATE) != 0U,
                                     false, kernel_path);
    return status == K_OK ? vfs_open_kernel(kernel_path, flags, mode, out) : status;
}

kstatus_t vfs_stat(const char __user *path, os_file_info_t *out) {
    char kernel_path[VFS_PATH_LIMIT];
    if (out == 0) return K_EACCES;
    kstatus_t status = vfs_user_path(path, 0, true, false, false, kernel_path);
    return status == K_OK ? vfs_stat_kernel(kernel_path, out) : status;
}

kstatus_t vfs_enumerate(const char __user *path, uint32_t index,
                        os_file_info_t *out) {
    char kernel_path[VFS_PATH_LIMIT];
    if (out == 0) return K_EACCES;
    kstatus_t status = vfs_user_path(path, 0, true, false, false, kernel_path);
    return status == K_OK ? vfs_enumerate_kernel(kernel_path, index, out) : status;
}

kstatus_t vfs_enumerate_batch(const char __user *path, uint32_t index,
                              os_file_info_t *out, uint32_t capacity,
                              uint32_t *count) {
    char kernel_path[VFS_PATH_LIMIT];
    if (count == 0) return K_EINVAL;
    *count = 0U;
    if (out == 0 || capacity == 0U) return K_EINVAL;
    kstatus_t status = vfs_user_path(path, 0, true, false, false, kernel_path);
    return status == K_OK ? vfs_enumerate_kernel_batch(kernel_path, index, out, capacity, count) : status;
}

kstatus_t vfs_remove(const char __user *path) {
    char kernel_path[VFS_PATH_LIMIT];
    kstatus_t status = vfs_user_path(path, 0, false, false, false, kernel_path);
    return status == K_OK ? vfs_remove_kernel(kernel_path) : status;
}

kstatus_t vfs_mkdir(const char __user *path, uint32_t mode) {
    char kernel_path[VFS_PATH_LIMIT];
    kstatus_t status = vfs_user_path(path, 0, false, true, false, kernel_path);
    return status == K_OK ? vfs_mkdir_kernel(kernel_path, mode) : status;
}

kstatus_t vfs_rename(const char __user *old_path,
                     const char __user *new_path) {
    char kernel_old_path[VFS_PATH_LIMIT];
    char kernel_new_path[VFS_PATH_LIMIT];
    char cwd[VFS_PATH_LIMIT];
    process_t *process = current_process();
    const char *snapshot = 0;
    if (process != 0 && process_cwd_copy(process, cwd) == K_OK) snapshot = cwd;
    /* 源和目标必须基于同一目录快照，即使另一个线程正在切换目录。 */
    kstatus_t status = vfs_user_path(old_path, snapshot, false, false, false, kernel_old_path);
    if (status == K_OK)
        status = vfs_user_path(new_path, snapshot, false, true, false, kernel_new_path);
    return status == K_OK ? vfs_rename_kernel(kernel_old_path, kernel_new_path) : status;
}
