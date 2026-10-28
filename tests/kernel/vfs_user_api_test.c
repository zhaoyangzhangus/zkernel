#include <arch/x86_64/uaccess.h>
#include <kernel/process.h>
#include <kernel/vfs.h>

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define TEST_PATH_CAPACITY 1024U

static const char *g_user_old_path;
static const char *g_user_new_path;
static char g_kernel_old_path[TEST_PATH_CAPACITY];
static char g_kernel_new_path[TEST_PATH_CAPACITY];
static unsigned g_rename_calls;
static bool g_forwarded_user_pointer;

/* 本测试只覆盖绝对路径；相对路径与工作目录在专用测试中验证。 */
process_t *current_process(void) { return 0; }
kstatus_t process_cwd_copy(process_t *process, char output[PROCESS_CWD_CAPACITY]) {
    (void)process; (void)output; return K_EPERM;
}
kstatus_t process_cwd_set(process_t *process, const char *path) {
    (void)process; (void)path; return K_EPERM;
}
size_t vfs_string_length(const char *text) {
    size_t length = 0U;
    while (text != 0 && length < 256U && text[length] != 0) ++length;
    return length;
}
bool vfs_path_needs_resolution(const char *path) { (void)path; return false; }
kstatus_t vfs_resolve_path_kernel(const char *path, bool follow_final,
                                  bool allow_missing, char output[256]) {
    (void)path; (void)follow_final; (void)allow_missing; (void)output;
    return K_ENOSYS;
}

kstatus_t copy_from_user(void *destination, const void __user *source,
                         size_t size) {
    if (destination == 0 || source == 0 || (uintptr_t)source < 4096U) {
        return K_EACCES;
    }
    memcpy(destination, source, size);
    return K_OK;
}

kstatus_t vfs_open_kernel(const char *path, uint32_t flags, uint32_t mode,
                          file_t **out) {
    (void)path;
    (void)flags;
    (void)mode;
    (void)out;
    return K_EINVAL;
}

kstatus_t vfs_stat_kernel(const char *path, os_file_info_t *out) {
    (void)path;
    (void)out;
    return K_EINVAL;
}

kstatus_t vfs_enumerate_kernel(const char *path, uint32_t index,
                               os_file_info_t *out) {
    (void)path;
    (void)index;
    (void)out;
    return K_EINVAL;
}

kstatus_t vfs_enumerate_kernel_batch(const char *path, uint32_t index,
                                     os_file_info_t *out, uint32_t capacity,
                                     uint32_t *count) {
    (void)path; (void)index; (void)out; (void)capacity; (void)count;
    return K_EINVAL;
}

kstatus_t vfs_remove_kernel(const char *path) {
    (void)path;
    return K_EINVAL;
}

kstatus_t vfs_mkdir_kernel(const char *path, uint32_t mode) {
    (void)path;
    (void)mode;
    return K_EINVAL;
}

kstatus_t vfs_rename_kernel(const char *old_path, const char *new_path) {
    size_t old_length = strlen(old_path);
    size_t new_length = strlen(new_path);
    if (old_length >= sizeof(g_kernel_old_path) ||
        new_length >= sizeof(g_kernel_new_path)) return K_EOVERFLOW;
    g_forwarded_user_pointer = old_path == g_user_old_path ||
                               new_path == g_user_new_path;
    memcpy(g_kernel_old_path, old_path, old_length + 1U);
    memcpy(g_kernel_new_path, new_path, new_length + 1U);
    ++g_rename_calls;
    return K_OK;
}

static int fail(const char *message) {
    fprintf(stderr, "vfs-user-api-test: %s\n", message);
    return 1;
}

int main(void) {
    char old_path[] = "/tmp/chrome-profile-old";
    char new_path[] = "/tmp/chrome-profile-new";

    g_user_old_path = old_path;
    g_user_new_path = new_path;
    if (vfs_rename(old_path, new_path) != K_OK) {
        return fail("valid user paths were rejected");
    }
    if (g_rename_calls != 1U || g_forwarded_user_pointer ||
        strcmp(g_kernel_old_path, old_path) != 0 ||
        strcmp(g_kernel_new_path, new_path) != 0) {
        return fail("rename did not pass copied kernel paths to VFS");
    }
    if (vfs_rename((const char __user *)(uintptr_t)1U, new_path) != K_EACCES ||
        g_rename_calls != 1U) {
        return fail("invalid old user pointer reached the kernel VFS");
    }
    if (vfs_rename(old_path, (const char __user *)(uintptr_t)1U) != K_EACCES ||
        g_rename_calls != 1U) {
        return fail("invalid new user pointer reached the kernel VFS");
    }
    puts("VFS_USER_RENAME_COPY_OK");
    return 0;
}
