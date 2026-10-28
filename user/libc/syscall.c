#include "libc/libc.h"

#include <fcntl.h>
#include <limits.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include <uapi/linux_abi.h>

#define LIBC_FD_LIMIT 1024U
#define LIBC_PATH_LIMIT 256U
#define LIBC_FD_CLOEXEC (1U << 0)
#define LIBC_MODE_OVERRIDE_LIMIT 64U

int __libc_errno;

int *__errno_location(void) {
    return __libc_thread_errno_slot();
}

typedef struct libc_fd_resource {
    os_handle_t handle;
    uint32_t open_flags;
    uint32_t references;
    bool used;
    bool debug_output;
    bool linux_fd;
    bool socket_handle;
    uint32_t socket_family;
    bool pipe_handle;
    bool pipe_read_end;
    char path[LIBC_PATH_LIMIT];
} libc_fd_resource_t;

typedef struct libc_fd_entry {
    libc_fd_resource_t *resource;
    os_handle_t handle;
    uint32_t descriptor_flags;
} libc_fd_entry_t;

static libc_fd_resource_t g_fd_resources[LIBC_FD_LIMIT];
static libc_fd_entry_t g_fd_table[LIBC_FD_LIMIT];
static volatile uint32_t g_fd_table_lock_word;
static volatile uint32_t g_libc_open_fail_debug_calls;
static volatile uint32_t g_libc_database_open_debug_calls;
static volatile uint32_t g_libc_profile_open_debug_calls;
static volatile uint32_t g_libc_fcntl_lock_debug_calls;
static volatile uint32_t g_libc_fstat_fail_debug_calls;
static volatile uint32_t g_libc_ftruncate_fail_debug_calls;
static volatile uint32_t g_libc_fsync_fail_debug_calls;
static volatile uint32_t g_libc_lseek_fail_debug_calls;
#if OS_DEBUG_SERIAL
static volatile uint32_t g_libc_close_fail_debug_calls;
#endif
/*
 * Chromium 的 keyed-service 启动跟踪会拆成五次 write(2) 调用输出。保留少量进程级
 * 排空状态，避免旧版 Chrome 二进制在添加跟踪门控前构建，从而将启动过程变成数千次
 * UART 写入。此状态仅用于诊断，不会改变调用方可见的返回值。
 */
static volatile uint32_t g_libc_service_trace_drop_chunks;

/* 中性命名空间或已部署二进制旧命名空间之后的后缀。 */
#define LIBC_SERVICE_TRACE_PREFIX "PROFILE_SERVICE_"
#define LIBC_SERVICE_TRACE_PREFIX_LENGTH \
    (sizeof(LIBC_SERVICE_TRACE_PREFIX) - 1U)
#define LIBC_SERVICE_TRACE_FOLLOWUP_CHUNKS 4U

/* 诊断额度达到上限后不再递增，耗尽额度的热路径无需执行 atomic 写入。 */
static bool libc_trace_budget(volatile uint32_t *counter, uint32_t limit) {
    uint32_t observed = __atomic_load_n(counter, __ATOMIC_RELAXED);
    while (observed < limit) {
        if (__atomic_compare_exchange_n(counter, &observed, observed + 1U,
                                        false, __ATOMIC_RELAXED,
                                        __ATOMIC_RELAXED)) return true;
    }
    return false;
}

static void libc_debug_text(const char *text, size_t length) {
#if defined(OS_RUNTIME_TRACE)
    (void)__libc_linux_syscall6(OS_SYS_DEBUG_WRITE,
                                (uint64_t)(uintptr_t)text, length,
                                0U, 0U, 0U, 0U);
#else
    (void)text;
    (void)length;
#endif
}

static void libc_debug_fd_event(const char *event, int descriptor,
                                os_handle_t handle, const char *path) {
#if OS_DEBUG_SERIAL
    char output[384];
    static volatile uint32_t calls;
    int length;
    bool snapshot_open = event != 0 && event[0] == 'o' &&
                         path != 0 && strstr(path, "snapshot") != 0;
    bool trace = descriptor == 103 || snapshot_open;
    int64_t process_id;
    if (!trace ||
        !libc_trace_budget(&calls, 64U)) {
        return;
    }
    process_id = __libc_linux_syscall6(LINUX_SYS_GETPID,
                                       0U, 0U, 0U, 0U, 0U, 0U);
    length = snprintf(output, sizeof(output),
                      "OS_LIBC_FD_TRACE pid=%lld event=%s fd=%d handle=%llx path=%s\r\n",
                      (long long)process_id,
                      event != 0 ? event : "?", descriptor,
                      (unsigned long long)handle, path != 0 ? path : "?");
    if (length > 0 && (size_t)length < sizeof(output)) {
        (void)__libc_linux_syscall6(OS_SYS_DEBUG_WRITE,
                                   (uint64_t)(uintptr_t)output,
                                   (uint64_t)length, 0U, 0U, 0U, 0U);
    }
#else
    (void)event;
    (void)descriptor;
    (void)handle;
    (void)path;
#endif
}

static void libc_debug_close_failure(const char *reason, int descriptor,
                                    uintptr_t caller_pc) {
#if OS_DEBUG_SERIAL
    char output[112];
    char digits[12];
    char hex_digits[sizeof(uintptr_t) * 2U];
    size_t length = 0U;
    size_t digit_count = 0U;
    size_t hex_count = 0U;
    int saved_errno = errno;
    int64_t signed_value = descriptor;
    int64_t process_id;
    uint32_t value;
    uintptr_t pc = caller_pc;
    static const char hex[] = "0123456789abcdef";
    if (reason == 0 ||
        !libc_trace_budget(&g_libc_close_fail_debug_calls, 64U)) {
        errno = saved_errno;
        return;
    }
    while (*reason != '\0' && length < sizeof(output) - 20U) {
        output[length++] = *reason++;
    }
    output[length++] = ' ';
    output[length++] = 'f';
    output[length++] = 'd';
    output[length++] = '=';
    if (signed_value < 0) {
        output[length++] = '-';
        signed_value = -signed_value;
    }
    value = (uint32_t)signed_value;
    do {
        digits[digit_count++] = (char)('0' + (value % 10U));
        value /= 10U;
    } while (value != 0U && digit_count < sizeof(digits));
    while (digit_count != 0U) output[length++] = digits[--digit_count];
    process_id = __libc_linux_syscall6(LINUX_SYS_GETPID,
                                       0U, 0U, 0U, 0U, 0U, 0U);
    output[length++] = ' ';
    output[length++] = 'p';
    output[length++] = 'i';
    output[length++] = 'd';
    output[length++] = '=';
    value = process_id > 0 ? (uint32_t)process_id : 0U;
    digit_count = 0U;
    do {
        digits[digit_count++] = (char)('0' + (value % 10U));
        value /= 10U;
    } while (value != 0U && digit_count < sizeof(digits));
    while (digit_count != 0U) output[length++] = digits[--digit_count];
    output[length++] = ' ';
    output[length++] = 'p';
    output[length++] = 'c';
    output[length++] = '=';
    output[length++] = '0';
    output[length++] = 'x';
    do {
        hex_digits[hex_count++] = hex[pc & 0x0fU];
        pc >>= 4U;
    } while (pc != 0U && hex_count < sizeof(hex_digits));
    while (hex_count != 0U) output[length++] = hex_digits[--hex_count];
    output[length++] = '\r';
    output[length++] = '\n';
    (void)__libc_linux_syscall6(OS_SYS_DEBUG_WRITE,
                                (uint64_t)(uintptr_t)output, length,
                                0U, 0U, 0U, 0U);
    errno = saved_errno;
#else
    (void)reason;
    (void)descriptor;
    (void)caller_pc;
#endif
}

static bool libc_debug_budget(volatile uint32_t *counter) {
#if defined(OS_RUNTIME_TRACE)
    return libc_trace_budget(counter, 128U);
#else
    (void)counter;
    return false;
#endif
}

