#pragma once
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* 复制上下文只属于工作线程；取消和进度通过原子变量与界面交换。 */
typedef struct fm_copy_context {
    atomic_bool cancel;
    _Atomic uint64_t bytes;
    unsigned char *buffer;
} fm_copy_context_t;

static int fm_copy_child_path(char out[256], const char *parent, const char *name) {
    size_t a = strlen(parent), b = strlen(name);
    bool separator = a && parent[a - 1U] != '/';
    if (a + separator + b >= 256U) return ENAMETOOLONG;
    memcpy(out, parent, a);
    if (separator) out[a++] = '/';
    memcpy(out + a, name, b + 1U);
    return 0;
}

/* 与当前 FAT 后端一致，仅折叠 ASCII 大小写；必须同时匹配目录分隔边界。 */
static bool fm_copy_inside_source(const char *source, const char *target) {
    size_t index = 0U;
    while (source[index]) {
        unsigned char a = (unsigned char)source[index];
        unsigned char b = (unsigned char)target[index];
        if (a >= 'a' && a <= 'z') a -= 'a' - 'A';
        if (b >= 'a' && b <= 'z') b -= 'a' - 'A';
        if (a != b) return false;
        ++index;
    }
    return index != 0U && (target[index] == '\0' || target[index] == '/' ||
                           source[index - 1U] == '/');
}

static int fm_copy_node(fm_copy_context_t *context, const char *source,
                        const char *destination, uint32_t depth) {
    struct stat info;
    if (atomic_load_explicit(&context->cancel, memory_order_relaxed)) return ECANCELED;
    if (depth > 64U) return ELOOP;
    if (lstat(source, &info) != 0) return errno;
    if (S_ISREG(info.st_mode)) {
        int input = open(source, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
        if (input < 0) return errno;
        int output = open(destination, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644U);
        if (output < 0) { int error = errno; close(input); return error; }
        int error = 0;
        for (;;) {
            if (atomic_load_explicit(&context->cancel, memory_order_relaxed)) { error = ECANCELED; break; }
            ssize_t count = read(input, context->buffer, 65536U);
            if (count < 0 && errno == EINTR) continue;
            if (count < 0) { error = errno; break; }
            if (!count) break;
            ssize_t offset = 0;
            while (offset < count) {
                if (atomic_load_explicit(&context->cancel, memory_order_relaxed)) { error = ECANCELED; break; }
                ssize_t written = write(output, context->buffer + offset, (size_t)(count - offset));
                if (written < 0 && errno == EINTR) continue;
                if (written <= 0) { error = written < 0 ? errno : EIO; break; }
                offset += written;
                atomic_fetch_add_explicit(&context->bytes, (uint64_t)written, memory_order_relaxed);
            }
            if (error) break;
        }
        if (!error && fsync(output) != 0) error = errno;
        if (close(output) != 0 && !error) error = errno;
        close(input);
        /* 失败或取消时保留已创建内容，避免清理路径误删其他进程的替换文件。 */
        return error;
    }
    /* 不跟随符号链接，也不把设备、管道等特殊文件当作普通数据复制。 */
    if (!S_ISDIR(info.st_mode)) return ENOTSUP;
    if (mkdir(destination, 0755U) != 0) return errno;
    DIR *directory = opendir(source);
    if (!directory) return errno;
    int error = 0;
    for (;;) {
        if (atomic_load_explicit(&context->cancel, memory_order_relaxed)) { error = ECANCELED; break; }
        errno = 0;
        struct dirent *entry = readdir(directory);
        if (!entry) { error = errno; break; }
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
        char from[256], to[256];
        error = fm_copy_child_path(from, source, entry->d_name);
        if (!error) error = fm_copy_child_path(to, destination, entry->d_name);
        if (!error) error = fm_copy_node(context, from, to, depth + 1U);
        if (error) break;
    }
    if (closedir(directory) != 0 && !error) error = errno;
    return error;
}

static int fm_copy_run(fm_copy_context_t *context, const char *source,
                       const char *destination) {
    struct stat source_info;
    if (lstat(source, &source_info) != 0) return errno;
    if (!S_ISREG(source_info.st_mode) && !S_ISDIR(source_info.st_mode)) return ENOTSUP;
    /* 对现有源及目标父目录先规范化，拒绝向源自身或其后代递归复制。 */
    char parent[256];
    size_t length = strlen(destination);
    if (!length || length >= sizeof(parent)) return ENAMETOOLONG;
    memcpy(parent, destination, length + 1U);
    char *slash = strrchr(parent, '/');
    if (!slash || !slash[1]) return EINVAL;
    char name[256];
    memcpy(name, slash + 1U, strlen(slash + 1U) + 1U);
    if (!strcmp(name, ".") || !strcmp(name, "..")) return EINVAL;
    if (slash == parent) slash[1] = '\0'; else *slash = '\0';
    char *canonical_source = realpath(source, 0);
    if (!canonical_source) return errno;
    char *canonical_parent = realpath(parent, 0);
    if (!canonical_parent) { int error = errno; free(canonical_source); return error; }
    char target[256];
    int error = fm_copy_child_path(target, canonical_parent, name);
    if (!error && fm_copy_inside_source(canonical_source, target)) error = EINVAL;
    free(canonical_parent);
    if (!error) {
        context->buffer = malloc(65536U);
        if (!context->buffer) error = ENOMEM;
        else {
            error = fm_copy_node(context, canonical_source, target, 0U);
            free(context->buffer);
            context->buffer = 0;
        }
    }
    free(canonical_source);
    return error;
}