static bool libc_drop_service_trace_chunk(const void *buffer, size_t length) {
#if defined(OS_RUNTIME_TRACE)
    (void)buffer;
    (void)length;
    return false;
#else
    uint32_t remaining;
    /* 已部署 Chrome 中完整且无条件输出的移植阶段标记。
     * 即使 UART 额度已耗尽，也要在进入内核前丢弃。
     * 白名单必须保持精简：警告/错误和未知标记仍写入 stderr，显式运行时跟踪则绕过上方门控。 */
    const char *marker = 0;
    size_t marker_length = 0U;
    if (length >= 3U && memcmp(buffer, "OS_", 3U) == 0) {
        marker = (const char *)buffer + 3U;
        marker_length = length - 3U;
    }
    static const struct {
        const char *text;
        size_t length;
    } stage_prefixes[] = {
        {"NAVIGATE_", sizeof("NAVIGATE_") - 1U},
        {"TAB_MODEL_", sizeof("TAB_MODEL_") - 1U},
        {"VIZ_BACK_TO_BACK_", sizeof("VIZ_BACK_TO_BACK_") - 1U},
        {"CC_UPDATE_", sizeof("CC_UPDATE_") - 1U},
        {"EXTERNAL_BEGIN_FRAME_", sizeof("EXTERNAL_BEGIN_FRAME_") - 1U},
    };
    for (size_t index = 0U;
         index < sizeof(stage_prefixes) / sizeof(stage_prefixes[0]); ++index) {
        size_t prefix_length = stage_prefixes[index].length;
        if (marker != 0 && marker_length >= prefix_length &&
            memcmp(marker, stage_prefixes[index].text, prefix_length) == 0) {
            return true;
        }
    }
    if (marker != 0 && marker_length >= LIBC_SERVICE_TRACE_PREFIX_LENGTH &&
        memcmp(marker, LIBC_SERVICE_TRACE_PREFIX,
               LIBC_SERVICE_TRACE_PREFIX_LENGTH) == 0) {
        __atomic_store_n(&g_libc_service_trace_drop_chunks,
                         LIBC_SERVICE_TRACE_FOLLOWUP_CHUNKS,
                         __ATOMIC_RELAXED);
        return true;
    }
    remaining = __atomic_load_n(&g_libc_service_trace_drop_chunks,
                                __ATOMIC_RELAXED);
    while (remaining != 0U) {
        if (__atomic_compare_exchange_n(&g_libc_service_trace_drop_chunks,
                                        &remaining, remaining - 1U, false,
                                        __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {
            return true;
        }
    }
    return false;
#endif
}

static void libc_debug_path(const char *prefix, const char *path) {
    libc_debug_text(prefix, strlen(prefix));
    libc_debug_text(path, strlen(path));
    libc_debug_text("\n", 1U);
}

static bool libc_database_path(const char *path) {
    return path != 0 &&
           (strstr(path, "History") != 0 ||
            strstr(path, "Local Storage") != 0 ||
            strstr(path, "LocalStorage") != 0 ||
            strstr(path, "UKM") != 0 ||
            strstr(path, "Cookies") != 0 ||
            strstr(path, "Web Data") != 0 ||
            strstr(path, ".db") != 0 ||
            strstr(path, "-journal") != 0 ||
            strstr(path, "-wal") != 0);
}

static bool libc_profile_path(const char *path) {
    return path != 0 && strncmp(path, "/tmp/chrome-data/", 17U) == 0;
}

static void libc_debug_profile_open(const char *path) {
    if (!libc_profile_path(path)) return;
    if (!libc_trace_budget(&g_libc_profile_open_debug_calls, 256U)) {
        return;
    }
    libc_debug_path("OS_LIBC_PROFILE_OPEN_OK path=", path);
}

static void libc_debug_database_path(const char *prefix, const char *path) {
    if (!libc_database_path(path)) return;
    if (!libc_trace_budget(&g_libc_database_open_debug_calls, 128U)) {
        return;
    }
    libc_debug_path(prefix, path);
}

static bool libc_linux_personality(void) {
    return __libc_linux_personality();
}
static bool g_fd_table_initialized;
static char g_current_directory[LIBC_PATH_LIMIT] = "/";

/* FAT32 不保存 POSIX 权限位。为当前进程创建的路径保留 Linux 兼容模式请求的权限，
 * 使 stat() 的可见结果与 mkdir(2) 一致。Chrome 的 ProcessSingleton 会检查
 * 临时目录是否为 0700。 */
typedef struct libc_mode_override {
    bool used;
    char path[LIBC_PATH_LIMIT];
    mode_t mode;
} libc_mode_override_t;

static libc_mode_override_t g_mode_overrides[LIBC_MODE_OVERRIDE_LIMIT];

static int install_linux_fd(int linux_descriptor, int flags,
                            const char *path);
static void initialize_fd_table(void);
static libc_fd_resource_t *resource_for_descriptor(int descriptor);
static void libc_fd_table_lock(void);
static void libc_fd_table_unlock(void);

static void remember_linux_mode(const char *path, mode_t mode) {
    libc_mode_override_t *free_slot = 0;
    if (!libc_linux_personality() || path == 0) return;
    for (unsigned int index = 0U; index < LIBC_MODE_OVERRIDE_LIMIT; ++index) {
        libc_mode_override_t *entry = &g_mode_overrides[index];
        if (!entry->used) {
            if (free_slot == 0) free_slot = entry;
            continue;
        }
        if (strcmp(entry->path, path) == 0) {
            entry->mode = mode & 07777U;
            return;
        }
    }
    if (free_slot == 0 || strlen(path) >= LIBC_PATH_LIMIT) return;
    free_slot->used = true;
    strcpy(free_slot->path, path);
    free_slot->mode = mode & 07777U;
}

static void apply_linux_mode_override(const char *path, struct stat *status) {
    if (!libc_linux_personality() || path == 0 || status == 0) return;
    for (unsigned int index = 0U; index < LIBC_MODE_OVERRIDE_LIMIT; ++index) {
        const libc_mode_override_t *entry = &g_mode_overrides[index];
        if (entry->used && strcmp(entry->path, path) == 0) {
            status->st_mode = (status->st_mode & S_IFMT) | entry->mode;
            return;
        }
    }
}

typedef struct libc_linux_timespec {
    int64_t seconds;
    int64_t nanoseconds;
} libc_linux_timespec_t;

/* 这是兼容层返回的 x86-64 Linux stat ABI。
 * 刻意保持为私有类型：libc 对外提供下方 POSIX stat 布局，并在边界处转换。 */
typedef struct libc_linux_stat {
    uint64_t device;
    uint64_t inode;
    uint64_t links;
    uint32_t mode;
    uint32_t uid;
    uint32_t gid;
    uint32_t mode_padding;
    uint64_t rdevice;
    int64_t size;
    int64_t block_size;
    int64_t blocks;
    libc_linux_timespec_t access_time;
    libc_linux_timespec_t modify_time;
    libc_linux_timespec_t change_time;
    int64_t reserved[3];
} libc_linux_stat_t;

static bool linux_abi_path(const char *path) {
    if (path == 0) return false;
    return strcmp(path, "/proc") == 0 || strncmp(path, "/proc/", 6U) == 0 ||
           strcmp(path, "/sys") == 0 || strncmp(path, "/sys/", 5U) == 0 ||
           strcmp(path, "/tmp") == 0 || strncmp(path, "/tmp/", 5U) == 0 ||
           strcmp(path, "/dev/null") == 0 ||
           strcmp(path, "/dev/nvidiactl") == 0 ||
           strcmp(path, "/dev/nvidia0") == 0 ||
           strcmp(path, LINUX_WINDOW_DEVICE_PATH) == 0;
}

static uint32_t linux_open_flags_from_libc(int flags) {
    uint32_t result = (uint32_t)(flags & O_ACCMODE);
    if ((flags & O_CREAT) != 0) result |= LINUX_O_CREAT;
    if ((flags & O_EXCL) != 0) result |= LINUX_O_EXCL;
    if ((flags & O_TRUNC) != 0) result |= LINUX_O_TRUNC;
    if ((flags & O_APPEND) != 0) result |= LINUX_O_APPEND;
    if ((flags & O_NONBLOCK) != 0) result |= LINUX_O_NONBLOCK;
    if ((flags & O_NOCTTY) != 0) result |= LINUX_O_NOCTTY;
    if ((flags & O_DIRECTORY) != 0) result |= LINUX_O_DIRECTORY;
    if ((flags & O_CLOEXEC) != 0) result |= LINUX_O_CLOEXEC;
    return result;
}

static void fill_linux_stat(const libc_linux_stat_t *source,
                            struct stat *destination) {
    memset(destination, 0, sizeof(*destination));
    destination->st_dev = (dev_t)source->device;
    destination->st_ino = (ino_t)source->inode;
    destination->st_nlink = (nlink_t)source->links;
    destination->st_mode = (mode_t)source->mode;
    destination->st_uid = (uid_t)source->uid;
    destination->st_gid = (gid_t)source->gid;
    destination->st_rdev = (dev_t)source->rdevice;
    destination->st_size = (off_t)source->size;
    destination->st_blksize = (blksize_t)source->block_size;
    destination->st_blocks = (blkcnt_t)source->blocks;
    destination->st_atim.tv_sec = (time_t)source->access_time.seconds;
    destination->st_atim.tv_nsec = (long)source->access_time.nanoseconds;
    destination->st_mtim.tv_sec = (time_t)source->modify_time.seconds;
    destination->st_mtim.tv_nsec = (long)source->modify_time.nanoseconds;
    destination->st_ctim.tv_sec = (time_t)source->change_time.seconds;
    destination->st_ctim.tv_nsec = (long)source->change_time.nanoseconds;
}

static int libc_error(int64_t status) {
    if (status < 0) {
        uint64_t value = (uint64_t)(-(status + 1)) + 1U;
        errno = value > INT32_MAX ? INT32_MAX : (int)value;
        return -1;
    }
    return 0;
}

static int libc_status_error(int64_t status) {
    return status < 0 ? libc_error(status) : 0;
}

/* 仅在失败时输出诊断：保留原始 errno，并避免分配内存。 */
static void libc_tmp_open_failure(const char *stage, const char *path,
                                  int64_t status) {
    static uint32_t count;
    if (path == 0 || strncmp(path, "/tmp/", 5U) != 0 ||
        status == -EEXIST || status == -ENOENT ||
        !libc_trace_budget(&count, 16U)) return;
    int saved_errno = errno;
    char output[384];
    int length = snprintf(output, sizeof(output),
                          "OS_LIBC_TMP_OPEN_FAIL stage=%s status=%lld errno=%d path=%s\n",
                          stage, (long long)status, saved_errno, path);
    if (length > 0) {
        size_t bytes = (size_t)length < sizeof(output) ?
                       (size_t)length : sizeof(output) - 1U;
        (void)write(2, output, bytes);
    }
    errno = saved_errno;
}

static void close_raw_linux_descriptor(int descriptor) {
    if (descriptor >= 0) {
        (void)__libc_linux_syscall6(
            LINUX_SYS_CLOSE, (uint64_t)(uint32_t)descriptor,
            0U, 0U, 0U, 0U, 0U);
    }
}

static int install_linux_descriptor_at(int descriptor, int linux_descriptor,
                                       int open_flags) {
    libc_fd_resource_t *resource = 0;
    if (descriptor < 0 || (unsigned int)descriptor >= LIBC_FD_LIMIT ||
        linux_descriptor < 0 || (unsigned int)linux_descriptor >= LIBC_FD_LIMIT) {
        errno = EMFILE;
        return -1;
    }
    if (g_fd_table[descriptor].resource != 0) {
        errno = EMFILE;
        return -1;
    }
    for (unsigned int index = 3U; index < LIBC_FD_LIMIT; ++index) {
        if (!g_fd_resources[index].used) {
            resource = &g_fd_resources[index];
            break;
        }
    }
    if (resource == 0) {
        errno = EMFILE;
        return -1;
    }
    memset(resource, 0, sizeof(*resource));
    resource->handle = (os_handle_t)(uint32_t)linux_descriptor;
    resource->open_flags = (uint32_t)open_flags;
    resource->references = 1U;
    resource->used = true;
    resource->linux_fd = true;
    resource->path[0] = '\0';
    g_fd_table[descriptor].resource = resource;
    g_fd_table[descriptor].handle = resource->handle;
    g_fd_table[descriptor].descriptor_flags =
        (open_flags & O_CLOEXEC) != 0 ? FD_CLOEXEC : 0U;
    return descriptor;
}

static int adopt_linux_fd_locked(int linux_descriptor, int open_flags) {
    int duplicate_descriptors[LIBC_FD_LIMIT];
    unsigned int duplicate_count = 0U;
    if (g_fd_table[linux_descriptor].resource == 0) {
        int result = install_linux_descriptor_at(
            linux_descriptor, linux_descriptor, open_flags);
        if (result < 0) close_raw_linux_descriptor(linux_descriptor);
        return result;
    }

    /* Linux 内核分配描述符时并不知道 System 原生描述符表的情况。如果返回编号已被
     * 原生描述符占用，向上查找时必须保持每个临时 Linux 副本处于打开状态。
     * 若立即关闭冲突副本，内核下次 DUP 仍可能返回同一编号，也可能导致请求的
     * F_DUPFD 槽位替换已有 Linux 描述符。 */
    while (duplicate_count < LIBC_FD_LIMIT) {
        int64_t duplicate;
        duplicate = __libc_linux_syscall6(
            LINUX_SYS_DUP, (uint64_t)(uint32_t)linux_descriptor,
            0U, 0U, 0U, 0U, 0U);
        if (duplicate < 0) {
            while (duplicate_count != 0U) {
                close_raw_linux_descriptor(
                    duplicate_descriptors[--duplicate_count]);
            }
            close_raw_linux_descriptor(linux_descriptor);
            (void)libc_error(duplicate);
            return -1;
        }
        if (duplicate >= (int64_t)LIBC_FD_LIMIT) {
            close_raw_linux_descriptor((int)duplicate);
            while (duplicate_count != 0U) {
                close_raw_linux_descriptor(
                    duplicate_descriptors[--duplicate_count]);
            }
            close_raw_linux_descriptor(linux_descriptor);
            errno = EMFILE;
            return -1;
        }
        if (g_fd_table[(int)duplicate].resource == 0) {
            if ((open_flags & O_CLOEXEC) != 0) {
                int64_t flag_status = __libc_linux_syscall6(
                    LINUX_SYS_FCNTL, (uint64_t)(uint32_t)duplicate,
                    LINUX_F_SETFD, LINUX_FD_CLOEXEC, 0U, 0U, 0U);
                if (flag_status < 0) {
                    close_raw_linux_descriptor((int)duplicate);
                    while (duplicate_count != 0U) {
                        close_raw_linux_descriptor(
                            duplicate_descriptors[--duplicate_count]);
                    }
                    close_raw_linux_descriptor(linux_descriptor);
                    return libc_error(flag_status);
                }
            }
            close_raw_linux_descriptor(linux_descriptor);
            while (duplicate_count != 0U) {
                close_raw_linux_descriptor(
                    duplicate_descriptors[--duplicate_count]);
            }
            int result = install_linux_descriptor_at(
                (int)duplicate, (int)duplicate, open_flags);
            if (result < 0) close_raw_linux_descriptor((int)duplicate);
            return result;
        }
        duplicate_descriptors[duplicate_count++] = (int)duplicate;
    }
    while (duplicate_count != 0U) {
        close_raw_linux_descriptor(duplicate_descriptors[--duplicate_count]);
    }
    close_raw_linux_descriptor(linux_descriptor);
    errno = EMFILE;
    return -1;
}

int __libc_adopt_linux_fd(int linux_descriptor, int open_flags) {
    int result;
    if (linux_descriptor < 0 ||
        (unsigned int)linux_descriptor >= LIBC_FD_LIMIT) {
        errno = EBADF;
        return -1;
    }
    initialize_fd_table();
    libc_fd_table_lock();
    result = adopt_linux_fd_locked(linux_descriptor, open_flags);
    libc_fd_table_unlock();
    return result;
}

int __libc_linux_fd_for_descriptor(int descriptor) {
    libc_fd_resource_t *resource = resource_for_descriptor(descriptor);
    if (resource == 0 || !resource->linux_fd) {
        errno = EBADF;
        return -1;
    }
    return (int)g_fd_table[descriptor].handle;
}

bool __libc_descriptor_is_linux_fd(int descriptor) {
    int saved_errno = errno;
    libc_fd_resource_t *resource = resource_for_descriptor(descriptor);
    bool result = resource != 0 && resource->linux_fd;
    errno = saved_errno;
    return result;
}

int __libc_mark_linux_socket_descriptor(int descriptor, uint32_t family) {
    libc_fd_resource_t *resource = resource_for_descriptor(descriptor);
    if (resource == 0 || !resource->linux_fd) {
        errno = EBADF;
        return -1;
    }
    resource->socket_handle = true;
    resource->socket_family = family;
    return 0;
}

static void libc_fd_table_lock(void) {
    while (__atomic_exchange_n(&g_fd_table_lock_word, 1U,
                               __ATOMIC_ACQUIRE) != 0U) {
        while (__atomic_load_n(&g_fd_table_lock_word,
                               __ATOMIC_RELAXED) != 0U) {
            __asm__ volatile("pause");
        }
    }
}

static void libc_fd_table_unlock(void) {
    __atomic_store_n(&g_fd_table_lock_word, 0U, __ATOMIC_RELEASE);
}

static void initialize_fd_table(void) {
    if (__atomic_load_n(&g_fd_table_initialized, __ATOMIC_ACQUIRE)) return;
    libc_fd_table_lock();
    if (!g_fd_table_initialized) {
        memset(g_fd_resources, 0, sizeof(g_fd_resources));
        memset(g_fd_table, 0, sizeof(g_fd_table));
        for (unsigned int descriptor = 0U; descriptor < 3U; ++descriptor) {
            libc_fd_resource_t *resource = &g_fd_resources[descriptor];
            resource->handle = descriptor;
            resource->open_flags = descriptor == STDIN_FILENO ?
                                   O_RDONLY : O_WRONLY;
            resource->references = 1U;
            resource->used = true;
            resource->debug_output = descriptor != STDIN_FILENO;
            g_fd_table[descriptor].resource = resource;
            g_fd_table[descriptor].handle = resource->handle;
        }
        __atomic_store_n(&g_fd_table_initialized, true, __ATOMIC_RELEASE);
    }
    libc_fd_table_unlock();
}

/* fork() 会连同地址空间一起复制此锁字。先将 fork 快照与描述符表变更串行化，
 * 再在两个新进程中释放复制得到的锁，然后运行用户 atfork 回调。 */
void __libc_fd_table_fork_prepare(void) {
    initialize_fd_table();
    libc_fd_table_lock();
}

void __libc_fd_table_fork_parent(void) {
    libc_fd_table_unlock();
}

void __libc_fd_table_fork_child(void) {
    libc_fd_table_unlock();
}

static libc_fd_resource_t *resource_for_descriptor(int descriptor) {
    initialize_fd_table();
    if (descriptor < 0 || (unsigned int)descriptor >= LIBC_FD_LIMIT) {
        errno = EBADF;
        return 0;
    }
    if (g_fd_table[descriptor].resource != 0) {
        return g_fd_table[descriptor].resource;
    }
    libc_fd_table_lock();
    if (g_fd_table[descriptor].resource == 0 && descriptor >= 3 &&
        libc_linux_personality()) {
        int64_t descriptor_flags = __libc_linux_syscall6(
            LINUX_SYS_FCNTL, (uint64_t)(uint32_t)descriptor,
            LINUX_F_GETFD, 0U, 0U, 0U, 0U);
        int64_t linux_flags = descriptor_flags < 0 ? descriptor_flags :
            __libc_linux_syscall6(
                LINUX_SYS_FCNTL, (uint64_t)(uint32_t)descriptor,
                LINUX_F_GETFL, 0U, 0U, 0U, 0U);
        if (descriptor_flags >= 0 && linux_flags >= 0) {
            int open_flags = (int)(linux_flags &
                (LINUX_O_WRONLY | LINUX_O_RDWR));
            if ((linux_flags & LINUX_O_APPEND) != 0U) open_flags |= O_APPEND;
            if ((linux_flags & LINUX_O_NONBLOCK) != 0U) open_flags |= O_NONBLOCK;
            if ((linux_flags & LINUX_O_DIRECTORY) != 0U) open_flags |= O_DIRECTORY;
            if ((descriptor_flags & LINUX_FD_CLOEXEC) != 0U) open_flags |= O_CLOEXEC;
            if (install_linux_descriptor_at(descriptor, descriptor,
                                            open_flags) >= 0) {
                libc_fd_resource_t *resource =
                    g_fd_table[descriptor].resource;
                libc_fd_table_unlock();
                return resource;
            }
            if (g_fd_table[descriptor].resource != 0) {
                libc_fd_resource_t *resource =
                    g_fd_table[descriptor].resource;
                libc_fd_table_unlock();
                return resource;
            }
        }
    }
    if (g_fd_table[descriptor].resource == 0) {
        libc_fd_table_unlock();
        errno = EBADF;
        return 0;
    }
    libc_fd_resource_t *resource = g_fd_table[descriptor].resource;
    libc_fd_table_unlock();
    return resource;
}

static int allocate_descriptor_locked(libc_fd_resource_t *resource,
                                      unsigned int minimum) {
    if (resource == 0 || minimum >= LIBC_FD_LIMIT) {
        errno = EMFILE;
        return -1;
    }
    for (unsigned int descriptor = minimum; descriptor < LIBC_FD_LIMIT; ++descriptor) {
        if (g_fd_table[descriptor].resource == 0) {
            /* Linux exec 继承会填充内核的 Linux fd 表，而不是此 libc 表。
             * 为原生 System 句柄分配相同编号前，先预留/接管这些槽位。 */
            if (libc_linux_personality()) {
                int64_t descriptor_flags = __libc_linux_syscall6(
                    LINUX_SYS_FCNTL, (uint64_t)descriptor,
                    LINUX_F_GETFD, 0U, 0U, 0U, 0U);
                if (descriptor_flags >= 0) {
                    int64_t linux_flags = __libc_linux_syscall6(
                        LINUX_SYS_FCNTL, (uint64_t)descriptor,
                        LINUX_F_GETFL, 0U, 0U, 0U, 0U);
                    if (linux_flags >= 0) {
                        int open_flags = (int)(linux_flags &
                            (LINUX_O_WRONLY | LINUX_O_RDWR));
                        if ((linux_flags & LINUX_O_APPEND) != 0U)
                            open_flags |= O_APPEND;
                        if ((linux_flags & LINUX_O_NONBLOCK) != 0U)
                            open_flags |= O_NONBLOCK;
                        if ((linux_flags & LINUX_O_DIRECTORY) != 0U)
                            open_flags |= O_DIRECTORY;
                        if ((descriptor_flags & LINUX_FD_CLOEXEC) != 0U)
                            open_flags |= O_CLOEXEC;
                        (void)install_linux_descriptor_at(
                            (int)descriptor, (int)descriptor, open_flags);
                    }
                    continue;
                }
            }
            g_fd_table[descriptor].resource = resource;
            g_fd_table[descriptor].descriptor_flags = 0U;
            ++resource->references;
            return (int)descriptor;
        }
    }
    errno = EMFILE;
    return -1;
}

int __libc_install_handle(os_handle_t handle, uint32_t open_flags,
                          bool socket_handle, uint32_t socket_family) {
    libc_fd_resource_t *resource = 0;
    int descriptor;
    if (handle == OS_INVALID_HANDLE) {
        errno = EINVAL;
        return -1;
    }
    initialize_fd_table();
    libc_fd_table_lock();
    for (unsigned int index = 3U; index < LIBC_FD_LIMIT; ++index) {
        if (!g_fd_resources[index].used) {
            resource = &g_fd_resources[index];
            break;
        }
    }
    if (resource == 0) {
        libc_fd_table_unlock();
        (void)__libc_native_syscall6(OS_SYS_HANDLE_CLOSE, handle, 0U, 0U, 0U, 0U, 0U);
        errno = EMFILE;
        return -1;
    }
    resource->handle = handle;
    resource->open_flags = open_flags;
    resource->references = 0U;
    resource->used = true;
    resource->debug_output = false;
    resource->linux_fd = false;
    resource->socket_handle = socket_handle;
    resource->socket_family = socket_family;
    resource->pipe_handle = false;
    resource->pipe_read_end = false;
    resource->path[0] = '\0';
    descriptor = allocate_descriptor_locked(resource, 3U);
    if (descriptor >= 0) g_fd_table[descriptor].handle = handle;
    if (descriptor < 0) resource->used = false;
    libc_fd_table_unlock();
    if (descriptor < 0) {
        (void)__libc_native_syscall6(OS_SYS_HANDLE_CLOSE, handle, 0U, 0U, 0U, 0U, 0U);
    }
    return descriptor;
}

int __libc_install_pipe_handle(os_handle_t handle, uint32_t open_flags,
                               bool read_end) {
    int descriptor = __libc_install_handle(handle, open_flags, false, 0U);
    libc_fd_resource_t *resource;
    if (descriptor < 0) return -1;
    resource = resource_for_descriptor(descriptor);
    if (resource == 0) {
        (void)close(descriptor);
        return -1;
    }
    resource->pipe_handle = true;
    resource->pipe_read_end = read_end;
    g_fd_table[descriptor].handle = handle;
    if ((open_flags & O_CLOEXEC) != 0U) {
        g_fd_table[descriptor].descriptor_flags |= FD_CLOEXEC;
    }
    return descriptor;
}

os_handle_t __libc_handle_for_descriptor(int descriptor) {
    libc_fd_resource_t *resource = resource_for_descriptor(descriptor);
    return resource == 0 ? OS_INVALID_HANDLE : g_fd_table[descriptor].handle;
}

bool __libc_descriptor_is_socket(int descriptor) {
    libc_fd_resource_t *resource = resource_for_descriptor(descriptor);
    return resource != 0 && resource->socket_handle;
}

bool __libc_descriptor_is_pipe(int descriptor) {
    libc_fd_resource_t *resource = resource_for_descriptor(descriptor);
    return resource != 0 && resource->pipe_handle;
}

bool __libc_pipe_descriptor_is_read(int descriptor) {
    libc_fd_resource_t *resource = resource_for_descriptor(descriptor);
    return resource != 0 && resource->pipe_handle && resource->pipe_read_end;
}

uint32_t __libc_socket_family(int descriptor) {
    libc_fd_resource_t *resource = resource_for_descriptor(descriptor);
    return resource == 0 || !resource->socket_handle ? 0U : resource->socket_family;
}

uint32_t __libc_descriptor_open_flags(int descriptor) {
    libc_fd_resource_t *resource = resource_for_descriptor(descriptor);
    return resource == 0 ? 0U : resource->open_flags;
}

int __libc_descriptor_path(int descriptor, char *output, size_t capacity) {
    libc_fd_resource_t *resource = resource_for_descriptor(descriptor);
    size_t length;
    if (resource == 0) return -1;
    if (resource->socket_handle || resource->path[0] == '\0') {
        errno = ENOTDIR;
        return -1;
    }
    if (output == 0 || capacity == 0U) {
        errno = EINVAL;
        return -1;
    }
    length = strlen(resource->path);
    if (length >= capacity) {
        errno = ERANGE;
        return -1;
    }
    memcpy(output, resource->path, length + 1U);
    return 0;
}

int __libc_status_result(int64_t status) {
    return libc_status_error(status);
}

static void release_resource_reference(libc_fd_resource_t *resource) {
    if (resource == 0 || resource->references == 0U) return;
    --resource->references;
    if (resource->references != 0U) return;
    resource->open_flags = 0U;
    resource->debug_output = false;
    resource->linux_fd = false;
    resource->socket_handle = false;
    resource->socket_family = 0U;
    resource->pipe_handle = false;
    resource->pipe_read_end = false;
    resource->path[0] = '\0';
    resource->used = false;
}

static int release_descriptor(int descriptor) {
    libc_fd_resource_t *resource = g_fd_table[descriptor].resource;
    os_handle_t handle = g_fd_table[descriptor].handle;
    int result = 0;
    if (resource == 0 || resource->references == 0U) return 0;
    if (!resource->debug_output) {
        int64_t status = resource->linux_fd ?
            __libc_linux_syscall6(LINUX_SYS_CLOSE, (uint64_t)handle,
                                  0U, 0U, 0U, 0U, 0U) :
            __libc_native_syscall6(OS_SYS_HANDLE_CLOSE, handle,
                            0U, 0U, 0U, 0U, 0U);
        result = libc_status_error(status);
    }
    release_resource_reference(resource);
    return result;
}

static int close_descriptor_locked(int descriptor) {
    int result;
    if (descriptor < 0 || (unsigned int)descriptor >= LIBC_FD_LIMIT ||
        g_fd_table[descriptor].resource == 0) {
        errno = EBADF;
        return -1;
    }
    result = release_descriptor(descriptor);
    g_fd_table[descriptor].resource = 0;
    g_fd_table[descriptor].handle = OS_INVALID_HANDLE;
    g_fd_table[descriptor].descriptor_flags = 0U;
    return result;
}

void __libc_build_exec_fd_map(os_exec_fd_map_t *map) {
    if (map == 0) return;
    initialize_fd_table();
    memset(map, 0, sizeof(*map));
    map->hdr.size = sizeof(*map);
    map->hdr.version = OS_SYSCALL_ABI_VERSION;
    for (unsigned int descriptor = 0U;
         descriptor < LIBC_FD_LIMIT && map->count < OS_EXEC_FD_LIMIT;
         ++descriptor) {
        libc_fd_resource_t *resource = g_fd_table[descriptor].resource;
        os_exec_fd_entry_t *entry;
        if (resource == 0 || resource->linux_fd ||
            (g_fd_table[descriptor].descriptor_flags & FD_CLOEXEC) != 0U) {
            continue;
        }
        entry = &map->entries[map->count++];
        entry->descriptor = descriptor;
        entry->descriptor_flags = g_fd_table[descriptor].descriptor_flags;
        entry->handle = g_fd_table[descriptor].handle;
        entry->open_flags = resource->open_flags;
        entry->socket_family = resource->socket_family;
        if (resource->socket_handle) entry->resource_flags |= OS_EXEC_FD_SOCKET;
        if (resource->pipe_handle) {
            entry->resource_flags |= OS_EXEC_FD_PIPE;
            if (resource->pipe_read_end) entry->resource_flags |= OS_EXEC_FD_PIPE_READ;
        }
        if (resource->debug_output) entry->resource_flags |= OS_EXEC_FD_DEBUG;
        memcpy(entry->path, resource->path, sizeof(entry->path));
    }
}

void __libc_init_descriptors(char **envp) {
    uint64_t *auxiliary;
    os_exec_fd_map_t *map = 0;
    if (envp == 0) return;
    while (*envp != 0) ++envp;
    auxiliary = (uint64_t *)(uintptr_t)(envp + 1);
    for (unsigned int index = 0U; index < 128U; ++index) {
        uint64_t type = auxiliary[index * 2U];
        if (type == 0U) break;
        if (type == OS_AUX_FD_MAP) {
            map = (os_exec_fd_map_t *)(uintptr_t)auxiliary[index * 2U + 1U];
            break;
        }
    }
    if (map == 0 || map->hdr.size < sizeof(*map) ||
        map->hdr.version != OS_SYSCALL_ABI_VERSION || map->hdr.flags != 0U ||
        map->reserved != 0U || map->count > OS_EXEC_FD_LIMIT) return;

    initialize_fd_table();
    for (uint32_t index = 0U; index < map->count; ++index) {
        const os_exec_fd_entry_t *entry = &map->entries[index];
        libc_fd_resource_t *resource = 0;
        if (entry->descriptor >= LIBC_FD_LIMIT) continue;
        if (entry->descriptor < 3U) {
            resource = &g_fd_resources[entry->descriptor];
        } else {
            if (g_fd_table[entry->descriptor].resource != 0) continue;
            for (unsigned int slot = 3U; slot < LIBC_FD_LIMIT; ++slot) {
                if (!g_fd_resources[slot].used) {
                    resource = &g_fd_resources[slot];
                    break;
                }
            }
        }
        if (resource == 0) break;
        resource->handle = entry->handle;
        resource->open_flags = entry->open_flags;
        resource->references = 1U;
        resource->used = true;
        resource->debug_output =
            (entry->resource_flags & OS_EXEC_FD_DEBUG) != 0U;
        resource->linux_fd = false;
        resource->socket_handle =
            (entry->resource_flags & OS_EXEC_FD_SOCKET) != 0U;
        resource->socket_family = entry->socket_family;
        resource->pipe_handle =
            (entry->resource_flags & OS_EXEC_FD_PIPE) != 0U;
        resource->pipe_read_end =
            (entry->resource_flags & OS_EXEC_FD_PIPE_READ) != 0U;
        memcpy(resource->path, entry->path, sizeof(resource->path));
        resource->path[sizeof(resource->path) - 1U] = '\0';
        g_fd_table[entry->descriptor].resource = resource;
        g_fd_table[entry->descriptor].handle = entry->handle;
        g_fd_table[entry->descriptor].descriptor_flags =
            entry->descriptor_flags & FD_CLOEXEC;
    }
}

static bool append_path_component(char output[LIBC_PATH_LIMIT],
                                  size_t *length, const char *component,
                                  size_t component_length) {
    if (component_length == 0U) return true;
    if (*length != 1U && *length + 1U >= LIBC_PATH_LIMIT) return false;
    if (*length + component_length + 1U > LIBC_PATH_LIMIT) return false;
    if (*length != 1U) output[(*length)++] = '/';
    memcpy(output + *length, component, component_length);
    *length += component_length;
    output[*length] = '\0';
    return true;
}

static bool path_length_limited(const char *path, size_t *length) {
    size_t index;
    if (path == 0 || length == 0) {
        errno = EINVAL;
        return false;
    }
    for (index = 0U; index < LIBC_PATH_LIMIT; ++index) {
        if (path[index] == '\0') {
            *length = index;
            return true;
        }
    }
    errno = ENAMETOOLONG;
    return false;
}

static bool make_absolute_path(const char *path, char output[LIBC_PATH_LIMIT]) {
    char combined[LIBC_PATH_LIMIT * 2U];
    size_t input_length;
    size_t combined_length;
    size_t position;
    size_t output_length = 1U;
    size_t component_starts[LIBC_PATH_LIMIT / 2U];
    size_t component_count = 0U;

    if (path == 0 || path[0] == '\0') {
        errno = EINVAL;
        return false;
    }
    if (!path_length_limited(path, &input_length)) return false;
    if (input_length >= LIBC_PATH_LIMIT) {
        errno = ENAMETOOLONG;
        return false;
    }
    if (path[0] == '/') {
        memcpy(combined, path, input_length + 1U);
        combined_length = input_length;
    } else {
        size_t cwd_length = strlen(g_current_directory);
        if (cwd_length + 1U + input_length >= sizeof(combined)) {
            errno = ENAMETOOLONG;
            return false;
        }
        memcpy(combined, g_current_directory, cwd_length);
        combined[cwd_length] = '/';
        memcpy(combined + cwd_length + 1U, path, input_length + 1U);
        combined_length = cwd_length + 1U + input_length;
    }
    output[0] = '/';
    output[1] = '\0';
    position = 0U;
    while (position < combined_length) {
        size_t start;
        size_t length;
        while (position < combined_length && combined[position] == '/') ++position;
        if (position == combined_length) break;
        start = position;
        while (position < combined_length && combined[position] != '/') ++position;
        length = position - start;
        if (length == 1U && combined[start] == '.') continue;
        if (length == 2U && combined[start] == '.' && combined[start + 1U] == '.') {
            if (component_count != 0U) {
                output_length = component_starts[--component_count];
                output[output_length] = '\0';
            }
            continue;
        }
        if (component_count >= sizeof(component_starts) / sizeof(component_starts[0]) ||
            !append_path_component(output, &output_length, combined + start, length)) {
            errno = ENAMETOOLONG;
            return false;
        }
        component_starts[component_count] = output_length - length;
        if (component_starts[component_count] > 1U) --component_starts[component_count];
        ++component_count;
    }
    return true;
}

static bool make_path_at(int directory_descriptor, const char *path,
                         char output[LIBC_PATH_LIMIT]) {
    char directory_path[LIBC_PATH_LIMIT];
    char combined[LIBC_PATH_LIMIT * 2U];
    struct stat directory_status;
    size_t directory_length;
    size_t path_length;

    if (path == 0 || output == 0) {
        errno = EINVAL;
        return false;
    }
    if (path[0] == '/' || directory_descriptor == AT_FDCWD) {
        return make_absolute_path(path, output);
    }
    if (__libc_descriptor_path(directory_descriptor, directory_path,
                               sizeof(directory_path)) < 0) {
        return false;
    }
    if (stat(directory_path, &directory_status) < 0) return false;
    if (!S_ISDIR(directory_status.st_mode)) {
        errno = ENOTDIR;
        return false;
    }
    directory_length = strlen(directory_path);
    if (!path_length_limited(path, &path_length)) return false;
    if (path_length == 0U) {
        errno = ENOENT;
        return false;
    }
    size_t separator_length = directory_length > 1U &&
                              directory_path[directory_length - 1U] == '/' ?
                              0U : 1U;
    if (directory_length > sizeof(combined) - separator_length - 1U ||
        path_length > sizeof(combined) - directory_length -
                      separator_length - 1U) {
        errno = ENAMETOOLONG;
        return false;
    }
    memcpy(combined, directory_path, directory_length);
    if (separator_length == 0U) {
        memcpy(combined + directory_length, path, path_length + 1U);
    } else {
        combined[directory_length] = '/';
        memcpy(combined + directory_length + 1U, path, path_length + 1U);
    }
    return make_absolute_path(combined, output);
}

int __libc_make_absolute_path(const char *path, char *output) {
    return make_absolute_path(path, output) ? 0 : -1;
}

static void fill_stat(const os_file_info_t *info, struct stat *status) {
    memset(status, 0, sizeof(*status));
    status->st_mode = info->mode;
    if ((status->st_mode & S_IFMT) == 0U) {
        status->st_mode |= info->type == OS_FILE_TYPE_DIRECTORY ?
                           S_IFDIR : S_IFREG;
    }
    status->st_nlink = 1U;
    status->st_size = (off_t)info->size;
    status->st_blksize = 4096;
    status->st_blocks = (blkcnt_t)((info->size + 511U) / 512U);
}

/* 原生文件 ABI 早于 POSIX inode 标识，因此 st_dev/st_ino 返回零。SQLite 使用这两个
 * 字段作为进程内锁键；两者都为零会使所有数据库看起来都是同一个文件。
 * System 不支持硬链接，因此可用规范路径的稳定哈希为原生文件提供所需标识，
 * 同时保持 stat() 与 fstat() 结果一致。 */
static void libc_set_file_identity(const char *path, struct stat *status) {
    const unsigned char *cursor = (const unsigned char *)path;
    uint64_t hash = 1469598103934665603ULL;
    if (path == 0 || status == 0) return;
    while (*cursor != 0U) {
        hash ^= (uint64_t)*cursor++;
        hash *= 1099511628211ULL;
    }
    if (hash == 0U) hash = 1U;
    status->st_dev = (dev_t)1U;
    status->st_ino = (ino_t)hash;
}

static int64_t libc_raw_syscall6(uint64_t number, uint64_t argument0,
                        uint64_t argument1, uint64_t argument2,
                        uint64_t argument3, uint64_t argument4,
                        uint64_t argument5) {
    register uint64_t r10 __asm__("r10") = argument3;
    register uint64_t r8 __asm__("r8") = argument4;
    register uint64_t r9 __asm__("r9") = argument5;
    uint64_t result;
    __asm__ volatile("syscall"
                     : "=a"(result)
                     : "a"(number), "D"(argument0), "S"(argument1),
                       "d"(argument2), "r"(r10), "r"(r8), "r"(r9)
                     : "rcx", "r11", "memory");
    return (int64_t)result;
}

int64_t __libc_linux_syscall6(uint64_t number, uint64_t argument0,
                              uint64_t argument1, uint64_t argument2,
                              uint64_t argument3, uint64_t argument4,
                              uint64_t argument5) {
    return libc_raw_syscall6(number, argument0, argument1, argument2,
                               argument3, argument4, argument5);
}

int64_t __libc_native_syscall6(uint64_t number, uint64_t argument0,
                        uint64_t argument1, uint64_t argument2,
                        uint64_t argument3, uint64_t argument4,
                        uint64_t argument5) {
    return libc_raw_syscall6(number | OS_SYSCALL_NATIVE_FLAG,
                               argument0, argument1, argument2, argument3,
                               argument4, argument5);
}

/* Linux 兼容的可变参数入口。Chromium 在少数热路径（futex、getrandom、gettid 和信号
 * 辅助函数）中直接使用 syscall()，而不是具名 libc 封装。内核根据原始系统调用号
 * 分发 Linux 兼容模式进程，因此必须准确保留全部六个 ABI 参数槽。 */
static size_t linux_syscall_argument_count(long number) {
    switch ((uint64_t)number) {
    case LINUX_SYS_READ:
    case LINUX_SYS_WRITE:
    case LINUX_SYS_OPEN:
    case LINUX_SYS_POLL:
    case LINUX_SYS_READV:
    case LINUX_SYS_WRITEV:
    case LINUX_SYS_LSEEK:
    case LINUX_SYS_MPROTECT:
    case LINUX_SYS_MADVISE:
    case LINUX_SYS_SOCKET:
    case LINUX_SYS_CONNECT:
    case LINUX_SYS_ACCEPT:
    case LINUX_SYS_BIND:
    case LINUX_SYS_SENDMSG:
    case LINUX_SYS_RECVMSG:
    case LINUX_SYS_GETSOCKNAME:
    case LINUX_SYS_GETPEERNAME:
    case LINUX_SYS_IOCTL:
    case LINUX_SYS_FCNTL:
    case LINUX_SYS_GETDENTS64:
    case LINUX_SYS_READLINK:
    case LINUX_SYS_GETRANDOM:
    case LINUX_SYS_SCHED_GETAFFINITY:
    case LINUX_SYS_DUP3:
    case LINUX_SYS_EXECVE:
    case LINUX_SYS_GETRESUID:
    case LINUX_SYS_GETRESGID:
        return 3U;
    case LINUX_SYS_CLOSE:
    case LINUX_SYS_PIPE:
    case LINUX_SYS_DUP:
    case LINUX_SYS_BRK:
    case LINUX_SYS_FSYNC:
    case LINUX_SYS_FDATASYNC:
    case LINUX_SYS_CHDIR:
    case LINUX_SYS_SET_TID_ADDRESS:
    case LINUX_SYS_EXIT:
    case LINUX_SYS_EXIT_GROUP:
    case LINUX_SYS_UNAME:
    case LINUX_SYS_EVENTFD:
    case LINUX_SYS_EPOLL_CREATE:
    case LINUX_SYS_EPOLL_CREATE1:
    case LINUX_SYS_WINDOW_CREATE:
    case LINUX_SYS_DISPLAY_GET_INFO:
        return 1U;
    case LINUX_SYS_STAT:
    case LINUX_SYS_FSTAT:
    case LINUX_SYS_LSTAT:
    case LINUX_SYS_ACCESS:
    case LINUX_SYS_MUNMAP:
    case LINUX_SYS_FTRUNCATE:
    case LINUX_SYS_SHUTDOWN:
    case LINUX_SYS_LISTEN:
    case LINUX_SYS_RENAME:
    case LINUX_SYS_KILL:
    case LINUX_SYS_PRCTL:
    case LINUX_SYS_ARCH_PRCTL:
    case LINUX_SYS_GETCWD:
    case LINUX_SYS_SET_ROBUST_LIST:
    case LINUX_SYS_DUP2:
    case LINUX_SYS_PIPE2:
    case LINUX_SYS_EVENTFD2:
    case LINUX_SYS_NANOSLEEP:
    case LINUX_SYS_CLOCK_GETTIME:
        return 2U;
    case LINUX_SYS_RT_SIGACTION:
    case LINUX_SYS_RT_SIGPROCMASK:
    case LINUX_SYS_PREAD64:
    case LINUX_SYS_PWRITE64:
    case LINUX_SYS_SOCKETPAIR:
    case LINUX_SYS_WAIT4:
    case LINUX_SYS_EPOLL_WAIT:
    case LINUX_SYS_EPOLL_CTL:
    case LINUX_SYS_OPENAT:
    case LINUX_SYS_NEWFSTATAT:
    case LINUX_SYS_READLINKAT:
    case LINUX_SYS_FACCESSAT2:
    case LINUX_SYS_ACCEPT4:
    case LINUX_SYS_RSEQ:
    case LINUX_SYS_PRLIMIT64:
        return 4U;
    case LINUX_SYS_MREMAP:
    case LINUX_SYS_CLONE:
    case LINUX_SYS_SETSOCKOPT:
    case LINUX_SYS_GETSOCKOPT:
    case LINUX_SYS_PREADV:
    case LINUX_SYS_PWRITEV:
    case LINUX_SYS_STATX:
        return 5U;
    case LINUX_SYS_EPOLL_PWAIT:
    case LINUX_SYS_FUTEX:
    case LINUX_SYS_SENDTO:
    case LINUX_SYS_RECVFROM:
        return 6U;
    case LINUX_SYS_MMAP:
        return 6U;
    case LINUX_SYS_SCHED_YIELD:
    case LINUX_SYS_GETPID:
    case LINUX_SYS_GETTID:
    case LINUX_SYS_GETUID:
    case LINUX_SYS_GETGID:
    case LINUX_SYS_GETEUID:
    case LINUX_SYS_GETEGID:
    case LINUX_SYS_FORK:
    case LINUX_SYS_VFORK:
        return 0U;
    default:
        /* 不支持的系统调用无需读取任何可变参数。返回 ENOSYS 前不要读取调用方未提供的参数。 */
        return 0U;
    }
}

/* Chromium 可能从回调进入此可变参数 ABI，而此时栈对齐不受 System CRT 控制
 *（尤其是早期线程和引导路径）。GCC 的重新对齐序言可避免 va_arg 数组使用的 SSE
 * 存储指令触发 #GP，确保原本有效的 Linux 系统调用正常执行。 */
__attribute__((force_align_arg_pointer))
long syscall(long number, ...) {
    va_list arguments;
    uint64_t values[6] = {0U};
    size_t count = linux_syscall_argument_count(number);
    int64_t result;

    va_start(arguments, number);
    for (size_t index = 0U; index < count; ++index) {
        values[index] = va_arg(arguments, uint64_t);
    }
    va_end(arguments);
    result = __libc_linux_syscall6((uint64_t)number,
                                   values[0], values[1], values[2], values[3],
                                   values[4], values[5]);
    if (result < 0) {
        (void)libc_error(result);
        return -1L;
    }
    return (long)result;
}

ssize_t read(int descriptor, void *buffer, size_t length) {
    libc_fd_resource_t *resource = resource_for_descriptor(descriptor);
    uint64_t bytes = 0U;
    uint32_t source_address = 0U;
    uint16_t source_port = 0U;
    uint8_t source_address6[16] = {0};
    int64_t status;
    if (resource == 0) return -1;
    if (buffer == 0 && length != 0U) {
        errno = EINVAL;
        return -1;
    }
    if (resource->linux_fd) {
        status = __libc_linux_syscall6(
            LINUX_SYS_READ, (uint64_t)g_fd_table[descriptor].handle,
            (uint64_t)(uintptr_t)buffer, length, 0U, 0U, 0U);
        if (status < 0) {
            (void)libc_error(status);
            return -1;
        }
        return status > INT64_MAX ? (errno = EOVERFLOW, -1) :
               (ssize_t)status;
    } else if (resource->socket_handle) {
        uint64_t timeout = (resource->open_flags & O_NONBLOCK) != 0U ?
                           0U : OS_WAIT_INFINITE;
        if (resource->socket_family == AF_INET6) {
            status = __libc_native_syscall6(OS_SYS_SOCKET_RECV6,
                                     g_fd_table[descriptor].handle,
                                     (uint64_t)(uintptr_t)buffer, length,
                                     (uint64_t)(uintptr_t)source_address6,
                                     (uint64_t)(uintptr_t)&source_port,
                                     timeout);
        } else {
            status = __libc_native_syscall6(OS_SYS_SOCKET_RECV,
                                     g_fd_table[descriptor].handle,
                                     (uint64_t)(uintptr_t)buffer, length,
                                     (uint64_t)(uintptr_t)&source_address,
                                     (uint64_t)(uintptr_t)&source_port,
                                     timeout);
        }
    } else if (resource->pipe_handle) {
        uint64_t timeout = (resource->open_flags & O_NONBLOCK) != 0U ?
                           0U : OS_WAIT_INFINITE;
        status = __libc_native_syscall6(OS_SYS_PIPE_READ, g_fd_table[descriptor].handle,
                                 (uint64_t)(uintptr_t)buffer, length,
                                 timeout, (uint64_t)(uintptr_t)&bytes, 0U);
    } else {
        status = __libc_native_syscall6(OS_SYS_FILE_READ, g_fd_table[descriptor].handle,
                                 (uint64_t)(uintptr_t)buffer, length,
                                 (uint64_t)(uintptr_t)&bytes, 0U, 0U);
    }
    if (status < 0) {
        if ((resource->socket_handle || resource->pipe_handle) &&
            (resource->open_flags & O_NONBLOCK) != 0U &&
            status == -(int64_t)ETIMEDOUT) {
            errno = EAGAIN;
        } else {
            libc_error(status);
        }
        return -1;
    }
    if (resource->socket_handle) {
        return status > INT64_MAX ? (errno = EOVERFLOW, -1) : (ssize_t)status;
    }
    if (bytes > INT64_MAX) {
        errno = EOVERFLOW;
        return -1;
    }
    return (ssize_t)bytes;
}

static ssize_t debug_write(const void *buffer, size_t length) {
    const unsigned char *cursor = (const unsigned char *)buffer;
    size_t remaining = length;
    if (length == 0U) return 0;
    if (libc_drop_service_trace_chunk(buffer, length)) return (ssize_t)length;
    while (remaining != 0U) {
        size_t chunk = remaining > 240U ? 240U : remaining;
        int64_t status = __libc_native_syscall6(OS_SYS_DEBUG_WRITE,
                                         (uint64_t)(uintptr_t)cursor, chunk,
                                         0U, 0U, 0U, 0U);
        if (status < 0) {
            libc_error(status);
            return -1;
        }
        if ((uint64_t)status != chunk) {
            errno = EIO;
            return -1;
        }
        cursor += chunk;
        remaining -= chunk;
    }
    return (ssize_t)length;
}

ssize_t write(int descriptor, const void *buffer, size_t length) {
    libc_fd_resource_t *resource = resource_for_descriptor(descriptor);
    uint64_t bytes = 0U;
    uint8_t destination_address6[16] = {0};
    int64_t status;
    if (resource == 0) return -1;
    if (buffer == 0 && length != 0U) {
        errno = EINVAL;
        return -1;
    }
    if (resource->linux_fd) {
        status = __libc_linux_syscall6(
            LINUX_SYS_WRITE, (uint64_t)g_fd_table[descriptor].handle,
            (uint64_t)(uintptr_t)buffer, length, 0U, 0U, 0U);
        if (status < 0) {
            (void)libc_error(status);
            return -1;
        }
        return status > INT64_MAX ? (errno = EOVERFLOW, -1) :
               (ssize_t)status;
    } else if (resource->debug_output) return debug_write(buffer, length);
    else if (resource->socket_handle) {
        if (resource->socket_family == AF_INET6) {
            status = __libc_native_syscall6(OS_SYS_SOCKET_SEND6,
                                     g_fd_table[descriptor].handle,
                                     (uint64_t)(uintptr_t)buffer, length,
                                     (uint64_t)(uintptr_t)destination_address6,
                                     0U, 0U);
        } else {
            status = __libc_native_syscall6(OS_SYS_SOCKET_SEND,
                                     g_fd_table[descriptor].handle,
                                     (uint64_t)(uintptr_t)buffer, length,
                                     0U, 0U, 0U);
        }
    } else if (resource->pipe_handle) {
        uint64_t timeout = (resource->open_flags & O_NONBLOCK) != 0U ?
                           0U : OS_WAIT_INFINITE;
        status = __libc_native_syscall6(OS_SYS_PIPE_WRITE,
                                 g_fd_table[descriptor].handle,
                                 (uint64_t)(uintptr_t)buffer, length,
                                 timeout, (uint64_t)(uintptr_t)&bytes, 0U);
    } else {
        status = __libc_native_syscall6(OS_SYS_FILE_WRITE,
                                 g_fd_table[descriptor].handle,
                                 (uint64_t)(uintptr_t)buffer, length,
                                 (uint64_t)(uintptr_t)&bytes, 0U, 0U);
    }
    if (status < 0) {
        if (resource->pipe_handle &&
            (resource->open_flags & O_NONBLOCK) != 0U &&
            status == -(int64_t)ETIMEDOUT) {
            errno = EAGAIN;
        } else {
            libc_error(status);
        }
        return -1;
    }
    if (resource->socket_handle) {
        return status > INT64_MAX ? (errno = EOVERFLOW, -1) : (ssize_t)status;
    }
    if (bytes > INT64_MAX) {
        errno = EOVERFLOW;
        return -1;
    }
    return (ssize_t)bytes;
}

int open(const char *path, int flags, ...) {
    os_handle_t handle = OS_INVALID_HANDLE;
    uint32_t mode = 0U;
    uint32_t file_flags = 0U;
    libc_fd_resource_t *resource;
    int descriptor;
    int64_t status;
    char absolute_path[LIBC_PATH_LIMIT];
    const int supported_flags = O_ACCMODE | O_CREAT | O_EXCL | O_NOCTTY |
                                O_TRUNC | O_APPEND | O_NONBLOCK | O_LARGEFILE |
                                O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC;
    if ((flags & ~supported_flags) != 0) {
        errno = EINVAL;
        return -1;
    }
    switch (flags & O_ACCMODE) {
    case O_RDONLY:
        file_flags |= OS_FILE_OPEN_READ;
        break;
    case O_WRONLY:
        file_flags |= OS_FILE_OPEN_WRITE;
        break;
    case O_RDWR:
        file_flags |= OS_FILE_OPEN_READ | OS_FILE_OPEN_WRITE;
        break;
    default:
        errno = EINVAL;
        return -1;
    }
    if ((flags & O_CREAT) != 0) file_flags |= OS_FILE_OPEN_CREATE;
    if ((flags & O_EXCL) != 0) file_flags |= OS_FILE_OPEN_EXCLUSIVE;
    if ((flags & O_TRUNC) != 0) file_flags |= OS_FILE_OPEN_TRUNCATE;
    if ((flags & O_APPEND) != 0) file_flags |= OS_FILE_OPEN_APPEND;
    if ((flags & O_DIRECTORY) != 0) file_flags |= OS_FILE_OPEN_DIRECTORY;
    if ((flags & O_CLOEXEC) != 0) file_flags |= OS_FILE_OPEN_CLOEXEC;
    if (!make_absolute_path(path, absolute_path)) return -1;
    if ((flags & O_CREAT) != 0) {
        va_list arguments;
        va_start(arguments, flags);
        mode = (uint32_t)va_arg(arguments, int);
        va_end(arguments);
    }
    if (linux_abi_path(absolute_path) && libc_linux_personality()) {
        status = __libc_linux_syscall6(
            LINUX_SYS_OPENAT, (uint64_t)(int64_t)LINUX_AT_FDCWD,
            (uint64_t)(uintptr_t)absolute_path,
            linux_open_flags_from_libc(flags), mode, 0U, 0U);
        if (status < 0) {
            (void)libc_error(status);
            libc_tmp_open_failure("openat", absolute_path, status);
            return -1;
        }
        int result = install_linux_fd((int)status, flags, absolute_path);
        if (result < 0)
            libc_tmp_open_failure("adopt", absolute_path, -(int64_t)errno);
        return result;
    }
    status = __libc_native_syscall6(OS_SYS_FILE_OPEN,
                             (uint64_t)(uintptr_t)absolute_path,
                             file_flags,
                             mode,
                             (uint64_t)(uintptr_t)&handle, 0U, 0U);
    if (status < 0) {
        if (libc_debug_budget(&g_libc_open_fail_debug_calls)) {
            libc_debug_text("OS_LIBC_OPEN_FAIL status=",
                            sizeof("OS_LIBC_OPEN_FAIL status=") - 1U);
            char status_text[24];
            int status_length = snprintf(status_text, sizeof(status_text),
                                         "%lld path=", (long long)status);
            if (status_length > 0) {
                libc_debug_text(status_text, (size_t)status_length);
            }
            libc_debug_text(absolute_path, strlen(absolute_path));
            libc_debug_text("\n", 1U);
        }
        libc_debug_database_path("OS_LIBC_DB_OPEN_FAIL path=", absolute_path);
        libc_error(status);
        libc_tmp_open_failure("native-open", absolute_path, status);
        return -1;
    }
    initialize_fd_table();
    libc_fd_table_lock();
    resource = 0;
    for (unsigned int index = 3U; index < LIBC_FD_LIMIT; ++index) {
        if (!g_fd_resources[index].used) {
            resource = &g_fd_resources[index];
            break;
        }
    }
    if (resource == 0) {
        libc_fd_table_unlock();
        (void)__libc_native_syscall6(OS_SYS_HANDLE_CLOSE, handle, 0U, 0U, 0U, 0U, 0U);
        libc_debug_database_path("OS_LIBC_DB_OPEN_NO_RESOURCE path=",
                                 absolute_path);
        errno = EMFILE;
        return -1;
    }
    resource->handle = handle;
    resource->open_flags = (uint32_t)flags;
    resource->references = 0U;
    resource->used = true;
    resource->debug_output = false;
    resource->socket_handle = false;
    resource->socket_family = 0U;
    resource->pipe_handle = false;
    resource->pipe_read_end = false;
    strcpy(resource->path, absolute_path);
    descriptor = allocate_descriptor_locked(resource, 3U);
    if (descriptor >= 0) g_fd_table[descriptor].handle = handle;
    if (descriptor >= 0 && (flags & O_CLOEXEC) != 0) {
        g_fd_table[descriptor].descriptor_flags |= FD_CLOEXEC;
    }
    if (descriptor < 0) resource->used = false;
    libc_fd_table_unlock();
    if (descriptor < 0) {
        (void)__libc_native_syscall6(OS_SYS_HANDLE_CLOSE, handle, 0U, 0U, 0U, 0U, 0U);
        libc_debug_database_path("OS_LIBC_DB_OPEN_NO_DESCRIPTOR path=",
                                 absolute_path);
    }
    if (descriptor >= 0) {
    libc_debug_fd_event("open-native", descriptor, handle, absolute_path);
        libc_debug_profile_open(absolute_path);
        libc_debug_database_path("OS_LIBC_DB_OPEN_OK path=", absolute_path);
    }
    return descriptor;
}

int openat(int directory_descriptor, const char *path, int flags, ...) {
    char absolute_path[LIBC_PATH_LIMIT];
    uint32_t mode = 0U;
    if ((flags & O_CREAT) != 0) {
        va_list arguments;
        va_start(arguments, flags);
        mode = (uint32_t)va_arg(arguments, int);
        va_end(arguments);
    }
    if (!make_path_at(directory_descriptor, path, absolute_path)) return -1;
    return open(absolute_path, flags, mode);
}

int open64(const char *path, int flags, ...) {
    va_list arguments;
    int mode = 0;
    int descriptor;
    if ((flags & O_CREAT) == 0) return open(path, flags);
    va_start(arguments, flags);
    mode = va_arg(arguments, int);
    va_end(arguments);
    descriptor = open(path, flags, mode);
    return descriptor;
}

int openat64(int directory_descriptor, const char *path, int flags, ...) {
    va_list arguments;
    int mode = 0;
    int descriptor;
    if ((flags & O_CREAT) == 0) return openat(directory_descriptor, path, flags);
    va_start(arguments, flags);
    mode = va_arg(arguments, int);
    va_end(arguments);
    descriptor = openat(directory_descriptor, path, flags, mode);
    return descriptor;
}

int creat(const char *path, unsigned int mode) {
    return open(path, O_WRONLY | O_CREAT | O_TRUNC, mode);
}

int creat64(const char *path, unsigned int mode) {
    return creat(path, mode);
}

int memfd_create(const char *name, unsigned int flags) {
    char path[] = "/tmp/memfd-XXXXXX";
    int descriptor;

    (void)name;
    if ((flags & ~(MFD_CLOEXEC | MFD_ALLOW_SEALING)) != 0U) {
        errno = EINVAL;
        return -1;
    }
    descriptor = mkstemp(path);
    if (descriptor < 0) return -1;
    if ((flags & MFD_CLOEXEC) != 0U && fcntl(descriptor, F_SETFD,
                                             FD_CLOEXEC) < 0) {
        int saved_errno = errno;
        (void)close(descriptor);
        (void)unlink(path);
        errno = saved_errno;
        return -1;
    }
    /* 将后备文件设为匿名文件时，保持描述符打开。 */
    if (unlink(path) < 0) {
        int saved_errno = errno;
        (void)close(descriptor);
        errno = saved_errno;
        return -1;
    }
    return descriptor;
}

static int install_linux_fd(int linux_descriptor, int flags,
                            const char *path) {
    libc_fd_resource_t *resource;
    int descriptor;
    if (linux_descriptor < 0 || path == 0) {
        errno = EBADF;
        return -1;
    }
    descriptor = __libc_adopt_linux_fd(linux_descriptor, flags);
    if (descriptor < 0) return -1;
    resource = g_fd_table[descriptor].resource;
    if (resource == 0) {
        (void)close(descriptor);
        errno = EIO;
        return -1;
    }
    strcpy(resource->path, path);
    libc_debug_fd_event("open-linux", descriptor, resource->handle, path);
    return descriptor;
}

int close(int descriptor) {
    libc_fd_resource_t *resource;
    int result;
    uintptr_t caller_pc = (uintptr_t)__builtin_return_address(0);
    initialize_fd_table();
    if (descriptor < 0 || (unsigned int)descriptor >= LIBC_FD_LIMIT) {
        errno = EBADF;
        libc_debug_close_failure("OS_LIBC_CLOSE_RANGE", descriptor,
                                 caller_pc);
        return -1;
    }

    /* 在内核关闭描述符期间保持映射关系稳定。如果先于获取表锁查找资源，其他线程可能
     * 在查找与关闭之间回收或重用该槽位。 */
    libc_fd_table_lock();
    resource = g_fd_table[descriptor].resource;
    if (resource != 0) {
        bool linux_fd = resource->linux_fd;
        libc_debug_fd_event(linux_fd ? "close-linux" : "close-native",
                            descriptor, g_fd_table[descriptor].handle,
                            resource->path);
        result = close_descriptor_locked(descriptor);
        libc_fd_table_unlock();
        if (result != 0) {
            libc_debug_close_failure(linux_fd ?
                "OS_LIBC_CLOSE_MAPPED_LINUX" :
                "OS_LIBC_CLOSE_MAPPED_NATIVE", descriptor, caller_pc);
        }
        return result;
    }

    /* 直接使用 Linux 系统调用的代码可能引入未经过 libc 封装的描述符。释放锁后再接管
     * 这些描述符，因为 resource_for_descriptor() 会自行加锁探测。 */
    libc_fd_table_unlock();
    resource = resource_for_descriptor(descriptor);
    if (resource == 0) {
        libc_debug_close_failure("OS_LIBC_CLOSE_ADOPT_MISS", descriptor,
                                 caller_pc);
        return -1;
    }
    libc_fd_table_lock();
    if (g_fd_table[descriptor].resource != resource) {
        libc_fd_table_unlock();
        errno = EBADF;
        libc_debug_close_failure("OS_LIBC_CLOSE_ADOPT_RACE", descriptor,
                                 caller_pc);
        return -1;
    }
    bool linux_fd = resource->linux_fd;
    result = close_descriptor_locked(descriptor);
    libc_fd_table_unlock();
    if (result != 0) {
        libc_debug_close_failure(linux_fd ?
            "OS_LIBC_CLOSE_ADOPTED_LINUX" :
            "OS_LIBC_CLOSE_ADOPTED_NATIVE", descriptor, caller_pc);
    }
    return result;
}

int pipe2(int pipefd[2], int flags) {
    os_pipe_create_t request = {0};
    uint32_t pipe_flags = 0U;
    uint32_t open_flags;
    int read_descriptor;
    int write_descriptor;
    int64_t status;

    if (pipefd == 0) {
        errno = EFAULT;
        return -1;
    }
    if ((flags & ~(O_CLOEXEC | O_NONBLOCK)) != 0) {
        errno = EINVAL;
        return -1;
    }
    if (libc_linux_personality()) {
        int32_t linux_descriptors[2] = {-1, -1};
        int64_t linux_status = __libc_linux_syscall6(
            LINUX_SYS_PIPE2, (uint64_t)(uintptr_t)linux_descriptors,
            (uint64_t)(uint32_t)flags, 0U, 0U, 0U, 0U);
        if (linux_status < 0) return libc_error(linux_status);
        read_descriptor = __libc_adopt_linux_fd(
            linux_descriptors[0],
            O_RDONLY | (flags & (O_NONBLOCK | O_CLOEXEC)));
        if (read_descriptor < 0) {
            close_raw_linux_descriptor(linux_descriptors[1]);
            return -1;
        }
        write_descriptor = __libc_adopt_linux_fd(
            linux_descriptors[1],
            O_WRONLY | (flags & (O_NONBLOCK | O_CLOEXEC)));
        if (write_descriptor < 0) {
            int saved_errno = errno;
            (void)close(read_descriptor);
            errno = saved_errno;
            return -1;
        }
        pipefd[0] = read_descriptor;
        pipefd[1] = write_descriptor;
        return 0;
    }
    if ((flags & O_CLOEXEC) != 0) pipe_flags |= OS_PIPE_FLAG_CLOEXEC;
    if ((flags & O_NONBLOCK) != 0) pipe_flags |= OS_PIPE_FLAG_NONBLOCK;
    request.hdr.size = sizeof(request);
    request.hdr.version = OS_SYSCALL_ABI_VERSION;
    request.flags = pipe_flags;
    status = __libc_native_syscall6(OS_SYS_PIPE_CREATE,
                             (uint64_t)(uintptr_t)&request,
                             0U, 0U, 0U, 0U, 0U);
    if (status < 0) return libc_error(status);
    open_flags = O_RDONLY | (uint32_t)(flags & (O_NONBLOCK | O_CLOEXEC));
    read_descriptor = __libc_install_pipe_handle(request.read_handle,
                                                  open_flags, true);
    if (read_descriptor < 0) {
        (void)__libc_native_syscall6(OS_SYS_HANDLE_CLOSE, request.write_handle,
                              0U, 0U, 0U, 0U, 0U);
        return -1;
    }
    open_flags = O_WRONLY | (uint32_t)(flags & (O_NONBLOCK | O_CLOEXEC));
    write_descriptor = __libc_install_pipe_handle(request.write_handle,
                                                   open_flags, false);
    if (write_descriptor < 0) {
        int saved_errno = errno;
        (void)close(read_descriptor);
        errno = saved_errno;
        return -1;
    }
    if ((flags & O_CLOEXEC) != 0) {
        /* __libc_install_pipe_handle 会为管道两端记录 close-on-exec 标志。 */
    }
    pipefd[0] = read_descriptor;
    pipefd[1] = write_descriptor;
    return 0;
}

int pipe(int pipefd[2]) {
    return pipe2(pipefd, 0);
}

off_t lseek(int descriptor, off_t offset, int whence) {
    libc_fd_resource_t *resource = resource_for_descriptor(descriptor);
    os_file_seek_t request = {0};
    int64_t status;
    if (resource == 0) return -1;
    if (resource->linux_fd) {
        status = __libc_linux_syscall6(
            LINUX_SYS_LSEEK, (uint64_t)g_fd_table[descriptor].handle,
            (uint64_t)offset, (uint64_t)(uint32_t)whence, 0U, 0U, 0U);
        if (status < 0) {
            (void)libc_error(status);
            return -1;
        }
        return status > INT64_MAX ? (errno = EOVERFLOW, -1) : (off_t)status;
    }
    if (resource->pipe_handle) {
        errno = ESPIPE;
        return -1;
    }
    request.hdr.size = sizeof(request);
    request.hdr.version = OS_SYSCALL_ABI_VERSION;
    request.handle = g_fd_table[descriptor].handle;
    request.offset = offset;
    request.whence = (uint32_t)whence;
    status = __libc_native_syscall6(OS_SYS_FILE_SEEK,
                             (uint64_t)(uintptr_t)&request,
                             0U, 0U, 0U, 0U, 0U);
    if (status < 0) {
        if (libc_debug_budget(&g_libc_lseek_fail_debug_calls))
            libc_debug_path("OS_LIBC_LSEEK_FAIL path=", resource->path);
        libc_error(status);
        return -1;
    }
    if (request.position > INT64_MAX) {
        errno = EOVERFLOW;
        return -1;
    }
    return (off_t)request.position;
}

off_t lseek64(int descriptor, off_t offset, int whence) {
    return lseek(descriptor, offset, whence);
}

ssize_t pread(int descriptor, void *buffer, size_t length, off_t offset) {
    if (libc_linux_personality() &&
        __libc_descriptor_is_linux_fd(descriptor)) {
        int raw = __libc_linux_fd_for_descriptor(descriptor);
        if (raw < 0) return -1;
        int64_t result = __libc_linux_syscall6(
            LINUX_SYS_PREAD64, (uint64_t)(uint32_t)raw,
            (uint64_t)(uintptr_t)buffer, length, (uint64_t)offset, 0U, 0U);
        if (result < 0) return libc_error(result);
        return (ssize_t)result;
    }
    off_t saved = lseek(descriptor, 0, SEEK_CUR);
    ssize_t result;
    if (saved < 0 || lseek(descriptor, offset, SEEK_SET) < 0) return -1;
    result = read(descriptor, buffer, length);
    if (lseek(descriptor, saved, SEEK_SET) < 0 && result >= 0) {
        errno = EIO;
        return -1;
    }
    return result;
}

ssize_t pread64(int descriptor, void *buffer, size_t length, off_t offset) {
    return pread(descriptor, buffer, length, offset);
}

ssize_t pwrite(int descriptor, const void *buffer, size_t length, off_t offset) {
    if (libc_linux_personality() &&
        __libc_descriptor_is_linux_fd(descriptor)) {
        int raw = __libc_linux_fd_for_descriptor(descriptor);
        if (raw < 0) return -1;
        int64_t result = __libc_linux_syscall6(
            LINUX_SYS_PWRITE64, (uint64_t)(uint32_t)raw,
            (uint64_t)(uintptr_t)buffer, length, (uint64_t)offset, 0U, 0U);
        if (result < 0) return libc_error(result);
        return (ssize_t)result;
    }
    off_t saved = lseek(descriptor, 0, SEEK_CUR);
    ssize_t result;
    if (saved < 0 || lseek(descriptor, offset, SEEK_SET) < 0) return -1;
    result = write(descriptor, buffer, length);
    if (lseek(descriptor, saved, SEEK_SET) < 0 && result >= 0) {
        errno = EIO;
        return -1;
    }
    return result;
}

ssize_t pwrite64(int descriptor, const void *buffer, size_t length, off_t offset) {
    return pwrite(descriptor, buffer, length, offset);
}

int fsync(int descriptor) {
    libc_fd_resource_t *resource = resource_for_descriptor(descriptor);
    int64_t status;
    if (resource == 0) return -1;
    if (resource->debug_output) return 0;
    status = resource->linux_fd ?
        __libc_linux_syscall6(LINUX_SYS_FSYNC,
                              (uint64_t)g_fd_table[descriptor].handle,
                              0U, 0U, 0U, 0U, 0U) :
        __libc_native_syscall6(OS_SYS_FILE_FSYNC, g_fd_table[descriptor].handle,
                             0U, 0U, 0U, 0U, 0U);
    if (status < 0 && libc_debug_budget(&g_libc_fsync_fail_debug_calls))
        libc_debug_path("OS_LIBC_FSYNC_FAIL path=", resource->path);
    return libc_error(status);
}

int fdatasync(int descriptor) {
    return fsync(descriptor);
}

int ftruncate(int descriptor, off_t length) {
    libc_fd_resource_t *resource = resource_for_descriptor(descriptor);
    os_file_truncate_t request = {0};
    int64_t status;
    if (resource == 0) return -1;
    if (length < 0) {
        errno = EINVAL;
        return -1;
    }
    if (resource->linux_fd) {
        status = __libc_linux_syscall6(
            LINUX_SYS_FTRUNCATE, (uint64_t)g_fd_table[descriptor].handle,
            (uint64_t)length, 0U, 0U, 0U, 0U);
        return libc_error(status);
    }
    request.hdr.size = sizeof(request);
    request.hdr.version = OS_SYSCALL_ABI_VERSION;
    request.handle = g_fd_table[descriptor].handle;
    request.size = (uint64_t)length;
    status = __libc_native_syscall6(OS_SYS_FILE_TRUNCATE,
                             (uint64_t)(uintptr_t)&request,
                             0U, 0U, 0U, 0U, 0U);
    if (status < 0 && libc_debug_budget(&g_libc_ftruncate_fail_debug_calls))
        libc_debug_path("OS_LIBC_FTRUNCATE_FAIL path=", resource->path);
    return libc_error(status);
}

int ftruncate64(int descriptor, off_t length) {
    return ftruncate(descriptor, length);
}

int truncate(const char *path, off_t length) {
    int descriptor;
    int result;
    int saved_errno;
    if (path == 0 || length < 0) {
        errno = EINVAL;
        return -1;
    }
    descriptor = open(path, O_WRONLY);
    if (descriptor < 0) return -1;
    result = ftruncate(descriptor, length);
    saved_errno = errno;
    if (close(descriptor) < 0 && result == 0) return -1;
    if (result < 0) errno = saved_errno;
    return result;
}

int truncate64(const char *path, off_t length) {
    return truncate(path, length);
}

void sync(void) {
    initialize_fd_table();
    for (unsigned int descriptor = 0U; descriptor < LIBC_FD_LIMIT;
         ++descriptor) {
        if (g_fd_table[descriptor].resource != 0) {
            (void)fsync((int)descriptor);
        }
    }
}

int stat(const char *path, struct stat *status) {
    os_file_stat_t request = {0};
    char absolute_path[LIBC_PATH_LIMIT];
    int64_t result;
    if (status == 0) {
        errno = EINVAL;
        return -1;
    }
    if (!make_absolute_path(path, absolute_path)) {
        return -1;
    }
    if (linux_abi_path(absolute_path) && libc_linux_personality()) {
        libc_linux_stat_t linux_status = {0};
        result = __libc_linux_syscall6(
            LINUX_SYS_STAT, (uint64_t)(uintptr_t)absolute_path,
            (uint64_t)(uintptr_t)&linux_status, 0U, 0U, 0U, 0U);
        if (result < 0) return libc_error(result);
        fill_linux_stat(&linux_status, status);
        return 0;
    }
    request.hdr.size = sizeof(request);
    request.hdr.version = OS_SYSCALL_ABI_VERSION;
    request.path = (uint64_t)(uintptr_t)absolute_path;
    result = __libc_native_syscall6(OS_SYS_FILE_STAT,
                             (uint64_t)(uintptr_t)&request,
                             0U, 0U, 0U, 0U, 0U);
    if (result < 0) return libc_error(result);
    fill_stat(&request.info, status);
    libc_set_file_identity(absolute_path, status);
    apply_linux_mode_override(absolute_path, status);
    return 0;
}

int lstat(const char *path, struct stat *status) {
    /* System 没有符号链接 vnode 类型；stat 与 lstat 的行为相同。 */
    return stat(path, status);
}

int fstatat(int directory_descriptor, const char *path,
            struct stat *status, int flags) {
    char absolute_path[LIBC_PATH_LIMIT];
    if ((flags & ~AT_SYMLINK_NOFOLLOW) != 0) {
        errno = EINVAL;
        return -1;
    }
    if (!make_path_at(directory_descriptor, path, absolute_path)) return -1;
    return stat(absolute_path, status);
}

int fstat(int descriptor, struct stat *status) {
    libc_fd_resource_t *resource;
    os_file_handle_stat_t request = {0};
    int64_t result;
    if (status == 0) {
        errno = EINVAL;
        return -1;
    }
    resource = resource_for_descriptor(descriptor);
    if (resource == 0) return -1;
    if (resource->linux_fd) {
        libc_linux_stat_t linux_status = {0};
        result = __libc_linux_syscall6(
            LINUX_SYS_FSTAT, (uint64_t)g_fd_table[descriptor].handle,
            (uint64_t)(uintptr_t)&linux_status, 0U, 0U, 0U, 0U);
        if (result < 0) return libc_error(result);
        fill_linux_stat(&linux_status, status);
        return 0;
    }
    if (resource->debug_output) {
        memset(status, 0, sizeof(*status));
        status->st_mode = S_IFCHR | S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP |
                          S_IROTH | S_IWOTH;
        status->st_nlink = 1U;
        return 0;
    }
    if (resource->pipe_handle) {
        memset(status, 0, sizeof(*status));
        status->st_mode = S_IFIFO | S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP |
                          S_IROTH | S_IWOTH;
        status->st_nlink = 1U;
        return 0;
    }
    request.hdr.size = sizeof(request);
    request.hdr.version = OS_SYSCALL_ABI_VERSION;
    request.handle = g_fd_table[descriptor].handle;
    result = __libc_native_syscall6(OS_SYS_FILE_FSTAT,
                             (uint64_t)(uintptr_t)&request,
                             0U, 0U, 0U, 0U, 0U);
    if (result < 0) {
        if (libc_debug_budget(&g_libc_fstat_fail_debug_calls))
            libc_debug_path("OS_LIBC_FSTAT_FAIL path=", resource->path);
        return libc_error(result);
    }
    fill_stat(&request.info, status);
    libc_set_file_identity(resource->path, status);
    return 0;
}

int access(const char *path, int mode) {
    struct stat status;
    if ((mode & ~(R_OK | W_OK | X_OK)) != 0 || stat(path, &status) < 0) return -1;
    if (mode == F_OK) return 0;
    if ((mode & R_OK) != 0 && (status.st_mode & (S_IRUSR | S_IRGRP | S_IROTH)) == 0) {
        errno = EACCES;
        return -1;
    }
    if ((mode & W_OK) != 0 && (status.st_mode & (S_IWUSR | S_IWGRP | S_IWOTH)) == 0) {
        errno = EACCES;
        return -1;
    }
    if ((mode & X_OK) != 0 && (status.st_mode & (S_IXUSR | S_IXGRP | S_IXOTH)) == 0) {
        errno = EACCES;
        return -1;
    }
    return 0;
}

int faccessat(int directory_descriptor, const char *path, int mode, int flags) {
    char absolute_path[LIBC_PATH_LIMIT];
    if (flags != 0 && flags != AT_EACCESS) {
        errno = EINVAL;
        return -1;
    }
    if (!make_path_at(directory_descriptor, path, absolute_path)) return -1;
    return access(absolute_path, mode);
}

static int path_operation(const char *path, uint64_t syscall_number,
                          uint32_t mode) {
    os_file_path_op_t request = {0};
    char absolute_path[LIBC_PATH_LIMIT];
    if (!make_absolute_path(path, absolute_path)) return -1;
    request.hdr.size = sizeof(request);
    request.hdr.version = OS_SYSCALL_ABI_VERSION;
    request.path = (uint64_t)(uintptr_t)absolute_path;
    request.mode = mode;
    return libc_error(__libc_native_syscall6(syscall_number,
                                      (uint64_t)(uintptr_t)&request,
                                      0U, 0U, 0U, 0U, 0U));
}

int unlink(const char *path) {
    struct stat status;
    if (stat(path, &status) < 0) return -1;
    if (S_ISDIR(status.st_mode)) {
        errno = EISDIR;
        return -1;
    }
    return path_operation(path, OS_SYS_FILE_REMOVE, 0U);
}

int unlinkat(int directory_descriptor, const char *path, int flags) {
    char absolute_path[LIBC_PATH_LIMIT];
    if (flags != 0 && flags != AT_REMOVEDIR) {
        errno = EINVAL;
        return -1;
    }
    if (!make_path_at(directory_descriptor, path, absolute_path)) return -1;
    return (flags & AT_REMOVEDIR) != 0 ? rmdir(absolute_path) :
                                        unlink(absolute_path);
}

int rmdir(const char *path) {
    struct stat status;
    if (stat(path, &status) < 0) return -1;
    if (!S_ISDIR(status.st_mode)) {
        errno = ENOTDIR;
        return -1;
    }
    return path_operation(path, OS_SYS_FILE_REMOVE, 0U);
}

int mkdir(const char *path, unsigned int mode) {
    char absolute_path[LIBC_PATH_LIMIT];
    int result = path_operation(path, OS_SYS_FILE_MKDIR, mode);
    if (result == 0 && libc_linux_personality() &&
        make_absolute_path(path, absolute_path)) {
        remember_linux_mode(absolute_path, (mode_t)mode);
    }
    return result;
}

int mkdirat(int directory_descriptor, const char *path, unsigned int mode) {
    char absolute_path[LIBC_PATH_LIMIT];
    if (!make_path_at(directory_descriptor, path, absolute_path)) return -1;
    return mkdir(absolute_path, mode);
}

int rename(const char *old_path, const char *new_path) {
    os_file_rename_t request = {0};
    char absolute_old[LIBC_PATH_LIMIT];
    char absolute_new[LIBC_PATH_LIMIT];
    if (!make_absolute_path(old_path, absolute_old) ||
        !make_absolute_path(new_path, absolute_new)) return -1;
    request.hdr.size = sizeof(request);
    request.hdr.version = OS_SYSCALL_ABI_VERSION;
    request.old_path = (uint64_t)(uintptr_t)absolute_old;
    request.new_path = (uint64_t)(uintptr_t)absolute_new;
    return libc_error(__libc_native_syscall6(OS_SYS_FILE_RENAME,
                                      (uint64_t)(uintptr_t)&request,
                                      0U, 0U, 0U, 0U, 0U));
}

int renameat(int old_directory_descriptor, const char *old_path,
             int new_directory_descriptor, const char *new_path) {
    char absolute_old[LIBC_PATH_LIMIT];
    char absolute_new[LIBC_PATH_LIMIT];
    if (!make_path_at(old_directory_descriptor, old_path, absolute_old) ||
        !make_path_at(new_directory_descriptor, new_path, absolute_new)) {
        return -1;
    }
    return rename(absolute_old, absolute_new);
}

char *realpath(const char *path, char *resolved) {
    char absolute_path[LIBC_PATH_LIMIT];
    struct stat status;
    size_t length;
    if (!make_absolute_path(path, absolute_path)) return 0;
    if (stat(absolute_path, &status) < 0) return 0;
    length = strlen(absolute_path) + 1U;
    if (resolved == 0) {
        resolved = (char *)malloc(length);
        if (resolved == 0) return 0;
    }
    memcpy(resolved, absolute_path, length);
    return resolved;
}

int chdir(const char *path) {
    struct stat status;
    char absolute_path[LIBC_PATH_LIMIT];
    if (!make_absolute_path(path, absolute_path) ||
        stat(absolute_path, &status) < 0) return -1;
    if (!S_ISDIR(status.st_mode)) {
        errno = ENOTDIR;
        return -1;
    }
    strcpy(g_current_directory, absolute_path);
    return 0;
}

int fchdir(int descriptor) {
    char path[LIBC_PATH_LIMIT];
    if (__libc_descriptor_path(descriptor, path, sizeof(path)) < 0) return -1;
    return chdir(path);
}

char *getcwd(char *buffer, size_t size) {
    size_t length = strlen(g_current_directory) + 1U;
    if (buffer == 0) {
        buffer = (char *)malloc(length);
        if (buffer == 0) return 0;
        size = length;
    }
    if (size < length) {
        errno = ERANGE;
        return 0;
    }
    memcpy(buffer, g_current_directory, length);
    return buffer;
}

int gethostname(char *buffer, size_t size) {
    static const char hostname[] = "localhost";
    const size_t hostname_size = sizeof(hostname);
    if (buffer == 0 || size == 0U) {
        errno = EINVAL;
        return -1;
    }
    if (size < hostname_size) {
        memcpy(buffer, hostname, size);
        errno = ENAMETOOLONG;
        return -1;
    }
    memcpy(buffer, hostname, hostname_size);
    return 0;
}

int isatty(int descriptor) {
    libc_fd_resource_t *resource = resource_for_descriptor(descriptor);
    if (resource == 0) return 0;
    if (resource->debug_output || descriptor == STDIN_FILENO) return 1;
    errno = ENOTTY;
    return 0;
}

static long pathconf_value(int name) {
    switch (name) {
    case _PC_LINK_MAX: return 1L;
    case _PC_MAX_CANON: return 255L;
    case _PC_MAX_INPUT: return 255L;
    case _PC_NAME_MAX: return (long)OS_FILE_NAME_MAX - 1L;
    case _PC_PATH_MAX: return (long)PATH_MAX - 1L;
    case _PC_PIPE_BUF: return 4096L;
    case _PC_CHOWN_RESTRICTED: return 1L;
    case _PC_NO_TRUNC: return 1L;
    case _PC_VDISABLE: return -1L;
    case _PC_FILESIZEBITS: return 64L;
    default:
        errno = EINVAL;
        return -1L;
    }
}

long pathconf(const char *path, int name) {
    struct stat status;
    if (path == 0 || stat(path, &status) < 0) return -1L;
    return pathconf_value(name);
}

long fpathconf(int descriptor, int name) {
    struct stat status;
    if (fstat(descriptor, &status) < 0) return -1L;
    return pathconf_value(name);
}

size_t confstr(int name, char *buffer, size_t size) {
    const char *value;
    size_t length;
    size_t copy_length;
    if (name != _CS_PATH) {
        errno = EINVAL;
        return 0U;
    }
    value = "/sbin:/bin";
    length = strlen(value);
    if (buffer == 0 || size == 0U) return length;
    copy_length = length < size - 1U ? length : size - 1U;
    memcpy(buffer, value, copy_length);
    buffer[copy_length] = '\0';
    return length;
}

static int duplicate_descriptor(int descriptor, unsigned int minimum,
                                uint32_t handle_flags);

int dup(int descriptor) {
    return duplicate_descriptor(descriptor, 3U, 0U);
}

int getdtablesize(void) {
    long value = sysconf(_SC_OPEN_MAX);
    if (value < 0 || value > INT_MAX) {
        if (errno == 0) errno = EOVERFLOW;
        return -1;
    }
    return (int)value;
}

static int duplicate_kernel_handle(int descriptor, uint32_t flags,
                                   os_handle_t *duplicate) {
    libc_fd_resource_t *resource = resource_for_descriptor(descriptor);
    int64_t status;
    if (resource == 0 || duplicate == 0) return -1;
    if (resource->debug_output) {
        *duplicate = g_fd_table[descriptor].handle;
        return 0;
    }
    if (resource->linux_fd) {
        status = __libc_linux_syscall6(
            LINUX_SYS_DUP, (uint64_t)g_fd_table[descriptor].handle,
            0U, 0U, 0U, 0U, 0U);
        if (status < 0) return libc_error(status);
        *duplicate = (os_handle_t)(uint32_t)status;
        return 0;
    }
    status = __libc_native_syscall6(OS_SYS_HANDLE_DUP,
                             g_fd_table[descriptor].handle,
                             flags, (uint64_t)(uintptr_t)duplicate,
                             0U, 0U, 0U);
    return status < 0 ? libc_error(status) : 0;
}

static int duplicate_descriptor(int descriptor, unsigned int minimum,
                                uint32_t handle_flags) {
    libc_fd_resource_t *resource = resource_for_descriptor(descriptor);
    os_handle_t duplicate = OS_INVALID_HANDLE;
    int new_descriptor;
    if (resource == 0) return -1;
    libc_fd_table_lock();
    if (descriptor < 0 || (unsigned int)descriptor >= LIBC_FD_LIMIT ||
        g_fd_table[descriptor].resource != resource) {
        libc_fd_table_unlock();
        errno = EBADF;
        return -1;
    }
    if (resource->linux_fd) {
        unsigned int candidate = minimum;
        uint32_t command = (handle_flags & OS_HANDLE_FLAG_CLOEXEC) != 0U ?
            LINUX_F_DUPFD_CLOEXEC : LINUX_F_DUPFD;
        while (candidate < LIBC_FD_LIMIT) {
            int64_t status = __libc_linux_syscall6(
                LINUX_SYS_FCNTL,
                (uint64_t)g_fd_table[descriptor].handle,
                command, (uint64_t)candidate, 0U, 0U, 0U);
            if (status < 0) {
                libc_fd_table_unlock();
                return libc_error(status);
            }
            if ((uint64_t)status >= LIBC_FD_LIMIT) {
                close_raw_linux_descriptor((int)status);
                libc_fd_table_unlock();
                errno = EMFILE;
                return -1;
            }
            new_descriptor = (int)status;
            if (g_fd_table[new_descriptor].resource != 0) {
                close_raw_linux_descriptor(new_descriptor);
                candidate = (unsigned int)new_descriptor + 1U;
                continue;
            }
            g_fd_table[new_descriptor].resource = resource;
            g_fd_table[new_descriptor].handle =
                (os_handle_t)(uint32_t)new_descriptor;
            g_fd_table[new_descriptor].descriptor_flags =
                (handle_flags & OS_HANDLE_FLAG_CLOEXEC) != 0U ?
                    FD_CLOEXEC : 0U;
            ++resource->references;
            libc_fd_table_unlock();
            return new_descriptor;
        }
        libc_fd_table_unlock();
        errno = EMFILE;
        return -1;
    }
    if (duplicate_kernel_handle(descriptor, handle_flags, &duplicate) < 0) {
        libc_fd_table_unlock();
        return -1;
    }
    new_descriptor = allocate_descriptor_locked(resource, minimum);
    if (new_descriptor < 0) {
        if (!resource->debug_output) {
            if (resource->linux_fd) {
                (void)__libc_linux_syscall6(
                    LINUX_SYS_CLOSE, (uint64_t)duplicate,
                    0U, 0U, 0U, 0U, 0U);
            } else {
            (void)__libc_native_syscall6(OS_SYS_HANDLE_CLOSE, duplicate,
                                  0U, 0U, 0U, 0U, 0U);
        }
        }
        libc_fd_table_unlock();
        return -1;
    }
    g_fd_table[new_descriptor].handle = duplicate;
    g_fd_table[new_descriptor].descriptor_flags =
        (handle_flags & OS_HANDLE_FLAG_CLOEXEC) != 0U ? FD_CLOEXEC : 0U;
    libc_fd_table_unlock();
    return new_descriptor;
}

static int duplicate_to_descriptor(int old_descriptor, int new_descriptor,
                                   uint32_t handle_flags) {
    libc_fd_resource_t *resource = resource_for_descriptor(old_descriptor);
    os_handle_t duplicate = OS_INVALID_HANDLE;
    if (resource == 0 || new_descriptor < 0 ||
        (unsigned int)new_descriptor >= LIBC_FD_LIMIT) {
        errno = EBADF;
        return -1;
    }
    libc_fd_table_lock();
    if (old_descriptor < 0 || (unsigned int)old_descriptor >= LIBC_FD_LIMIT ||
        g_fd_table[old_descriptor].resource != resource) {
        libc_fd_table_unlock();
        errno = EBADF;
        return -1;
    }
    if (old_descriptor == new_descriptor) {
        libc_fd_table_unlock();
        return new_descriptor;
    }
    if (resource != 0 && resource->linux_fd) {
        int64_t status = __libc_linux_syscall6(
            LINUX_SYS_DUP3, (uint64_t)g_fd_table[old_descriptor].handle,
            (uint64_t)(uint32_t)new_descriptor,
            (handle_flags & OS_HANDLE_FLAG_CLOEXEC) != 0U ?
                LINUX_O_CLOEXEC : 0U, 0U, 0U, 0U);
        if (status < 0) {
            libc_fd_table_unlock();
            return libc_error(status);
        }
        if (g_fd_table[new_descriptor].resource != 0) {
            libc_fd_resource_t *replaced =
                g_fd_table[new_descriptor].resource;
            if (replaced->linux_fd) {
                /* dup3 已关闭旧 Linux 描述符，并在此编号安装了替换项。这里只释放 libc
                 * 的引用；若在此处调用 close(new_descriptor)，就会关闭刚创建的新描述符。 */
                release_resource_reference(replaced);
            } else {
                /* 此编号槽位可能原先持有原生 System 句柄，该句柄不属于 Linux dup3 表，
                 * 仍需将其关闭。 */
                (void)release_descriptor(new_descriptor);
            }
            g_fd_table[new_descriptor].resource = 0;
            g_fd_table[new_descriptor].handle = OS_INVALID_HANDLE;
            g_fd_table[new_descriptor].descriptor_flags = 0U;
        }
        g_fd_table[new_descriptor].resource = resource;
        g_fd_table[new_descriptor].handle = (os_handle_t)(uint32_t)status;
        g_fd_table[new_descriptor].descriptor_flags =
            (handle_flags & OS_HANDLE_FLAG_CLOEXEC) != 0U ? FD_CLOEXEC : 0U;
        ++resource->references;
        libc_fd_table_unlock();
        return new_descriptor;
    }
    if (duplicate_kernel_handle(old_descriptor, handle_flags, &duplicate) < 0) {
        libc_fd_table_unlock();
        return -1;
    }
    if (g_fd_table[new_descriptor].resource != 0)
        (void)close_descriptor_locked(new_descriptor);
    g_fd_table[new_descriptor].resource = resource;
    g_fd_table[new_descriptor].handle = duplicate;
    g_fd_table[new_descriptor].descriptor_flags =
        (handle_flags & OS_HANDLE_FLAG_CLOEXEC) != 0U ? FD_CLOEXEC : 0U;
    ++resource->references;
    libc_fd_table_unlock();
    return new_descriptor;
}

int dup2(int old_descriptor, int new_descriptor) {
    return duplicate_to_descriptor(old_descriptor, new_descriptor, 0U);
}

int dup3(int old_descriptor, int new_descriptor, int flags) {
    if ((flags & ~O_CLOEXEC) != 0 || old_descriptor == new_descriptor) {
        errno = EINVAL;
        return -1;
    }
    return duplicate_to_descriptor(
        old_descriptor, new_descriptor,
        (flags & O_CLOEXEC) != 0 ? OS_HANDLE_FLAG_CLOEXEC : 0U);
}

int fcntl(int descriptor, int command, ...) {
    libc_fd_resource_t *resource = resource_for_descriptor(descriptor);
    va_list arguments;
    int value;
    if (resource == 0) return -1;
    va_start(arguments, command);
    switch (command) {
    case F_GETFD:
        if (resource->debug_output) {
            value = (int)g_fd_table[descriptor].descriptor_flags;
        } else if (resource->linux_fd) {
            int64_t status = __libc_linux_syscall6(
                LINUX_SYS_FCNTL, (uint64_t)g_fd_table[descriptor].handle,
                LINUX_F_GETFD, 0U, 0U, 0U, 0U);
            value = status < 0 ? libc_error(status) : (int)status;
        } else {
            int64_t status = __libc_native_syscall6(
                OS_SYS_HANDLE_GET_FLAGS, g_fd_table[descriptor].handle,
                0U, 0U, 0U, 0U, 0U);
            if (status < 0) {
                value = libc_error(status);
            } else {
                value = (int)((uint32_t)status & FD_CLOEXEC);
                g_fd_table[descriptor].descriptor_flags = (uint32_t)value;
            }
        }
        break;
    case F_SETFD:
        value = va_arg(arguments, int);
        value = (uint32_t)value & FD_CLOEXEC;
        if (resource->linux_fd) {
            int64_t status = __libc_linux_syscall6(
                LINUX_SYS_FCNTL, (uint64_t)g_fd_table[descriptor].handle,
                LINUX_F_SETFD,
                (value & FD_CLOEXEC) != 0 ? LINUX_FD_CLOEXEC : 0U,
                0U, 0U, 0U);
            if (status < 0) {
                value = libc_error(status);
                break;
            }
        } else if (!resource->debug_output) {
            int64_t status = __libc_native_syscall6(
                OS_SYS_HANDLE_SET_FLAGS, g_fd_table[descriptor].handle,
                (value & FD_CLOEXEC) != 0 ? OS_HANDLE_FLAG_CLOEXEC : 0U,
                0U, 0U, 0U, 0U);
            if (status < 0) {
                value = libc_error(status);
                break;
            }
        }
        g_fd_table[descriptor].descriptor_flags = (uint32_t)value;
        value = 0;
        break;
    case F_GETFL:
        if (resource->linux_fd) {
            int64_t status = __libc_linux_syscall6(
                LINUX_SYS_FCNTL, (uint64_t)g_fd_table[descriptor].handle,
                LINUX_F_GETFL, 0U, 0U, 0U, 0U);
            if (status < 0) {
                value = libc_error(status);
                break;
            }
            value = (int)(status & (LINUX_O_WRONLY | LINUX_O_RDWR));
            if ((status & LINUX_O_APPEND) != 0U) value =
                value | O_APPEND;
            if ((status & LINUX_O_NONBLOCK) != 0U) value =
                value | O_NONBLOCK;
            if ((status & LINUX_O_DIRECTORY) != 0U) value |= O_DIRECTORY;
        } else {
        value = (int)resource->open_flags;
        }
        break;
    case F_SETFL:
        value = va_arg(arguments, int);
        if (resource->linux_fd) {
            uint32_t linux_flags = 0U;
            if ((value & O_APPEND) != 0) linux_flags |= LINUX_O_APPEND;
            if ((value & O_NONBLOCK) != 0) linux_flags |= LINUX_O_NONBLOCK;
            int64_t status = __libc_linux_syscall6(
                LINUX_SYS_FCNTL, (uint64_t)g_fd_table[descriptor].handle,
                LINUX_F_SETFL, linux_flags, 0U, 0U, 0U);
            if (status < 0) {
                value = libc_error(status);
                break;
            }
            resource->open_flags = (resource->open_flags &
                                    ~(O_APPEND | O_NONBLOCK)) |
                                   ((uint32_t)value & (O_APPEND | O_NONBLOCK));
            value = 0;
        } else {
        resource->open_flags = (resource->open_flags &
                                ~(O_APPEND | O_NONBLOCK)) |
                               ((uint32_t)value & (O_APPEND | O_NONBLOCK));
        value = 0;
        }
        break;
    case F_GETLK:
    case F_SETLK:
    case F_SETLKW: {
        /* System 当前以单进程方式运行 Chromium 浏览器，且不提供内核级 POSIX 记录锁表。
         * 但 SQLite 的 Unix VFS 仍会为每个数据库探测并获取这些建议锁。在此模式下将锁
         * 视为无竞争；对于 F_GETLK，按 POSIX 要求返回“无冲突锁”。浏览器自身的 SQLite
         * 连接状态仍会串行化线程间访问，同时保证 sqlite3_open_v2() 可用。 */
        struct flock *lock = va_arg(arguments, struct flock *);
        if (lock == 0) {
            errno = EFAULT;
            value = -1;
            break;
        }
        if (command == F_GETLK) {
            lock->l_type = F_UNLCK;
            lock->l_pid = 0;
        }
        if (libc_trace_budget(&g_libc_fcntl_lock_debug_calls, 64U)) {
            libc_debug_text("OS_LIBC_FCNTL_LOCK path=",
                            sizeof("OS_LIBC_FCNTL_LOCK path=") - 1U);
            libc_debug_text(resource->path, strlen(resource->path));
            libc_debug_text("\n", 1U);
        }
        value = 0;
        break;
    }
    case F_DUPFD:
    case F_DUPFD_CLOEXEC:
        value = va_arg(arguments, int);
        if (value < 0) {
            errno = EINVAL;
            value = -1;
        } else {
            value = duplicate_descriptor(
                descriptor, (unsigned int)value,
                command == F_DUPFD_CLOEXEC ? OS_HANDLE_FLAG_CLOEXEC : 0U);
        }
        break;
    default:
        /* 未知的 fcntl 命令属于无效请求，而非缺失的系统调用；这符合 POSIX 错误约定。 */
        errno = EINVAL;
        value = -1;
        break;
    }
    va_end(arguments);
    return value;
}

int ioctl(int descriptor, unsigned long request, ...) {
    (void)request;
    if (resource_for_descriptor(descriptor) == 0) return -1;
    /* System ABI 尚未公开设备专用的 ioctl 命名空间。 */
    errno = ENOTTY;
    return -1;
}

static int process_info(os_process_info_t *info) {
    int64_t status;
    if (info == 0) {
        errno = EINVAL;
        return -1;
    }
    memset(info, 0, sizeof(*info));
    info->hdr.size = sizeof(*info);
    info->hdr.version = OS_SYSCALL_ABI_VERSION;
    status = __libc_native_syscall6(OS_SYS_PROCESS_INFO,
                             (uint64_t)(uintptr_t)info,
                             0U, 0U, 0U, 0U, 0U);
    return status < 0 ? libc_error(status) : 0;
}

int getpid(void) {
    os_process_info_t info;
    if (process_info(&info) < 0 || info.pid > INT_MAX) {
        if (errno == 0) errno = EOVERFLOW;
        return -1;
    }
    return (int)info.pid;
}

int getppid(void) {
    os_process_info_t info;
    if (process_info(&info) < 0 || info.parent_pid > INT_MAX) {
        if (errno == 0) errno = EOVERFLOW;
        return -1;
    }
    return (int)info.parent_pid;
}
int getuid(void) { return 0; }
int geteuid(void) { return 0; }
int getgid(void) { return 0; }
int getegid(void) { return 0; }

__attribute__((force_align_arg_pointer))
int prctl(int option, ...) {
    va_list arguments;
    uint64_t argument0 = 0U;
    int64_t status;
    switch (option) {
    case PR_SET_PDEATHSIG:
    case PR_GET_DUMPABLE:
    case PR_SET_DUMPABLE:
    case PR_SET_NAME:
    case PR_GET_NAME:
        va_start(arguments, option);
        argument0 = va_arg(arguments, uint64_t);
        va_end(arguments);
        break;
    default:
        break;
    }
    if (!libc_linux_personality()) {
    errno = ENOSYS;
    return -1;
}
    status = __libc_linux_syscall6(LINUX_SYS_PRCTL, (uint64_t)option,
                                    argument0, 0U, 0U, 0U, 0U);
    return status < 0 ? __libc_status_result(status) : 0;
}

ssize_t readlink(const char *path, char *buffer, size_t size) {
    int64_t status;
    if (!libc_linux_personality()) {
        errno = ENOSYS;
        return -1;
    }
    status = __libc_linux_syscall6(
        LINUX_SYS_READLINK, (uint64_t)(uintptr_t)path,
        (uint64_t)(uintptr_t)buffer, (uint64_t)size, 0U, 0U, 0U);
    if (status < 0) {
        (void)libc_error(status);
        return -1;
    }
    return (ssize_t)status;
}

ssize_t readlinkat(int directory_descriptor, const char *path,
                   char *buffer, size_t size) {
    if (directory_descriptor == AT_FDCWD ||
        (path != 0 && path[0] == '/')) {
        return readlink(path, buffer, size);
    }
    (void)path;
    (void)buffer;
    (void)size;
    errno = ENOSYS;
    return -1;
}

int link(const char *old_path, const char *new_path) {
    (void)old_path;
    (void)new_path;
    errno = ENOSYS;
    return -1;
}

int symlink(const char *target, const char *link_path) {
    if (libc_linux_personality()) {
        return __libc_status_result(__libc_linux_syscall6(
            LINUX_SYS_SYMLINK, (uint64_t)(uintptr_t)target,
            (uint64_t)(uintptr_t)link_path, 0U, 0U, 0U, 0U));
    }
    (void)target;
    (void)link_path;
    errno = ENOSYS;
    return -1;
}

int chmod(const char *path, mode_t mode) {
    char absolute_path[LIBC_PATH_LIMIT];
    if (!libc_linux_personality()) {
        errno = ENOSYS;
        return -1;
    }
    if (!make_absolute_path(path, absolute_path) ||
        access(absolute_path, F_OK) < 0) return -1;
    remember_linux_mode(absolute_path, mode);
    return 0;
}

int fchmod(int descriptor, mode_t mode) {
    (void)descriptor;
    (void)mode;
    errno = ENOSYS;
    return -1;
}

int fchmodat(int directory_descriptor, const char *path, mode_t mode,
             int flags) {
    (void)directory_descriptor;
    (void)path;
    (void)mode;
    (void)flags;
    errno = ENOSYS;
    return -1;
}

pid_t setsid(void) {
    errno = ENOSYS;
    return (pid_t)-1;
}

int setpgid(pid_t pid, pid_t process_group) {
    (void)pid;
    (void)process_group;
    errno = ENOSYS;
    return -1;
}

unsigned int sleep(unsigned int seconds) {
    struct timespec request = {(time_t)seconds, 0};
    return nanosleep(&request, 0) == 0 ? 0U : seconds;
}

int usleep(unsigned int microseconds) {
    struct timespec request = {
        (time_t)(microseconds / 1000000U),
        (long)((microseconds % 1000000U) * 1000U),
    };
    return nanosleep(&request, 0);
}

int pause(void) {
    /* 避免子进程沙箱等待时忙循环。支持信号递送时，已递送的信号会中断 nanosleep；
     * 即使进程正在休眠，SIGKILL 仍会将其终止。 */
    for (;;) {
        struct timespec request = {(time_t)3600, 0};
        if (nanosleep(&request, 0) < 0) return -1;
    }
}

int clock_gettime(int clock_id, struct timespec *value) {
    if (libc_linux_personality()) {
        struct {
            int64_t seconds;
            int64_t nanoseconds;
        } linux_value = {0};
        int64_t linux_status = __libc_linux_syscall6(
            LINUX_SYS_CLOCK_GETTIME, (uint64_t)(uint32_t)clock_id,
            (uint64_t)(uintptr_t)&linux_value, 0U, 0U, 0U, 0U);
        if (linux_status < 0) return libc_error(linux_status);
        value->tv_sec = (time_t)linux_value.seconds;
        value->tv_nsec = (long)linux_value.nanoseconds;
        return 0;
    }
    os_timespec_t raw = {0};
    int64_t status;
    if (value == 0) {
        errno = EINVAL;
        return -1;
    }
    status = __libc_native_syscall6(OS_SYS_CLOCK_GET, (uint64_t)(uint32_t)clock_id,
                             (uint64_t)(uintptr_t)&raw, 0U, 0U, 0U, 0U);
    if (status < 0) return libc_error(status);
    value->tv_sec = raw.seconds;
    value->tv_nsec = raw.nanoseconds;
    return 0;
}

int clock_settime(int clock_id, const struct timespec *value) {
    os_clock_set_t request = {0};
    int64_t status;
    if (value == 0 || value->tv_nsec < 0 || value->tv_nsec >= 1000000000L) {
        errno = EINVAL;
        return -1;
    }
    request.hdr.size = sizeof(request);
    request.hdr.version = OS_SYSCALL_ABI_VERSION;
    request.clock_id = (uint32_t)clock_id;
    request.value.seconds = value->tv_sec;
    request.value.nanoseconds = (int32_t)value->tv_nsec;
    status = __libc_native_syscall6(OS_SYS_CLOCK_SET,
                             (uint64_t)(uintptr_t)&request,
                             0U, 0U, 0U, 0U, 0U);
    return libc_error(status);
}

int nanosleep(const struct timespec *request, struct timespec *remaining) {
    if (libc_linux_personality()) {
        int64_t linux_status = __libc_linux_syscall6(
            LINUX_SYS_NANOSLEEP, (uint64_t)(uintptr_t)request,
            (uint64_t)(uintptr_t)remaining, 0U, 0U, 0U, 0U);
        if (linux_status < 0) return libc_error(linux_status);
        return 0;
    }
    uint64_t delay_ns;
    os_handle_t timer = OS_INVALID_HANDLE;
    os_wait_result_t result = {0};
    int64_t status;
    if (request == 0 || request->tv_sec < 0 || request->tv_nsec < 0 ||
        request->tv_nsec >= 1000000000L) {
        errno = EINVAL;
        return -1;
    }
    if ((uint64_t)request->tv_sec >
        (UINT64_MAX - (uint64_t)request->tv_nsec) / 1000000000ULL) {
        errno = EOVERFLOW;
        return -1;
    }
    delay_ns = (uint64_t)request->tv_sec * 1000000000ULL +
               (uint64_t)request->tv_nsec;
    status = __libc_native_syscall6(OS_SYS_TIMER_CREATE, delay_ns, 0U,
                             (uint64_t)(uintptr_t)&timer, 0U, 0U, 0U);
    if (status < 0) return libc_error(status);
    status = __libc_native_syscall6(OS_SYS_WAIT_ONE, timer, OS_WAIT_INFINITE,
                             (uint64_t)(uintptr_t)&result, 0U, 0U, 0U);
    (void)__libc_native_syscall6(OS_SYS_HANDLE_CLOSE, timer, 0U, 0U, 0U, 0U, 0U);
    if (status < 0) {
        if (remaining != 0) *remaining = *request;
        return libc_error(status);
    }
    if (remaining != 0) *remaining = (struct timespec){0, 0};
    return 0;
}
