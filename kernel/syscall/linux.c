#include <arch/x86_64/cpu.h>
#include <uapi/window_state.h>
#include <arch/x86_64/acpi.h>
#include <arch/x86_64/paging.h>
#include <arch/x86_64/smp.h>
#include <arch/x86_64/uaccess.h>
#include <kernel/console.h>
#include <kernel/clock.h>
#include <kernel/futex.h>
#include <kernel/file_lock.h>
#include <kernel/frame_trace.h>
#include <kernel/kmem.h>
#include <kernel/mutex.h>
#include <kernel/mm.h>
#include <kernel/nv_frontend.h>
#include <kernel/object.h>
#include <kernel/pipe.h>
#include <kernel/process.h>
#include <kernel/sched.h>
#include <kernel/socket.h>
#include <kernel/string.h>
#include <kernel/timer.h>
#include <kernel/wait.h>
#include <kernel/vfs.h>
#include <kernel/vm.h>
#include <kernel/window_server.h>
#include <uapi/display.h>
#include <uapi/linux_abi.h>
#include <uapi/nvidia_frontend.h>
#include <uapi/syscall.h>
#include <uapi/time.h>
#include <uapi/wait.h>

#include "internal.h"
#include "../process/internal.h"
#include "../mm/internal.h"

#ifndef OS_DEBUG_SERIAL
#define OS_DEBUG_SERIAL 0
#endif

/* NVIDIA Vulkan 运行时使用的 Linux x86-64 旧版墙上时钟。 */
#define LINUX_SYS_GETTIMEOFDAY 96U
#define LINUX_MAP_32BIT 0x40U
#define LINUX_MAP_NORESERVE 0x4000U

static uint32_t g_linux_fd_exhaustion_logs;
static uint32_t g_linux_tmp_open_failure_logs;
static uint32_t g_linux_mmap_fail_logs;
static uint32_t g_linux_chrome_vm_fail_logs;
static uint32_t g_linux_chrome_stderr_bytes;
static uint32_t g_linux_chrome_trace_bytes;

#define LINUX_DEBUG_COM1_DATA   0x3F8U
#define LINUX_DEBUG_COM1_STATUS 0x3FDU
#define LINUX_CHROME_STDERR_LIMIT (16U * 1024U)
#define LINUX_CHROME_UART_POLL_LIMIT 128U

/* 默认关闭诊断串口；需要诊断时可随串口构建开关启用，或由调试器显式开启。 */
static bool g_linux_chrome_uart_disabled = !OS_DEBUG_SERIAL;

static uint8_t linux_chrome_uart_status(void) {
    uint8_t status;
    __asm__ volatile ("inb %w1, %0" : "=a"(status) : "d"(LINUX_DEBUG_COM1_STATUS));
    return status;
}

static void linux_chrome_uart_write(uint8_t value) {
    __asm__ volatile ("outb %0, %w1" : : "a"(value), "d"(LINUX_DEBUG_COM1_DATA));
}

static bool linux_process_is_chrome_image(const process_t *process) {
    static const char path[] = "/usr/apps/chrome/chrome";
    if (process == 0) return false;
    for (size_t index = 0U; index < sizeof(path); ++index) {
        if (process->linux_exe[index] != path[index]) return false;
    }
    return true;
}

static void linux_chrome_diag_char(char value) {
    /* 不可因 UART 缺失或卡住而延迟系统调用。尽力输出失败一次后，
     * 就禁用该诊断输出通道，而不是每个字节都重试。 */
    if (__atomic_load_n(&g_linux_chrome_uart_disabled, __ATOMIC_RELAXED)) return;
    for (uint32_t attempt = 0U; attempt < LINUX_CHROME_UART_POLL_LIMIT; ++attempt) {
        uint8_t ready = linux_chrome_uart_status();
        if (ready == 0xffU) break;
        if ((ready & 0x20U) != 0U) {
            linux_chrome_uart_write((uint8_t)value);
            return;
        }
        __asm__ volatile ("pause");
    }
    __atomic_store_n(&g_linux_chrome_uart_disabled, true, __ATOMIC_RELAXED);
}

static void linux_chrome_diag_text(const char *text) {
    if (text == 0) return;
    while (*text != '\0') linux_chrome_diag_char(*text++);
}

static void linux_chrome_diag_hex(uint64_t value) {
    static const char digits[] = "0123456789abcdef";
    linux_chrome_diag_text("0x");
    for (int shift = 60; shift >= 0; shift -= 4) {
        linux_chrome_diag_char(digits[(value >> (uint32_t)shift) & 0xfU]);
    }
}

static void linux_chrome_vm_failure(const char *operation,
                                    process_t *process, uint64_t address,
                                    uint64_t length, uint64_t protection,
                                    uint64_t flags, int64_t status) {
    if (!linux_process_is_chrome_image(process) || operation == 0) return;
    linux_chrome_diag_text("OS_CHROME_");
    linux_chrome_diag_text(operation);
    linux_chrome_diag_text("_FAIL pid=");
    linux_chrome_diag_hex(process->pid);
    linux_chrome_diag_text(" addr=");
    linux_chrome_diag_hex(address);
    linux_chrome_diag_text(" len=");
    linux_chrome_diag_hex(length);
    linux_chrome_diag_text(" prot=");
    linux_chrome_diag_hex(protection);
    linux_chrome_diag_text(" flags=");
    linux_chrome_diag_hex(flags);
    linux_chrome_diag_text(" status=");
    linux_chrome_diag_hex((uint64_t)status);
    linux_chrome_diag_text("\r\n");
}

void linux_chrome_stderr_write(process_t *process,
                                      const char *text, size_t length) {
    uint32_t captured;
    if (!linux_process_is_chrome_image(process) || text == 0 || length == 0U)
        return;
    /* 高频移植标记不能占用错误日志额度。 */
    bool trace = (length >= 3U && text[0] == 'O' && text[1] == 'S' &&
                  text[2] == '_') ||
                 (length >= 7U && text[0] == 'L' && text[1] == 'I' &&
                 text[2] == 'T' && text[3] == 'E' && text[4] == 'O' &&
                  text[5] == 'S' && text[6] == '_');
    uint32_t limit = trace ? 4096U : LINUX_CHROME_STDERR_LIMIT;
    uint32_t *counter = trace ? &g_linux_chrome_trace_bytes :
                               &g_linux_chrome_stderr_bytes;
    captured = __atomic_load_n(counter, __ATOMIC_RELAXED);
    for (;;) {
        if (captured >= limit) return;
        size_t remaining = limit - captured;
        size_t reserved = length < remaining ? length : remaining;
        if (__atomic_compare_exchange_n(counter, &captured,
                captured + (uint32_t)reserved, true,
                __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {
            length = reserved;
            break;
        }
    }
    for (size_t index = 0U; index < length; ++index)
        linux_chrome_diag_char(text[index]);
}

#if OS_DEBUG_SERIAL
static uint32_t g_debug_linux_socket_create_logs;
static uint32_t g_debug_linux_socket_connect_logs;
static uint32_t g_debug_linux_socket_io_logs;
static uint32_t g_debug_linux_futex_logs;
static uint32_t g_debug_linux_close_badfd_logs;
static uint32_t g_debug_linux_exec_fd6_logs;
static uint32_t g_debug_epoll_socket_ctl_logs;
static uint32_t g_debug_epoll_socket_ready_logs;
static uint32_t g_debug_socket_error_logs;
static uint32_t g_debug_socket_name_logs;
static uint32_t g_debug_socket_peek_logs;
static uint32_t g_debug_inet_recv_logs;

static void linux_debug_fd103(const char *operation, process_t *process,
                              uint32_t kind) {
    if (operation == 0 || process == 0) return;
    kernel_serial_printf_serial_only(
        "OS_FD103 op=%s pid=%llu kind=%u\r\n", operation,
        (unsigned long long)process->pid, kind);
}

static void linux_debug_socket_io(uint32_t operation, int64_t descriptor,
                                  int64_t result) {
    if (g_debug_linux_socket_io_logs >= 256U) return;
    kernel_serial_printf_serial_only(
        "OS_LINUX_SOCKET_IO op=%u fd=%lld result=%lld\r\n",
        operation, (long long)descriptor, (long long)result);
    ++g_debug_linux_socket_io_logs;
}
#endif

/*
 * Linux ABI 刻意设计为适配层，而不是第二套内核原生 ABI。
 * 它只负责 Linux x86-64 映像所需的转换，实际操作调用现有的 System
 * VFS/VM/调度器原语完成。
 */

#define LINUX_EFAULT        K_EFAULT
#define LINUX_ERANGE        (-34LL)
#define LINUX_ENOTTY        (-25LL)
#define LINUX_ENODEV        (-19LL)
#define LINUX_EEXIST        (-17LL)
#define LINUX_EOVERFLOW     (-75LL)
#define LINUX_AT_FDCWD_U64  ((uint64_t)(int64_t)LINUX_AT_FDCWD)
#define LINUX_MAX_IOV       64U
#define LINUX_MAX_PATH      256U
#define LINUX_MAX_COPY      (64U * 1024U)
#define LINUX_MAX_POLL_FDS  OS_LINUX_FD_LIMIT
#define LINUX_ECONNRESET    (-104LL)
#define LINUX_ENOTCONN      (-107LL)
#define LINUX_EAFNOSUPPORT  (-97LL)
#define LINUX_EMSGSIZE      (-90LL)
#define LINUX_EINPROGRESS   (-115LL)
#define KOBJECT_TYPE_LINUX_EPOLL   0x0120U
#define KOBJECT_TYPE_LINUX_EVENTFD 0x0121U
#define KOBJECT_TYPE_LINUX_UNIX_SOCKET 0x0122U
#define KOBJECT_TYPE_LINUX_UNIX_CHANNEL 0x0123U
#define LINUX_MAX_EPOLL_WATCHES    OS_LINUX_FD_LIMIT
#define LINUX_EPOLL_WAIT_SOURCES   (3U * LINUX_MAX_EPOLL_WATCHES + 1U)
#define LINUX_PIPE_COPY_MAX        4096U
#define LINUX_MSG_CMSG_CLOEXEC     0x40000000U
#define LINUX_SOL_SOCKET           1U
#define LINUX_SCM_RIGHTS           1U
#define LINUX_CMSG_HEADER_SIZE     16U
#define LINUX_CMSG_ALIGNMENT       8U
#define LINUX_UNIX_MAX_FDS         16U
#define LINUX_UNIX_MAX_MESSAGES    64U
#define LINUX_UNIX_MAX_CONTROL     4096U

/* 正常启动时保持 Linux ABI 热路径安静。这些诊断曾用于调通 PartitionAlloc，
 * 但每个字符都会经由模拟的 COM1 设备发送，徒增 Chrome 启动耗时。 */
#define OS_LINUX_MAP_DIAGNOSTICS 0
#define OS_PARTITION_DIAG_RANGES 8U

#if OS_DEBUG_SERIAL
static uint32_t g_linux_window_create_debug_count;
#if OS_ENABLE_FRAME_TRACE
static uint32_t g_linux_window_present_debug_count;
#endif
#if OS_LINUX_MAP_DIAGNOSTICS
static uint32_t g_linux_mmap_debug_count;
static uint32_t g_linux_partition_mmap_debug_count;
static uint32_t g_linux_partition_mprotect_debug_count;
static uint32_t g_linux_high_mmap_debug_count;
static struct {
    uint64_t start;
    uint64_t end;
} g_linux_partition_diag_ranges[OS_PARTITION_DIAG_RANGES];
static uint32_t g_linux_partition_diag_range_count;
#endif
static uint32_t g_linux_open_debug_count;
static uint32_t g_linux_open_fail_debug_count;
static uint32_t g_linux_exit_debug_count;
#endif

#if OS_DEBUG_SERIAL && OS_LINUX_MAP_DIAGNOSTICS
static bool linux_partition_diag_range(uint64_t address, uint64_t length) {
    uint64_t end;
    if (length == 0U || address > UINT64_MAX - length) return true;
    end = address + length;
    for (uint32_t index = 0U; index < g_linux_partition_diag_range_count;
         ++index) {
        if (address < g_linux_partition_diag_ranges[index].end &&
            end > g_linux_partition_diag_ranges[index].start) {
            return true;
        }
    }
    return false;
}

static void linux_partition_diag_record(uint64_t address, uint64_t length) {
    if (g_linux_partition_diag_range_count >= OS_PARTITION_DIAG_RANGES ||
        length == 0U || address > UINT64_MAX - length) return;
    g_linux_partition_diag_ranges[g_linux_partition_diag_range_count].start =
        address;
    g_linux_partition_diag_ranges[g_linux_partition_diag_range_count].end =
        address + length;
    ++g_linux_partition_diag_range_count;
}
#endif

typedef struct linux_timespec {
    int64_t seconds;
    int64_t nanoseconds;
} linux_timespec_t;

typedef struct linux_rlimit {
    uint64_t current;
    uint64_t maximum;
} linux_rlimit_t;

typedef struct linux_iovec {
    uint64_t base;
    uint64_t length;
} linux_iovec_t;

/* Linux x86-64 的 struct msghdr。保留显式填充字段：该结构从 Linux 映像复制而来，
 * 不能采用宿主机 ABI。 */
typedef struct linux_msghdr {
    uint64_t name;
    uint32_t name_length;
    uint32_t name_padding;
    uint64_t iov_pointer;
    uint64_t iov_length;
    uint64_t control;
    uint64_t control_length;
    uint32_t flags;
    uint32_t flags_padding;
} linux_msghdr_t;

typedef struct linux_pollfd {
    int32_t descriptor;
    int16_t events;
    int16_t revents;
} linux_pollfd_t;

typedef struct linux_sockaddr_in {
    uint16_t family;
    uint16_t port;
    uint32_t address;
    uint8_t zero[8];
} linux_sockaddr_in_t;

typedef struct linux_sockaddr_in6 {
    uint16_t family;
    uint16_t port;
    uint32_t flowinfo;
    uint8_t address[16];
    uint32_t scope_id;
} linux_sockaddr_in6_t;

/* x86-64 Linux 将 epoll_event 定义为 12 字节紧凑系统调用 ABI：uint32_t events
 * 后紧接 64 位用户数据联合体。必须保持紧凑；添加填充/保留字段会导致
 * epoll_ctl() 越界读取调用方事件，epoll_wait() 也会按错误步长写入数组。 */
typedef struct __attribute__((packed)) linux_epoll_event {
    uint32_t events;
    uint64_t data;
} linux_epoll_event_t;

typedef struct linux_epoll_watch {
    int32_t descriptor;
    uint32_t events;
    uint64_t data;
    file_t *file;
    void *private_data;
    uint8_t kind;
    uint8_t used;
    uint8_t disabled;
    uint8_t reserved[5];
    struct linux_fd_description *description;
    struct linux_epoll_object *epoll;
    struct linux_epoll_watch *description_next;
    struct linux_epoll_watch **description_previous;
} linux_epoll_watch_t;

typedef struct linux_epoll_object {
    object_header_t object;
    spinlock_t lock;
    uint64_t control_sequence;
    uint32_t watch_limit;
    uint32_t next_watch_index;
    linux_epoll_watch_t watches[LINUX_MAX_EPOLL_WATCHES];
} linux_epoll_object_t;

/* 外部引用包括描述符、fork/dup 查找固定引用和排队中的 SCM_RIGHTS。
 * Epoll interest 会单独保留元数据，因此不能让打开描述永久存活。
 * 此锁只保护反向 interest 链表；持有此锁时不可获取 epoll 锁。 */
typedef struct linux_fd_description {
    atomic_uint references;
    atomic_uint external_references;
    spinlock_t lock;
    linux_epoll_watch_t *watches;
} linux_fd_description_t;

typedef struct linux_eventfd_object {
    object_header_t object;
    spinlock_t lock;
    wait_queue_t waitq;
    uint64_t counter;
    bool semaphore;
} linux_eventfd_object_t;

static void linux_fd_entry_put(linux_fd_entry_t *entry);

typedef struct linux_unix_ancillary_record {
    uint64_t start_offset;
    uint32_t payload_length;
    uint8_t fd_count;
    uint8_t reserved[3];
    linux_fd_entry_t entries[LINUX_UNIX_MAX_FDS];
} linux_unix_ancillary_record_t;

static void linux_unix_ancillary_put_record(
    linux_unix_ancillary_record_t *record);

/* 管道字节和 SCM_RIGHTS 元数据共用一个流位置。普通 send()/write() 字节无需
 * 记录，但必须推进同一个位置。 */
typedef struct linux_unix_channel {
    object_header_t object;
    spinlock_t lock;
    kmutex_t read_mutex;
    kmutex_t write_mutex;
    uint64_t bytes_read;
    uint64_t bytes_written;
    uint32_t head;
    uint32_t tail;
    uint32_t count;
    linux_unix_ancillary_record_t records[LINUX_UNIX_MAX_MESSAGES];
} linux_unix_channel_t;

typedef struct linux_unix_socket {
    object_header_t object;
    spinlock_t lock;
    pipe_endpoint_t *read_end;
    pipe_endpoint_t *write_end;
    linux_unix_channel_t *read_channel;
    linux_unix_channel_t *write_channel;
    bool read_shutdown;
    bool write_shutdown;
} linux_unix_socket_t;

_Static_assert(sizeof(linux_pollfd_t) == 8U,
               "Linux pollfd ABI changed");
_Static_assert(sizeof(linux_sockaddr_in_t) == 16U,
               "Linux IPv4 sockaddr ABI changed");
_Static_assert(sizeof(linux_sockaddr_in6_t) == 28U,
               "Linux IPv6 sockaddr ABI changed");
_Static_assert(sizeof(linux_epoll_event_t) == 12U,
               "Linux epoll event ABI changed");
_Static_assert(sizeof(linux_msghdr_t) == 56U,
               "Linux msghdr ABI changed");

typedef struct linux_stat {
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
    linux_timespec_t access_time;
    linux_timespec_t modify_time;
    linux_timespec_t change_time;
    int64_t reserved[3];
} linux_stat_t;

typedef struct linux_statx_timestamp {
    int64_t seconds;
    uint32_t nanoseconds;
    int32_t reserved;
} linux_statx_timestamp_t;

typedef struct linux_statx {
    uint32_t mask;
    uint32_t block_size;
    uint64_t attributes;
    uint32_t link_count;
    uint32_t uid;
    uint32_t gid;
    uint16_t mode;
    uint16_t reserved0;
    uint64_t inode;
    uint64_t size;
    uint64_t blocks;
    uint64_t attributes_mask;
    linux_statx_timestamp_t access_time;
    linux_statx_timestamp_t birth_time;
    linux_statx_timestamp_t change_time;
    linux_statx_timestamp_t modify_time;
    uint32_t rdevice_major;
    uint32_t rdevice_minor;
    uint32_t device_major;
    uint32_t device_minor;
    uint64_t reserved[14];
} linux_statx_t;

typedef struct linux_sigaction {
    uint64_t handler;
    uint64_t flags;
    uint64_t restorer;
    uint64_t mask[2];
} linux_sigaction_t;

typedef struct linux_utsname {
    char sysname[65];
    char nodename[65];
    char release[65];
    char version[65];
    char machine[65];
    char domainname[65];
} linux_utsname_t;

typedef struct linux_dirent64_header {
    uint64_t inode;
    uint64_t offset;
    uint16_t record_length;
    uint8_t type;
} __attribute__((packed)) linux_dirent64_header_t;

typedef struct linux_proc_file linux_proc_file_t;

static void linux_proc_file_get(linux_proc_file_t *file);
static void linux_proc_file_put(linux_proc_file_t *file);

static void linux_zero(void *destination, size_t size) {
    memset(destination, 0, size);
}

static void linux_copy(void *destination, const void *source, size_t size) {
    memmove(destination, source, size);
}

static int64_t linux_copy_status(kstatus_t status) {
    return status == K_EACCES ? LINUX_EFAULT : status;
}

static bool linux_fd_valid(int64_t descriptor) {
    return descriptor >= 0 && descriptor < (int64_t)OS_LINUX_FD_LIMIT;
}

static void linux_file_put(file_t *file) {
    if (file != 0) object_put(file);
}

static bool linux_user_pointer(uint64_t pointer, size_t size);

static uint32_t linux_unix_socket_ready(const linux_unix_socket_t *socket,
                                        uint32_t requested) {
    int64_t read_value;
    int64_t write_value;
    uint32_t ready = 0U;
    if (socket == 0 || socket->object.type != KOBJECT_TYPE_LINUX_UNIX_SOCKET)
        return 0U;
    spinlock_lock((spinlock_t *)&socket->lock);
    if (socket->read_end != 0) {
        read_value = object_wait_value(socket->read_end);
        if (read_value >= 0) {
            if ((requested & (LINUX_EPOLLIN | LINUX_EPOLLPRI |
                              LINUX_EPOLLRDNORM | LINUX_EPOLLRDHUP |
                              LINUX_EPOLLMSG)) != 0U &&
                (read_value & PIPE_WAIT_READABLE) != 0) {
                ready |= LINUX_EPOLLIN;
            }
            if ((read_value & PIPE_WAIT_HUP) != 0) {
                ready |= LINUX_EPOLLHUP;
            }
        }
    } else if (socket->read_shutdown) {
        ready |= LINUX_EPOLLHUP;
    }
    if (socket->write_end != 0) {
        write_value = object_wait_value(socket->write_end);
        if (write_value >= 0) {
            if ((requested & (LINUX_EPOLLOUT | LINUX_EPOLLWRNORM)) != 0U &&
                (write_value & PIPE_WAIT_WRITABLE) != 0) {
                ready |= LINUX_EPOLLOUT;
            }
            if ((write_value & PIPE_WAIT_ERROR) != 0) {
                ready |= LINUX_EPOLLERR;
            }
        }
    } else if (socket->write_shutdown) {
        ready |= LINUX_EPOLLERR;
    }
    spinlock_unlock((spinlock_t *)&socket->lock);
    return ready;
}

static bool linux_unix_socket_is_signaled(const void *object) {
    const linux_unix_socket_t *socket =
        (const linux_unix_socket_t *)object;
    return linux_unix_socket_ready(
        socket, LINUX_EPOLLIN | LINUX_EPOLLPRI | LINUX_EPOLLOUT |
        LINUX_EPOLLRDNORM | LINUX_EPOLLWRNORM | LINUX_EPOLLRDHUP |
        LINUX_EPOLLMSG) != 0U;
}

static int64_t linux_unix_socket_wait_value(const void *object) {
    return (int64_t)linux_unix_socket_ready(
        (const linux_unix_socket_t *)object,
        LINUX_EPOLLIN | LINUX_EPOLLPRI | LINUX_EPOLLOUT |
        LINUX_EPOLLRDNORM | LINUX_EPOLLWRNORM | LINUX_EPOLLRDHUP |
        LINUX_EPOLLMSG);
}

static void linux_unix_socket_destroy(void *object) {
    linux_unix_socket_t *socket = (linux_unix_socket_t *)object;
    pipe_endpoint_t *read_end;
    pipe_endpoint_t *write_end;
    linux_unix_channel_t *read_channel;
    linux_unix_channel_t *write_channel;
    if (socket == 0) return;
    read_end = socket->read_end;
    write_end = socket->write_end;
    read_channel = socket->read_channel;
    write_channel = socket->write_channel;
    socket->read_end = 0;
    socket->write_end = 0;
    socket->read_channel = 0;
    socket->write_channel = 0;
    if (read_end != 0) object_put(read_end);
    if (write_end != 0) object_put(write_end);
    if (read_channel != 0) object_put(read_channel);
    if (write_channel != 0) object_put(write_channel);
    kfree(socket);
}

static void linux_unix_socket_release_endpoint(pipe_endpoint_t **endpoint) {
    pipe_endpoint_t *owned;
    if (endpoint == 0 || *endpoint == 0) return;
    owned = *endpoint;
    *endpoint = 0;
    object_put(owned);
}

static void linux_unix_socket_release_channel(linux_unix_channel_t **channel) {
    linux_unix_channel_t *owned;
    if (channel == 0 || *channel == 0) return;
    owned = *channel;
    *channel = 0;
    object_put(owned);
}

static kstatus_t linux_unix_socket_shutdown(linux_unix_socket_t *socket,
                                            uint32_t how) {
    pipe_endpoint_t *read_end = 0;
    pipe_endpoint_t *write_end = 0;
    linux_unix_channel_t *read_channel = 0;
    linux_unix_channel_t *write_channel = 0;
    if (socket == 0 || socket->object.type != KOBJECT_TYPE_LINUX_UNIX_SOCKET ||
        how > OS_SOCKET_SHUT_BOTH) return K_EINVAL;
    spinlock_lock(&socket->lock);
    if (how == OS_SOCKET_SHUT_READ || how == OS_SOCKET_SHUT_BOTH) {
        read_end = socket->read_end;
        read_channel = socket->read_channel;
        socket->read_end = 0;
        socket->read_channel = 0;
        socket->read_shutdown = true;
    }
    if (how == OS_SOCKET_SHUT_WRITE || how == OS_SOCKET_SHUT_BOTH) {
        write_end = socket->write_end;
        write_channel = socket->write_channel;
        socket->write_end = 0;
        socket->write_channel = 0;
        socket->write_shutdown = true;
    }
    spinlock_unlock(&socket->lock);
    if (read_end != 0) (void)pipe_shutdown(read_end);
    if (write_end != 0) (void)pipe_shutdown(write_end);
    linux_unix_socket_release_endpoint(&read_end);
    linux_unix_socket_release_channel(&read_channel);
    linux_unix_socket_release_endpoint(&write_end);
    linux_unix_socket_release_channel(&write_channel);
    object_notify_changed(socket);
    return K_OK;
}

/* 描述符固定的是 socket，而不是 shutdown 时分离的端点。
 * 在各自的发布锁保护下保留每个方向的端点/通道对。 */
static void linux_unix_socket_pin_io(linux_unix_socket_t *socket, bool write,
                                     pipe_endpoint_t **endpoint,
                                     linux_unix_channel_t **channel) {
    spinlock_lock(&socket->lock);
    *endpoint = write ? socket->write_end : socket->read_end;
    *channel = *endpoint != 0 ?
               (write ? socket->write_channel : socket->read_channel) : 0;
    if (*endpoint != 0) object_get(*endpoint);
    if (*channel != 0) object_get(*channel);
    spinlock_unlock(&socket->lock);
}

static void linux_unix_socket_unpin_io(pipe_endpoint_t *endpoint,
                                       linux_unix_channel_t *channel) {
    if (endpoint != 0) object_put(endpoint);
    if (channel != 0) object_put(channel);
}

static const object_ops_t g_linux_unix_socket_ops = {
    .destroy = linux_unix_socket_destroy,
    .handle_close = 0,
    .type_name = "LinuxUnixSocket",
    .is_signaled = linux_unix_socket_is_signaled,
    .wait_value = linux_unix_socket_wait_value,
};

static bool linux_epoll_read_requested(uint32_t events) {
    return (events & (LINUX_EPOLLIN | LINUX_EPOLLPRI |
                      LINUX_EPOLLRDNORM | LINUX_EPOLLRDHUP |
                      LINUX_EPOLLMSG)) != 0U;
}

static bool linux_epoll_write_requested(uint32_t events) {
    return (events & (LINUX_EPOLLOUT | LINUX_EPOLLWRNORM)) != 0U;
}

static bool linux_eventfd_can_write(const linux_eventfd_object_t *eventfd) {
    bool ready;
    if (eventfd == 0) return false;
    spinlock_lock((spinlock_t *)&eventfd->lock);
    ready = eventfd->counter != UINT64_MAX - 1U;
    spinlock_unlock((spinlock_t *)&eventfd->lock);
    return ready;
}

static bool linux_epoll_watch_ready(const linux_epoll_watch_t *watch,
                                    uint32_t *ready_events) {
    uint32_t ready = 0U;
    bool read_requested;
    bool write_requested;
    /* interest 掩码为零时仍会收到 ERR/HUP。只有 ONESHOT 投递会禁用该 interest，
     * 直到 EPOLL_CTL_MOD 显式重新启用。 */
    if (watch == 0 || !watch->used || watch->disabled) return false;
    read_requested = linux_epoll_read_requested(watch->events);
    write_requested = linux_epoll_write_requested(watch->events);
    switch (watch->kind) {
    case LINUX_FD_NVIDIA: {
        uint32_t state = nv_frontend_poll_state(watch->private_data, false);
        if (read_requested && (state & NV_FRONTEND_POLL_READ) != 0U) {
            uint32_t bits = watch->events & (LINUX_EPOLLIN | LINUX_EPOLLPRI);
            ready |= bits != 0U ? bits : LINUX_EPOLLIN;
        }
        if ((state & NV_FRONTEND_POLL_CLOSED) != 0U) ready |= LINUX_EPOLLHUP;
        break;
    }
    case LINUX_FD_FILE:
        if (read_requested) ready |= LINUX_EPOLLIN;
        if (write_requested) ready |= LINUX_EPOLLOUT;
        break;
    case LINUX_FD_PIPE: {
        pipe_endpoint_t *endpoint = (pipe_endpoint_t *)watch->private_data;
        bool signaled = object_is_signaled(endpoint);
        if (pipe_endpoint_is_read(endpoint)) {
            if (read_requested && signaled) ready |= LINUX_EPOLLIN;
        } else if (signaled) {
            int64_t value = object_wait_value(endpoint);
            if ((value & PIPE_WAIT_ERROR) != 0) ready |= LINUX_EPOLLERR;
            else if (write_requested) ready |= LINUX_EPOLLOUT;
        }
        break;
    }
    case LINUX_FD_SOCKET:
        {
            uint32_t state = socket_poll_state((socket_t *)watch->private_data);
            if (read_requested && (state & SOCKET_POLL_READ) != 0U)
                ready |= LINUX_EPOLLIN;
            if (write_requested && (state & SOCKET_POLL_WRITE) != 0U)
                ready |= LINUX_EPOLLOUT;
            if ((state & SOCKET_POLL_ERROR) != 0U) ready |= LINUX_EPOLLERR;
            if ((state & SOCKET_POLL_HUP) != 0U) ready |= LINUX_EPOLLHUP;
        }
        break;
    case LINUX_FD_UNIX_SOCKET:
        ready |= linux_unix_socket_ready(
            (const linux_unix_socket_t *)watch->private_data,
            watch->events);
        break;
    case LINUX_FD_WINDOW:
        if (read_requested && object_is_signaled(watch->private_data)) {
            ready |= LINUX_EPOLLIN;
        }
        break;
    case LINUX_FD_EPOLL:
        if (read_requested && object_is_signaled(watch->private_data)) {
            ready |= LINUX_EPOLLIN;
        }
        break;
    case LINUX_FD_EVENTFD: {
        linux_eventfd_object_t *eventfd =
            (linux_eventfd_object_t *)watch->private_data;
        if (read_requested && object_is_signaled(eventfd)) {
            ready |= LINUX_EPOLLIN;
        }
        if (write_requested && linux_eventfd_can_write(eventfd)) {
            ready |= LINUX_EPOLLOUT;
        }
        break;
    }
    default:
        break;
    }
    if (ready_events != 0) *ready_events = ready;
    return ready != 0U;
}

static bool linux_epoll_is_signaled(const void *object) {
    const linux_epoll_object_t *epoll =
        (const linux_epoll_object_t *)object;
    bool signaled = false;
    if (epoll == 0 || epoll->object.type != KOBJECT_TYPE_LINUX_EPOLL) {
        return false;
    }
    spinlock_lock((spinlock_t *)&epoll->lock);
    for (uint32_t index = 0U; index < epoll->watch_limit; ++index) {
        if (linux_epoll_watch_ready(&epoll->watches[index], 0)) {
            signaled = true;
            break;
        }
    }
    spinlock_unlock((spinlock_t *)&epoll->lock);
    return signaled;
}

static int64_t linux_epoll_wait_value(const void *object) {
    return linux_epoll_is_signaled(object) ? 1 : 0;
}

static bool linux_eventfd_is_signaled(const void *object) {
    const linux_eventfd_object_t *eventfd =
        (const linux_eventfd_object_t *)object;
    bool signaled;
    if (eventfd == 0 || eventfd->object.type != KOBJECT_TYPE_LINUX_EVENTFD) {
        return false;
    }
    spinlock_lock((spinlock_t *)&eventfd->lock);
    signaled = eventfd->counter != 0U;
    spinlock_unlock((spinlock_t *)&eventfd->lock);
    return signaled;
}

static int64_t linux_eventfd_wait_value(const void *object) {
    const linux_eventfd_object_t *eventfd =
        (const linux_eventfd_object_t *)object;
    uint64_t counter;
    if (eventfd == 0 || eventfd->object.type != KOBJECT_TYPE_LINUX_EVENTFD) {
        return K_EBADF;
    }
    spinlock_lock((spinlock_t *)&eventfd->lock);
    counter = eventfd->counter;
    spinlock_unlock((spinlock_t *)&eventfd->lock);
    return counter > (uint64_t)INT64_MAX ? INT64_MAX : (int64_t)counter;
}

static void linux_eventfd_destroy(void *object) {
    kfree(object);
}

static const object_ops_t g_linux_eventfd_ops = {
    .destroy = linux_eventfd_destroy,
    .handle_close = 0,
    .type_name = "LinuxEventfd",
    .is_signaled = linux_eventfd_is_signaled,
    .wait_value = linux_eventfd_wait_value,
};

static void linux_description_put(linux_fd_description_t *description) {
    if (description != 0 && atomic_fetch_sub_explicit(
            &description->references, 1U, memory_order_acq_rel) == 1U) {
        kfree(description);
    }
}

static void linux_description_get_external(linux_fd_description_t *description) {
    if (description == 0) return;
    atomic_fetch_add_explicit(&description->references, 1U, memory_order_relaxed);
    atomic_fetch_add_explicit(&description->external_references, 1U,
                              memory_order_relaxed);
}

static void linux_epoll_watch_unlink(linux_epoll_watch_t *watch) {
    linux_fd_description_t *description = watch->description;
    if (description == 0) return;
    spinlock_lock(&description->lock);
    if (watch->description_previous != 0) {
        *watch->description_previous = watch->description_next;
        if (watch->description_next != 0) {
            watch->description_next->description_previous =
                watch->description_previous;
        }
        watch->description_next = 0;
        watch->description_previous = 0;
    }
    spinlock_unlock(&description->lock);
}

static void linux_epoll_watch_put(linux_epoll_watch_t *watch) {
    if (watch == 0 || !watch->used) return;
    linux_epoll_watch_unlink(watch);
    if (watch->kind == LINUX_FD_FILE) {
        linux_file_put(watch->file);
    } else if (watch->kind == LINUX_FD_PROC) {
        linux_proc_file_put((linux_proc_file_t *)watch->private_data);
    } else if (watch->kind == LINUX_FD_NVIDIA) {
        nv_frontend_file_put((nv_frontend_file_t *)watch->private_data);
    } else {
        object_put(watch->private_data);
    }
    linux_description_put(watch->description);
    *watch = (linux_epoll_watch_t){0};
}

static bool linux_epoll_watch_matches_entry(const linux_epoll_watch_t *watch,
                                            const linux_fd_entry_t *entry) {
    return watch != 0 && watch->used && entry != 0 &&
           watch->description != 0 && watch->description == entry->description;
}

/* 仅当底层描述的最后一个描述符关闭时才移除注册。dup 别名必须保持旧 interest 有效，
 * 即使之后相同数字 fd 被另一个 socket 复用也一样。 */
static void linux_epoll_prune_description(linux_epoll_object_t *epoll,
                                          linux_fd_description_t *description) {
    bool changed = false;
    for (;;) {
        linux_epoll_watch_t removed = {0};
        spinlock_lock(&epoll->lock);
        for (uint32_t index = 0U; index < epoll->watch_limit; ++index) {
            linux_epoll_watch_t *watch = &epoll->watches[index];
            if (!watch->used || watch->description != description) continue;
            linux_epoll_watch_unlink(watch);
            removed = *watch;
            *watch = (linux_epoll_watch_t){0};
            if (index + 1U == epoll->watch_limit) {
                while (epoll->watch_limit != 0U &&
                       !epoll->watches[epoll->watch_limit - 1U].used) {
                    --epoll->watch_limit;
                }
            }
            ++epoll->control_sequence;
            break;
        }
        spinlock_unlock(&epoll->lock);
        if (!removed.used) break;
        linux_epoll_watch_put(&removed);
        changed = true;
    }
    if (changed) object_notify_changed(epoll);
}

static void linux_description_put_external(linux_fd_description_t *description) {
    if (description == 0) return;
    if (atomic_fetch_sub_explicit(&description->external_references, 1U,
                                  memory_order_acq_rel) == 1U) {
      for (;;) {
        linux_epoll_object_t *epoll = 0;
        spinlock_lock(&description->lock);
        for (linux_epoll_watch_t *watch = description->watches; watch != 0;
             watch = watch->description_next) {
            /* 引用计数为零的 epoll 已在销毁过程中。析构函数会解除其余 interest 的链接，
             * 并释放元数据。 */
            if (object_try_get(watch->epoll)) {
                epoll = watch->epoll;
                break;
            }
        }
        spinlock_unlock(&description->lock);
        if (epoll == 0) break;
        linux_epoll_prune_description(epoll, description);
        object_put(epoll);
      }
    }
    linux_description_put(description);
}

static void linux_epoll_destroy(void *object) {
    linux_epoll_object_t *epoll = (linux_epoll_object_t *)object;
    if (epoll == 0) return;
    for (uint32_t index = 0U; index < epoll->watch_limit; ++index) {
        linux_epoll_watch_put(&epoll->watches[index]);
    }
    kfree(epoll);
}

static const object_ops_t g_linux_epoll_ops = {
    .destroy = linux_epoll_destroy,
    .handle_close = 0,
    .type_name = "LinuxEpoll",
    .is_signaled = linux_epoll_is_signaled,
    .wait_value = linux_epoll_wait_value,
};

static void linux_fd_entry_put(linux_fd_entry_t *entry) {
    if (entry == 0) return;
    linux_description_put_external(entry->description);
    entry->description = 0;
    if (entry->kind == LINUX_FD_FILE) {
        linux_file_put(entry->file);
    } else if (entry->kind == LINUX_FD_NVIDIA) {
        nv_frontend_file_put((nv_frontend_file_t *)entry->private_data);
    } else if (entry->kind == LINUX_FD_PROC) {
        linux_proc_file_put((linux_proc_file_t *)entry->private_data);
    } else if (entry->kind == LINUX_FD_WINDOW) {
        window_server_put((window_server_window_t *)entry->private_data);
    } else if (entry->kind == LINUX_FD_SOCKET ||
               entry->kind == LINUX_FD_PIPE ||
               entry->kind == LINUX_FD_EPOLL ||
               entry->kind == LINUX_FD_EVENTFD ||
               entry->kind == LINUX_FD_UNIX_SOCKET) {
        object_put(entry->private_data);
    }
    entry->file = 0;
    entry->private_data = 0;
}

static void linux_unix_channel_destroy(void *object) {
    linux_unix_channel_t *channel = (linux_unix_channel_t *)object;
    if (channel == 0) return;
    for (uint32_t index = 0U; index < LINUX_UNIX_MAX_MESSAGES; ++index) {
        linux_unix_ancillary_record_t *record = &channel->records[index];
        for (uint32_t fd = 0U; fd < record->fd_count; ++fd) {
            linux_fd_entry_put(&record->entries[fd]);
        }
        *record = (linux_unix_ancillary_record_t){0};
    }
    kfree(channel);
}

static const object_ops_t g_linux_unix_channel_ops = {
    .destroy = linux_unix_channel_destroy,
    .handle_close = 0,
    .type_name = "LinuxUnixChannel",
    .is_signaled = 0,
    .wait_value = 0,
};

static linux_unix_channel_t *linux_unix_channel_create(void) {
    linux_unix_channel_t *channel =
        (linux_unix_channel_t *)kzalloc(sizeof(*channel), 0U);
    if (channel == 0) return 0;
    refcount_init(&channel->object.refs, 1U);
    channel->object.type = KOBJECT_TYPE_LINUX_UNIX_CHANNEL;
    channel->object.flags = 0U;
    channel->object.ops = &g_linux_unix_channel_ops;
    spinlock_init(&channel->lock);
    kmutex_init(&channel->read_mutex, false);
    kmutex_init(&channel->write_mutex, false);
    return channel;
}

static kstatus_t linux_unix_channel_push(
    linux_unix_channel_t *channel, linux_unix_ancillary_record_t *record) {
    if (channel == 0 || record == 0) return K_EINVAL;
    spinlock_lock(&channel->lock);
    if (channel->count == LINUX_UNIX_MAX_MESSAGES) {
        spinlock_unlock(&channel->lock);
        return K_EAGAIN;
    }
    record->start_offset = channel->bytes_written;
    channel->records[channel->tail] = *record;
    channel->tail = (channel->tail + 1U) % LINUX_UNIX_MAX_MESSAGES;
    ++channel->count;
    *record = (linux_unix_ancillary_record_t){0};
    spinlock_unlock(&channel->lock);
    return K_OK;
}

static void linux_unix_channel_rollback(linux_unix_channel_t *channel) {
    linux_unix_ancillary_record_t record;
    if (channel == 0) return;
    spinlock_lock(&channel->lock);
    if (channel->count == 0U) {
        spinlock_unlock(&channel->lock);
        return;
    }
    channel->tail = (channel->tail + LINUX_UNIX_MAX_MESSAGES - 1U) %
                    LINUX_UNIX_MAX_MESSAGES;
    record = channel->records[channel->tail];
    channel->records[channel->tail] = (linux_unix_ancillary_record_t){0};
    --channel->count;
    spinlock_unlock(&channel->lock);
    linux_unix_ancillary_put_record(&record);
}

static void linux_unix_channel_commit_write(linux_unix_channel_t *channel,
                                            uint64_t bytes) {
    if (channel == 0 || bytes == 0U) return;
    spinlock_lock(&channel->lock);
    channel->bytes_written += bytes;
    spinlock_unlock(&channel->lock);
}

/* recvmsg 必须在第一个携带 FD 的字节前停止。到达该字节后，随 sendmsg 负载的首次读取
 * 一并交付 FD。 */
static size_t linux_unix_channel_read_limit(linux_unix_channel_t *channel,
                                            size_t request) {
    uint64_t remaining;
    if (channel == 0 || request == 0U) return request;
    spinlock_lock(&channel->lock);
    if (channel->count != 0U) {
        linux_unix_ancillary_record_t *record =
            &channel->records[channel->head];
        remaining = record->start_offset > channel->bytes_read ?
                    record->start_offset - channel->bytes_read :
                    (uint64_t)record->payload_length;
        if (remaining < request) request = (size_t)remaining;
    }
    spinlock_unlock(&channel->lock);
    return request;
}

/* pipe_read_limited 在每次等待后、持有环锁时调用此函数。写入方在获取环锁前发布
 * 辅助数据元信息，且不会持有通道锁进入管道操作（ring -> channel）。 */
static size_t linux_unix_channel_pipe_limit(void *context, size_t available) {
    return linux_unix_channel_read_limit(context, available);
}

/* pipe_read 后在 read_mutex 保护下调用；read()/recv() 跳过的记录会被丢弃，
 * 而 recvmsg() 可以在旧偏移处取得该记录。 */
static bool linux_unix_channel_commit_read(
    linux_unix_channel_t *channel, uint64_t bytes, bool receive_control,
    linux_unix_ancillary_record_t *received) {
    uint64_t old_offset;
    bool delivered = false;
    if (channel == 0 || bytes == 0U) return false;
    spinlock_lock(&channel->lock);
    old_offset = channel->bytes_read;
    channel->bytes_read += bytes;
    while (channel->count != 0U &&
           channel->records[channel->head].start_offset <
               channel->bytes_read) {
        linux_unix_ancillary_record_t record =
            channel->records[channel->head];
        channel->records[channel->head] =
            (linux_unix_ancillary_record_t){0};
        channel->head = (channel->head + 1U) % LINUX_UNIX_MAX_MESSAGES;
        --channel->count;
        spinlock_unlock(&channel->lock);
        if (receive_control && !delivered && received != 0 &&
            record.start_offset == old_offset) {
            *received = record;
            delivered = true;
        } else {
            linux_unix_ancillary_put_record(&record);
        }
        spinlock_lock(&channel->lock);
    }
    spinlock_unlock(&channel->lock);
    return delivered;
}

void linux_fd_table_reset_stdio(linux_fd_table_t *table,
                                linux_fd_entry_t previous[3]) {
    if (table == 0 || previous == 0) return;
    spinlock_lock(&table->lock);
    for (uint32_t descriptor = 0U; descriptor < 3U; ++descriptor) {
        previous[descriptor] = table->entries[descriptor];
        table->entries[descriptor] = (linux_fd_entry_t){
            .kind = LINUX_FD_CONSOLE,
            .flags = descriptor == 0U ? 0U : 1U,
        };
    }
    spinlock_unlock(&table->lock);
}

void linux_fd_table_release_stdio(linux_fd_entry_t previous[3]) {
    if (previous == 0) return;
    for (uint32_t descriptor = 0U; descriptor < 3U; ++descriptor)
        linux_fd_entry_put(&previous[descriptor]);
}

/* 调用方持有源表锁，或以其他方式拥有此表项。文件描述及其具体对象始终作为一个外部引用
 * 一同传递。 */
static void linux_fd_entry_get(const linux_fd_entry_t *entry) {
    linux_description_get_external(entry->description);
    if (entry->kind == LINUX_FD_FILE && entry->file != 0) {
        object_get(entry->file);
    } else if (entry->kind == LINUX_FD_NVIDIA && entry->private_data != 0) {
        nv_frontend_file_get((nv_frontend_file_t *)entry->private_data);
    } else if (entry->kind == LINUX_FD_PROC && entry->private_data != 0) {
        linux_proc_file_get((linux_proc_file_t *)entry->private_data);
    } else if ((entry->kind == LINUX_FD_WINDOW ||
                entry->kind == LINUX_FD_SOCKET ||
                entry->kind == LINUX_FD_PIPE ||
                entry->kind == LINUX_FD_EPOLL ||
                entry->kind == LINUX_FD_EVENTFD ||
                entry->kind == LINUX_FD_UNIX_SOCKET) && entry->private_data != 0) {
        object_get(entry->private_data);
    }
}

void linux_fd_table_init(linux_fd_table_t *table) {
    if (table == 0) return;
    /* 初始化阶段拥有尚未发布的表：在同一轮中安装 stdio，无需加锁，也不必之后重写前三个槽位。 */
    spinlock_init(&table->lock);
    for (uint32_t descriptor = 0U; descriptor < OS_LINUX_FD_LIMIT;
         ++descriptor) {
        table->entries[descriptor] = (linux_fd_entry_t){
            .kind = descriptor < 3U ? LINUX_FD_CONSOLE : LINUX_FD_UNUSED,
            .flags = descriptor == 1U || descriptor == 2U ? 1U : 0U,
        };
    }
}

void linux_fd_table_destroy(linux_fd_table_t *table) {
    linux_fd_entry_t entries[OS_LINUX_FD_LIMIT];
    uint32_t count = 0U;
    if (table == 0) return;
    /* 仅压缩自有表项；运行任何回调前先分离整张表，包括可能重入或再次销毁该表的回调。 */
    spinlock_lock(&table->lock);
    for (uint32_t descriptor = 0U; descriptor < OS_LINUX_FD_LIMIT;
         ++descriptor) {
        const linux_fd_entry_t *entry = &table->entries[descriptor];
        if (entry->kind != LINUX_FD_UNUSED || entry->description != 0 ||
            entry->file != 0 || entry->private_data != 0)
            entries[count++] = *entry;
        table->entries[descriptor] = (linux_fd_entry_t){0};
    }
    spinlock_unlock(&table->lock);
    for (uint32_t descriptor = 0U; descriptor < count;
         ++descriptor) {
        linux_fd_entry_put(&entries[descriptor]);
    }
}

kstatus_t linux_fd_table_clone(const linux_fd_table_t *source,
                               linux_fd_table_t *destination) {
    if (source == 0 || destination == 0 || source == destination) {
        return source == destination ? K_OK : K_EINVAL;
    }
    linux_fd_table_destroy(destination);
    /* 销毁过程保留已初始化的锁；复制操作会替换所有槽位，包括 stdio 和未使用项，因此无需再次初始化。 */
    /* 无论克隆方向如何，都采用固定顺序。uintptr_t 可避免直接比较无关的 C 对象指针。 */
    spinlock_t *source_lock = (spinlock_t *)&source->lock;
    spinlock_t *destination_lock = &destination->lock;
    spinlock_t *first = (uintptr_t)source_lock < (uintptr_t)destination_lock ?
                         source_lock : destination_lock;
    spinlock_t *second = first == source_lock ? destination_lock : source_lock;
    spinlock_lock(first);
    spinlock_lock(second);
    for (uint32_t descriptor = 0U; descriptor < OS_LINUX_FD_LIMIT;
         ++descriptor) {
        const linux_fd_entry_t *source_entry = &source->entries[descriptor];
        linux_fd_entry_t *destination_entry = &destination->entries[descriptor];
        *destination_entry = *source_entry;
        linux_fd_entry_get(destination_entry);
    }
    spinlock_unlock(second);
    spinlock_unlock(first);
    return K_OK;
}

static void linux_fd_release_locks(process_t *process, const linux_fd_entry_t *entry) {
    if (entry->kind == LINUX_FD_FILE && entry->file && entry->file->vnode)
        file_lock_release_file(entry->file->vnode, process->pid);
}

static void linux_fd_notify_locks(const linux_fd_entry_t *entry) {
    if (entry->kind == LINUX_FD_FILE && entry->file && entry->file->vnode)
        wake_all(&entry->file->vnode->record_lock_waitq);
}

void linux_fd_table_close_on_exec(process_t *process) {
    linux_fd_table_t *table;
    linux_fd_entry_t entries[OS_LINUX_FD_LIMIT];
    uint32_t entry_count = 0U;
#if OS_DEBUG_SERIAL
    uint32_t descriptor_103_kind = LINUX_FD_UNUSED;
    uint32_t descriptor_6_kind = LINUX_FD_UNUSED;
    uint32_t descriptor_6_flags = 0U;
    bool descriptor_6_removed = false;
#endif
    if (process == 0) return;
    table = &process->linux_fds;
    spinlock_lock(&table->lock);
    for (uint32_t descriptor = 0U; descriptor < OS_LINUX_FD_LIMIT;
         ++descriptor) {
#if OS_DEBUG_SERIAL
        if (descriptor == 6U) {
            descriptor_6_kind = table->entries[descriptor].kind;
            descriptor_6_flags = table->entries[descriptor].flags;
        }
#endif
        if ((table->entries[descriptor].flags & LINUX_O_CLOEXEC) == 0U) {
            continue;
        }
#if OS_DEBUG_SERIAL
        if (descriptor == 103U) {
            descriptor_103_kind = table->entries[descriptor].kind;
        }
        if (descriptor == 6U) {
            descriptor_6_removed = table->entries[descriptor].kind != LINUX_FD_UNUSED;
        }
#endif
        /* 只有已分离的表项会被初始化并在之后释放。 */
        entries[entry_count++] = table->entries[descriptor];
        linux_fd_release_locks(process, &table->entries[descriptor]);
        table->entries[descriptor] = (linux_fd_entry_t){0};
    }
    spinlock_unlock(&table->lock);
#if OS_DEBUG_SERIAL
    if (descriptor_6_kind != LINUX_FD_UNUSED &&
        g_debug_linux_exec_fd6_logs < 128U) {
        kernel_serial_printf_serial_only(
            "OS_EXEC_FD6_CLOEXEC pid=%llu kind=%u flags=%x removed=%u\r\n",
            (unsigned long long)process->pid, descriptor_6_kind,
            descriptor_6_flags,
            descriptor_6_removed ? 1U : 0U);
        ++g_debug_linux_exec_fd6_logs;
    }
    if (descriptor_103_kind != LINUX_FD_UNUSED) {
        linux_debug_fd103("CLOEXEC", process, descriptor_103_kind);
    }
#endif
    for (uint32_t index = 0U; index < entry_count; ++index) {
        linux_fd_notify_locks(&entries[index]);
        linux_fd_entry_put(&entries[index]);
    }
}

static kstatus_t linux_fd_lookup(process_t *process, int64_t descriptor,
                                  linux_fd_entry_t *entry) {
    if (process == 0 || entry == 0 || !linux_fd_valid(descriptor)) {
        return K_EBADF;
    }
    spinlock_lock(&process->linux_fds.lock);
    *entry = process->linux_fds.entries[(uint32_t)descriptor];
    if (entry->kind == LINUX_FD_UNUSED) {
        spinlock_unlock(&process->linux_fds.lock);
        return K_EBADF;
    }
    linux_fd_entry_get(entry);
    spinlock_unlock(&process->linux_fds.lock);
    return K_OK;
}

static int64_t linux_fd_install_description(process_t *process, int64_t requested,
                                file_t *file, void *private_data,
                                uint8_t kind, uint32_t flags,
                                linux_fd_description_t *description) {
    linux_fd_entry_t old_entry = {0};
    uint32_t first;
    uint32_t last;
    int64_t result = K_EMFILE;
    bool new_description = false;
    if (process == 0 || (kind == LINUX_FD_FILE && file == 0) ||
        (kind == LINUX_FD_NVIDIA && private_data == 0) ||
        (kind == LINUX_FD_PROC && private_data == 0) ||
        (kind == LINUX_FD_WINDOW && private_data == 0) ||
        (kind == LINUX_FD_SOCKET && private_data == 0) ||
        (kind == LINUX_FD_UNIX_SOCKET && private_data == 0) ||
        (kind == LINUX_FD_NULL && (file != 0 || private_data != 0)) ||
        (kind != LINUX_FD_FILE && kind != LINUX_FD_CONSOLE &&
         kind != LINUX_FD_NVIDIA && kind != LINUX_FD_PROC &&
         kind != LINUX_FD_WINDOW && kind != LINUX_FD_SOCKET &&
         kind != LINUX_FD_PIPE && kind != LINUX_FD_EPOLL &&
         kind != LINUX_FD_EVENTFD && kind != LINUX_FD_UNIX_SOCKET &&
         kind != LINUX_FD_NULL)) {
        return K_EINVAL;
    }
    if (requested >= 0) {
        if (!linux_fd_valid(requested)) return K_EBADF;
        first = (uint32_t)requested;
        last = first + 1U;
    } else {
        first = 0U;
        last = OS_LINUX_FD_LIMIT;
    }
    if (description == 0 && (kind == LINUX_FD_FILE || kind == LINUX_FD_PIPE ||
        kind == LINUX_FD_SOCKET || kind == LINUX_FD_UNIX_SOCKET ||
        kind == LINUX_FD_WINDOW || kind == LINUX_FD_EVENTFD)) {
        description = kzalloc(sizeof(*description), 0U);
        if (description == 0) return K_ENOMEM;
        atomic_init(&description->references, 1U);
        atomic_init(&description->external_references, 1U);
        spinlock_init(&description->lock);
        new_description = true;
    }
    spinlock_lock(&process->linux_fds.lock);
    for (uint32_t descriptor = first; descriptor < last; ++descriptor) {
        if (requested < 0 &&
            process->linux_fds.entries[descriptor].kind != LINUX_FD_UNUSED) {
            continue;
        }
        old_entry = process->linux_fds.entries[descriptor];
        linux_fd_release_locks(process, &old_entry);
        process->linux_fds.entries[descriptor] = (linux_fd_entry_t){
            .file = file,
            .private_data = private_data,
            .flags = flags,
            .kind = kind,
            .description = description,
        };
        result = (int64_t)descriptor;
        break;
    }
    spinlock_unlock(&process->linux_fds.lock);
    if (result == K_EMFILE &&
        __atomic_fetch_add(&g_linux_fd_exhaustion_logs, 1U,
                           __ATOMIC_RELAXED) < 16U) {
        /* EMFILE 表示加锁的分配扫描发现所有槽位均被占用。
         * 报告该次快照，不要再进行一次可能与并发关闭竞争的无锁扫描。 */
        kernel_serial_printf_serial_only(
            "OS_LINUX_FD_FULL pid=%llu occupied=%u kind=%u\r\n",
            (unsigned long long)process->pid,
            (unsigned)OS_LINUX_FD_LIMIT, (unsigned)kind);
    }
#if OS_DEBUG_SERIAL
    if (result == 103) {
        linux_debug_fd103("INSTALL", process, kind);
        if (old_entry.kind != LINUX_FD_UNUSED) {
            linux_debug_fd103("REPLACE", process, old_entry.kind);
        }
    }
#endif
    if (result < 0 && new_description) linux_description_put_external(description);
    linux_fd_notify_locks(&old_entry);
    linux_fd_entry_put(&old_entry);
    return result;
}

static int64_t linux_fd_install(process_t *process, int64_t requested,
                                file_t *file, void *private_data,
                                uint8_t kind, uint32_t flags) {
    return linux_fd_install_description(process, requested, file, private_data,
                                         kind, flags, 0);
}

static kstatus_t linux_fd_detach(process_t *process, int64_t descriptor,
                                  linux_fd_entry_t *entry) {
    if (process == 0 || entry == 0 || !linux_fd_valid(descriptor)) {
        return K_EBADF;
    }
    spinlock_lock(&process->linux_fds.lock);
    *entry = process->linux_fds.entries[(uint32_t)descriptor];
    if (entry->kind == LINUX_FD_UNUSED) {
        spinlock_unlock(&process->linux_fds.lock);
#if OS_DEBUG_SERIAL
        if (descriptor == 103) {
            linux_debug_fd103("DETACH_MISS", process, LINUX_FD_UNUSED);
        }
#endif
        return K_EBADF;
    }
    linux_fd_release_locks(process, entry);
    process->linux_fds.entries[(uint32_t)descriptor] =
        (linux_fd_entry_t){0};
    spinlock_unlock(&process->linux_fds.lock);
    linux_fd_notify_locks(entry);
#if OS_DEBUG_SERIAL
    if (descriptor == 103) {
        linux_debug_fd103("DETACH", process, entry->kind);
    }
#endif
    return K_OK;
}

static kstatus_t linux_fd_set_flags(process_t *process, int64_t descriptor,
                                     uint32_t flags, uint32_t mask) {
    if (process == 0 || !linux_fd_valid(descriptor)) return K_EBADF;
    spinlock_lock(&process->linux_fds.lock);
    linux_fd_entry_t *entry = &process->linux_fds.entries[(uint32_t)descriptor];
    if (entry->kind == LINUX_FD_UNUSED) {
        spinlock_unlock(&process->linux_fds.lock);
        return K_EBADF;
    }
    entry->flags = (entry->flags & ~mask) | (flags & mask);
    spinlock_unlock(&process->linux_fds.lock);
    return K_OK;
}

static void linux_fd_close_silent(process_t *process, int64_t descriptor) {
    linux_fd_entry_t entry = {0};
    if (linux_fd_detach(process, descriptor, &entry) == K_OK) {
        linux_fd_entry_put(&entry);
    }
}

static int64_t linux_pipe_status(kstatus_t status) {
    return status == K_ETIMEDOUT ? K_EAGAIN : linux_copy_status(status);
}

static int64_t linux_pipe_create(process_t *process, uint32_t flags,
                                 uint64_t output_pointer) {
    pipe_endpoint_t *reader = 0;
    pipe_endpoint_t *writer = 0;
    uint32_t descriptor_flags;
    int64_t read_descriptor;
    int64_t write_descriptor;
    int32_t descriptors[2];
    kstatus_t status;
    if (process == 0 || output_pointer == 0U ||
        (flags & ~(LINUX_O_NONBLOCK | LINUX_O_CLOEXEC)) != 0U ||
        !linux_user_pointer(output_pointer, sizeof(descriptors))) {
        return flags == 0U ? LINUX_EFAULT : K_EINVAL;
    }
    status = pipe_create(0U, &reader, &writer);
    if (status != K_OK) return status;
    descriptor_flags = (uint32_t)flags &
                       (LINUX_O_NONBLOCK | LINUX_O_CLOEXEC);
    read_descriptor = linux_fd_install(process, -1, 0, reader,
                                       LINUX_FD_PIPE, descriptor_flags);
    if (read_descriptor < 0) {
        object_put(reader);
        object_put(writer);
        return read_descriptor;
    }
    reader = 0;
    write_descriptor = linux_fd_install(process, -1, 0, writer,
                                        LINUX_FD_PIPE, descriptor_flags);
    if (write_descriptor < 0) {
        linux_fd_close_silent(process, read_descriptor);
        object_put(writer);
        return write_descriptor;
    }
    writer = 0;
    descriptors[0] = (int32_t)read_descriptor;
    descriptors[1] = (int32_t)write_descriptor;
    status = copy_to_user((void __user *)(uintptr_t)output_pointer,
                          descriptors, sizeof(descriptors));
    if (status != K_OK) {
        linux_fd_close_silent(process, read_descriptor);
        linux_fd_close_silent(process, write_descriptor);
        return LINUX_EFAULT;
    }
    return K_OK;
}

static void linux_unix_socket_initialize(linux_unix_socket_t *socket,
                                         pipe_endpoint_t *read_end,
                                         pipe_endpoint_t *write_end,
                                         linux_unix_channel_t *read_channel,
                                         linux_unix_channel_t *write_channel) {
    if (socket == 0) return;
    object_header_init(&socket->object, KOBJECT_TYPE_LINUX_UNIX_SOCKET,
                       &g_linux_unix_socket_ops);
    spinlock_init(&socket->lock);
    socket->read_end = read_end;
    socket->write_end = write_end;
    socket->read_channel = read_channel;
    socket->write_channel = write_channel;
    if (read_channel != 0) object_get(read_channel);
    if (write_channel != 0) object_get(write_channel);
}

static linux_unix_socket_t *linux_unix_socket_create(void) {
    linux_unix_socket_t *socket =
        (linux_unix_socket_t *)kzalloc(sizeof(*socket), 0U);
    if (socket == 0) return 0;
    linux_unix_socket_initialize(socket, 0, 0, 0, 0);
    return socket;
}

static int64_t linux_socketpair(process_t *process, uint64_t domain,
                                uint64_t type, uint64_t protocol,
                                uint64_t output_pointer) {
    pipe_endpoint_t *a_to_b_read = 0;
    pipe_endpoint_t *a_to_b_write = 0;
    pipe_endpoint_t *b_to_a_read = 0;
    pipe_endpoint_t *b_to_a_write = 0;
    linux_unix_channel_t *a_to_b_channel = 0;
    linux_unix_channel_t *b_to_a_channel = 0;
    linux_unix_socket_t *left = 0;
    linux_unix_socket_t *right = 0;
    int32_t descriptors[2];
    uint64_t type_flags;
    uint64_t base_type;
    uint32_t descriptor_flags = LINUX_O_RDWR;
    int64_t left_descriptor;
    int64_t right_descriptor;
    kstatus_t status;
    if (process == 0 || output_pointer == 0U ||
        !linux_user_pointer(output_pointer, sizeof(descriptors))) {
        return LINUX_EFAULT;
    }
    type_flags = type & (LINUX_SOCK_NONBLOCK | LINUX_SOCK_CLOEXEC);
    base_type = type & ~(uint64_t)(LINUX_SOCK_NONBLOCK | LINUX_SOCK_CLOEXEC);
    if (domain != LINUX_AF_UNIX || protocol != 0U ||
        (base_type != LINUX_SOCK_STREAM &&
         base_type != LINUX_SOCK_SEQPACKET)) return LINUX_EAFNOSUPPORT;
    if ((type_flags & LINUX_SOCK_NONBLOCK) != 0U) {
        descriptor_flags |= LINUX_O_NONBLOCK;
    }
    if ((type_flags & LINUX_SOCK_CLOEXEC) != 0U) {
        descriptor_flags |= LINUX_O_CLOEXEC;
    }
    status = pipe_create(0U, &a_to_b_read, &a_to_b_write);
    if (status != K_OK) return status;
    status = pipe_create(0U, &b_to_a_read, &b_to_a_write);
    if (status != K_OK) {
        object_put(a_to_b_read);
        object_put(a_to_b_write);
        return status;
    }
    a_to_b_channel = linux_unix_channel_create();
    b_to_a_channel = linux_unix_channel_create();
    if (a_to_b_channel == 0 || b_to_a_channel == 0) {
        object_put(b_to_a_channel);
        object_put(a_to_b_channel);
        object_put(a_to_b_read);
        object_put(a_to_b_write);
        object_put(b_to_a_read);
        object_put(b_to_a_write);
        return K_ENOMEM;
    }
    left = (linux_unix_socket_t *)kzalloc(sizeof(*left), 0U);
    right = (linux_unix_socket_t *)kzalloc(sizeof(*right), 0U);
    if (left == 0 || right == 0) {
        kfree(right);
        kfree(left);
        object_put(b_to_a_channel);
        object_put(a_to_b_channel);
        object_put(a_to_b_read);
        object_put(a_to_b_write);
        object_put(b_to_a_read);
        object_put(b_to_a_write);
        return K_ENOMEM;
    }
    linux_unix_socket_initialize(left, b_to_a_read, a_to_b_write,
                                 b_to_a_channel, a_to_b_channel);
    b_to_a_read = 0;
    a_to_b_write = 0;
    linux_unix_socket_initialize(right, a_to_b_read, b_to_a_write,
                                 a_to_b_channel, b_to_a_channel);
    a_to_b_read = 0;
    b_to_a_write = 0;
    object_put(a_to_b_channel);
    object_put(b_to_a_channel);
    a_to_b_channel = 0;
    b_to_a_channel = 0;
    left_descriptor = linux_fd_install(process, -1, 0, left,
                                       LINUX_FD_UNIX_SOCKET,
                                       descriptor_flags);
    if (left_descriptor < 0) {
        object_put(left);
        object_put(right);
        object_put(a_to_b_read);
        object_put(a_to_b_write);
        object_put(b_to_a_read);
        object_put(b_to_a_write);
        return left_descriptor;
    }
    left = 0;
    right_descriptor = linux_fd_install(process, -1, 0, right,
                                        LINUX_FD_UNIX_SOCKET,
                                        descriptor_flags);
    if (right_descriptor < 0) {
        linux_fd_close_silent(process, left_descriptor);
        object_put(right);
        object_put(a_to_b_read);
        object_put(a_to_b_write);
        object_put(b_to_a_read);
        object_put(b_to_a_write);
        return right_descriptor;
    }
    right = 0;
    descriptors[0] = (int32_t)left_descriptor;
    descriptors[1] = (int32_t)right_descriptor;
    status = copy_to_user((void __user *)(uintptr_t)output_pointer,
                          descriptors, sizeof(descriptors));
    if (status != K_OK) {
        linux_fd_close_silent(process, left_descriptor);
        linux_fd_close_silent(process, right_descriptor);
        return LINUX_EFAULT;
    }
    return K_OK;
}

static bool linux_eventfd_write_ready(void *context) {
    return linux_eventfd_can_write((const linux_eventfd_object_t *)context);
}

static int64_t linux_eventfd_create(process_t *process, uint64_t initial,
                                    uint32_t flags) {
    linux_eventfd_object_t *eventfd;
    int64_t descriptor;
    uint32_t descriptor_flags = 0U;
    if (process == 0 || initial > UINT32_MAX ||
        (flags & ~(LINUX_O_NONBLOCK | LINUX_O_CLOEXEC |
                   LINUX_EFD_SEMAPHORE)) != 0U) return K_EINVAL;
    eventfd = (linux_eventfd_object_t *)kzalloc(sizeof(*eventfd), 0U);
    if (eventfd == 0) return K_ENOMEM;
    object_header_init(&eventfd->object, KOBJECT_TYPE_LINUX_EVENTFD,
                       &g_linux_eventfd_ops);
    spinlock_init(&eventfd->lock);
    wait_queue_init(&eventfd->waitq);
    eventfd->counter = initial;
    eventfd->semaphore = (flags & LINUX_EFD_SEMAPHORE) != 0U;
    descriptor_flags = flags & (LINUX_O_NONBLOCK | LINUX_O_CLOEXEC);
    descriptor = linux_fd_install(process, -1, 0, eventfd,
                                  LINUX_FD_EVENTFD, descriptor_flags);
    if (descriptor < 0) object_put(eventfd);
    return descriptor;
}

static int64_t linux_eventfd_read(linux_eventfd_object_t *eventfd,
                                  const linux_fd_entry_t *entry,
                                  uint64_t buffer, uint64_t length) {
    uint64_t value;
    if (eventfd == 0 || entry == 0 || length != sizeof(value) ||
        !linux_user_pointer(buffer, sizeof(value))) return K_EINVAL;
    for (;;) {
        spinlock_lock(&eventfd->lock);
        if (eventfd->counter != 0U) {
            value = eventfd->semaphore ? 1U : eventfd->counter;
            eventfd->counter -= value;
            spinlock_unlock(&eventfd->lock);
            (void)wake_all(&eventfd->waitq);
            object_notify_signaled(eventfd);
            return copy_to_user((void __user *)(uintptr_t)buffer,
                                &value, sizeof(value)) == K_OK ?
                   (int64_t)sizeof(value) : LINUX_EFAULT;
        }
        spinlock_unlock(&eventfd->lock);
        if ((entry->flags & LINUX_O_NONBLOCK) != 0U) return K_EAGAIN;
        void *objects[1] = {eventfd};
        uint32_t signaled_index = 0U;
        int64_t wait_value = 0;
        kstatus_t status = object_wait_many(objects, 1U, false, UINT64_MAX,
                                            &signaled_index, &wait_value);
        (void)signaled_index;
        (void)wait_value;
        if (status != K_OK) return status;
    }
}

static int64_t linux_eventfd_write(linux_eventfd_object_t *eventfd,
                                   const linux_fd_entry_t *entry,
                                   uint64_t buffer, uint64_t length) {
    uint64_t value;
    if (eventfd == 0 || entry == 0 || length != sizeof(value) ||
        !linux_user_pointer(buffer, sizeof(value))) return K_EINVAL;
    if (copy_from_user(&value, (const void __user *)(uintptr_t)buffer,
                       sizeof(value)) != K_OK) return LINUX_EFAULT;
    if (value == UINT64_MAX) return K_EINVAL;
    for (;;) {
        spinlock_lock(&eventfd->lock);
        if (value <= (UINT64_MAX - 1U) - eventfd->counter) {
            eventfd->counter += value;
            spinlock_unlock(&eventfd->lock);
            object_notify_signaled(eventfd);
            return (int64_t)sizeof(value);
        }
        spinlock_unlock(&eventfd->lock);
        if ((entry->flags & LINUX_O_NONBLOCK) != 0U) return K_EAGAIN;
        kstatus_t status = wait_on_queue(&eventfd->waitq,
                                          linux_eventfd_write_ready, eventfd,
                                          UINT64_MAX);
        if (status != K_OK) return status;
    }
}

static int64_t linux_epoll_create(process_t *process, uint32_t flags) {
    linux_epoll_object_t *epoll;
    int64_t descriptor;
    if (process == 0 || (flags & ~LINUX_O_CLOEXEC) != 0U) return K_EINVAL;
    epoll = (linux_epoll_object_t *)kzalloc(sizeof(*epoll), 0U);
    if (epoll == 0) return K_ENOMEM;
    object_header_init(&epoll->object, KOBJECT_TYPE_LINUX_EPOLL,
                       &g_linux_epoll_ops);
    spinlock_init(&epoll->lock);
    descriptor = linux_fd_install(process, -1, 0, epoll, LINUX_FD_EPOLL,
                                  flags & LINUX_O_CLOEXEC);
    if (descriptor < 0) object_put(epoll);
    return descriptor;
}

static linux_epoll_watch_t *linux_epoll_find_watch(
    linux_epoll_object_t *epoll, int32_t descriptor,
    const linux_fd_entry_t *target) {
    if (epoll == 0) return 0;
    for (uint32_t index = 0U; index < epoll->watch_limit; ++index) {
        linux_epoll_watch_t *watch = &epoll->watches[index];
        if (watch->descriptor == descriptor &&
            linux_epoll_watch_matches_entry(watch, target)) return watch;
    }
    return 0;
}

static linux_epoll_watch_t *linux_epoll_find_free_watch(
    linux_epoll_object_t *epoll) {
    if (epoll == 0) return 0;
    for (uint32_t index = 0U; index < epoll->watch_limit; ++index) {
        if (!epoll->watches[index].used) return &epoll->watches[index];
    }
    return epoll->watch_limit < LINUX_MAX_EPOLL_WATCHES ?
           &epoll->watches[epoll->watch_limit] : 0;
}

static bool linux_epoll_target_supported(const linux_fd_entry_t *entry) {
    if (entry == 0) return false;
    return entry->kind == LINUX_FD_FILE || entry->kind == LINUX_FD_PIPE ||
           entry->kind == LINUX_FD_NVIDIA ||
           entry->kind == LINUX_FD_SOCKET ||
           entry->kind == LINUX_FD_UNIX_SOCKET ||
           entry->kind == LINUX_FD_WINDOW ||
           entry->kind == LINUX_FD_EVENTFD;
}

static void linux_epoll_watch_adopt(linux_epoll_watch_t *watch,
                                    linux_epoll_object_t *epoll,
                                    int32_t descriptor,
                                    const linux_fd_entry_t *entry,
                                    const linux_epoll_event_t *event) {
    *watch = (linux_epoll_watch_t){
        .descriptor = descriptor,
        .events = event->events,
        .data = event->data,
        .file = entry->file,
        .private_data = entry->private_data,
        .kind = entry->kind,
        .used = 1U,
        .description = entry->description,
        .epoll = epoll,
    };
    linux_fd_description_t *description = watch->description;
    atomic_fetch_add_explicit(&description->references, 1U, memory_order_relaxed);
    spinlock_lock(&description->lock);
    watch->description_next = description->watches;
    watch->description_previous = &description->watches;
    if (description->watches != 0) {
        description->watches->description_previous = &watch->description_next;
    }
    description->watches = watch;
    spinlock_unlock(&description->lock);
}

static int64_t linux_epoll_ctl(process_t *process, int64_t epoll_descriptor,
                               uint32_t operation, int64_t target_descriptor,
                               uint64_t event_pointer) {
    linux_fd_entry_t epoll_entry = {0};
    linux_fd_entry_t target_entry = {0};
    linux_epoll_object_t *epoll;
    linux_epoll_watch_t *watch;
    linux_epoll_event_t event;
    kstatus_t status;
    if (operation < LINUX_EPOLL_CTL_ADD || operation > LINUX_EPOLL_CTL_MOD ||
        !linux_fd_valid(target_descriptor) ||
        epoll_descriptor == target_descriptor) return K_EINVAL;
    status = linux_fd_lookup(process, epoll_descriptor, &epoll_entry);
    if (status != K_OK) return status;
    if (epoll_entry.kind != LINUX_FD_EPOLL ||
        epoll_entry.private_data == 0) {
        linux_fd_entry_put(&epoll_entry);
        return K_EBADF;
    }
    epoll = (linux_epoll_object_t *)epoll_entry.private_data;
    status = linux_fd_lookup(process, target_descriptor, &target_entry);
    if (status != K_OK) {
        linux_fd_entry_put(&epoll_entry);
        return status;
    }
    if (!linux_epoll_target_supported(&target_entry)) {
        linux_fd_entry_put(&target_entry);
        linux_fd_entry_put(&epoll_entry);
        return K_EPERM;
    }
#if OS_DEBUG_SERIAL
    if (target_entry.kind == LINUX_FD_SOCKET &&
        g_debug_epoll_socket_ctl_logs < 128U) {
        kernel_serial_printf_serial_only(
            "OS_EPOLL_SOCKET_CTL_REQUEST pid=%llu epfd=%d fd=%d op=%u epoll=%llx\r\n",
            (unsigned long long)process->pid, (int)epoll_descriptor,
            (int)target_descriptor, operation,
            (unsigned long long)(uintptr_t)epoll);
        ++g_debug_epoll_socket_ctl_logs;
    }
#endif
    if (operation == LINUX_EPOLL_CTL_DEL) {
        linux_epoll_watch_t removed = {0};
        spinlock_lock(&epoll->lock);
        watch = linux_epoll_find_watch(epoll, (int32_t)target_descriptor,
                                       &target_entry);
        if (watch != 0) {
            uint32_t removed_index = (uint32_t)(watch - epoll->watches);
            linux_epoll_watch_unlink(watch);
            removed = *watch;
            *watch = (linux_epoll_watch_t){0};
            if (removed_index + 1U == epoll->watch_limit) {
                while (epoll->watch_limit != 0U &&
                       !epoll->watches[epoll->watch_limit - 1U].used) {
                    --epoll->watch_limit;
                }
            }
            ++epoll->control_sequence;
        }
        spinlock_unlock(&epoll->lock);
        if (watch == 0) {
            linux_fd_entry_put(&target_entry);
            linux_fd_entry_put(&epoll_entry);
            return K_ENOENT;
        }
        linux_epoll_watch_put(&removed);
        object_notify_changed(epoll);
        linux_fd_entry_put(&target_entry);
        linux_fd_entry_put(&epoll_entry);
        return K_OK;
    }
    if (event_pointer == 0U ||
        !linux_user_pointer(event_pointer, sizeof(event)) ||
        copy_from_user(&event, (const void __user *)(uintptr_t)event_pointer,
                       sizeof(event)) != K_OK) {
        linux_fd_entry_put(&target_entry);
        linux_fd_entry_put(&epoll_entry);
        return LINUX_EFAULT;
    }
    spinlock_lock(&epoll->lock);
    watch = linux_epoll_find_watch(epoll, (int32_t)target_descriptor,
                                   &target_entry);
    if (operation == LINUX_EPOLL_CTL_ADD) {
        if (watch != 0) {
            spinlock_unlock(&epoll->lock);
            linux_fd_entry_put(&target_entry);
            linux_fd_entry_put(&epoll_entry);
            return K_EEXIST;
        }
        watch = linux_epoll_find_free_watch(epoll);
        if (watch == 0) {
            spinlock_unlock(&epoll->lock);
            linux_fd_entry_put(&target_entry);
            linux_fd_entry_put(&epoll_entry);
            return K_ENOSPC;
        }
        linux_epoll_watch_adopt(watch, epoll, (int32_t)target_descriptor,
                                &target_entry, &event);
        uint32_t added_index = (uint32_t)(watch - epoll->watches);
        if (added_index == epoll->watch_limit) ++epoll->watch_limit;
        target_entry.file = 0;
        target_entry.private_data = 0;
    } else {
        if (watch == 0) {
            spinlock_unlock(&epoll->lock);
            linux_fd_entry_put(&target_entry);
            linux_fd_entry_put(&epoll_entry);
            return K_ENOENT;
        }
        watch->events = event.events;
        watch->data = event.data;
        watch->disabled = 0U;
    }
    ++epoll->control_sequence;
    spinlock_unlock(&epoll->lock);
#if OS_DEBUG_SERIAL
    if (target_entry.kind == LINUX_FD_SOCKET &&
        g_debug_epoll_socket_ctl_logs < 128U) {
        kernel_serial_printf_serial_only(
            "OS_EPOLL_SOCKET_CTL_OK pid=%llu epfd=%d fd=%d op=%u events=%x epoll=%llx\r\n",
            (unsigned long long)process->pid, (int)epoll_descriptor,
            (int)target_descriptor, operation, event.events,
            (unsigned long long)(uintptr_t)epoll);
        ++g_debug_epoll_socket_ctl_logs;
    }
#endif
    linux_fd_entry_put(&target_entry);
    object_notify_changed(epoll);
    linux_fd_entry_put(&epoll_entry);
    return K_OK;
}

static int64_t linux_epoll_collect(linux_epoll_object_t *epoll,
                                   linux_epoll_event_t *events,
                                   uint32_t capacity) {
    uint32_t count = 0U;
    uint32_t watch_limit;
    uint32_t start;
    uint32_t next = 0U;
    if (epoll == 0 || events == 0) return K_EINVAL;
    spinlock_lock(&epoll->lock);
    watch_limit = epoll->watch_limit;
    start = watch_limit != 0U ? epoll->next_watch_index % watch_limit : 0U;
    for (uint32_t scanned = 0U; scanned < watch_limit &&
                                count < capacity; ++scanned) {
        uint32_t index = start + scanned;
        if (index >= watch_limit) index -= watch_limit;
        linux_epoll_watch_t *watch = &epoll->watches[index];
        uint32_t ready = 0U;
        if (!linux_epoll_watch_ready(watch, &ready)) continue;
        if (watch->kind == LINUX_FD_NVIDIA &&
            (ready & (LINUX_EPOLLIN | LINUX_EPOLLPRI)) != 0U)
            (void)nv_frontend_poll_state(watch->private_data, true);
#if OS_DEBUG_SERIAL
        if (watch->kind == LINUX_FD_SOCKET &&
            g_debug_epoll_socket_ready_logs < 128U) {
            kernel_serial_printf_serial_only(
                "OS_EPOLL_SOCKET_READY fd=%d ready=%x events=%x epoll=%llx\r\n",
                watch->descriptor, ready, watch->events,
                (unsigned long long)(uintptr_t)epoll);
            ++g_debug_epoll_socket_ready_logs;
        }
#endif
        events[count++] = (linux_epoll_event_t){
            .events = ready,
            .data = watch->data,
        };
        next = index + 1U;
        if (next == watch_limit) next = 0U;
        if ((watch->events & LINUX_EPOLLONESHOT) != 0U) {
            watch->disabled = 1U;
        }
    }
    if (count != 0U) epoll->next_watch_index = next;
    spinlock_unlock(&epoll->lock);
    return (int64_t)count;
}

typedef struct linux_epoll_wait_context {
    linux_epoll_object_t *epoll;
    uint64_t control_sequence;
} linux_epoll_wait_context_t;

static bool linux_epoll_watch_has_wait_source(
    const linux_epoll_watch_t *watch) {
    if (watch == 0 || !watch->used || watch->private_data == 0 ||
        watch->disabled) return false;
    switch (watch->kind) {
    case LINUX_FD_NVIDIA:
    case LINUX_FD_PIPE:
    case LINUX_FD_SOCKET:
    case LINUX_FD_UNIX_SOCKET:
    case LINUX_FD_WINDOW:
    case LINUX_FD_EVENTFD:
        return true;
    default:
        /* 普通文件的就绪状态由 linux_epoll_collect() 提供，不存在可供订阅的对象信号队列。 */
        return false;
    }
}

static bool linux_epoll_wait_set_contains(void *const *objects, size_t count,
                                          const void *candidate) {
    for (size_t index = 1U; index < count; ++index) {
        if (objects[index] == candidate) return true;
    }
    return false;
}

static kstatus_t linux_epoll_wait_set_add(void **objects, size_t *count,
                                         size_t capacity, void *source) {
    if (source == 0 || linux_epoll_wait_set_contains(objects, *count, source))
        return K_OK;
    if (*count >= capacity) return K_ENOSPC;
    if (!object_try_get(source)) return K_ENOENT;
    objects[(*count)++] = source;
    return K_OK;
}

/* 持有监视表锁时获取源对象引用。linux_epoll_wait() 的整个生命周期内，epoll 本身已由 fd 查找固定。 */
static kstatus_t linux_epoll_capture_wait_set(
    linux_epoll_object_t *epoll, void **objects, size_t capacity,
    size_t *object_count, uint64_t *control_sequence) {
    size_t count = 1U;
    kstatus_t status = K_OK;
    if (epoll == 0 || objects == 0 || capacity == 0U || object_count == 0 ||
        control_sequence == 0) return K_EINVAL;
    objects[0] = epoll;
    spinlock_lock(&epoll->lock);
    *control_sequence = epoll->control_sequence;
    for (uint32_t index = 0U; index < epoll->watch_limit; ++index) {
        linux_epoll_watch_t *watch = &epoll->watches[index];
        void *source;
        if (!linux_epoll_watch_has_wait_source(watch)) continue;
        source = watch->kind == LINUX_FD_NVIDIA ?
                 nv_frontend_file_wait_object(watch->private_data) : watch->private_data;
        status = linux_epoll_wait_set_add(objects, &count, capacity, source);
        if (status != K_OK) break;
        if (watch->kind == LINUX_FD_UNIX_SOCKET) {
            linux_unix_socket_t *socket = (linux_unix_socket_t *)source;
            /* 管道 I/O 不会通知包装对象本身。持有 socket 锁时订阅两个端点，确保它们保持存活。 */
            spinlock_lock(&socket->lock);
            status = linux_epoll_wait_set_add(objects, &count, capacity,
                                              socket->read_end);
            if (status == K_OK) {
                status = linux_epoll_wait_set_add(objects, &count, capacity,
                                                  socket->write_end);
            }
            spinlock_unlock(&socket->lock);
            if (status != K_OK) break;
        }
    }
    spinlock_unlock(&epoll->lock);
    if (status != K_OK) {
        for (size_t index = 1U; index < count; ++index) {
            object_put(objects[index]);
        }
        return status;
    }
    *object_count = count;
    return K_OK;
}

static void linux_epoll_release_wait_set(void *const *objects, size_t count) {
    if (objects == 0) return;
    for (size_t index = 1U; index < count; ++index) {
        object_put(objects[index]);
    }
}

/* 谓词会在每个已订阅对象队列完成登记后运行。检查按掩码过滤的就绪状态和控制代数，
 * 避免 ctl 变更在快照捕获与等待者登记之间丢失。 */
static bool linux_epoll_wait_ready(void *opaque) {
    linux_epoll_wait_context_t *context =
        (linux_epoll_wait_context_t *)opaque;
    linux_epoll_object_t *epoll = context != 0 ? context->epoll : 0;
    bool ready = false;
    if (epoll == 0) return false;
    spinlock_lock(&epoll->lock);
    if (epoll->control_sequence != context->control_sequence) {
        ready = true;
    } else {
        for (uint32_t index = 0U; index < epoll->watch_limit; ++index) {
            if (linux_epoll_watch_ready(&epoll->watches[index], 0)) {
                ready = true;
                break;
            }
        }
    }
    spinlock_unlock(&epoll->lock);
    return ready;
}

/* 保存有界失败快照，而非逐次等待日志。渲染器拆除并释放内存前，先保留各阶段首次压力采样。
 * 只有成功获取写入权的一方才读取无锁物理页计数器。 */
enum { LINUX_EPOLL_FAILURE_SCRATCH, LINUX_EPOLL_FAILURE_SIGNALS };
typedef struct linux_epoll_wait_failure {
    uint64_t failures;
    uint32_t state; /* 0 empty, 1 writer, 2 published (read with acquire). */
    kstatus_t stats_status;
    uint64_t capacity;
    uint64_t captured;
    uint64_t total_pages;
    uint64_t free_pages;
} linux_epoll_wait_failure_t;
static linux_epoll_wait_failure_t g_linux_epoll_wait_failures[2];

static void linux_epoll_note_wait_failure(unsigned stage, size_t capacity,
                                           size_t captured) {
    if (stage >= 2U) return;
    linux_epoll_wait_failure_t *failure = &g_linux_epoll_wait_failures[stage];
    __atomic_fetch_add(&failure->failures, 1U, __ATOMIC_RELAXED);
    uint32_t expected = 0U;
    if (!__atomic_compare_exchange_n(&failure->state, &expected, 1U, false,
                                    __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) return;
    uint64_t total = 0U, free = 0U;
    failure->stats_status = page_memory_get_stats(&total, &free);
    failure->capacity = capacity;
    failure->captured = captured;
    failure->total_pages = total;
    failure->free_pages = free;
    __atomic_store_n(&failure->state, 2U, __ATOMIC_RELEASE);
}

static int64_t linux_epoll_wait(process_t *process, int64_t descriptor,
                                uint64_t events_pointer, int64_t maxevents,
                                int64_t timeout_ms) {
    linux_fd_entry_t entry = {0};
    linux_epoll_object_t *epoll;
    linux_epoll_event_t events[LINUX_MAX_EPOLL_WATCHES];
    void *inline_wait_objects[WAIT_INLINE_QUEUE_COUNT];
    void **wait_objects = inline_wait_objects;
    size_t wait_object_capacity = WAIT_INLINE_QUEUE_COUNT;
    size_t wait_object_count = 0U;
    uint32_t capacity;
    uint64_t timeout_ns;
    uint64_t deadline;
    kstatus_t status;
    int64_t result = K_OK;
    if (maxevents <= 0 || events_pointer == 0U) return K_EINVAL;
    capacity = (uint64_t)maxevents > LINUX_MAX_EPOLL_WATCHES ?
               LINUX_MAX_EPOLL_WATCHES : (uint32_t)maxevents;
    if (!linux_user_pointer(events_pointer,
                            (size_t)capacity * sizeof(linux_epoll_event_t))) {
        return LINUX_EFAULT;
    }
    status = linux_fd_lookup(process, descriptor, &entry);
    if (status != K_OK) return status;
    if (entry.kind != LINUX_FD_EPOLL || entry.private_data == 0) {
        linux_fd_entry_put(&entry);
        return K_EBADF;
    }
    epoll = (linux_epoll_object_t *)entry.private_data;
    timeout_ns = timeout_ms < 0 ? UINT64_MAX :
                 (uint64_t)timeout_ms > UINT64_MAX / 1000000ULL ?
                 UINT64_MAX : (uint64_t)timeout_ms * 1000000ULL;
    if (timeout_ns == UINT64_MAX) deadline = UINT64_MAX;
    else {
        uint64_t now = x86_read_tsc();
        uint64_t ticks = x86_timeout_ns_to_tsc(timeout_ns);
        deadline = ticks > UINT64_MAX - now ? UINT64_MAX : now + ticks;
    }
    for (;;) {
        int64_t count = linux_epoll_collect(epoll, events, capacity);
        if (count < 0) {
            result = count;
            break;
        }
        if (count != 0) {
            status = copy_to_user(
                (void __user *)(uintptr_t)events_pointer, events,
                (size_t)count * sizeof(linux_epoll_event_t));
            result = status == K_OK ? count : LINUX_EFAULT;
            break;
        }
        if (timeout_ms == 0) break;
        if (deadline != UINT64_MAX) {
            uint64_t now = x86_read_tsc();
            if (now >= deadline) break;
            timeout_ns = x86_tsc_to_ns(deadline - now);
        } else {
            timeout_ns = UINT64_MAX;
        }
        /* 在监视表保护锁外计算大小，并在伪唤醒之间保留分配结果。小型 epoll（包括一个 Unix socket 的
         * 三个来源）无需临时分配，更不需要旧版固定的 24 KiB 缓冲区。提示与捕获之间若监视项增长，
         * 会在下方重试。 */
        spinlock_lock(&epoll->lock);
        size_t required = (size_t)epoll->watch_limit * 3U + 1U;
        spinlock_unlock(&epoll->lock);
        if (required > LINUX_EPOLL_WAIT_SOURCES) {
            result = K_EINVAL;
            break;
        }
        if (required > wait_object_capacity) {
            void **grown = kmalloc(required * sizeof(*grown), 0U);
            if (grown == 0) {
                linux_epoll_note_wait_failure(LINUX_EPOLL_FAILURE_SCRATCH,
                                               required, 0U);
                result = K_ENOMEM;
                break;
            }
            if (wait_objects != inline_wait_objects) kfree(wait_objects);
            wait_objects = grown;
            wait_object_capacity = required;
        }
        linux_epoll_wait_context_t context = {0};
        status = linux_epoll_capture_wait_set(
            epoll, wait_objects, wait_object_capacity,
            &wait_object_count, &context.control_sequence);
        /* 溢出时 capture 会释放已固定的部分对象。重新检查就绪状态和原始截止时间，再次采样并发增长。 */
        if (status == K_ENOSPC) continue;
        if (status != K_OK) {
            result = status;
            break;
        }
        context.epoll = epoll;
        status = object_wait_on_signals(
            wait_objects, wait_object_count, linux_epoll_wait_ready, &context,
            timeout_ns);
        if (status == K_ENOMEM) {
            linux_epoll_note_wait_failure(LINUX_EPOLL_FAILURE_SIGNALS,
                                           wait_object_capacity,
                                           wait_object_count);
        }
        linux_epoll_release_wait_set(wait_objects, wait_object_count);
        wait_object_count = 0U;
        if (status == K_ETIMEDOUT) break;
        if (status != K_OK) {
            result = status;
            break;
        }
    }
    if (wait_object_count != 0U) {
        linux_epoll_release_wait_set(wait_objects, wait_object_count);
    }
    if (wait_objects != inline_wait_objects) kfree(wait_objects);
    linux_fd_entry_put(&entry);
    return result;
}

static bool linux_user_pointer(uint64_t pointer, size_t size) {
    return pointer != 0U && x86_user_range_valid(
        (const void __user *)(uintptr_t)pointer, size);
}

static int64_t linux_getresids(uint64_t real_pointer,
                               uint64_t effective_pointer,
                               uint64_t saved_pointer) {
    uint32_t zero = 0U;

    /* System 当前向 Linux 兼容进程公开一个统一身份。写入前先验证三个目标地址，
     * 避免错误指针导致结果元组只更新一部分。 */
    if (!linux_user_pointer(real_pointer, sizeof(zero)) ||
        !linux_user_pointer(effective_pointer, sizeof(zero)) ||
        !linux_user_pointer(saved_pointer, sizeof(zero))) {
        return LINUX_EFAULT;
    }
    if (copy_to_user((void __user *)(uintptr_t)real_pointer,
                     &zero, sizeof(zero)) != K_OK ||
        copy_to_user((void __user *)(uintptr_t)effective_pointer,
                     &zero, sizeof(zero)) != K_OK ||
        copy_to_user((void __user *)(uintptr_t)saved_pointer,
                     &zero, sizeof(zero)) != K_OK) {
        return LINUX_EFAULT;
    }
    return K_OK;
}

static kstatus_t linux_copy_string(uint64_t pointer, char *destination,
                                   size_t capacity) {
    if (pointer == 0U || destination == 0 || capacity < 2U) return K_EINVAL;
    for (size_t index = 0U; index + 1U < capacity; ++index) {
        char value = 0;
        kstatus_t status = copy_from_user(&value,
            (const void __user *)(uintptr_t)(pointer + index), sizeof(value));
        if (status != K_OK) return status;
        destination[index] = value;
        if (value == '\0') return K_OK;
    }
    destination[capacity - 1U] = '\0';
    return K_ENAMETOOLONG;
}

static kstatus_t linux_system_config_path(char path[LINUX_MAX_PATH]) {
    static const char previous[] = "/etc";
    static const char current[] = "/EFI/OS";
    size_t index = 0U;
    while (index < sizeof(previous) - 1U && path[index] == previous[index]) ++index;
    if (index != sizeof(previous) - 1U ||
        (path[index] != '\0' && path[index] != '/')) return K_OK;
    size_t length = index;
    while (length < LINUX_MAX_PATH && path[length] != '\0') ++length;
    size_t growth = sizeof(current) - sizeof(previous);
    if (length >= LINUX_MAX_PATH - growth) return K_ENAMETOOLONG;
    /* 预编译 libc 与驱动仍请求传统配置路径。只在 ABI 入口换算，磁盘不保留旧目录。 */
    linux_copy(path + sizeof(current) - 1U, path + index, length - index + 1U);
    linux_copy(path, current, sizeof(current) - 1U);
    return K_OK;
}

static kstatus_t linux_path_from_user(process_t *process, int64_t dirfd,
                                      uint64_t pointer, char output[256]) {
    char input[LINUX_MAX_PATH];
    char directory[LINUX_MAX_PATH];
    const char *base = 0;
    size_t base_length = 0U;
    size_t input_length = 0U;
    if (process == 0 || output == 0) return K_EPERM;
    kstatus_t status = linux_copy_string(pointer, input, sizeof(input));
    if (status != K_OK) return status;
    while (input[input_length] != '\0') ++input_length;
    if (input_length == 0U) return K_ENOENT;
    if (input[0] != '/') {
        dirfd = (int64_t)(int32_t)dirfd;
        if (dirfd != LINUX_AT_FDCWD) {
            linux_fd_entry_t entry;
            status = linux_fd_lookup(process, dirfd, &entry);
            if (status != K_OK) return status;
            status = entry.kind == LINUX_FD_FILE ?
                vfs_directory_path_kernel(entry.file, directory, sizeof(directory)) :
                K_ENOTDIR;
            linux_fd_entry_put(&entry);
            if (status != K_OK) return status;
            base = directory;
        } else {
            status = process_cwd_copy(process, directory);
            if (status != K_OK) return status;
            base = directory;
        }
        while (base_length + 1U < LINUX_MAX_PATH &&
               base[base_length] != '\0') ++base_length;
        size_t separator = base_length == 0U || base[base_length - 1U] != '/' ? 1U : 0U;
        if (base_length + separator + input_length >= LINUX_MAX_PATH) {
            return K_ENAMETOOLONG;
        }
        linux_copy(output, base, base_length);
        if (separator) {
            output[base_length++] = '/';
        }
    }
    else {
        base_length = 0U;
    }
    linux_copy(output + base_length, input, input_length + 1U);
    if (output[0] == '\0') return K_ENOENT;
    return linux_system_config_path(output);
}

static uint32_t linux_open_flags(uint64_t flags) {
    uint32_t result = (flags & LINUX_O_RDWR) == LINUX_O_RDWR ?
        VFS_OPEN_READ | VFS_OPEN_WRITE :
        ((flags & LINUX_O_WRONLY) != 0U ? VFS_OPEN_WRITE : VFS_OPEN_READ);
    if ((flags & LINUX_O_CREAT) != 0U) result |= VFS_OPEN_CREATE;
    if ((flags & LINUX_O_EXCL) != 0U) result |= VFS_OPEN_EXCLUSIVE;
    if ((flags & LINUX_O_TRUNC) != 0U) result |= VFS_OPEN_TRUNCATE;
    if ((flags & LINUX_O_APPEND) != 0U) result |= VFS_OPEN_APPEND;
    if ((flags & LINUX_O_DIRECTORY) != 0U) result |= VFS_OPEN_DIRECTORY;
    return result;
}

static bool linux_exact_path(const char *path, const char *expected) {
    size_t index = 0U;
    if (path == 0 || expected == 0) return false;
    while (path[index] != '\0' && expected[index] != '\0') {
        if (path[index] != expected[index]) return false;
        ++index;
    }
    return path[index] == '\0' && expected[index] == '\0';
}

static int64_t linux_readlinkat(process_t *process, int64_t dirfd,
                                uint64_t path_pointer, uint64_t buffer,
                                uint64_t size) {
    char path[LINUX_MAX_PATH];
    char resolved[LINUX_MAX_PATH], link_text[LINUX_MAX_PATH];
    const char *target;
    size_t target_length = 0U;
    size_t count;
    kstatus_t status;

    status = linux_path_from_user(process, dirfd, path_pointer, path);
    if (status != K_OK) return status;
    if (!linux_exact_path(path, "/proc/self/exe")) {
        status = vfs_resolve_path_kernel(path, false, false, resolved);
        if (status != K_OK) return status;
        status = vfs_readlink_kernel(resolved, link_text, sizeof(link_text), &target_length);
        if (status != K_OK) return status;
        target = link_text;
    } else {
        target = process->linux_exe;
        while (target_length < sizeof(process->linux_exe) && target[target_length] != '\0')
            ++target_length;
    }
    if (target_length == 0U || target_length >= sizeof(process->linux_exe))
        return K_ENOENT;
    if (size == 0U) return K_EINVAL;
    if (size > (uint64_t)SIZE_MAX) return K_EOVERFLOW;
    count = size < (uint64_t)target_length ? (size_t)size : target_length;
    if (!linux_user_pointer(buffer, count)) return LINUX_EFAULT;
    status = copy_to_user((void __user *)(uintptr_t)buffer, target, count);
    return status == K_OK ? (int64_t)count : LINUX_EFAULT;
}

/* 读取少量内存计数时，不应要求分配 128 KiB Buddy 块。maps 缓冲区会在 map_lock 外按需扩容。 */
#define LINUX_PROC_DATA_MAX 4096U
#define LINUX_PROC_MAPS_MAX (16U * 1024U * 1024U)
#define LINUX_PROC_MAP_LINE_MAX (128U + 4U * LINUX_MAX_PATH)

enum linux_proc_node {
    LINUX_PROC_NODE_PROC_DIR = 1U,
    LINUX_PROC_NODE_SELF_DIR,
    LINUX_PROC_NODE_TASK_DIR,
    LINUX_PROC_NODE_SYS_DIR,
    LINUX_PROC_NODE_SYS_DEVICES_DIR,
    LINUX_PROC_NODE_SYS_SYSTEM_DIR,
    LINUX_PROC_NODE_SYS_CPU_DIR,
    LINUX_PROC_NODE_CPU_DIR,
    LINUX_PROC_NODE_CACHE_DIR,
    LINUX_PROC_NODE_CACHE_INDEX0_DIR,
    LINUX_PROC_NODE_TOPOLOGY_DIR,
    LINUX_PROC_NODE_CMDLINE,
    LINUX_PROC_NODE_COMM,
    LINUX_PROC_NODE_STATUS,
    LINUX_PROC_NODE_CPUINFO,
    LINUX_PROC_NODE_MAPS,
    LINUX_PROC_NODE_STAT,
    LINUX_PROC_NODE_CPU_ONLINE,
    LINUX_PROC_NODE_CPU_CACHE_LEVEL,
    LINUX_PROC_NODE_CPU_CACHE_ID,
    LINUX_PROC_NODE_CPU_PACKAGE,
    LINUX_PROC_NODE_CPU_CORE_CPUS,
    LINUX_PROC_NODE_CPU_THREAD_SIBLINGS,
    LINUX_PROC_NODE_NVIDIA_PARAMS,
    LINUX_PROC_NODE_SELF_FD_DIR,
    LINUX_PROC_NODE_MEMINFO,
};

struct linux_proc_file {
    atomic_uint refs;
    spinlock_t lock;
    uint64_t inode;
    uint64_t position;
    uint32_t length;
    uint32_t cpu_index;
    uint32_t cpu_cache_index;
    uint64_t process_id;
    uint8_t node;
    uint8_t reserved[3];
    uint16_t fd_count;
    uint16_t fd_entries[OS_LINUX_FD_LIMIT];
    char *extended_data;
    uint32_t extended_capacity;
    char data[LINUX_PROC_DATA_MAX];
};

#if OS_DEBUG_SERIAL
static void linux_proc_maps_diag(const char *stage, uint32_t value) {
    kernel_serial_write("OS_PROC_MAPS_");
    kernel_serial_write(stage);
    kernel_serial_write_u32(value);
    kernel_serial_write("\r\n");
}
#endif

static atomic_uint_fast64_t g_linux_proc_next_inode;

static char *linux_proc_data(linux_proc_file_t *file) {
    return file->extended_data != 0 ? file->extended_data : file->data;
}

static size_t linux_proc_capacity(const linux_proc_file_t *file) {
    return file->extended_data != 0 ? file->extended_capacity :
                                     sizeof(file->data);
}

static bool linux_proc_append_bytes(linux_proc_file_t *file,
                                    const void *source, size_t length) {
    if (file == 0 || source == 0 ||
        file->length > linux_proc_capacity(file) ||
        length > linux_proc_capacity(file) - file->length) {
        return false;
    }
    linux_copy(linux_proc_data(file) + file->length, source, length);
    file->length += (uint32_t)length;
    return true;
}

static bool linux_proc_append_text(linux_proc_file_t *file, const char *text) {
    size_t length = 0U;
    if (text == 0) return false;
    while (text[length] != '\0') ++length;
    return linux_proc_append_bytes(file, text, length);
}

static bool linux_proc_append_char(linux_proc_file_t *file, char value) {
    return linux_proc_append_bytes(file, &value, sizeof(value));
}

static size_t linux_proc_u64_text(uint64_t value, char *output,
                                  size_t capacity) {
    char reversed[24];
    size_t count = 0U;
    if (output == 0 || capacity == 0U) return 0U;
    do {
        reversed[count++] = (char)('0' + (value % 10U));
        value /= 10U;
    } while (value != 0U && count < sizeof(reversed));
    if (count + 1U > capacity) return 0U;
    for (size_t index = 0U; index < count; ++index) {
        output[index] = reversed[count - index - 1U];
    }
    output[count] = '\0';
    return count;
}

static bool linux_proc_append_u64(linux_proc_file_t *file, uint64_t value) {
    char digits[24];
    size_t count = linux_proc_u64_text(value, digits, sizeof(digits));
    return count != 0U && linux_proc_append_bytes(file, digits, count);
}

static void linux_proc_copy_process_name(process_t *process,
                                         char output[OS_PROCESS_NAME_MAX]) {
    if (output == 0) return;
    linux_zero(output, OS_PROCESS_NAME_MAX);
    if (process == 0) {
        linux_copy(output, "system", sizeof("system"));
        return;
    }
    sched_preempt_disable();
    spinlock_lock(&process->info_lock);
    linux_copy(output, process->name, OS_PROCESS_NAME_MAX);
    spinlock_unlock(&process->info_lock);
    sched_preempt_enable();
    if (output[0] == '\0') linux_copy(output, "system", sizeof("system"));
}

static uint32_t linux_proc_cpu_count(void) {
    uint32_t count = x86_smp_discovered_count();
    if (count == 0U) count = 1U;
    return count < MAX_CPUS ? count : MAX_CPUS;
}

static bool linux_proc_cpu_online(uint32_t cpu, uint32_t count) {
    if (cpu >= count) return false;
    return x86_smp_discovered_count() == 0U ? cpu == 0U :
           x86_smp_cpu_online(cpu);
}

static void linux_proc_cpuid(uint32_t leaf, uint32_t subleaf,
                             uint32_t *eax, uint32_t *ebx,
                             uint32_t *ecx, uint32_t *edx) {
    __asm__ volatile ("cpuid"
                      : "=a"(*eax), "=b"(*ebx), "=c"(*ecx), "=d"(*edx)
                      : "a"(leaf), "c"(subleaf));
}

static void linux_proc_cpu_topology_shifts(uint32_t *smt_shift,
                                           uint32_t *core_shift,
                                           uint32_t *package_shift) {
    uint32_t maximum_basic = 0U;
    uint32_t ebx;
    uint32_t ecx;
    uint32_t edx;
    uint32_t smt = 0U;
    uint32_t core = 0U;
    uint32_t package = 32U;
    if (smt_shift == 0 || core_shift == 0 || package_shift == 0) return;
    linux_proc_cpuid(0U, 0U, &maximum_basic, &ebx, &ecx, &edx);
    if (maximum_basic >= 0xBU) {
        for (uint32_t level = 0U; level < 8U; ++level) {
            uint32_t eax;
            uint32_t level_ebx;
            uint32_t level_ecx;
            uint32_t level_edx;
            linux_proc_cpuid(0xBU, level, &eax, &level_ebx, &level_ecx,
                             &level_edx);
            if ((level_ebx & 0xFFFFU) == 0U) break;
            switch ((level_ecx >> 8) & 0xFFU) {
            case 1U:
                smt = eax & 0x1FU;
                break;
            case 2U:
                core = eax & 0x1FU;
                break;
            default:
                break;
            }
        }
    }
    if (core != 0U) package = core;
    if (core == 0U) core = smt;
    *smt_shift = smt;
    *core_shift = core;
    *package_shift = package;
}

static uint32_t linux_proc_cpu_apic_id(uint32_t cpu_index) {
    const x86_acpi_platform_t *platform = x86_acpi_platform();
    if (platform != 0 && cpu_index < platform->cpu_count) {
        return platform->cpus[cpu_index].apic_id;
    }
    return cpu_index == x86_current_cpu_index() ? x86_current_apic_id() :
           cpu_index;
}

static bool linux_proc_cpu_same_core(uint32_t left, uint32_t right) {
    uint32_t smt_shift;
    uint32_t core_shift;
    uint32_t package_shift;
    uint32_t left_id;
    uint32_t right_id;
    (void)smt_shift;
    (void)package_shift;
    linux_proc_cpu_topology_shifts(&smt_shift, &core_shift, &package_shift);
    left_id = linux_proc_cpu_apic_id(left);
    right_id = linux_proc_cpu_apic_id(right);
    if (core_shift >= 32U) return true;
    return (left_id >> core_shift) == (right_id >> core_shift);
}

static uint32_t linux_proc_cpu_package_id(uint32_t cpu_index) {
    uint32_t smt_shift;
    uint32_t core_shift;
    uint32_t package_shift;
    uint32_t apic_id;
    (void)smt_shift;
    (void)core_shift;
    linux_proc_cpu_topology_shifts(&smt_shift, &core_shift, &package_shift);
    apic_id = linux_proc_cpu_apic_id(cpu_index);
    return package_shift >= 32U ? 0U : apic_id >> package_shift;
}

static bool linux_proc_append_hex32(linux_proc_file_t *file, uint32_t value) {
    static const char digits[] = "0123456789abcdef";
    for (int32_t shift = 28; shift >= 0; shift -= 4) {
        if (!linux_proc_append_char(file, digits[(value >> shift) & 0xFU])) {
            return false;
        }
    }
    return true;
}

static bool linux_proc_append_hex64(linux_proc_file_t *file, uint64_t value) {
    static const char digits[] = "0123456789abcdef";
    for (int32_t shift = 60; shift >= 0; shift -= 4) {
        if (!linux_proc_append_char(file, digits[(value >> shift) & 0xFU])) {
            return false;
        }
    }
    return true;
}

/* 与 proc maps 一致转义换行，避免一个文件名伪造多条映射记录。 */
static bool linux_proc_append_maps_label(linux_proc_file_t *file, const char *label) {
    for (size_t index = 0U; label[index] != 0; ++index) {
        if (label[index] == '\n') {
            if (!linux_proc_append_text(file, "\\012")) return false;
        } else if (!linux_proc_append_char(file, label[index])) return false;
    }
    return true;
}

static bool linux_proc_append_maps_line(linux_proc_file_t *file,
                                        const vm_area_t *area,
                                        const process_t *process) {
    const vm_object_t *object;
    uint64_t offset = 0U;
    uint64_t inode = 0U;
    const char *label = 0;
    char mapping_path[256];
    bool deleted = false;
    if (file == 0 || area == 0 || area->end <= area->start) return false;
    object = area->object;
    if (object != 0 && object->type == VM_OBJECT_FILE) {
        offset = object->u.file.file_offset;
        if (offset > UINT64_MAX - area->object_offset) return false;
        offset += area->object_offset;
        vnode_t *vnode = (vnode_t *)object->u.file.mapping;
        if (vnode != 0 && vnode->object.type == KOBJECT_TYPE_VNODE) {
            inode = vnode->inode_id;
            if (vfs_vnode_path_kernel(vnode, mapping_path, sizeof(mapping_path), &deleted) == K_OK)
                label = mapping_path;
        }
        if (label == 0) label = "[file]";
    } else if ((area->flags & VM_MAP_STACK) != 0U) {
        label = "[stack]";
    } else if ((area->flags & VM_MAP_GUARD) != 0U) {
        label = "[guard]";
    } else if (process != 0 && process->linux_brk_base >= area->start &&
               process->linux_brk_base < area->end) {
        label = "[heap]";
    } else {
        label = "[anon]";
    }

    if (!linux_proc_append_hex64(file, area->start) ||
        !linux_proc_append_char(file, '-') ||
        !linux_proc_append_hex64(file, area->end) ||
        !linux_proc_append_char(file, ' ') ||
        !linux_proc_append_char(file, (area->prot & VM_PROT_READ) != 0U ?
                                      'r' : '-') ||
        !linux_proc_append_char(file, (area->prot & VM_PROT_WRITE) != 0U ?
                                      'w' : '-') ||
        !linux_proc_append_char(file, (area->prot & VM_PROT_EXEC) != 0U ?
                                      'x' : '-') ||
        !linux_proc_append_char(file, (area->flags & VM_MAP_SHARED) != 0U ?
                                      's' : 'p') ||
        !linux_proc_append_char(file, ' ') ||
        !linux_proc_append_hex64(file, offset) ||
        !linux_proc_append_text(file, " 00:00 ") ||
        !linux_proc_append_u64(file, inode) ||
        !linux_proc_append_char(file, ' ') ||
        !linux_proc_append_maps_label(file, label) ||
        (deleted && !linux_proc_append_text(file, " (deleted)")) ||
        !linux_proc_append_char(file, '\n')) {
        return false;
    }
    return true;
}

static bool linux_proc_build_maps(linux_proc_file_t *file,
                                  process_t *process) {
    vm_space_t *space;
    uint32_t initial_length;
#if OS_DEBUG_SERIAL
    uint32_t area_count = 0U;
#endif
    if (file == 0) return false;
    initial_length = file->length;
    if (initial_length > linux_proc_capacity(file)) return false;
    space = process != 0 ? process_vm_reference(process) : 0;
    if (space == 0) {
#if OS_DEBUG_SERIAL
        linux_proc_maps_diag("NO_VM=", 0U);
#endif
        return true;
    }

retry_snapshot:
    file->length = initial_length;
#if OS_DEBUG_SERIAL
    area_count = 0U;
#endif
    map_lock(space);
    for (list_head_t *item = space->area_list.next;
         item != &space->area_list; item = item->next) {
        vm_area_t *area = area_from_list(item);
        /* maps 文件被截断时绝不能报告快照成功。尤其不能仅因 Chrome 创建了许多低地址映射，
         * 就漏掉高地址栈。先在 VM 锁外分配，再对同一份仍持有引用的 VM 重新生成完整快照。 */
        size_t capacity = linux_proc_capacity(file);
        if (file->length > capacity ||
            capacity - file->length < LINUX_PROC_MAP_LINE_MAX) {
            map_unlock(space);
            if (capacity >= LINUX_PROC_MAPS_MAX) {
                vm_space_put(space);
                return false;
            }
            size_t next_capacity = capacity * 2U;
            if (next_capacity > LINUX_PROC_MAPS_MAX)
                next_capacity = LINUX_PROC_MAPS_MAX;
            char *expanded = (char *)kmalloc(next_capacity, 0U);
            if (expanded == 0) {
                vm_space_put(space);
                return false;
            }
            linux_copy(expanded, linux_proc_data(file), initial_length);
            kfree(file->extended_data);
            file->extended_data = expanded;
            file->extended_capacity = (uint32_t)next_capacity;
            goto retry_snapshot;
        }
        if (!linux_proc_append_maps_line(file, area, process)) {
#if OS_DEBUG_SERIAL
            linux_proc_maps_diag("LINE_FAIL=", area_count);
#endif
            map_unlock(space);
            vm_space_put(space);
            return false;
        }
#if OS_DEBUG_SERIAL
        ++area_count;
#endif
    }
    map_unlock(space);
    vm_space_put(space);
#if OS_DEBUG_SERIAL
    linux_proc_maps_diag("OK=", file->length);
#endif
    return true;
}

static bool linux_proc_build_cpu_mask(linux_proc_file_t *file,
                                      uint32_t target_cpu) {
    uint32_t count = linux_proc_cpu_count();
    uint32_t words = (count + 31U) / 32U;
    if (words == 0U || target_cpu >= count) return false;
    for (uint32_t word = words; word != 0U; --word) {
        uint32_t word_index = word - 1U;
        uint32_t mask = 0U;
        for (uint32_t cpu = word_index * 32U;
             cpu < count && cpu < (word_index + 1U) * 32U; ++cpu) {
            if (linux_proc_cpu_same_core(target_cpu, cpu)) {
                mask |= 1U << (cpu - word_index * 32U);
            }
        }
        if (word != words && !linux_proc_append_char(file, ',')) return false;
        if (!linux_proc_append_hex32(file, mask)) return false;
    }
    return linux_proc_append_char(file, '\n');
}

static bool linux_proc_build_cpu_online(linux_proc_file_t *file) {
    uint32_t count = linux_proc_cpu_count();
    bool first = true;
    uint32_t cpu = 0U;
    while (cpu < count) {
        uint32_t start;
        uint32_t end;
        while (cpu < count && !linux_proc_cpu_online(cpu, count)) ++cpu;
        if (cpu == count) break;
        start = cpu++;
        end = start;
        while (cpu < count && linux_proc_cpu_online(cpu, count)) {
            end = cpu++;
        }
        if (!first && !linux_proc_append_char(file, ',')) return false;
        if (!linux_proc_append_u64(file, start)) return false;
        if (end != start &&
            (!linux_proc_append_char(file, '-') ||
             !linux_proc_append_u64(file, end))) return false;
        first = false;
    }
    if (first && !linux_proc_append_char(file, '0')) return false;
    return linux_proc_append_char(file, '\n');
}

static bool linux_proc_append_cpu_stat_line(linux_proc_file_t *file,
                                            const char *prefix,
                                            uint32_t cpu) {
    if (!linux_proc_append_text(file, prefix)) return false;
    if (cpu != UINT32_MAX && !linux_proc_append_u64(file, cpu)) return false;
    for (uint32_t field = 0U; field < 10U; ++field) {
        if (!linux_proc_append_text(file, " 0")) return false;
    }
    return linux_proc_append_char(file, '\n');
}

static bool linux_proc_build_meminfo(linux_proc_file_t *file) {
    uint64_t total, free;
    if (page_memory_get_stats(&total, &free) != K_OK || free > total ||
        total > UINT64_MAX / (PAGE_SIZE / 1024U)) return false;
    /* free 包含可复用的 CPU 池槽位。这些页面上的所有者标签可能已过期，不要将其计为活动匿名分配。
     * 没有回收估算器时，MemAvailable 保守地报告空闲 RAM。未实现的缓存/回收统计应省略，不能伪造。 */
    const char *names[] = {"MemTotal:\t", "MemFree:\t", "MemAvailable:\t"};
    const uint64_t pages[] = {total, free, free};
    for (size_t index = 0U; index < 3U; ++index) {
        if (!linux_proc_append_text(file, names[index]) ||
            !linux_proc_append_u64(file, pages[index] * (PAGE_SIZE / 1024U)) ||
            !linux_proc_append_text(file, " kB\n")) return false;
    }
    return linux_proc_append_text(file, "SwapTotal:\t0 kB\nSwapFree:\t0 kB\n");
}

static bool linux_proc_build_data(linux_proc_file_t *file,
                                  process_t *process) {
    char name[OS_PROCESS_NAME_MAX];
    size_t name_length;
    uint32_t thread_count;
    if (file == 0) return false;
    file->length = 0U;
    switch (file->node) {
    case LINUX_PROC_NODE_MEMINFO:
        return linux_proc_build_meminfo(file);
    case LINUX_PROC_NODE_CMDLINE:
        linux_proc_copy_process_name(process, name);
        name_length = 0U;
        while (name_length + 1U < sizeof(name) && name[name_length] != '\0') {
            ++name_length;
        }
        return linux_proc_append_bytes(file, name, name_length) &&
               linux_proc_append_char(file, '\0');
    case LINUX_PROC_NODE_COMM:
        linux_proc_copy_process_name(process, name);
        if (!linux_proc_append_text(file, name)) return false;
        return linux_proc_append_char(file, '\n');
    case LINUX_PROC_NODE_STATUS:
        linux_proc_copy_process_name(process, name);
        thread_count = 0U;
        if (process != 0) {
            spinlock_lock(&process->thread_lock);
            thread_count = process->thread_count;
            spinlock_unlock(&process->thread_lock);
        }
        return linux_proc_append_text(file, "Name:\t") &&
               linux_proc_append_text(file, name) &&
               linux_proc_append_text(file, "\nState:\tR (running)\nPid:\t") &&
               linux_proc_append_u64(file, process != 0 ? process->pid : 0U) &&
               linux_proc_append_text(file, "\nThreads:\t") &&
               linux_proc_append_u64(file, thread_count) &&
               linux_proc_append_char(file, '\n');
    case LINUX_PROC_NODE_CPUINFO:
        for (uint32_t cpu = 0U; cpu < linux_proc_cpu_count(); ++cpu) {
            if (!linux_proc_append_text(file, "processor\t: ") ||
                !linux_proc_append_u64(file, cpu) ||
                !linux_proc_append_text(file,
                                        "\nmodel name\t: System x86_64\n")) {
                return false;
            }
        }
        return true;
    case LINUX_PROC_NODE_MAPS:
        return linux_proc_build_maps(file, process);
    case LINUX_PROC_NODE_STAT:
        if (!linux_proc_append_cpu_stat_line(file, "cpu ", UINT32_MAX)) {
            return false;
        }
        for (uint32_t cpu = 0U; cpu < linux_proc_cpu_count(); ++cpu) {
            if (!linux_proc_append_cpu_stat_line(file, "cpu", cpu)) {
                return false;
            }
        }
        return true;
    case LINUX_PROC_NODE_CPU_ONLINE:
        return linux_proc_build_cpu_online(file);
    case LINUX_PROC_NODE_CPU_CACHE_LEVEL:
        return linux_proc_append_u64(file, file->cpu_cache_index + 1U) &&
               linux_proc_append_char(file, '\n');
    case LINUX_PROC_NODE_CPU_CACHE_ID:
        return linux_proc_append_u64(file, file->cpu_cache_index) &&
               linux_proc_append_char(file, '\n');
    case LINUX_PROC_NODE_CPU_PACKAGE:
        return linux_proc_append_u64(file,
                                     linux_proc_cpu_package_id(file->cpu_index)) &&
               linux_proc_append_char(file, '\n');
    case LINUX_PROC_NODE_CPU_CORE_CPUS:
    case LINUX_PROC_NODE_CPU_THREAD_SIBLINGS:
        return linux_proc_build_cpu_mask(file, file->cpu_index);
    case LINUX_PROC_NODE_NVIDIA_PARAMS:
        /* 此快照仅包含真实 System RM/frontend 路径支持的参数。
         * 只有硬件 GSP 状态达到 READY 后才公开该文件。 */
        return linux_proc_append_text(file, "ModifyDeviceFiles: 0\n") &&
               linux_proc_append_text(file, "RegisterPCIDriver: 1\n") &&
               linux_proc_append_text(file, "EnableGpuFirmware: 1\n") &&
               linux_proc_append_text(file, "EnableGpuFirmwareLogs: 0\n") &&
               linux_proc_append_text(file, "CoherentGPUMemoryMode: \"\"\n") &&
               linux_proc_append_text(file, "RegistryDwords: \"\"\n") &&
               linux_proc_append_text(file,
                                      "RegistryDwordsPerDevice: \"\"\n") &&
               linux_proc_append_text(file, "RmMsg: \"\"\n") &&
               linux_proc_append_text(file, "GpuBlacklist: \"\"\n") &&
               linux_proc_append_text(file, "TemporaryFilePath: \"\"\n") &&
               linux_proc_append_text(file, "ExcludedGpus: \"\"\n");
    default:
        return true;
    }
}

static void linux_proc_file_get(linux_proc_file_t *file) {
    if (file != 0) {
        (void)atomic_fetch_add_explicit(&file->refs, 1U, memory_order_relaxed);
    }
}

static void linux_proc_file_put(linux_proc_file_t *file) {
    if (file != 0 && atomic_fetch_sub_explicit(&file->refs, 1U,
                                                memory_order_acq_rel) == 1U) {
        kfree(file->extended_data);
        kfree(file);
    }
}

static bool linux_proc_node_is_directory(uint8_t node) {
    return (node <= LINUX_PROC_NODE_TOPOLOGY_DIR &&
            node >= LINUX_PROC_NODE_PROC_DIR) ||
           node == LINUX_PROC_NODE_SELF_FD_DIR;
}

static linux_proc_file_t *linux_proc_file_create(process_t *process,
                                                 uint8_t node,
                                                 uint32_t cpu_index,
                                                 uint32_t cpu_cache_index) {
    linux_proc_file_t *file = (linux_proc_file_t *)kzalloc(sizeof(*file), 0U);
    uint64_t inode;
    if (file == 0) return 0;
    inode = atomic_fetch_add_explicit(&g_linux_proc_next_inode, 1U,
                                      memory_order_relaxed) + 1U;
    if (inode == 0U) {
        kfree(file);
        return 0;
    }
    atomic_init(&file->refs, 1U);
    spinlock_init(&file->lock);
    file->inode = inode;
    file->node = node;
    file->cpu_index = cpu_index;
    file->cpu_cache_index = cpu_cache_index;
    file->process_id = process != 0 ? process->pid : 0U;
    if (node == LINUX_PROC_NODE_CPUINFO || node == LINUX_PROC_NODE_STAT) {
        /* 两种格式每 CPU 使用量均小于 64 字节；stat 还包含汇总行。
         * 格式化前一次性预留容量，不能因主机 CPU 数超过内嵌区可容纳数量而截断快照。 */
        size_t capacity = ((size_t)linux_proc_cpu_count() + 1U) * 64U;
        if (capacity > sizeof(file->data)) {
            file->extended_data = kmalloc(capacity, 0U);
            if (file->extended_data == 0) {
                linux_proc_file_put(file);
                return 0;
            }
            file->extended_capacity = (uint32_t)capacity;
        }
    }
    if (node == LINUX_PROC_NODE_SELF_FD_DIR && process != 0) {
        /* 在 linux_fd_install() 加入目录自身 fd 前获取快照。这正是 CloseSuperfluousFds() 要检查的集合，
         * 也可避免 procfs 枚举与实时 fd 表发生竞争。 */
        spinlock_lock(&process->linux_fds.lock);
        for (uint32_t descriptor = 0U;
             descriptor < OS_LINUX_FD_LIMIT; ++descriptor) {
            if (process->linux_fds.entries[descriptor].kind !=
                LINUX_FD_UNUSED) {
                file->fd_entries[file->fd_count++] = (uint16_t)descriptor;
            }
        }
        spinlock_unlock(&process->linux_fds.lock);
    }
    if (!linux_proc_node_is_directory(node) &&
        !linux_proc_build_data(file, process)) {
        linux_proc_file_put(file);
        return 0;
    }
    return file;
}

static bool linux_proc_parse_cpu_leaf(const char *path, uint32_t *cpu,
                                      const char **leaf) {
    static const char prefix[] = "/sys/devices/system/cpu/cpu";
    size_t index = 0U;
    uint64_t value = 0U;
    if (path == 0 || cpu == 0 || leaf == 0) return false;
    while (prefix[index] != '\0') {
        if (path[index] != prefix[index]) return false;
        ++index;
    }
    if (path[index] < '0' || path[index] > '9') return false;
    do {
        value = value * 10U + (uint64_t)(path[index] - '0');
        ++index;
    } while (path[index] >= '0' && path[index] <= '9');
    if (value >= MAX_CPUS || path[index] != '/') return false;
    *cpu = (uint32_t)value;
    *leaf = path + index + 1U;
    return true;
}

static bool linux_proc_parse_cache_leaf(const char *leaf,
                                        uint32_t *cache_index,
                                        const char **attribute) {
    static const char prefix[] = "cache/index";
    size_t index = 0U;
    uint64_t value = 0U;
    if (leaf == 0 || cache_index == 0 || attribute == 0) return false;
    while (prefix[index] != '\0') {
        if (leaf[index] != prefix[index]) return false;
        ++index;
    }
    if (leaf[index] < '0' || leaf[index] > '9') return false;
    do {
        value = value * 10U + (uint64_t)(leaf[index] - '0');
        ++index;
    } while (leaf[index] >= '0' && leaf[index] <= '9');
    if (value >= 16U || leaf[index] != '/') return false;
    *cache_index = (uint32_t)value;
    *attribute = leaf + index + 1U;
    return true;
}

static kstatus_t linux_proc_open_path(process_t *process, const char *path,
                                      linux_proc_file_t **output) {
    uint8_t node = 0U;
    uint32_t cpu_index = 0U;
    uint32_t cache_index = 0U;
    const char *leaf = 0;
    const char *attribute = 0;
    if (process == 0 || path == 0 || output == 0) return K_EINVAL;
    if (linux_exact_path(path, "/proc")) node = LINUX_PROC_NODE_PROC_DIR;
    else if (linux_exact_path(path, "/proc/self")) {
        node = LINUX_PROC_NODE_SELF_DIR;
    } else if (linux_exact_path(path, "/proc/self/fd")) {
        node = LINUX_PROC_NODE_SELF_FD_DIR;
    } else if (linux_exact_path(path, "/proc/self/task")) {
        node = LINUX_PROC_NODE_TASK_DIR;
    } else if (linux_exact_path(path, "/proc/self/cmdline")) {
        node = LINUX_PROC_NODE_CMDLINE;
    } else if (linux_exact_path(path, "/proc/self/comm")) {
        node = LINUX_PROC_NODE_COMM;
    } else if (linux_exact_path(path, "/proc/self/status")) {
        node = LINUX_PROC_NODE_STATUS;
    } else if (linux_exact_path(path, "/proc/cpuinfo")) {
        node = LINUX_PROC_NODE_CPUINFO;
    } else if (linux_exact_path(path, "/proc/meminfo")) {
        node = LINUX_PROC_NODE_MEMINFO;
    } else if (linux_exact_path(path, "/proc/self/maps")) {
#if OS_DEBUG_SERIAL
        linux_proc_maps_diag("PATH=", 1U);
#endif
        node = LINUX_PROC_NODE_MAPS;
    } else if (linux_exact_path(path, "/proc/stat")) {
        node = LINUX_PROC_NODE_STAT;
    } else if (linux_exact_path(path, "/proc/driver/nvidia/params")) {
        if (!nv_frontend_driver_ready()) return K_ENOENT;
        node = LINUX_PROC_NODE_NVIDIA_PARAMS;
    } else if (linux_exact_path(path, "/sys")) {
        node = LINUX_PROC_NODE_SYS_DIR;
    } else if (linux_exact_path(path, "/sys/devices")) {
        node = LINUX_PROC_NODE_SYS_DEVICES_DIR;
    } else if (linux_exact_path(path, "/sys/devices/system")) {
        node = LINUX_PROC_NODE_SYS_SYSTEM_DIR;
    } else if (linux_exact_path(path, "/sys/devices/system/cpu")) {
        node = LINUX_PROC_NODE_SYS_CPU_DIR;
    } else if (linux_exact_path(path, "/sys/devices/system/cpu/online")) {
        node = LINUX_PROC_NODE_CPU_ONLINE;
    } else if (linux_proc_parse_cpu_leaf(path, &cpu_index, &leaf) &&
               linux_proc_parse_cache_leaf(leaf, &cache_index, &attribute) &&
               linux_exact_path(attribute, "level")) {
        node = LINUX_PROC_NODE_CPU_CACHE_LEVEL;
    } else if (linux_proc_parse_cpu_leaf(path, &cpu_index, &leaf) &&
               linux_proc_parse_cache_leaf(leaf, &cache_index, &attribute) &&
               linux_exact_path(attribute, "id")) {
        node = LINUX_PROC_NODE_CPU_CACHE_ID;
    } else if (linux_proc_parse_cpu_leaf(path, &cpu_index, &leaf) &&
               linux_exact_path(leaf, "topology/physical_package_id")) {
        node = LINUX_PROC_NODE_CPU_PACKAGE;
    } else if (linux_proc_parse_cpu_leaf(path, &cpu_index, &leaf) &&
               linux_exact_path(leaf, "topology/core_cpus")) {
        node = LINUX_PROC_NODE_CPU_CORE_CPUS;
    } else if (linux_proc_parse_cpu_leaf(path, &cpu_index, &leaf) &&
               linux_exact_path(leaf, "topology/thread_siblings")) {
        node = LINUX_PROC_NODE_CPU_THREAD_SIBLINGS;
    } else {
        return K_ENOENT;
    }
    *output = linux_proc_file_create(process, node, cpu_index, cache_index);
#if OS_DEBUG_SERIAL
    if (node == LINUX_PROC_NODE_MAPS) {
        linux_proc_maps_diag("CREATE=", *output != 0 ? 1U : 0U);
    }
#endif
    return *output != 0 ? K_OK : K_ENOMEM;
}

static bool linux_proc_set_dirent_name(char *name, size_t capacity,
                                       const char *source) {
    size_t length = 0U;
    if (name == 0 || source == 0 || capacity == 0U) return false;
    while (source[length] != '\0') ++length;
    if (length + 1U > capacity) return false;
    linux_copy(name, source, length + 1U);
    return true;
}

static uint32_t linux_proc_directory_count(const linux_proc_file_t *file) {
    if (file == 0) return 0U;
    switch (file->node) {
    case LINUX_PROC_NODE_PROC_DIR: return 6U;
    case LINUX_PROC_NODE_SELF_DIR: return 7U;
    case LINUX_PROC_NODE_TASK_DIR: return 3U;
    case LINUX_PROC_NODE_SYS_DIR: return 3U;
    case LINUX_PROC_NODE_SYS_DEVICES_DIR: return 3U;
    case LINUX_PROC_NODE_SYS_SYSTEM_DIR: return 3U;
    case LINUX_PROC_NODE_SYS_CPU_DIR: return 3U + linux_proc_cpu_count();
    case LINUX_PROC_NODE_SELF_FD_DIR: return 2U + file->fd_count;
    default: return 0U;
    }
}

static bool linux_proc_directory_entry(const linux_proc_file_t *file,
                                       uint64_t index, char *name,
                                       size_t capacity, uint8_t *type,
                                       uint64_t *inode) {
    uint32_t cpu_count;
    if (file == 0 || name == 0 || type == 0 || inode == 0 ||
        index >= linux_proc_directory_count(file)) return false;
    *type = 4U;
    *inode = file->inode + index;
    if (index == 0U) return linux_proc_set_dirent_name(name, capacity, ".");
    if (index == 1U) return linux_proc_set_dirent_name(name, capacity, "..");
    switch (file->node) {
    case LINUX_PROC_NODE_PROC_DIR:
        if (index == 2U) return linux_proc_set_dirent_name(name, capacity, "self");
        if (index == 3U) {
            *type = 8U;
            return linux_proc_set_dirent_name(name, capacity, "cpuinfo");
        }
        if (index == 5U) {
            *type = 8U;
            return linux_proc_set_dirent_name(name, capacity, "meminfo");
        }
        *type = 8U;
        return linux_proc_set_dirent_name(name, capacity, "stat");
    case LINUX_PROC_NODE_SELF_DIR:
        *type = index == 6U ? 4U : 8U;
        if (index == 2U) return linux_proc_set_dirent_name(name, capacity, "cmdline");
        if (index == 3U) return linux_proc_set_dirent_name(name, capacity, "comm");
        if (index == 4U) return linux_proc_set_dirent_name(name, capacity, "status");
        if (index == 5U) return linux_proc_set_dirent_name(name, capacity, "maps");
        return linux_proc_set_dirent_name(name, capacity, "task");
    case LINUX_PROC_NODE_TASK_DIR:
        *type = 4U;
        {
            char digits[24];
            size_t digit_count = linux_proc_u64_text(file->process_id, digits,
                                                     sizeof(digits));
            if (digit_count == 0U || digit_count + 1U > capacity) return false;
            linux_copy(name, digits, digit_count + 1U);
            return true;
        }
    case LINUX_PROC_NODE_SELF_FD_DIR:
        *type = 10U;
        {
            char digits[16];
            size_t digit_count = linux_proc_u64_text(
                file->fd_entries[index - 2U], digits, sizeof(digits));
            if (digit_count == 0U || digit_count + 1U > capacity) return false;
            linux_copy(name, digits, digit_count + 1U);
            return true;
        }
    case LINUX_PROC_NODE_SYS_DIR:
        return linux_proc_set_dirent_name(name, capacity, "devices");
    case LINUX_PROC_NODE_SYS_DEVICES_DIR:
        return linux_proc_set_dirent_name(name, capacity, "system");
    case LINUX_PROC_NODE_SYS_SYSTEM_DIR:
        return linux_proc_set_dirent_name(name, capacity, "cpu");
    case LINUX_PROC_NODE_SYS_CPU_DIR:
        if (index == 2U) {
            *type = 8U;
            return linux_proc_set_dirent_name(name, capacity, "online");
        }
        cpu_count = linux_proc_cpu_count();
        if (index - 3U >= cpu_count) return false;
        *type = 4U;
        {
            char digits[16];
            size_t digit_count = linux_proc_u64_text(index - 3U, digits,
                                                     sizeof(digits));
            if (digit_count == 0U || capacity < digit_count + 4U) return false;
            if (!linux_proc_set_dirent_name(name, capacity, "cpu")) return false;
            linux_copy(name + 3U, digits, digit_count + 1U);
            return true;
        }
    default:
        return false;
    }
}

static void linux_proc_stat(const linux_proc_file_t *file,
                            linux_stat_t *output) {
    linux_zero(output, sizeof(*output));
    output->device = 2U;
    output->inode = file != 0 ? file->inode : 0U;
    output->links = 1U;
    output->mode = file != 0 && linux_proc_node_is_directory(file->node) ?
                   0040000U :
                   0100000U;
    output->mode |= 0444U;
    output->size = file != 0 && !linux_proc_node_is_directory(file->node) ?
                   (int64_t)file->length : 0;
    output->block_size = PAGE_SIZE;
    output->blocks = output->size > 0 ? (output->size + 511) / 512 : 0;
}

static bool linux_nvidia_device_path(const char *path, uint32_t *minor)
{
    if (path == 0 || minor == 0) return false;
    if (linux_exact_path(path, "/dev/nvidiactl")) {
        *minor = 255U;
        return true;
    }
    if (linux_exact_path(path, "/dev/nvidia0")) {
        *minor = 0U;
        return true;
    }
    return false;
}

static void linux_nvidia_stat(uint32_t minor, linux_stat_t *output)
{
    linux_zero(output, sizeof(*output));
    output->device = 3U;
    output->inode = 0x4E560000ULL + minor;
    output->links = 1U;
    output->mode = 0020000U | 0666U;
    output->rdevice = ((uint64_t)195U << 8U) | minor;
    output->block_size = PAGE_SIZE;
}

static int64_t linux_proc_read_file(linux_proc_file_t *file,
                                    uint64_t buffer, uint64_t length) {
    uint64_t available;
    uint64_t count;
    kstatus_t status;
    if (file == 0) return K_EBADF;
    if (linux_proc_node_is_directory(file->node)) return K_EISDIR;
    if (length == 0U) return 0;
    if (length > (uint64_t)SIZE_MAX ||
        !linux_user_pointer(buffer, (size_t)length)) return LINUX_EFAULT;
    spinlock_lock(&file->lock);
    if (file->position >= file->length) {
        spinlock_unlock(&file->lock);
        return 0;
    }
    available = file->length - file->position;
    count = length < available ? length : available;
    status = copy_to_user((void __user *)(uintptr_t)buffer,
                          linux_proc_data(file) + file->position, (size_t)count);
    if (status == K_OK) file->position += count;
    spinlock_unlock(&file->lock);
    return status == K_OK ? (int64_t)count : LINUX_EFAULT;
}

static kstatus_t linux_file_stat(file_t *file, linux_stat_t *output,
                                 uint64_t inode_hint) {
    os_file_info_t info;
    if (file == 0 || output == 0) return K_EBADF;
    kstatus_t status = vfs_file_stat(file, &info);
    if (status != K_OK) return status;
    linux_zero(output, sizeof(*output));
    output->device = 1U;
    output->inode = inode_hint != 0U ? inode_hint : file->vnode->inode_id;
    output->links = 1U;
    output->mode = (info.type == OS_FILE_TYPE_DIRECTORY ? 0040000U : 0100000U) |
                   (info.mode & 07777U);
    output->size = (int64_t)info.size;
    output->block_size = PAGE_SIZE;
    output->blocks = (int64_t)((info.size + 511U) / 512U);
    return K_OK;
}

static void linux_console_stat(int64_t descriptor, linux_stat_t *output) {
    linux_zero(output, sizeof(*output));
    output->device = 1U;
    output->inode = (uint64_t)(descriptor + 1);
    output->links = 1U;
    output->mode = 0020000U | 0600U;
    output->block_size = PAGE_SIZE;
}

static void linux_null_stat(linux_stat_t *output) {
    linux_zero(output, sizeof(*output));
    output->device = 1U;
    output->inode = 3U;
    output->links = 1U;
    output->mode = 0020000U | 0666U;
    output->rdevice = 0x103U; /* Linux makedev(1, 3): /dev/null. */
    output->block_size = PAGE_SIZE;
}

static uint16_t linux_be16(uint16_t value) {
    return (uint16_t)((value << 8U) | (value >> 8U));
}

static uint32_t linux_be32(uint32_t value) {
    return ((value & 0x000000FFU) << 24U) |
           ((value & 0x0000FF00U) << 8U) |
           ((value & 0x00FF0000U) >> 8U) |
           ((value & 0xFF000000U) >> 24U);
}

static int64_t linux_socket_status(kstatus_t status) {
    if (status == K_EDEVREMOVED || status == K_EDEVLOST) {
        return LINUX_ECONNRESET;
    }
    return status;
}

/* 接收等待超时是临时状态；TCP 传输超时则为终止状态。混淆两者会使 EPOLLERR 持续置位，
 * 同时 recv 永远报告 EAGAIN。只在特殊超时路径查询，且不消费 socket 错误。 */
static int64_t linux_socket_receive_status(socket_t *socket, kstatus_t status) {
    if (status == K_ETIMEDOUT) {
        int32_t error = 0;
        kstatus_t query = socket_get_option(socket, OS_SOCKET_OPTION_ERROR, &error);
        if (query != K_OK) return linux_socket_status(query);
        if (error == 0) return K_EAGAIN;
        return -error;
    }
    return linux_socket_status(status);
}

static uint64_t linux_socket_timeout(const linux_fd_entry_t *entry,
                                     uint32_t message_flags) {
    if (entry != 0 && (entry->flags & LINUX_O_NONBLOCK) != 0U) return 0U;
    return (message_flags & LINUX_MSG_DONTWAIT) != 0U ? 0U : UINT64_MAX;
}

static kstatus_t linux_socket_entry(process_t *process, int64_t descriptor,
                                    linux_fd_entry_t *entry,
                                    socket_t **socket) {
    kstatus_t status;
    if (entry == 0 || socket == 0) return K_EINVAL;
    *socket = 0;
    status = linux_fd_lookup(process, descriptor, entry);
    if (status != K_OK) return status;
    if (entry->kind != LINUX_FD_SOCKET || entry->private_data == 0) {
        linux_fd_entry_put(entry);
        return K_ENOTSOCK;
    }
    *socket = (socket_t *)entry->private_data;
    if ((*socket)->object.type != KOBJECT_TYPE_SOCKET) {
        linux_fd_entry_put(entry);
        *socket = 0;
        return K_ENOTSOCK;
    }
    return K_OK;
}

static kstatus_t linux_socket_decode_address(uint64_t address_pointer,
                                              uint64_t address_length,
                                              uint16_t expected_family,
                                              uint32_t *address,
                                              uint16_t *port,
                                              uint8_t address6[16]) {
    if (address == 0 || port == 0 || address_pointer == 0U) return K_EINVAL;
    if (expected_family == OS_AF_INET4) {
        linux_sockaddr_in_t input = {0};
        if (address_length < sizeof(input) ||
            !linux_user_pointer(address_pointer, sizeof(input))) {
            return LINUX_EFAULT;
        }
        if (copy_from_user(&input,
                           (const void __user *)(uintptr_t)address_pointer,
                           sizeof(input)) != K_OK) return LINUX_EFAULT;
        if (input.family != LINUX_AF_INET) return LINUX_EAFNOSUPPORT;
        *address = linux_be32(input.address);
        *port = linux_be16(input.port);
        return K_OK;
    }
    if (expected_family == OS_AF_INET6) {
        linux_sockaddr_in6_t input = {0};
        if (address_length < sizeof(input) || address6 == 0 ||
            !linux_user_pointer(address_pointer, sizeof(input))) {
            return LINUX_EFAULT;
        }
        if (copy_from_user(&input,
                           (const void __user *)(uintptr_t)address_pointer,
                           sizeof(input)) != K_OK) return LINUX_EFAULT;
        if (input.family != LINUX_AF_INET6) return LINUX_EAFNOSUPPORT;
        linux_copy(address6, input.address, sizeof(input.address));
        *address = 0U;
        *port = linux_be16(input.port);
        return K_OK;
    }
    return LINUX_EAFNOSUPPORT;
}

static size_t linux_socket_address_size(uint16_t family) {
    return family == OS_AF_INET6 ? sizeof(linux_sockaddr_in6_t) :
           sizeof(linux_sockaddr_in_t);
}

static kstatus_t linux_socket_encode_address(const os_socket_info_t *info,
                                              bool peer, void *output,
                                              size_t output_size) {
    if (info == 0 || output == 0 ||
        output_size < linux_socket_address_size(info->family)) return K_EINVAL;
    if (info->family == OS_AF_INET4) {
        linux_sockaddr_in_t address = {0};
        address.family = LINUX_AF_INET;
        address.port = linux_be16(peer ? info->peer_port : info->local_port);
        address.address = linux_be32(peer ? info->peer_address :
                                     info->local_address);
        linux_copy(output, &address, sizeof(address));
        return K_OK;
    }
    if (info->family == OS_AF_INET6) {
        linux_sockaddr_in6_t address = {0};
        address.family = LINUX_AF_INET6;
        address.port = linux_be16(peer ? info->peer_port : info->local_port);
        linux_copy(address.address, peer ? info->peer_address6 :
                   info->local_address6, sizeof(address.address));
        linux_copy(output, &address, sizeof(address));
        return K_OK;
    }
    return LINUX_EAFNOSUPPORT;
}

static kstatus_t linux_socket_copy_name(socket_t *socket, bool peer,
                                         uint64_t address_pointer,
                                         uint64_t length_pointer) {
    os_socket_info_t info = {0};
    uint32_t requested_length;
    uint32_t actual_length;
    uint8_t address[sizeof(linux_sockaddr_in6_t)] = {0};
    kstatus_t status;
    if (socket == 0 || length_pointer == 0U ||
        !linux_user_pointer(length_pointer, sizeof(requested_length))) {
        return LINUX_EFAULT;
    }
    if (copy_from_user(&requested_length,
                       (const void __user *)(uintptr_t)length_pointer,
                       sizeof(requested_length)) != K_OK) return LINUX_EFAULT;
    if (address_pointer == 0U && requested_length != 0U) return LINUX_EFAULT;
    status = socket_get_info(socket, &info);
    if (status != K_OK) return status;
    if (peer && (info.flags & OS_SOCKET_INFO_CONNECTED) == 0U) {
        return LINUX_ENOTCONN;
    }
    actual_length = (uint32_t)linux_socket_address_size(info.family);
    status = linux_socket_encode_address(&info, peer, address,
                                         sizeof(address));
    if (status != K_OK) return status;
    if (address_pointer != 0U && requested_length != 0U) {
        uint32_t copy_length = requested_length < actual_length ?
                               requested_length : actual_length;
        if (!linux_user_pointer(address_pointer, copy_length) ||
            copy_to_user((void __user *)(uintptr_t)address_pointer,
                         address, copy_length) != K_OK) return LINUX_EFAULT;
    }
    if (copy_to_user((void __user *)(uintptr_t)length_pointer,
                     &actual_length, sizeof(actual_length)) != K_OK) {
        return LINUX_EFAULT;
    }
    return K_OK;
}

static kstatus_t linux_socket_copy_endpoint(
    uint16_t family, uint32_t address_value, uint16_t port,
    const uint8_t address6_value[16], uint64_t address_pointer,
    uint64_t length_pointer) {
    os_socket_info_t info = {0};
    uint32_t requested_length;
    uint32_t actual_length;
    uint8_t address[sizeof(linux_sockaddr_in6_t)] = {0};
    kstatus_t status;
    if (length_pointer == 0U ||
        !linux_user_pointer(length_pointer, sizeof(requested_length))) {
        return LINUX_EFAULT;
    }
    if (copy_from_user(&requested_length,
                       (const void __user *)(uintptr_t)length_pointer,
                       sizeof(requested_length)) != K_OK) return LINUX_EFAULT;
    if (address_pointer == 0U && requested_length != 0U) return LINUX_EFAULT;
    info.family = family;
    info.local_address = address_value;
    info.local_port = port;
    if (address6_value != 0) linux_copy(info.local_address6,
                                        address6_value, 16U);
    actual_length = (uint32_t)linux_socket_address_size(family);
    status = linux_socket_encode_address(&info, false, address,
                                         sizeof(address));
    if (status != K_OK) return status;
    if (address_pointer != 0U && requested_length != 0U) {
        uint32_t copy_length = requested_length < actual_length ?
                               requested_length : actual_length;
        if (!linux_user_pointer(address_pointer, copy_length) ||
            copy_to_user((void __user *)(uintptr_t)address_pointer,
                         address, copy_length) != K_OK) return LINUX_EFAULT;
    }
    if (copy_to_user((void __user *)(uintptr_t)length_pointer,
                     &actual_length, sizeof(actual_length)) != K_OK) {
        return LINUX_EFAULT;
    }
    return K_OK;
}

static int64_t linux_socket_receive_payload(
    socket_t *socket, const linux_fd_entry_t *entry, uint8_t *payload,
    size_t request_length, uint32_t message_flags, uint64_t address_pointer,
    uint64_t address_length_pointer) {
    socket_ipv4_endpoint_t source4 = {0};
    socket_ipv6_endpoint_t source6 = {0};
    uint64_t bytes = 0U;
    kstatus_t status;
    if (socket == 0 || entry == 0 || payload == 0 ||
        request_length > SOCKET_MAX_PAYLOAD) return K_EINVAL;
    if ((address_pointer == 0U) != (address_length_pointer == 0U)) {
        return LINUX_EFAULT;
    }
    if ((message_flags & ~(LINUX_MSG_DONTWAIT | LINUX_MSG_PEEK)) != 0U) {
        return K_EINVAL;
    }
    if (socket->family == OS_AF_INET4) {
        status = (message_flags & LINUX_MSG_PEEK) != 0U ?
            socket_recv_peek(socket, payload, request_length, &source4,
                             linux_socket_timeout(entry, message_flags),
                             &bytes) :
            socket_recv(socket, payload, request_length, &source4,
                        linux_socket_timeout(entry, message_flags), &bytes);
    } else if (socket->family == OS_AF_INET6) {
        status = (message_flags & LINUX_MSG_PEEK) != 0U ?
            socket_recv_ipv6_peek(socket, payload, request_length, &source6,
                                  linux_socket_timeout(entry, message_flags),
                                  &bytes) :
            socket_recv_ipv6(socket, payload, request_length, &source6,
                             linux_socket_timeout(entry, message_flags),
                             &bytes);
    } else {
        return LINUX_EAFNOSUPPORT;
    }
    if (status != K_OK && !(status == K_EAGAIN && bytes != 0U)) {
        return linux_socket_receive_status(socket, status);
    }
    if (bytes > request_length) return K_EIO;
    if (address_pointer != 0U) {
        status = socket->family == OS_AF_INET4 ?
            linux_socket_copy_endpoint(OS_AF_INET4, source4.address,
                                       source4.port, 0,
                                       address_pointer,
                                       address_length_pointer) :
            linux_socket_copy_endpoint(OS_AF_INET6, 0U, source6.port,
                                       source6.address, address_pointer,
                                       address_length_pointer);
        if (status != K_OK) return status;
    }
    return (int64_t)bytes;
}

static int64_t linux_socket_receive(socket_t *socket,
                                    const linux_fd_entry_t *entry,
                                    uint64_t buffer, uint64_t length,
                                    uint32_t message_flags,
                                    uint64_t address_pointer,
                                    uint64_t address_length_pointer) {
    uint8_t payload[SOCKET_MAX_PAYLOAD];
    size_t request_length;
    int64_t result;
    if (socket == 0 || entry == 0) return K_EINVAL;
    if (length == 0U) return 0;
    request_length = length > SOCKET_MAX_PAYLOAD ? SOCKET_MAX_PAYLOAD :
                     (size_t)length;
    if (!linux_user_pointer(buffer, request_length)) return LINUX_EFAULT;
    result = linux_socket_receive_payload(
        socket, entry, payload, request_length, message_flags,
        address_pointer, address_length_pointer);
    if (result < 0) return result;
    if (copy_to_user((void __user *)(uintptr_t)buffer, payload,
                     (size_t)result) != K_OK) return LINUX_EFAULT;
    return result;
}

static int64_t linux_socket_send_payload(
    socket_t *socket, const linux_fd_entry_t *entry, const uint8_t *payload,
    size_t length, uint32_t message_flags, uint64_t address_pointer,
    uint64_t address_length) {
    uint8_t address6[16] = {0};
    uint32_t address4 = 0U;
    uint16_t port = 0U;
    uint64_t bytes = 0U;
    kstatus_t status;
    if (socket == 0 || entry == 0 || payload == 0 ||
        length > SOCKET_MAX_PAYLOAD) {
        return length > SOCKET_MAX_PAYLOAD ? LINUX_EMSGSIZE : K_EINVAL;
    }
    if ((message_flags & ~(LINUX_MSG_DONTWAIT | LINUX_MSG_NOSIGNAL)) != 0U) {
        return K_EINVAL;
    }
    if (address_pointer != 0U) {
        status = linux_socket_decode_address(address_pointer, address_length,
                                             socket->family, &address4, &port,
                                             address6);
        if (status != K_OK) return status;
    }
    if (socket->family == OS_AF_INET4) {
        status = socket_send(socket, payload, (size_t)length, address4, port,
                             &bytes);
    } else if (socket->family == OS_AF_INET6) {
        status = socket_send_ipv6(socket, payload, (size_t)length,
                                  address_pointer != 0U ? address6 : 0,
                                  port, &bytes);
    } else {
        return LINUX_EAFNOSUPPORT;
    }
    return status == K_OK ? (int64_t)bytes : linux_socket_status(status);
}

static int64_t linux_socket_send(socket_t *socket,
                                 const linux_fd_entry_t *entry,
                                 uint64_t buffer, uint64_t length,
                                 uint32_t message_flags,
                                 uint64_t address_pointer,
                                 uint64_t address_length) {
    uint8_t payload[SOCKET_MAX_PAYLOAD];
    size_t request_length;
    if (socket == 0 || entry == 0) return K_EINVAL;
    if (socket->type == OS_SOCK_DGRAM && length > SOCKET_MAX_PAYLOAD) {
        return LINUX_EMSGSIZE;
    }
    /* TCP 是字节流：调用方的 TLS 记录不是数据报，可能超过有界复制缓冲区。
     * 像 sendmsg 一样返回短写，使调用方重试未发送的后缀。 */
    request_length = length > SOCKET_MAX_PAYLOAD ? SOCKET_MAX_PAYLOAD :
                     (size_t)length;
    if (request_length != 0U) {
        if (!linux_user_pointer(buffer, request_length) ||
            copy_from_user(payload,
                           (const void __user *)(uintptr_t)buffer,
                           request_length) != K_OK) return LINUX_EFAULT;
    }
    return linux_socket_send_payload(
        socket, entry, payload, request_length, message_flags,
        address_pointer, address_length);
}

static int64_t linux_message_load(uint64_t message_pointer,
                                  linux_msghdr_t *message,
                                  linux_iovec_t vectors[LINUX_MAX_IOV],
                                  uint64_t *total_length) {
    if (message == 0 || vectors == 0 || total_length == 0 ||
        !linux_user_pointer(message_pointer, sizeof(*message)) ||
        copy_from_user(message,
                       (const void __user *)(uintptr_t)message_pointer,
                       sizeof(*message)) != K_OK) return LINUX_EFAULT;
    if (message->iov_length > LINUX_MAX_IOV) return K_EINVAL;
    if (message->control_length > (uint64_t)SIZE_MAX) return K_EOVERFLOW;
    if (message->control_length != 0U &&
        (message->control == 0U ||
         !linux_user_pointer(message->control, (size_t)message->control_length))) {
        return LINUX_EFAULT;
    }
    if (message->iov_length != 0U) {
        size_t vector_bytes = (size_t)message->iov_length *
                              sizeof(vectors[0]);
        if (!linux_user_pointer(message->iov_pointer, vector_bytes) ||
            copy_from_user(vectors,
                           (const void __user *)(uintptr_t)message->iov_pointer,
                           vector_bytes) != K_OK) return LINUX_EFAULT;
    }
    *total_length = 0U;
    for (uint64_t index = 0U; index < message->iov_length; ++index) {
        uint64_t length = vectors[index].length;
        if (length > (uint64_t)SIZE_MAX ||
            (length != 0U &&
             !linux_user_pointer(vectors[index].base, (size_t)length))) {
            return length > (uint64_t)SIZE_MAX ? K_EOVERFLOW : LINUX_EFAULT;
        }
        if (*total_length > UINT64_MAX - length) return K_EOVERFLOW;
        *total_length += length;
    }
    return K_OK;
}

typedef struct linux_cmsghdr {
    uint64_t length;
    int32_t level;
    int32_t type;
} linux_cmsghdr_t;

_Static_assert(sizeof(linux_cmsghdr_t) == LINUX_CMSG_HEADER_SIZE,
               "Linux cmsghdr ABI changed");

static void linux_unix_ancillary_put_record(
    linux_unix_ancillary_record_t *record) {
    if (record == 0) return;
    for (uint32_t index = 0U; index < record->fd_count; ++index) {
        linux_fd_entry_put(&record->entries[index]);
    }
    *record = (linux_unix_ancillary_record_t){0};
}

static kstatus_t linux_unix_ancillary_collect(
    process_t *process, const linux_msghdr_t *message,
    linux_unix_ancillary_record_t *record) {
    uint64_t offset = 0U;
    if (process == 0 || message == 0 || record == 0) return K_EINVAL;
    *record = (linux_unix_ancillary_record_t){0};
    if (message->control_length == 0U) return K_OK;
    if (message->control_length > LINUX_UNIX_MAX_CONTROL) {
        return LINUX_EOPNOTSUPP;
    }
    while (offset < message->control_length) {
        linux_cmsghdr_t header;
        uint64_t remaining = message->control_length - offset;
        uint64_t aligned_length;
        uint64_t data_length;
        if (remaining < sizeof(header) ||
            copy_from_user(&header,
                (const void __user *)(uintptr_t)(message->control + offset),
                sizeof(header)) != K_OK) {
            linux_unix_ancillary_put_record(record);
            return LINUX_EFAULT;
        }
        if (header.length < sizeof(header) || header.length > remaining) {
            linux_unix_ancillary_put_record(record);
            return K_EINVAL;
        }
        data_length = header.length - sizeof(header);
        if (header.level != (int32_t)LINUX_SOL_SOCKET ||
            header.type != (int32_t)LINUX_SCM_RIGHTS) {
            linux_unix_ancillary_put_record(record);
            return LINUX_EOPNOTSUPP;
        }
        if (data_length == 0U || (data_length % sizeof(int32_t)) != 0U ||
            data_length / sizeof(int32_t) > LINUX_UNIX_MAX_FDS -
                                            record->fd_count) {
            linux_unix_ancillary_put_record(record);
            return K_EINVAL;
        }
        for (uint64_t index = 0U;
             index < data_length / sizeof(int32_t); ++index) {
            int32_t descriptor;
            linux_fd_entry_t *entry =
                &record->entries[record->fd_count];
            kstatus_t lookup_status;
            uint64_t pointer = message->control + sizeof(header) +
                               offset + index * sizeof(descriptor);
            if (copy_from_user(&descriptor,
                    (const void __user *)(uintptr_t)pointer,
                    sizeof(descriptor)) != K_OK) {
                linux_unix_ancillary_put_record(record);
                return LINUX_EFAULT;
            }
            if (descriptor < 0) {
                linux_unix_ancillary_put_record(record);
                return K_EBADF;
            }
            lookup_status = linux_fd_lookup(process, descriptor, entry);
            if (lookup_status != K_OK) {
                linux_unix_ancillary_put_record(record);
                return lookup_status;
            }
            ++record->fd_count;
        }
        aligned_length = (header.length + (LINUX_CMSG_ALIGNMENT - 1U)) &
                         ~(uint64_t)(LINUX_CMSG_ALIGNMENT - 1U);
        /* Chromium 将 msg_controllen 设为 cmsg_len，因此最后一个 cmsg 不必包含尾部对齐字节。 */
        if (aligned_length > remaining) {
            if (header.length != remaining) {
                linux_unix_ancillary_put_record(record);
                return K_EINVAL;
            }
            offset = message->control_length;
        } else {
            offset += aligned_length;
        }
    }
    return K_OK;
}

static kstatus_t linux_unix_ancillary_deliver(
    process_t *process, const linux_msghdr_t *input,
    linux_msghdr_t *message, uint32_t message_flags,
    linux_unix_ancillary_record_t *record) {
    linux_cmsghdr_t header;
    int32_t descriptors[LINUX_UNIX_MAX_FDS];
    uint32_t capacity;
    uint32_t installed = 0U;
    uint32_t output_flags = 0U;
    if (process == 0 || input == 0 || message == 0 || record == 0) {
        return K_EINVAL;
    }
    if (record->fd_count == 0U) {
        message->control_length = 0U;
        message->flags = 0U;
        return K_OK;
    }
    if (input->control == 0U || input->control_length < sizeof(header)) {
        output_flags = LINUX_MSG_CTRUNC;
        linux_unix_ancillary_put_record(record);
        message->control_length = 0U;
        message->flags = output_flags;
        return K_OK;
    }
    capacity = ((size_t)input->control_length - sizeof(header)) /
               sizeof(int32_t);
    if (capacity > LINUX_UNIX_MAX_FDS) capacity = LINUX_UNIX_MAX_FDS;
    if (capacity < record->fd_count) output_flags = LINUX_MSG_CTRUNC;
    for (uint32_t index = 0U;
         index < capacity && index < record->fd_count; ++index) {
        linux_fd_entry_t *entry = &record->entries[index];
        uint32_t flags = entry->flags & ~LINUX_O_CLOEXEC;
        int64_t descriptor;
        if ((message_flags & LINUX_MSG_CMSG_CLOEXEC) != 0U) {
            flags |= LINUX_O_CLOEXEC;
        }
        descriptor = linux_fd_install_description(process, -1, entry->file,
                                      entry->private_data, entry->kind, flags,
                                      entry->description);
        if (descriptor < 0) {
            output_flags = LINUX_MSG_CTRUNC;
            linux_fd_entry_put(entry);
            *entry = (linux_fd_entry_t){0};
            continue;
        }
        descriptors[installed++] = (int32_t)descriptor;
        *entry = (linux_fd_entry_t){0};
    }
    for (uint32_t index = 0U; index < record->fd_count; ++index) {
        linux_fd_entry_put(&record->entries[index]);
    }
    record->fd_count = 0U;
    if (installed == 0U) {
        message->control_length = 0U;
        message->flags = output_flags;
        return K_OK;
    }
    header.length = sizeof(header) + installed * sizeof(int32_t);
    header.level = (int32_t)LINUX_SOL_SOCKET;
    header.type = (int32_t)LINUX_SCM_RIGHTS;
    if (copy_to_user((void __user *)(uintptr_t)input->control, &header,
                     sizeof(header)) != K_OK ||
        copy_to_user((void __user *)(uintptr_t)(input->control +
                     sizeof(header)), descriptors,
                     installed * sizeof(descriptors[0])) != K_OK) {
        return LINUX_EFAULT;
    }
    message->control_length = header.length;
    message->flags = output_flags;
    return K_OK;
}

static int64_t linux_message_gather(const linux_iovec_t *vectors,
                                    uint64_t count, uint8_t *payload,
                                    size_t capacity, size_t *length) {
    size_t copied = 0U;
    if (vectors == 0 || payload == 0 || length == 0) return K_EINVAL;
    for (uint64_t index = 0U; index < count && copied < capacity; ++index) {
        size_t available = (size_t)vectors[index].length;
        size_t chunk = available < capacity - copied ?
                       available : capacity - copied;
        if (chunk != 0U && copy_from_user(
                payload + copied,
                (const void __user *)(uintptr_t)vectors[index].base,
                chunk) != K_OK) return LINUX_EFAULT;
        copied += chunk;
    }
    *length = copied;
    return K_OK;
}

static int64_t linux_message_scatter(const linux_iovec_t *vectors,
                                     uint64_t count, const uint8_t *payload,
                                     size_t length) {
    size_t copied = 0U;
    if (vectors == 0 || payload == 0) return K_EINVAL;
    for (uint64_t index = 0U; index < count && copied < length; ++index) {
        size_t available = (size_t)vectors[index].length;
        size_t chunk = available < length - copied ?
                       available : length - copied;
        if (chunk != 0U && copy_to_user(
                (void __user *)(uintptr_t)vectors[index].base,
                payload + copied, chunk) != K_OK) return LINUX_EFAULT;
        copied += chunk;
    }
    return copied == length ? K_OK : LINUX_EFAULT;
}

static int64_t linux_sendmsg(process_t *process, int64_t descriptor,
                             uint64_t message_pointer, uint32_t message_flags) {
    linux_msghdr_t message;
    /* linux_message_load 会在任何消费者处理前复制所有活动向量。 */
    linux_iovec_t vectors[LINUX_MAX_IOV];
    uint64_t total_length;
    uint8_t payload[SOCKET_MAX_PAYLOAD];
    size_t payload_length;
    linux_fd_entry_t entry = {0};
    kstatus_t status = linux_message_load(message_pointer, &message, vectors,
                                          &total_length);
    if (status != K_OK) return status;
    if ((message_flags & ~(LINUX_MSG_DONTWAIT | LINUX_MSG_NOSIGNAL)) != 0U) {
        return K_EINVAL;
    }
    status = linux_fd_lookup(process, descriptor, &entry);
    if (status != K_OK) return status;
    if (entry.kind == LINUX_FD_SOCKET) {
        socket_t *socket = (socket_t *)entry.private_data;
        if (message.control_length != 0U) {
            linux_fd_entry_put(&entry);
            return LINUX_EOPNOTSUPP;
        }
        if (socket == 0 || socket->object.type != KOBJECT_TYPE_SOCKET) {
            linux_fd_entry_put(&entry);
            return K_ENOTSOCK;
        }
        if (socket->type == OS_SOCK_DGRAM &&
            total_length > SOCKET_MAX_PAYLOAD) {
            linux_fd_entry_put(&entry);
            return LINUX_EMSGSIZE;
        }
        payload_length = total_length > SOCKET_MAX_PAYLOAD ?
                         SOCKET_MAX_PAYLOAD : (size_t)total_length;
        status = linux_message_gather(vectors, message.iov_length, payload,
                                      payload_length, &payload_length);
        if (status == K_OK) {
            status = linux_socket_send_payload(
                socket, &entry, payload, payload_length, message_flags,
                message.name, message.name_length);
        }
        linux_fd_entry_put(&entry);
        return status;
    }
    if (entry.kind == LINUX_FD_UNIX_SOCKET) {
        linux_unix_socket_t *socket =
            (linux_unix_socket_t *)entry.private_data;
        linux_unix_ancillary_record_t record = {0};
        linux_unix_channel_t *channel;
        pipe_endpoint_t *write_end;
        bool locked = false;
        bool queued = false;
        uint64_t bytes = 0U;
        if (socket == 0) {
            linux_fd_entry_put(&entry);
            return K_ENOTSOCK;
        }
        linux_unix_socket_pin_io(socket, true, &write_end, &channel);
        if (write_end == 0) {
            linux_fd_entry_put(&entry);
            return K_EPIPE;
        }
        if (message.name != 0U || message.name_length != 0U) {
            linux_unix_socket_unpin_io(write_end, channel);
            linux_fd_entry_put(&entry);
            return LINUX_EAFNOSUPPORT;
        }
        payload_length = total_length > LINUX_PIPE_COPY_MAX ?
                         LINUX_PIPE_COPY_MAX : (size_t)total_length;
        status = linux_message_gather(vectors, message.iov_length, payload,
                                      payload_length, &payload_length);
        if (status == K_OK) {
            status = linux_unix_ancillary_collect(process, &message, &record);
        }
        if (status == K_OK) {
            if (record.fd_count != 0U && payload_length == 0U)
                status = K_EINVAL;
        }
        if (status == K_OK && channel != 0) {
            status = kmutex_lock(&channel->write_mutex,
                                 linux_socket_timeout(&entry, message_flags));
            locked = status == K_OK;
        }
        if (status == K_OK && record.fd_count != 0U) {
            record.payload_length = (uint32_t)payload_length;
            status = linux_unix_channel_push(channel, &record);
            queued = status == K_OK;
        }
        if (status == K_OK && payload_length != 0U) {
            status = pipe_write(write_end, payload, payload_length,
                                (entry.flags & LINUX_O_NONBLOCK) != 0U ||
                                (message_flags & LINUX_MSG_DONTWAIT) != 0U ?
                                0U : UINT64_MAX, &bytes);
        }
        if (channel != 0 && status == K_OK)
            linux_unix_channel_commit_write(channel, bytes);
        if (channel != 0 && status != K_OK && queued)
            linux_unix_channel_rollback(channel);
        if (locked)
            (void)kmutex_unlock(&channel->write_mutex);
        if (status != K_OK) linux_unix_ancillary_put_record(&record);
        linux_unix_socket_unpin_io(write_end, channel);
        linux_fd_entry_put(&entry);
        return status == K_OK ? (int64_t)bytes : linux_pipe_status(status);
    }
    linux_fd_entry_put(&entry);
    return K_ENOTSOCK;
}

static int64_t linux_recvmsg(process_t *process, int64_t descriptor,
                             uint64_t message_pointer, uint32_t message_flags) {
    linux_msghdr_t message;
    /* gather/scatter 不会检查未使用的向量槽。 */
    linux_iovec_t vectors[LINUX_MAX_IOV];
    uint64_t total_length;
    uint8_t payload[SOCKET_MAX_PAYLOAD];
    size_t request_length;
    linux_fd_entry_t entry = {0};
    int64_t result;
    kstatus_t status = linux_message_load(message_pointer, &message, vectors,
                                          &total_length);
    if (status != K_OK) return status;
    if ((message_flags & ~(LINUX_MSG_DONTWAIT | LINUX_MSG_CMSG_CLOEXEC |
                           LINUX_MSG_PEEK)) != 0U) {
        return K_EINVAL;
    }
    status = linux_fd_lookup(process, descriptor, &entry);
    if (status != K_OK) return status;
    request_length = total_length > SOCKET_MAX_PAYLOAD ?
                     SOCKET_MAX_PAYLOAD : (size_t)total_length;
    if (entry.kind == LINUX_FD_SOCKET) {
        socket_t *socket = (socket_t *)entry.private_data;
        uint64_t name_length_pointer = message_pointer +
            __builtin_offsetof(linux_msghdr_t, name_length);
        /* msg_control 是输出容量，不是发送辅助数据的请求。Chromium 等调用方每次 UDP 读取都会提供该值。
         * 未启用接收控制选项时，下方返回 msg_controllen=0；若拒绝该容量，连普通 DNS 读取也会失败。 */
        if (socket == 0 || socket->object.type != KOBJECT_TYPE_SOCKET) {
            linux_fd_entry_put(&entry);
            return K_ENOTSOCK;
        }
        result = request_length == 0U ? 0 : linux_socket_receive_payload(
            socket, &entry, payload, request_length,
            message_flags & ~LINUX_MSG_CMSG_CLOEXEC,
            message.name, message.name != 0U ? name_length_pointer : 0U);
#if OS_DEBUG_SERIAL
        if (g_debug_inet_recv_logs++ < 64U) {
            kernel_serial_printf_serial_only(
                "OS_INET_RECVMSG pid=%llu fd=%d flags=%x result=%d\r\n",
                (unsigned long long)process->pid, (int)descriptor,
                (unsigned int)message_flags, (int)result);
        }
#endif
        if (result >= 0 && linux_message_scatter(
                vectors, message.iov_length, payload, (size_t)result) != K_OK) {
            result = LINUX_EFAULT;
        }
        if (result >= 0) {
            message.flags = 0U;
            if (message.name != 0U) {
                message.name_length = (uint32_t)linux_socket_address_size(
                    socket->family);
            }
            message.control_length = 0U;
            if (copy_to_user((void __user *)(uintptr_t)message_pointer,
                             &message, sizeof(message)) != K_OK) {
                result = LINUX_EFAULT;
            }
        }
        linux_fd_entry_put(&entry);
        return result;
    }
    if (entry.kind == LINUX_FD_UNIX_SOCKET) {
        if ((message_flags & LINUX_MSG_PEEK) != 0U) {
            linux_fd_entry_put(&entry);
            return K_EINVAL;
        }
        linux_unix_socket_t *socket =
            (linux_unix_socket_t *)entry.private_data;
        linux_unix_ancillary_record_t record = {0};
        linux_unix_channel_t *channel;
        pipe_endpoint_t *read_end;
        bool have_record = false;
        uint64_t bytes = 0U;
        if (socket == 0) {
            linux_fd_entry_put(&entry);
            return K_ENOTSOCK;
        }
        if (message.name != 0U || message.name_length != 0U) {
            linux_fd_entry_put(&entry);
            return LINUX_EAFNOSUPPORT;
        }
        linux_unix_socket_pin_io(socket, false, &read_end, &channel);
        if (read_end == 0 || request_length == 0U) {
            result = 0;
        } else {
            status = channel != 0 ?
                     kmutex_lock(&channel->read_mutex,
                                 linux_socket_timeout(&entry, message_flags)) : K_OK;
            if (status == K_OK) {
                status = pipe_read_limited(read_end, payload, request_length,
                                   (entry.flags & LINUX_O_NONBLOCK) != 0U ||
                                   (message_flags & LINUX_MSG_DONTWAIT) != 0U ?
                                   0U : UINT64_MAX, &bytes,
                                   linux_unix_channel_pipe_limit, channel);
                if (status == K_OK && bytes != 0U)
                    have_record = linux_unix_channel_commit_read(
                        channel, bytes, true, &record);
                if (channel != 0)
                    (void)kmutex_unlock(&channel->read_mutex);
            }
            result = status == K_OK ? (int64_t)bytes : linux_pipe_status(status);
        }
        if (result >= 0 && linux_message_scatter(
                vectors, message.iov_length, payload, (size_t)result) != K_OK) {
            result = LINUX_EFAULT;
        }
        if (result >= 0) {
            message.flags = 0U;
            if (have_record) {
                status = linux_unix_ancillary_deliver(
                    process, &message, &message, message_flags, &record);
                if (status != K_OK) result = status;
            } else {
            message.control_length = 0U;
            }
            if (copy_to_user((void __user *)(uintptr_t)message_pointer,
                             &message, sizeof(message)) != K_OK) {
                result = LINUX_EFAULT;
            }
        }
        if (result < 0 && have_record)
            linux_unix_ancillary_put_record(&record);
        linux_unix_socket_unpin_io(read_end, channel);
        linux_fd_entry_put(&entry);
        return result;
    }
    linux_fd_entry_put(&entry);
    return K_ENOTSOCK;
}

static int64_t linux_unix_sendto(const linux_fd_entry_t *entry,
                                 uint64_t buffer, uint64_t length,
                                 uint32_t message_flags,
                                 uint64_t address_pointer,
                                 uint64_t address_length) {
    linux_unix_socket_t *socket =
        entry != 0 ? (linux_unix_socket_t *)entry->private_data : 0;
    linux_unix_channel_t *channel;
    pipe_endpoint_t *write_end;
    uint8_t inline_payload[256];
    uint8_t *payload;
    size_t request;
    uint64_t bytes = 0U;
    kstatus_t status;
    if (socket == 0 || socket->object.type != KOBJECT_TYPE_LINUX_UNIX_SOCKET)
        return K_ENOTSOCK;
    if ((message_flags & ~(LINUX_MSG_DONTWAIT | LINUX_MSG_NOSIGNAL)) != 0U)
        return K_EINVAL;
    if (address_pointer != 0U || address_length != 0U)
        return LINUX_EAFNOSUPPORT;
    if (length == 0U) return 0;
    request = length > LINUX_PIPE_COPY_MAX ? LINUX_PIPE_COPY_MAX :
              (size_t)length;
    if (!linux_user_pointer(buffer, request)) return LINUX_EFAULT;
    payload = request <= sizeof(inline_payload) ? inline_payload :
              (uint8_t *)kmalloc(request, 0U);
    if (payload == 0) return K_ENOMEM;
    status = copy_from_user(payload,
                            (const void __user *)(uintptr_t)buffer, request);
    if (status != K_OK) {
        if (payload != inline_payload) kfree(payload);
        return LINUX_EFAULT;
    }
    linux_unix_socket_pin_io(socket, true, &write_end, &channel);
    if (write_end == 0) {
        if (payload != inline_payload) kfree(payload);
        return K_EPIPE;
    }
    status = channel != 0 ?
             kmutex_lock(&channel->write_mutex,
                         linux_socket_timeout(entry, message_flags)) : K_OK;
    if (status == K_OK) {
        status = pipe_write(write_end, payload, request,
                            (entry->flags & LINUX_O_NONBLOCK) != 0U ||
                            (message_flags & LINUX_MSG_DONTWAIT) != 0U ?
                            0U : UINT64_MAX, &bytes);
        if (status == K_OK)
            linux_unix_channel_commit_write(channel, bytes);
        if (channel != 0)
            (void)kmutex_unlock(&channel->write_mutex);
    }
    linux_unix_socket_unpin_io(write_end, channel);
    if (payload != inline_payload) kfree(payload);
    return status == K_OK ? (int64_t)bytes : linux_pipe_status(status);
}

static int64_t linux_unix_recvfrom(const linux_fd_entry_t *entry,
                                   uint64_t buffer, uint64_t length,
                                   uint32_t message_flags,
                                   uint64_t address_pointer,
                                   uint64_t address_length_pointer) {
    linux_unix_socket_t *socket =
        entry != 0 ? (linux_unix_socket_t *)entry->private_data : 0;
    linux_unix_channel_t *channel;
    pipe_endpoint_t *read_end;
    uint8_t inline_payload[256];
    uint8_t *payload;
    size_t request;
    uint64_t bytes = 0U;
    kstatus_t status;
    if (socket == 0 || socket->object.type != KOBJECT_TYPE_LINUX_UNIX_SOCKET)
        return K_ENOTSOCK;
    if ((message_flags & ~LINUX_MSG_DONTWAIT) != 0U) return K_EINVAL;
    if (address_pointer != 0U || address_length_pointer != 0U)
        return LINUX_EAFNOSUPPORT;
    if (length == 0U) return 0;
    request = length > LINUX_PIPE_COPY_MAX ? LINUX_PIPE_COPY_MAX :
              (size_t)length;
    if (!linux_user_pointer(buffer, request)) return LINUX_EFAULT;
    payload = request <= sizeof(inline_payload) ? inline_payload :
              (uint8_t *)kmalloc(request, 0U);
    if (payload == 0) return K_ENOMEM;
    linux_unix_socket_pin_io(socket, false, &read_end, &channel);
    if (read_end == 0) {
        if (payload != inline_payload) kfree(payload);
        return 0;
    }
    status = channel != 0 ?
             kmutex_lock(&channel->read_mutex,
                         linux_socket_timeout(entry, message_flags)) : K_OK;
    if (status == K_OK) {
        status = pipe_read(read_end, payload, request,
                           (entry->flags & LINUX_O_NONBLOCK) != 0U ||
                           (message_flags & LINUX_MSG_DONTWAIT) != 0U ?
                           0U : UINT64_MAX, &bytes);
        if (status == K_OK)
            (void)linux_unix_channel_commit_read(channel, bytes, false, 0);
        if (channel != 0)
            (void)kmutex_unlock(&channel->read_mutex);
    }
    linux_unix_socket_unpin_io(read_end, channel);
    if (status == K_OK && copy_to_user(
            (void __user *)(uintptr_t)buffer, payload,
            (size_t)bytes) != K_OK) status = K_EACCES;
    if (payload != inline_payload) kfree(payload);
    return status == K_OK ? (int64_t)bytes : linux_pipe_status(status);
}

static int64_t linux_socket_create(process_t *process, uint64_t domain,
                                   uint64_t type, uint64_t protocol) {
    socket_t *socket = 0;
    linux_unix_socket_t *unix_socket = 0;
    uint64_t type_flags = type & (LINUX_SOCK_NONBLOCK | LINUX_SOCK_CLOEXEC);
    uint64_t base_type = type & ~(uint64_t)(LINUX_SOCK_NONBLOCK |
                                             LINUX_SOCK_CLOEXEC);
    uint32_t descriptor_flags = LINUX_O_RDWR;
    kstatus_t status;
    int64_t descriptor;
    if (process == 0 || domain > UINT16_MAX || protocol > UINT16_MAX ||
        (domain != LINUX_AF_UNIX && domain != LINUX_AF_INET &&
         domain != LINUX_AF_INET6) ||
        (base_type != LINUX_SOCK_STREAM && base_type != LINUX_SOCK_DGRAM &&
         base_type != LINUX_SOCK_SEQPACKET)) {
        return K_EINVAL;
    }
    if (domain == LINUX_AF_UNIX) {
        if (protocol != 0U || base_type != LINUX_SOCK_STREAM) {
            return LINUX_EAFNOSUPPORT;
        }
        unix_socket = linux_unix_socket_create();
        if (unix_socket == 0) return K_ENOMEM;
    } else {
    status = socket_create((uint16_t)domain, (uint16_t)base_type,
                           (uint16_t)protocol, &socket);
    if (status != K_OK) return status;
    }
    if ((type_flags & LINUX_SOCK_NONBLOCK) != 0U) {
        descriptor_flags |= LINUX_O_NONBLOCK;
    }
    if ((type_flags & LINUX_SOCK_CLOEXEC) != 0U) {
        descriptor_flags |= LINUX_O_CLOEXEC;
    }
    descriptor = domain == LINUX_AF_UNIX ?
        linux_fd_install(process, -1, 0, unix_socket, LINUX_FD_UNIX_SOCKET,
                         descriptor_flags) :
        linux_fd_install(process, -1, 0, socket, LINUX_FD_SOCKET,
                                  descriptor_flags);
#if OS_DEBUG_SERIAL
    if (g_debug_linux_socket_create_logs < 128U) {
        kernel_serial_printf_serial_only(
            "OS_LINUX_SOCKET_CREATE domain=%u type=%u fd=%d\r\n",
            domain, type, descriptor);
        ++g_debug_linux_socket_create_logs;
    }
#endif
    if (descriptor < 0) {
        if (domain == LINUX_AF_UNIX) object_put(unix_socket);
        else object_put(socket);
    }
    return descriptor;
}

static int64_t linux_socket_bind(process_t *process, int64_t descriptor,
                                 uint64_t address_pointer,
                                 uint64_t address_length) {
    linux_fd_entry_t entry;
    socket_t *socket;
    uint8_t address6[16] = {0};
    uint32_t address4 = 0U;
    uint16_t port = 0U;
    kstatus_t status = linux_fd_lookup(process, descriptor, &entry);
    if (status != K_OK) return status;
    if (entry.kind == LINUX_FD_UNIX_SOCKET) {
        uint16_t family = 0U;
        if (address_pointer == 0U || address_length < sizeof(family) ||
            !linux_user_pointer(address_pointer, sizeof(family)) ||
            copy_from_user(&family,
                           (const void __user *)(uintptr_t)address_pointer,
                           sizeof(family)) != K_OK) {
            status = LINUX_EFAULT;
        } else {
            status = family == LINUX_AF_UNIX ? K_OK : LINUX_EAFNOSUPPORT;
        }
        linux_fd_entry_put(&entry);
        return status;
    }
    linux_fd_entry_put(&entry);
    status = linux_socket_entry(process, descriptor, &entry, &socket);
    if (status != K_OK) return status;
    status = linux_socket_decode_address(address_pointer, address_length,
                                         socket->family, &address4, &port,
                                         address6);
    if (status == K_OK) {
        status = socket->family == OS_AF_INET4 ?
            socket_bind(socket, address4, port) :
            socket_bind_ipv6(socket, address6, port);
    }
    linux_fd_entry_put(&entry);
    return status;
}

static bool linux_socket_connect_complete(void *context) {
    return socket_connect_status((socket_t *)context) != K_EAGAIN;
}

static int64_t linux_socket_connect(process_t *process, int64_t descriptor,
                                    uint64_t address_pointer,
                                    uint64_t address_length) {
    linux_fd_entry_t entry;
    socket_t *socket;
    uint8_t address6[16] = {0};
    uint32_t address4 = 0U;
    uint16_t port = 0U;
    kstatus_t status = linux_fd_lookup(process, descriptor, &entry);
    if (status != K_OK) return status;
    if (entry.kind == LINUX_FD_UNIX_SOCKET) {
        linux_fd_entry_put(&entry);
        return K_ENOENT;
    }
    linux_fd_entry_put(&entry);
    status = linux_socket_entry(process, descriptor, &entry, &socket);
    if (status != K_OK) return status;
    status = linux_socket_decode_address(address_pointer, address_length,
                                         socket->family, &address4, &port,
                                         address6);
    if (status == K_OK) {
        status = socket->family == OS_AF_INET4 ?
            socket_connect(socket, address4, port) :
            socket_connect_ipv6(socket, address6, port);
        if (status == K_OK && socket->type == OS_SOCK_STREAM) {
            status = socket_connect_status(socket);
            if (status == K_EAGAIN) {
                if ((entry.flags & LINUX_O_NONBLOCK) != 0U) {
                    status = LINUX_EINPROGRESS;
                } else {
                    status = wait_on_queue(&socket->waitq,
                        linux_socket_connect_complete, socket, UINT64_MAX);
                    if (status == K_OK) status = socket_connect_status(socket);
                }
            }
        }
    }
#if OS_DEBUG_SERIAL
    if (g_debug_linux_socket_connect_logs < 128U) {
        kernel_serial_printf_serial_only(
            "OS_LINUX_SOCKET_CONNECT fd=%d addr=%x port=%u status=%d\r\n",
            descriptor, address4, port, status);
        ++g_debug_linux_socket_connect_logs;
    }
#endif
    linux_fd_entry_put(&entry);
    return linux_socket_status(status);
}

static int64_t linux_socket_listen(process_t *process, int64_t descriptor,
                                   uint64_t backlog) {
    linux_fd_entry_t entry;
    socket_t *socket;
    kstatus_t status = linux_fd_lookup(process, descriptor, &entry);
    if (status != K_OK) return status;
    if (entry.kind == LINUX_FD_UNIX_SOCKET) {
        status = backlog > UINT32_MAX ? K_EINVAL : K_OK;
        linux_fd_entry_put(&entry);
        return status;
    }
    linux_fd_entry_put(&entry);
    status = linux_socket_entry(process, descriptor, &entry, &socket);
    if (status != K_OK) return status;
    status = backlog > UINT32_MAX ? K_EINVAL :
             socket_listen(socket, (uint32_t)backlog);
    linux_fd_entry_put(&entry);
    return status;
}

static int64_t linux_socket_accept(process_t *process, int64_t descriptor,
                                   uint64_t address_pointer,
                                   uint64_t address_length_pointer,
                                   uint32_t flags) {
    linux_fd_entry_t entry;
    socket_t *listener;
    socket_t *accepted = 0;
    uint32_t descriptor_flags = LINUX_O_RDWR;
    int64_t new_descriptor;
    kstatus_t status;
    if ((flags & ~(LINUX_SOCK_NONBLOCK | LINUX_SOCK_CLOEXEC)) != 0U ||
        (address_pointer == 0U) != (address_length_pointer == 0U)) {
        return K_EINVAL;
    }
    status = linux_socket_entry(process, descriptor, &entry, &listener);
    if (status != K_OK) return status;
    status = socket_accept(listener,
                           (entry.flags & LINUX_O_NONBLOCK) != 0U ?
                               0U : UINT64_MAX,
                           &accepted);
    linux_fd_entry_put(&entry);
    /* accept 仅有队列等待超时，不涉及已建立 TCP 连接的在途数据。 */
    if (status == K_ETIMEDOUT) status = K_EAGAIN;
    status = linux_socket_status(status);
    if (status != K_OK) return status;
    if ((flags & LINUX_SOCK_NONBLOCK) != 0U) descriptor_flags |= LINUX_O_NONBLOCK;
    if ((flags & LINUX_SOCK_CLOEXEC) != 0U) descriptor_flags |= LINUX_O_CLOEXEC;
    new_descriptor = linux_fd_install(process, -1, 0, accepted,
                                      LINUX_FD_SOCKET, descriptor_flags);
    if (new_descriptor < 0) {
        object_put(accepted);
        return new_descriptor;
    }
    if (address_pointer != 0U) {
        status = linux_socket_copy_name(accepted, true, address_pointer,
                                        address_length_pointer);
        if (status != K_OK) {
            linux_fd_entry_t accepted_entry;
            if (linux_fd_detach(process, new_descriptor,
                                &accepted_entry) == K_OK) {
                linux_fd_entry_put(&accepted_entry);
            }
            return status;
        }
    }
    return new_descriptor;
}

static int64_t linux_socket_shutdown(process_t *process, int64_t descriptor,
                                     uint64_t how) {
    linux_fd_entry_t entry;
    socket_t *socket;
    kstatus_t status = linux_fd_lookup(process, descriptor, &entry);
    if (status != K_OK) return status;
    if (entry.kind == LINUX_FD_UNIX_SOCKET) {
        status = linux_unix_socket_shutdown(
            (linux_unix_socket_t *)entry.private_data, (uint32_t)how);
        linux_fd_entry_put(&entry);
        return status;
    }
    if (entry.kind != LINUX_FD_SOCKET || entry.private_data == 0) {
        linux_fd_entry_put(&entry);
        return K_ENOTSOCK;
    }
    socket = (socket_t *)entry.private_data;
    status = how > OS_SOCKET_SHUT_BOTH ? K_EINVAL :
             socket_shutdown(socket, (uint32_t)how);
    linux_fd_entry_put(&entry);
    return status;
}

static bool linux_socket_option(uint32_t level, uint32_t name,
                                uint32_t *native_option) {
    if (native_option == 0) return false;
    if (level == LINUX_SOL_TCP) {
        switch (name) {
        case LINUX_TCP_KEEPIDLE: *native_option = OS_SOCKET_OPTION_KEEPIDLE; return true;
        case LINUX_TCP_KEEPINTVL: *native_option = OS_SOCKET_OPTION_KEEPINTVL; return true;
        case LINUX_TCP_KEEPCNT: *native_option = OS_SOCKET_OPTION_KEEPCNT; return true;
        default: return false;
        }
    }
    if (level != LINUX_SOL_SOCKET || native_option == 0) return false;
    switch (name) {
    case LINUX_SO_REUSEADDR: *native_option = OS_SOCKET_OPTION_REUSE_ADDRESS; return true;
    case LINUX_SO_ERROR: *native_option = OS_SOCKET_OPTION_ERROR; return true;
    case LINUX_SO_TYPE: *native_option = OS_SOCKET_OPTION_TYPE; return true;
    case LINUX_SO_DOMAIN: *native_option = OS_SOCKET_OPTION_DOMAIN; return true;
    case LINUX_SO_ACCEPTCONN: *native_option = OS_SOCKET_OPTION_ACCEPT_CONNECTION; return true;
    case LINUX_SO_RCVBUF: *native_option = OS_SOCKET_OPTION_RECEIVE_BUFFER; return true;
    case LINUX_SO_SNDBUF: *native_option = OS_SOCKET_OPTION_SEND_BUFFER; return true;
    case LINUX_SO_KEEPALIVE: *native_option = OS_SOCKET_OPTION_KEEPALIVE; return true;
    default: return false;
    }
}

static int64_t linux_socket_setsockopt(process_t *process, int64_t descriptor,
                                       uint32_t level, uint32_t name,
                                       uint64_t value_pointer,
                                       uint64_t value_length) {
    linux_fd_entry_t entry;
    socket_t *socket;
    uint32_t native_option;
    int32_t value;
    kstatus_t status;
    if (!linux_socket_option(level, name, &native_option) ||
        value_length < sizeof(value) || value_pointer == 0U ||
        !linux_user_pointer(value_pointer, sizeof(value))) return K_EINVAL;
    if (copy_from_user(&value, (const void __user *)(uintptr_t)value_pointer,
                       sizeof(value)) != K_OK) return LINUX_EFAULT;
    status = linux_socket_entry(process, descriptor, &entry, &socket);
    if (status != K_OK) return status;
    status = socket_set_option(socket, native_option, value);
    linux_fd_entry_put(&entry);
    return status;
}

static int64_t linux_socket_getsockopt(process_t *process, int64_t descriptor,
                                       uint32_t level, uint32_t name,
                                       uint64_t value_pointer,
                                       uint64_t value_length_pointer) {
    linux_fd_entry_t entry;
    uint32_t native_option;
    uint32_t requested_length;
    uint32_t actual_length = sizeof(int32_t);
    int32_t value = 0;
    kstatus_t status;
    if (!linux_socket_option(level, name, &native_option) ||
        value_pointer == 0U || value_length_pointer == 0U ||
        !linux_user_pointer(value_length_pointer, sizeof(requested_length)) ||
        copy_from_user(&requested_length,
                       (const void __user *)(uintptr_t)value_length_pointer,
                       sizeof(requested_length)) != K_OK) return LINUX_EFAULT;
    status = linux_fd_lookup(process, descriptor, &entry);
    if (status != K_OK) return status;
    if (entry.kind == LINUX_FD_UNIX_SOCKET && entry.private_data != 0 &&
        ((linux_unix_socket_t *)entry.private_data)->object.type ==
            KOBJECT_TYPE_LINUX_UNIX_SOCKET) {
        switch (name) {
        case LINUX_SO_TYPE: value = LINUX_SOCK_STREAM; break;
        case LINUX_SO_DOMAIN: value = LINUX_AF_UNIX; break;
        case LINUX_SO_ERROR:
        case LINUX_SO_ACCEPTCONN: value = 0; break;
        default: status = K_EINVAL; break;
        }
    } else if (entry.kind == LINUX_FD_SOCKET && entry.private_data != 0 &&
               ((socket_t *)entry.private_data)->object.type ==
                   KOBJECT_TYPE_SOCKET) {
        status = socket_get_option((socket_t *)entry.private_data,
                                   native_option, &value);
#if OS_DEBUG_SERIAL
        if (name == LINUX_SO_ERROR && g_debug_socket_error_logs < 128U) {
            kernel_serial_printf_serial_only(
                "OS_SOCKET_SO_ERROR pid=%llu fd=%d status=%d value=%d\r\n",
                (unsigned long long)process->pid, (int)descriptor,
                status, value);
            ++g_debug_socket_error_logs;
        }
#endif
    } else {
        status = K_ENOTSOCK;
    }
    linux_fd_entry_put(&entry);
    if (status != K_OK) return status;
    if (requested_length != 0U) {
        uint32_t copy_length = requested_length < actual_length ?
                               requested_length : actual_length;
        if (!linux_user_pointer(value_pointer, copy_length) ||
            copy_to_user((void __user *)(uintptr_t)value_pointer, &value,
                         copy_length) != K_OK) return LINUX_EFAULT;
    }
    if (copy_to_user((void __user *)(uintptr_t)value_length_pointer,
                     &actual_length, sizeof(actual_length)) != K_OK) {
        return LINUX_EFAULT;
    }
    return K_OK;
}

static wait_queue_t g_linux_poll_waitq;
static atomic_uint g_linux_poll_waitq_state;

static void linux_poll_waitq_init(void) {
    unsigned expected = 0U;
    if (atomic_compare_exchange_strong_explicit(
            &g_linux_poll_waitq_state, &expected, 1U,
            memory_order_acq_rel, memory_order_acquire)) {
        wait_queue_init(&g_linux_poll_waitq);
        atomic_store_explicit(&g_linux_poll_waitq_state, 2U,
                              memory_order_release);
        return;
    }
    while (atomic_load_explicit(&g_linux_poll_waitq_state,
                                memory_order_acquire) != 2U) {
        __asm__ volatile("pause");
    }
}

static bool linux_poll_never_ready(void *context) {
    (void)context;
    return false;
}

static bool linux_poll_read_requested(int16_t events) {
    return (events & (LINUX_POLLIN | LINUX_POLLPRI | LINUX_POLLRDNORM |
                      LINUX_POLLRDBAND)) != 0;
}

static bool linux_poll_write_requested(int16_t events) {
    return (events & (LINUX_POLLOUT | LINUX_POLLWRNORM |
                      LINUX_POLLWRBAND)) != 0;
}

static int16_t linux_poll_ready(int32_t descriptor,
                                const linux_fd_entry_t *entry,
                                int16_t requested) {
    int16_t ready = 0;
    if (entry == 0) return LINUX_POLLNVAL;
    if (entry->kind == LINUX_FD_NULL) {
        if (linux_poll_read_requested(requested)) ready |= LINUX_POLLIN;
        if (linux_poll_write_requested(requested)) ready |= LINUX_POLLOUT;
        return ready;
    }
    if (entry->kind == LINUX_FD_NVIDIA) {
        uint32_t state = nv_frontend_poll_state(entry->private_data, false);
        if (linux_poll_read_requested(requested) &&
            (state & NV_FRONTEND_POLL_READ) != 0U) {
            int16_t bits = requested & (LINUX_POLLIN | LINUX_POLLPRI);
            ready |= bits != 0 ? bits : (int16_t)LINUX_POLLIN;
        }
        if ((state & NV_FRONTEND_POLL_CLOSED) != 0U) ready |= LINUX_POLLHUP;
        return ready;
    }
    if (entry->kind == LINUX_FD_CONSOLE) {
        if (descriptor == 1 || descriptor == 2) {
            if (linux_poll_write_requested(requested)) ready |= LINUX_POLLOUT;
        }
        return ready;
    }
    if (entry->kind == LINUX_FD_FILE || entry->kind == LINUX_FD_PROC) {
        if (linux_poll_read_requested(requested)) ready |= LINUX_POLLIN;
        if (linux_poll_write_requested(requested)) ready |= LINUX_POLLOUT;
        return ready;
    }
    if (entry->kind == LINUX_FD_SOCKET) {
        uint32_t state = socket_poll_state((socket_t *)entry->private_data);
        if (linux_poll_read_requested(requested) &&
            (state & SOCKET_POLL_READ) != 0U) ready |= LINUX_POLLIN;
        if (linux_poll_write_requested(requested) &&
            (state & SOCKET_POLL_WRITE) != 0U) ready |= LINUX_POLLOUT;
        if ((state & SOCKET_POLL_ERROR) != 0U) ready |= LINUX_POLLERR;
        if ((state & SOCKET_POLL_HUP) != 0U) ready |= LINUX_POLLHUP;
        return ready;
    }
    if (entry->kind == LINUX_FD_UNIX_SOCKET) {
        ready = (int16_t)linux_unix_socket_ready(
            (const linux_unix_socket_t *)entry->private_data,
            (uint32_t)(uint16_t)requested);
        return ready;
    }
    if (entry->kind == LINUX_FD_PIPE) {
        pipe_endpoint_t *endpoint = (pipe_endpoint_t *)entry->private_data;
        if (pipe_endpoint_is_read(endpoint)) {
            if (linux_poll_read_requested(requested) &&
                object_is_signaled(endpoint)) ready |= LINUX_POLLIN;
        } else if (object_is_signaled(endpoint)) {
            int64_t value = object_wait_value(endpoint);
            if ((value & PIPE_WAIT_ERROR) != 0) ready |= LINUX_POLLERR;
            else if (linux_poll_write_requested(requested)) {
                ready |= LINUX_POLLOUT;
            }
        }
        return ready;
    }
    if (entry->kind == LINUX_FD_WINDOW &&
        linux_poll_read_requested(requested) &&
        object_is_signaled(entry->private_data)) {
        ready |= LINUX_POLLIN;
    }
    if (entry->kind == LINUX_FD_EVENTFD) {
        if (linux_poll_read_requested(requested) &&
            object_is_signaled(entry->private_data)) ready |= LINUX_POLLIN;
        if (linux_poll_write_requested(requested) &&
            linux_eventfd_can_write((linux_eventfd_object_t *)
                                    entry->private_data)) {
            ready |= LINUX_POLLOUT;
        }
    }
    if (entry->kind == LINUX_FD_EPOLL &&
        linux_poll_read_requested(requested) &&
        object_is_signaled(entry->private_data)) {
        ready |= LINUX_POLLIN;
    }
    return ready;
}

typedef struct linux_poll_wait_context {
    const linux_pollfd_t *fds;
    const linux_fd_entry_t *entries;
    uint64_t count;
} linux_poll_wait_context_t;

typedef struct linux_poll_wait_set {
    void **objects;
    uint8_t *owned;
    uint32_t capacity;
    uint32_t count;
} linux_poll_wait_set_t;

static void linux_poll_wait_set_release(linux_poll_wait_set_t *set) {
    if (set == 0) return;
    for (uint32_t index = 0U; index < set->count; ++index) {
        if (set->owned[index]) object_put(set->objects[index]);
    }
    set->count = 0U;
}

static kstatus_t linux_poll_wait_set_add(linux_poll_wait_set_t *set,
                                         void *source, bool take_reference) {
    if (set == 0) return K_EINVAL;
    if (source == 0) return K_OK;
    for (uint32_t index = 0U; index < set->count; ++index) {
        if (set->objects[index] != source) continue;
        /* 可变 Unix socket 端点可能与 fd 快照中已借用的源对象别名。
         * 持有其所有者锁时只升级一次引用。 */
        if (take_reference && !set->owned[index]) {
            if (!object_try_get(source)) return K_ENOENT;
            set->owned[index] = 1U;
        }
        return K_OK;
    }
    if (set->count >= set->capacity) return K_ENOSPC;
    if (take_reference && !object_try_get(source)) return K_ENOENT;
    set->objects[set->count] = source;
    set->owned[set->count] = take_reference ? 1U : 0U;
    ++set->count;
    return K_OK;
}

static kstatus_t linux_poll_capture_wait_set(
    const linux_pollfd_t *fds, const linux_fd_entry_t *entries,
    uint64_t count, linux_poll_wait_set_t *set) {
    kstatus_t status = K_OK;
    if (fds == 0 || entries == 0 || set == 0) return K_EINVAL;
    set->count = 0U;
    for (uint64_t index = 0U; index < count; ++index) {
        const linux_fd_entry_t *entry = &entries[index];
        const linux_pollfd_t *pollfd = &fds[index];
        if (pollfd->revents != 0 || entry->private_data == 0 ||
            (!linux_poll_read_requested(pollfd->events) &&
             !linux_poll_write_requested(pollfd->events))) continue;
        if (entry->kind == LINUX_FD_NVIDIA) {
            status = linux_poll_wait_set_add(set,
                nv_frontend_file_wait_object(entry->private_data), false);
            if (status != K_OK) break;
            continue;
        }
        switch (entry->kind) {
        case LINUX_FD_SOCKET:
        case LINUX_FD_UNIX_SOCKET:
        case LINUX_FD_WINDOW:
        case LINUX_FD_PIPE:
        case LINUX_FD_EPOLL:
            break;
        case LINUX_FD_EVENTFD:
            if (linux_poll_read_requested(pollfd->events)) break;
            continue;
        default:
            continue;
        }
        status = linux_poll_wait_set_add(set, entry->private_data, false);
        if (status != K_OK) break;
        if (entry->kind == LINUX_FD_UNIX_SOCKET) {
            linux_unix_socket_t *socket =
                (linux_unix_socket_t *)entry->private_data;
            /* shutdown 可能移除任一端点：在 socket 锁保护指针期间固定两端，随后订阅管道 I/O。 */
            spinlock_lock(&socket->lock);
            status = linux_poll_wait_set_add(set, socket->read_end, true);
            if (status == K_OK) {
                status = linux_poll_wait_set_add(set, socket->write_end, true);
            }
            spinlock_unlock(&socket->lock);
            if (status != K_OK) break;
        }
    }
    if (status != K_OK) linux_poll_wait_set_release(set);
    return status;
}

static bool linux_poll_wait_ready(void *opaque) {
    linux_poll_wait_context_t *context = (linux_poll_wait_context_t *)opaque;
    if (context == 0) return false;
    for (uint64_t index = 0U; index < context->count; ++index) {
        const linux_pollfd_t *pollfd = &context->fds[index];
        if (pollfd->descriptor >= 0 &&
            context->entries[index].kind != LINUX_FD_UNUSED &&
            linux_poll_ready(pollfd->descriptor, &context->entries[index],
                             pollfd->events) != 0) return true;
    }
    return false;
}

static uint64_t linux_poll_timeout_ns(int64_t timeout_ms) {
    if (timeout_ms < 0) return UINT64_MAX;
    if ((uint64_t)timeout_ms > UINT64_MAX / 1000000ULL) return UINT64_MAX;
    return (uint64_t)timeout_ms * 1000000ULL;
}

/* 每次扫描获取一份表快照；就绪检查和所有释放回调均在表锁外执行。
 * 同一 fd 重复出现时仍分别持有引用。 */
static void linux_poll_capture_entries(process_t *process, linux_pollfd_t *fds,
                                       uint64_t count, linux_fd_entry_t *entries) {
    if (count == 0U) return;
    spinlock_lock(&process->linux_fds.lock);
    for (uint64_t index = 0U; index < count; ++index) {
        int32_t descriptor = fds[index].descriptor;
        fds[index].revents = 0;
        entries[index] = (linux_fd_entry_t){0};
        if (descriptor < 0) continue;
        if (!linux_fd_valid(descriptor)) {
            fds[index].revents = LINUX_POLLNVAL;
            continue;
        }
        entries[index] = process->linux_fds.entries[(uint32_t)descriptor];
        if (entries[index].kind == LINUX_FD_UNUSED) {
            fds[index].revents = LINUX_POLLNVAL;
        } else {
            linux_fd_entry_get(&entries[index]);
        }
    }
    spinlock_unlock(&process->linux_fds.lock);
}

static int64_t linux_poll(process_t *process, uint64_t fds_pointer,
                           uint64_t count, int64_t timeout_ms) {
    /* 用户态复制和捕获会在使用前初始化每个活动元素。 */
    linux_pollfd_t inline_fds[8];
    linux_fd_entry_t inline_entries[8];
    linux_pollfd_t *fds = inline_fds;
    linux_fd_entry_t *entries = inline_entries;
    /* 每个描述符最多贡献一个包装对象和两个 Unix 端点。
     * 常见的 <=8 fd 等待使用栈空间，不再使用旧版完整上限
     * 27 KiB table. Larger blocking sets allocate once, never under a lock. */
    void *inline_objects[3U * 8U];
    uint8_t inline_owned[3U * 8U];
    linux_poll_wait_set_t inline_wait_set = {
        .objects = inline_objects,
        .owned = inline_owned,
        .capacity = 3U * 8U,
    };
    linux_poll_wait_set_t *wait_set = &inline_wait_set;
    void *wait_storage = 0;
    int ready_count;
    uint64_t timeout_ns;
    uint64_t deadline;
    bool entries_held = false;
    kstatus_t status;
    if (process == 0 || count > LINUX_MAX_POLL_FDS ||
        count > (uint64_t)SIZE_MAX / sizeof(linux_pollfd_t) ||
        (count != 0U && !linux_user_pointer(fds_pointer,
                              (size_t)count * sizeof(linux_pollfd_t)))) {
        return count != 0U ? LINUX_EFAULT : K_EINVAL;
    }
    if (count > 8U) {
        fds = kmalloc((size_t)count * sizeof(*fds), 0U);
        entries = kmalloc((size_t)count * sizeof(*entries), 0U);
        if (!fds || !entries) {
            kfree(fds); kfree(entries);
            return K_ENOMEM;
        }
    }
    if (count != 0U && copy_from_user(fds,
            (const void __user *)(uintptr_t)fds_pointer,
            (size_t)count * sizeof(*fds)) != K_OK) {
        if (count > 8U) { kfree(fds); kfree(entries); }
        return LINUX_EFAULT;
    }
    timeout_ns = linux_poll_timeout_ns(timeout_ms);
    if (timeout_ns == UINT64_MAX) {
        deadline = UINT64_MAX;
    } else {
        uint64_t now = x86_read_tsc();
        uint64_t ticks = x86_timeout_ns_to_tsc(timeout_ns);
        deadline = ticks > UINT64_MAX - now ? UINT64_MAX : now + ticks;
    }

    for (;;) {
        if (entries_held) {
            for (uint64_t index = 0U; index < count; ++index) {
                linux_fd_entry_put(&entries[index]);
            }
        }
        entries_held = true;
        ready_count = 0;
        linux_poll_capture_entries(process, fds, count, entries);
        for (uint64_t index = 0U; index < count; ++index) {
            linux_pollfd_t *pollfd = &fds[index];
            if (pollfd->descriptor < 0) continue;
            if (pollfd->revents == 0) {
                pollfd->revents = linux_poll_ready(pollfd->descriptor,
                                                   &entries[index],
                                                   pollfd->events);
            }
            if (entries[index].kind == LINUX_FD_NVIDIA &&
                (pollfd->revents & (LINUX_POLLIN | LINUX_POLLPRI)) != 0)
                (void)nv_frontend_poll_state(entries[index].private_data, true);
            if (pollfd->revents != 0) ++ready_count;
        }
        if (ready_count != 0 || timeout_ms == 0) break;
        if (deadline != UINT64_MAX &&
            (int64_t)(x86_read_tsc() - deadline) >= 0) break;
        /* 就绪扫描和零超时扫描不会订阅对象。常见的小 fd 路径保持无分配，
         * 仅真实等待时才分配一次。 */
        if (count > 8U && wait_storage == 0) {
            size_t source_capacity = (size_t)count * 3U;
            wait_storage = kmalloc(source_capacity *
                (sizeof(*wait_set->objects) + sizeof(*wait_set->owned)), 0U);
            if (wait_storage == 0) {
                ready_count = K_ENOMEM;
                break;
            }
            wait_set->objects = wait_storage;
            wait_set->owned = (uint8_t *)(wait_set->objects + source_capacity);
            wait_set->capacity = (uint32_t)source_capacity;
        }
        status = linux_poll_capture_wait_set(fds, entries, count, wait_set);
        if (status != K_OK) {
            ready_count = (int)status;
            break;
        }
        uint64_t remaining_ns = UINT64_MAX;
        if (deadline != UINT64_MAX) {
            uint64_t now = x86_read_tsc();
            if (now >= deadline) {
                linux_poll_wait_set_release(wait_set);
                break;
            }
            remaining_ns = x86_tsc_to_ns(deadline - now);
        }
        if (wait_set->count != 0U) {
            linux_poll_wait_context_t context = {
                .fds = fds,
                .entries = entries,
                .count = count,
            };
            status = object_wait_on_signals(wait_set->objects, wait_set->count,
                                            linux_poll_wait_ready, &context,
                                            remaining_ns);
        } else {
            linux_poll_waitq_init();
            status = wait_on_queue(&g_linux_poll_waitq,
                                   linux_poll_never_ready, 0,
                                   remaining_ns);
        }
        linux_poll_wait_set_release(wait_set);
        if (status != K_OK && status != K_ETIMEDOUT) {
            ready_count = (int)status;
            break;
        }
    }

    for (uint64_t index = 0U; index < count; ++index) {
        if (entries[index].kind != LINUX_FD_UNUSED) {
            linux_fd_entry_put(&entries[index]);
        }
    }
    if (count != 0U && copy_to_user(
            (void __user *)(uintptr_t)fds_pointer, fds,
            (size_t)count * sizeof(linux_pollfd_t)) != K_OK) {
        ready_count = LINUX_EFAULT;
    }
    kfree(wait_storage);
    if (count > 8U) { kfree(fds); kfree(entries); }
    return ready_count;
}

static int64_t linux_read(process_t *process, int64_t descriptor,
                          uint64_t buffer, uint64_t length) {
    linux_fd_entry_t entry;
    if (length > (uint64_t)SIZE_MAX) return K_EOVERFLOW;
    kstatus_t status = linux_fd_lookup(process, descriptor, &entry);
    if (status != K_OK) return status;
    if (entry.kind == LINUX_FD_NULL) {
        linux_fd_entry_put(&entry);
        return 0;
    }
    if (entry.kind == LINUX_FD_CONSOLE) {
        linux_fd_entry_put(&entry);
        return descriptor == 0 ? 0 : K_EBADF;
    }
    if (entry.kind == LINUX_FD_NVIDIA) {
        linux_fd_entry_put(&entry);
        return K_EBADF;
    }
    if (entry.kind == LINUX_FD_WINDOW) {
        if (length == 0U) {
            linux_fd_entry_put(&entry);
            return 0;
        }
        if (length < sizeof(os_window_event_t)) {
            linux_fd_entry_put(&entry);
            return K_EINVAL;
        }
        os_window_event_t event;
        uint32_t identifier = window_server_window_identifier(
            (window_server_window_t *)entry.private_data);
        uint64_t timeout_ns = (entry.flags & LINUX_O_NONBLOCK) != 0U ?
                              0U : UINT64_MAX;
        status = window_server_event_read(process, identifier, &event,
                                          timeout_ns);
        if (status == K_OK) {
            status = copy_to_user(
                (void __user *)(uintptr_t)buffer, &event, sizeof(event));
#if OS_DEBUG_SERIAL
            /* 原生 shell 窗口使用原生 ABI。因此执行到此路径可证明 Linux/Ozone 客户端已从 System 窗口描述符
             * 消费输入事件。 */
            if (status == K_OK && event.type == OS_WINDOW_EVENT_INPUT &&
                event.input.type == OS_INPUT_EVENT_KEY &&
                event.input.value == OS_INPUT_VALUE_PRESS) {
                kernel_serial_printf_serial_only(
                    "OS_LINUX_WINDOW_INPUT_READ window=%u code=%u\r\n",
                    identifier, event.input.code);
            }
#endif
        }
        linux_fd_entry_put(&entry);
        return status == K_OK ? (int64_t)sizeof(event) :
               linux_copy_status(status);
    }
    if (entry.kind == LINUX_FD_SOCKET) {
        int64_t result = linux_socket_receive(
            (socket_t *)entry.private_data, &entry, buffer, length, 0U, 0U, 0U);
        linux_fd_entry_put(&entry);
        return result;
    }
    if (entry.kind == LINUX_FD_PIPE) {
        size_t request = (size_t)length;
        uint8_t *temporary;
        uint64_t bytes = 0U;
        if (request > LINUX_PIPE_COPY_MAX) request = LINUX_PIPE_COPY_MAX;
        if (request == 0U) {
            linux_fd_entry_put(&entry);
            return 0;
        }
        temporary = (uint8_t *)kmalloc(request, 0U);
        if (temporary == 0) {
            linux_fd_entry_put(&entry);
            return K_ENOMEM;
        }
        status = pipe_read((pipe_endpoint_t *)entry.private_data, temporary,
                           request, (entry.flags & LINUX_O_NONBLOCK) != 0U ?
                           0U : UINT64_MAX, &bytes);
        if (status == K_OK && copy_to_user(
                (void __user *)(uintptr_t)buffer, temporary,
                (size_t)bytes) != K_OK) status = K_EACCES;
        kfree(temporary);
        linux_fd_entry_put(&entry);
        return status == K_OK ? (int64_t)bytes : linux_pipe_status(status);
    }
    if (entry.kind == LINUX_FD_UNIX_SOCKET) {
        linux_unix_socket_t *socket =
            (linux_unix_socket_t *)entry.private_data;
        linux_unix_channel_t *channel;
        pipe_endpoint_t *read_end;
        size_t request = (size_t)length;
        uint8_t *temporary;
        uint64_t bytes = 0U;
        if (socket == 0) {
            linux_fd_entry_put(&entry);
            return K_EBADF;
        }
        linux_unix_socket_pin_io(socket, false, &read_end, &channel);
        if (read_end == 0) {
            linux_fd_entry_put(&entry);
            return K_EBADF;
        }
        if (request > LINUX_PIPE_COPY_MAX) request = LINUX_PIPE_COPY_MAX;
        if (request == 0U) {
            linux_unix_socket_unpin_io(read_end, channel);
            linux_fd_entry_put(&entry);
            return 0;
        }
        temporary = (uint8_t *)kmalloc(request, 0U);
        if (temporary == 0) {
            linux_unix_socket_unpin_io(read_end, channel);
            linux_fd_entry_put(&entry);
            return K_ENOMEM;
        }
        status = channel != 0 ?
                 kmutex_lock(&channel->read_mutex,
                             linux_socket_timeout(&entry, 0U)) : K_OK;
        if (status == K_OK) {
            status = pipe_read(read_end, temporary, request,
                               (entry.flags & LINUX_O_NONBLOCK) != 0U ?
                               0U : UINT64_MAX, &bytes);
            if (status == K_OK)
                (void)linux_unix_channel_commit_read(channel, bytes, false, 0);
            if (channel != 0)
                (void)kmutex_unlock(&channel->read_mutex);
        }
        if (status == K_OK && copy_to_user(
                (void __user *)(uintptr_t)buffer, temporary,
                (size_t)bytes) != K_OK) status = K_EACCES;
        kfree(temporary);
        linux_unix_socket_unpin_io(read_end, channel);
        linux_fd_entry_put(&entry);
        return status == K_OK ? (int64_t)bytes : linux_pipe_status(status);
    }
    if (entry.kind == LINUX_FD_EVENTFD) {
        int64_t result = linux_eventfd_read(
            (linux_eventfd_object_t *)entry.private_data, &entry, buffer,
            length);
        linux_fd_entry_put(&entry);
        return result;
    }
    if (entry.kind == LINUX_FD_EPOLL) {
        linux_fd_entry_put(&entry);
        return K_EBADF;
    }
    if (entry.kind == LINUX_FD_PROC) {
        int64_t result = linux_proc_read_file(
            (linux_proc_file_t *)entry.private_data, buffer, length);
        linux_fd_entry_put(&entry);
        return result;
    }
    uint64_t bytes = 0U;
    status = vfs_read(entry.file, (void __user *)(uintptr_t)buffer,
                      (size_t)length, &bytes);
    linux_fd_entry_put(&entry);
    /* 后续批次失败时，向调用方报告已经完成的字节，避免重试时重复处理。 */
    return status == K_OK || bytes != 0U ? (int64_t)bytes : linux_copy_status(status);
}

static int64_t linux_console_write(process_t *process, int64_t descriptor,
                                   uint64_t buffer, size_t length) {
    size_t total = 0U;
    while (total < length) {
        uint8_t temporary[128];
        size_t chunk = length - total;
        if (chunk >= sizeof(temporary)) chunk = sizeof(temporary) - 1U;
        kstatus_t status = copy_from_user(temporary,
            (const void __user *)(uintptr_t)(buffer + total), chunk);
        if (status != K_OK) return linux_copy_status(status);
        temporary[chunk] = '\0';
        if (descriptor == 2 && linux_process_is_chrome_image(process)) {
            linux_chrome_stderr_write(process, (const char *)temporary, chunk);
        } else {
            kernel_serial_write_serial_only((const char *)temporary);
        }
        total += chunk;
    }
    return (int64_t)total;
}

static int64_t linux_write(process_t *process, int64_t descriptor,
                           uint64_t buffer, uint64_t length) {
    linux_fd_entry_t entry;
    if (length > (uint64_t)SIZE_MAX) return K_EOVERFLOW;
    kstatus_t status = linux_fd_lookup(process, descriptor, &entry);
    if (status != K_OK) return status;
    if (entry.kind == LINUX_FD_NULL) {
        linux_fd_entry_put(&entry);
        return (int64_t)length;
    }
    if (entry.kind == LINUX_FD_CONSOLE) {
        int64_t result = descriptor == 1 || descriptor == 2 ?
            linux_console_write(process, descriptor, buffer,
                                (size_t)length) : K_EBADF;
        linux_fd_entry_put(&entry);
        return result;
    }
    if (entry.kind == LINUX_FD_NVIDIA) {
        linux_fd_entry_put(&entry);
        return K_EBADF;
    }
    if (entry.kind == LINUX_FD_PROC) {
        linux_fd_entry_put(&entry);
        return K_EBADF;
    }
    if (entry.kind == LINUX_FD_WINDOW) {
        linux_fd_entry_put(&entry);
        return K_EBADF;
    }
    if (entry.kind == LINUX_FD_SOCKET) {
        int64_t result = linux_socket_send(
            (socket_t *)entry.private_data, &entry, buffer, length, 0U, 0U, 0U);
        linux_fd_entry_put(&entry);
        return result;
    }
    if (entry.kind == LINUX_FD_PIPE) {
        size_t request = (size_t)length;
        uint8_t *temporary;
        uint64_t bytes = 0U;
        if (request > LINUX_PIPE_COPY_MAX) request = LINUX_PIPE_COPY_MAX;
        if (request == 0U) {
            linux_fd_entry_put(&entry);
            return 0;
        }
        temporary = (uint8_t *)kmalloc(request, 0U);
        if (temporary == 0) {
            linux_fd_entry_put(&entry);
            return K_ENOMEM;
        }
        status = copy_from_user(temporary,
                                (const void __user *)(uintptr_t)buffer,
                                request);
        if (status == K_OK) {
            status = pipe_write((pipe_endpoint_t *)entry.private_data,
                                temporary, request,
                                (entry.flags & LINUX_O_NONBLOCK) != 0U ?
                                0U : UINT64_MAX, &bytes);
        }
        kfree(temporary);
        linux_fd_entry_put(&entry);
        return status == K_OK ? (int64_t)bytes : linux_pipe_status(status);
    }
    if (entry.kind == LINUX_FD_UNIX_SOCKET) {
        linux_unix_socket_t *socket =
            (linux_unix_socket_t *)entry.private_data;
        linux_unix_channel_t *channel;
        pipe_endpoint_t *write_end;
        size_t request = (size_t)length;
        uint8_t *temporary;
        uint64_t bytes = 0U;
        if (socket == 0) {
            linux_fd_entry_put(&entry);
            return K_EBADF;
        }
        linux_unix_socket_pin_io(socket, true, &write_end, &channel);
        if (write_end == 0) {
            linux_fd_entry_put(&entry);
            return K_EBADF;
        }
        if (request > LINUX_PIPE_COPY_MAX) request = LINUX_PIPE_COPY_MAX;
        if (request == 0U) {
            linux_unix_socket_unpin_io(write_end, channel);
            linux_fd_entry_put(&entry);
            return 0;
        }
        temporary = (uint8_t *)kmalloc(request, 0U);
        if (temporary == 0) {
            linux_unix_socket_unpin_io(write_end, channel);
            linux_fd_entry_put(&entry);
            return K_ENOMEM;
        }
        status = copy_from_user(temporary,
                                (const void __user *)(uintptr_t)buffer,
                                request);
        if (status == K_OK)
            status = channel != 0 ?
                     kmutex_lock(&channel->write_mutex,
                                 linux_socket_timeout(&entry, 0U)) : K_OK;
        if (status == K_OK) {
            status = pipe_write(write_end, temporary, request,
                                (entry.flags & LINUX_O_NONBLOCK) != 0U ?
                                0U : UINT64_MAX, &bytes);
            if (status == K_OK)
                linux_unix_channel_commit_write(channel, bytes);
            if (channel != 0)
                (void)kmutex_unlock(&channel->write_mutex);
        }
        kfree(temporary);
        linux_unix_socket_unpin_io(write_end, channel);
        linux_fd_entry_put(&entry);
        return status == K_OK ? (int64_t)bytes : linux_pipe_status(status);
    }
    if (entry.kind == LINUX_FD_EVENTFD) {
        int64_t result = linux_eventfd_write(
            (linux_eventfd_object_t *)entry.private_data, &entry, buffer,
            length);
        linux_fd_entry_put(&entry);
        return result;
    }
    if (entry.kind == LINUX_FD_EPOLL) {
        linux_fd_entry_put(&entry);
        return K_EBADF;
    }
    uint64_t bytes = 0U;
    status = vfs_write(entry.file, (const void __user *)(uintptr_t)buffer,
                       (size_t)length, &bytes);
    linux_fd_entry_put(&entry);
    return status == K_OK || bytes != 0U ? (int64_t)bytes : linux_copy_status(status);
}

/* 保持普通 FAT 路径的快速打开，同时正确处理内存文件系统的链接和特殊分量。 */
static kstatus_t linux_resolve_open_path(const char *path, uint64_t flags,
                                         char resolved[LINUX_MAX_PATH],
                                         const char **result) {
    bool exclusive = (flags & (LINUX_O_CREAT | LINUX_O_EXCL)) ==
                     (LINUX_O_CREAT | LINUX_O_EXCL);
    bool nofollow = (flags & LINUX_O_NOFOLLOW) != 0U;
    bool create = (flags & LINUX_O_CREAT) != 0U;
    *result = path;
    if (!vfs_path_needs_resolution(path)) return K_OK;
    kstatus_t status = vfs_resolve_path_kernel(path, !exclusive && !nofollow,
                                               create, resolved);
    if (status != K_OK) return status;
    if (nofollow && !exclusive) {
        os_file_info_t info;
        status = vfs_stat_kernel(resolved, &info);
        if (status == K_OK && info.type == OS_FILE_TYPE_SYMLINK) return K_ELOOP;
        if (status != K_OK && !(create && status == K_ENOENT)) return status;
    }
    *result = resolved;
    return K_OK;
}

static int64_t linux_positioned_io(process_t *process, int64_t descriptor,
                                   uint64_t buffer, uint64_t length,
                                   int64_t offset, bool write_operation) {
    linux_fd_entry_t entry = {0};
    file_t positioned = {0};
    uint64_t bytes = 0U;
    if (offset < 0 || length > INT64_MAX) return K_EINVAL;
    kstatus_t status = linux_fd_lookup(process, descriptor, &entry);
    if (status != K_OK) return status;
    if (entry.kind != LINUX_FD_FILE || entry.file == 0) {
        linux_fd_entry_put(&entry);
        return K_ESPIPE;
    }
    /* 整个调用期间，描述符引用持有 vnode。使用借用的私有 I/O 视图，绝不改变共享打开文件偏移。 */
    positioned.object.type = KOBJECT_TYPE_FILE;
    positioned.vnode = entry.file->vnode;
    positioned.ops = entry.file->ops;
    positioned.position = (uint64_t)offset;
    positioned.flags = entry.file->flags;
    positioned.rights = entry.file->rights;
    positioned.private_data = entry.file->private_data;
    vfs_file_position_init(&positioned);
    status = write_operation ?
        vfs_write(&positioned, (const void __user *)(uintptr_t)buffer,
                  (size_t)length, &bytes) :
        vfs_read(&positioned, (void __user *)(uintptr_t)buffer,
                 (size_t)length, &bytes);
    linux_fd_entry_put(&entry);
    return status == K_OK || bytes != 0U ? (int64_t)bytes : linux_copy_status(status);
}

static int64_t linux_openat(process_t *process, int64_t dirfd,
                            uint64_t path_pointer, uint64_t flags,
                            uint64_t mode) {
    char path[LINUX_MAX_PATH];
    file_t *file = 0;
    nv_frontend_file_t *frontend = 0;
    linux_proc_file_t *proc_file = 0;
    window_server_window_t *window = 0;
    kstatus_t status = linux_path_from_user(process, dirfd, path_pointer, path);
    if (status != K_OK) return status;
#if OS_DEBUG_SERIAL
    if (g_linux_open_debug_count++ < 64U) {
        kernel_serial_printf_serial_only(
            "OS_LINUX_OPEN path=%s flags=%llx mode=%llx\r\n", path,
            (unsigned long long)flags, (unsigned long long)mode);
    }
#endif
    if (linux_exact_path(path, "/dev/null")) {
        const uint64_t supported_flags = 3U | LINUX_O_NOCTTY |
            LINUX_O_CREAT | LINUX_O_EXCL | LINUX_O_TRUNC |
            LINUX_O_APPEND | LINUX_O_NONBLOCK |
            LINUX_O_DIRECTORY | LINUX_O_NOFOLLOW | LINUX_O_CLOEXEC;
        if ((flags & ~supported_flags) != 0U || (flags & 3U) == 3U)
            return K_EINVAL;
        if ((flags & LINUX_O_DIRECTORY) != 0U) return K_ENOTDIR;
        if ((flags & (LINUX_O_CREAT | LINUX_O_EXCL)) ==
            (LINUX_O_CREAT | LINUX_O_EXCL)) return K_EEXIST;
        return linux_fd_install(process, -1, 0, 0, LINUX_FD_NULL,
                                (uint32_t)flags);
    }
    if (linux_exact_path(path, "/dev/nvidiactl") ||
        linux_exact_path(path, "/dev/nvidia0")) {
        bool control = linux_exact_path(path, "/dev/nvidiactl");
        if ((flags & (LINUX_O_CREAT | LINUX_O_EXCL | LINUX_O_TRUNC |
                      LINUX_O_DIRECTORY)) != 0U)
            return K_EINVAL;
        status = nv_frontend_open(control, 0U, &frontend);
        if (status != K_OK) return status;
        int64_t descriptor = linux_fd_install(
            process, -1, 0, frontend, LINUX_FD_NVIDIA, (uint32_t)flags);
        if (descriptor < 0) nv_frontend_file_put(frontend);
        return descriptor;
    }
    if (linux_exact_path(path, LINUX_WINDOW_DEVICE_PATH)) {
        if ((flags & (LINUX_O_CREAT | LINUX_O_EXCL | LINUX_O_TRUNC |
                      LINUX_O_DIRECTORY)) != 0U) return K_EINVAL;
        status = window_server_lookup_owner(process, &window);
        if (status != K_OK) return status;
        int64_t descriptor = linux_fd_install(
            process, -1, 0, window, LINUX_FD_WINDOW, (uint32_t)flags);
        if (descriptor < 0) window_server_put(window);
        return descriptor;
    }
    status = linux_proc_open_path(process, path, &proc_file);
    if (status == K_OK) {
        if ((flags & (LINUX_O_WRONLY | LINUX_O_RDWR | LINUX_O_CREAT |
                      LINUX_O_EXCL | LINUX_O_TRUNC | LINUX_O_APPEND)) != 0U) {
            linux_proc_file_put(proc_file);
            return K_EACCES;
        }
        if ((flags & LINUX_O_DIRECTORY) != 0U &&
            !linux_proc_node_is_directory(proc_file->node)) {
            linux_proc_file_put(proc_file);
            return K_ENOTDIR;
        }
        int64_t descriptor = linux_fd_install(
            process, -1, 0, proc_file, LINUX_FD_PROC, (uint32_t)flags);
        if (descriptor < 0) linux_proc_file_put(proc_file);
        return descriptor;
    }
    uint32_t vfs_flags = linux_open_flags(flags);
    char resolved[LINUX_MAX_PATH];
    const char *vfs_path;
    status = linux_resolve_open_path(path, flags, resolved, &vfs_path);
    if (status != K_OK) return status;
    status = vfs_open_kernel(vfs_path, vfs_flags, (uint32_t)mode, &file);
    if ((flags & (LINUX_O_WRONLY | LINUX_O_RDWR | LINUX_O_CREAT |
                  LINUX_O_TRUNC | LINUX_O_APPEND | LINUX_O_DIRECTORY)) == 0U &&
        (status == K_EISDIR ||
         (status == K_ENOENT && linux_exact_path(path, "/tmp")))) {
        /* Linux 允许目录仅以 O_RDONLY 打开，不要求设置 O_DIRECTORY。 */
        status = vfs_open_kernel(vfs_path, vfs_flags | VFS_OPEN_DIRECTORY,
                                 (uint32_t)mode, &file);
    }
    if (status != K_OK) {
        if (path[0] == '/' && path[1] == 't' && path[2] == 'm' &&
            path[3] == 'p' && path[4] == '/' &&
            status != K_EEXIST &&
            __atomic_fetch_add(&g_linux_tmp_open_failure_logs, 1U,
                               __ATOMIC_RELAXED) < 16U) {
            kernel_serial_printf_serial_only(
                "OS_TMP_OPEN_FAIL pid=%llu path=%s status=%d flags=%llx\r\n",
                (unsigned long long)process->pid, path, (int)status,
                (unsigned long long)flags);
        }
#if OS_DEBUG_SERIAL
        if (g_linux_open_fail_debug_count++ < 256U) {
            kernel_serial_printf_serial_only(
                "OS_LINUX_OPEN_FAIL path=%s status=%d flags=%llx\r\n",
                path, (int)status, (unsigned long long)flags);
        }
#endif
        return status;
    }
    int64_t descriptor = linux_fd_install(process, -1, file, 0,
                                          LINUX_FD_FILE,
                                          (uint32_t)flags);
    if (descriptor < 0) linux_file_put(file);
    return descriptor;
}

static int64_t linux_close(process_t *process, int64_t descriptor) {
    linux_fd_entry_t entry = {0};
    kstatus_t status = linux_fd_detach(process, descriptor, &entry);
    if (status != K_OK) {
#if OS_DEBUG_SERIAL
        if (status == K_EBADF &&
            __atomic_fetch_add(&g_debug_linux_close_badfd_logs, 1U,
                               __ATOMIC_RELAXED) < 64U) {
            kernel_serial_printf_serial_only(
                "OS_LINUX_CLOSE_BADFD pid=%llu fd=%lld status=%d\r\n",
                (unsigned long long)(process != 0 ? process->pid : 0U),
                (long long)descriptor, (int)status);
        }
#endif
        return status;
    }
    linux_fd_entry_put(&entry);
    return K_OK;
}

static int64_t linux_dup(process_t *process, int64_t descriptor,
                         int64_t requested, uint32_t flags) {
    linux_fd_entry_t entry;
    kstatus_t status = linux_fd_lookup(process, descriptor, &entry);
    if (status != K_OK) return status;
    uint32_t new_flags = entry.flags & ~LINUX_O_CLOEXEC;
    if ((flags & LINUX_FD_CLOEXEC) != 0U) new_flags |= LINUX_O_CLOEXEC;
    int64_t result = linux_fd_install_description(process, requested, entry.file,
                                      entry.private_data, entry.kind,
                                      new_flags, entry.description);
    if (result < 0) linux_fd_entry_put(&entry);
    return result;
}

static int64_t linux_dup_min(process_t *process, int64_t descriptor,
                             uint32_t minimum, uint32_t flags) {
    linux_fd_entry_t entry = {0};
    kstatus_t status;
    int64_t result = K_EMFILE;
    if (minimum >= OS_LINUX_FD_LIMIT) return K_EINVAL;
    status = linux_fd_lookup(process, descriptor, &entry);
    if (status != K_OK) return status;
    entry.flags &= ~LINUX_O_CLOEXEC;
    if ((flags & LINUX_FD_CLOEXEC) != 0U) {
        entry.flags |= LINUX_O_CLOEXEC;
    }
    spinlock_lock(&process->linux_fds.lock);
    for (uint32_t candidate = minimum;
         candidate < OS_LINUX_FD_LIMIT; ++candidate) {
        if (process->linux_fds.entries[candidate].kind != LINUX_FD_UNUSED) {
            continue;
        }
        process->linux_fds.entries[candidate] = entry;
        entry = (linux_fd_entry_t){0};
        result = (int64_t)candidate;
        break;
    }
    spinlock_unlock(&process->linux_fds.lock);
    linux_fd_entry_put(&entry);
    return result;
}

static int64_t linux_fstat(process_t *process, int64_t descriptor,
                           uint64_t output_pointer) {
    linux_fd_entry_t entry;
    linux_stat_t stat;
    kstatus_t status = linux_fd_lookup(process, descriptor, &entry);
    if (status != K_OK) return status;
    if (entry.kind == LINUX_FD_NULL) {
        linux_null_stat(&stat);
        status = K_OK;
    } else if (entry.kind == LINUX_FD_CONSOLE) {
        linux_console_stat(descriptor, &stat);
        status = K_OK;
    } else if (entry.kind == LINUX_FD_NVIDIA) {
        linux_fd_entry_put(&entry);
        return K_EBADF;
    } else if (entry.kind == LINUX_FD_WINDOW) {
        linux_fd_entry_put(&entry);
        return K_EBADF;
    } else if (entry.kind == LINUX_FD_SOCKET ||
               entry.kind == LINUX_FD_UNIX_SOCKET) {
        linux_zero(&stat, sizeof(stat));
        stat.device = 1U;
        stat.inode = (entry.kind == LINUX_FD_SOCKET ? 0x534F434BULL :
                      0x554E4958ULL) + (uint64_t)(uint32_t)descriptor;
        stat.links = 1U;
        stat.mode = 0140000U | 0777U;
        stat.block_size = PAGE_SIZE;
        status = K_OK;
    } else if (entry.kind == LINUX_FD_PIPE && entry.private_data != 0) {
        os_file_info_t info;
        status = pipe_stat((pipe_endpoint_t *)entry.private_data, &info);
        if (status == K_OK) {
            linux_zero(&stat, sizeof(stat));
            stat.device = 1U;
            stat.inode = 0x50495045ULL + (uint64_t)(uint32_t)descriptor;
            stat.links = 1U;
            stat.mode = 0010000U | (info.mode & 07777U);
            stat.block_size = PAGE_SIZE;
        }
    } else if (entry.kind == LINUX_FD_EPOLL ||
               entry.kind == LINUX_FD_EVENTFD) {
        linux_zero(&stat, sizeof(stat));
        stat.device = 1U;
        stat.inode = (entry.kind == LINUX_FD_EPOLL ? 0x45504F4CULL :
                      0x45564446ULL) + (uint64_t)(uint32_t)descriptor;
        stat.links = 1U;
        stat.mode = 0020000U | 0600U;
        stat.block_size = PAGE_SIZE;
        status = K_OK;
    } else if (entry.kind == LINUX_FD_PROC && entry.private_data != 0) {
        linux_proc_stat((linux_proc_file_t *)entry.private_data, &stat);
        status = K_OK;
    }
    else {
        status = linux_file_stat(entry.file, &stat, 0U);
    }
    linux_fd_entry_put(&entry);
    if (status != K_OK) return status;
    status = copy_to_user((void __user *)(uintptr_t)output_pointer,
                          &stat, sizeof(stat));
    return linux_copy_status(status);
}

static int64_t linux_statfs_copy(const vfs_space_info_t *space,
                                uint64_t output_pointer) {
    linux_statfs_t stat;
    linux_zero(&stat, sizeof(stat));
    stat.type = space->type;
    stat.block_size = space->block_size;
    stat.fragment_size = space->block_size;
    stat.blocks = space->blocks;
    stat.blocks_free = space->blocks_free;
    stat.blocks_available = space->blocks_free;
    stat.filesystem_id[0] = (uint32_t)space->filesystem_id;
    stat.filesystem_id[1] = (uint32_t)(space->filesystem_id >> 32U);
    stat.name_max = space->name_max;
    return linux_copy_status(copy_to_user(
        (void __user *)(uintptr_t)output_pointer, &stat, sizeof(stat)));
}

static int64_t linux_statfs(process_t *process, uint64_t path_pointer,
                           uint64_t output_pointer) {
    char path[LINUX_MAX_PATH];
    vfs_space_info_t space;
    kstatus_t status = linux_path_from_user(process, LINUX_AT_FDCWD,
                                           path_pointer, path);
    if (status != K_OK) return status;
    status = vfs_space_kernel(path, &space);
    return status == K_OK ? linux_statfs_copy(&space, output_pointer) : status;
}

static int64_t linux_fstatfs(process_t *process, int64_t descriptor,
                            uint64_t output_pointer) {
    linux_fd_entry_t entry;
    vfs_space_info_t space;
    kstatus_t status = linux_fd_lookup(process, descriptor, &entry);
    if (status != K_OK) return status;
    status = entry.file != 0 ? vfs_file_space(entry.file, &space) : K_ENOSYS;
    linux_fd_entry_put(&entry);
    return status == K_OK ? linux_statfs_copy(&space, output_pointer) : status;
}

static kstatus_t linux_vfs_stat_path(const char *path, uint64_t flags,
                                   os_file_info_t *info) {
    /* 磁盘上没有符号链接且无特殊分量时，先直接查询目标，避免逐级重复访问目录。
     * 缺失时仍须逐段解析，以区分文件不存在和中间分量不是目录。 */
    if (!vfs_path_needs_resolution(path)) {
        kstatus_t fast = vfs_stat_kernel(path, info);
        if (fast != K_ENOENT) return fast;
    }
    char resolved[LINUX_MAX_PATH];
    kstatus_t status = vfs_resolve_path_kernel(path,
        (flags & LINUX_AT_SYMLINK_NOFOLLOW) == 0U, false, resolved);
    return status == K_OK ? vfs_stat_kernel(resolved, info) : status;
}

static uint32_t linux_vfs_info_mode(const os_file_info_t *info) {
    uint32_t type = info->type == OS_FILE_TYPE_SYMLINK ? 0120000U :
        (info->type == OS_FILE_TYPE_DIRECTORY ? 0040000U : 0100000U);
    return type | (info->mode & 07777U);
}

static int64_t linux_stat_path(process_t *process, int64_t dirfd,
                               uint64_t path_pointer, uint64_t flags,
                               uint64_t output_pointer) {
    char path[LINUX_MAX_PATH];
    os_file_info_t info;
    linux_stat_t stat;
    linux_proc_file_t *proc_file = 0;
    uint32_t nvidia_minor;
    kstatus_t status;
    (void)flags;
    if ((flags & ~(uint64_t)(LINUX_AT_SYMLINK_NOFOLLOW | LINUX_AT_EMPTY_PATH)) != 0U) {
        return K_EINVAL;
    }
    if (path_pointer == 0U && (flags & LINUX_AT_EMPTY_PATH) != 0U) {
        return linux_fstat(process, dirfd, output_pointer);
    }
    if ((flags & LINUX_AT_EMPTY_PATH) != 0U && path_pointer != 0U) {
        status = linux_copy_string(path_pointer, path, sizeof(path));
        if (status != K_OK) return status;
        if (path[0] == '\0') {
            return linux_fstat(process, dirfd, output_pointer);
        }
    }
    status = linux_path_from_user(process, dirfd, path_pointer, path);
    if (status != K_OK) return status;
    status = linux_proc_open_path(process, path, &proc_file);
    if (status == K_OK) {
        linux_proc_stat(proc_file, &stat);
        linux_proc_file_put(proc_file);
        status = copy_to_user((void __user *)(uintptr_t)output_pointer,
                              &stat, sizeof(stat));
        return linux_copy_status(status);
    }
    if (linux_exact_path(path, "/dev/null")) {
        linux_null_stat(&stat);
        return linux_copy_status(copy_to_user(
            (void __user *)(uintptr_t)output_pointer, &stat, sizeof(stat)));
    }
    if (linux_nvidia_device_path(path, &nvidia_minor)) {
        if (!nv_frontend_driver_ready()) return K_ENOENT;
        linux_nvidia_stat(nvidia_minor, &stat);
        return linux_copy_status(copy_to_user(
            (void __user *)(uintptr_t)output_pointer, &stat, sizeof(stat)));
    }
    status = linux_vfs_stat_path(path, flags, &info);
    if (status != K_OK) return status;
    linux_zero(&stat, sizeof(stat));
    stat.device = 1U;
    stat.inode = 1U;
    stat.links = 1U;
    stat.mode = linux_vfs_info_mode(&info);
    stat.size = (int64_t)info.size;
    stat.block_size = PAGE_SIZE;
    stat.blocks = (int64_t)((info.size + 511U) / 512U);
    status = copy_to_user((void __user *)(uintptr_t)output_pointer,
                          &stat, sizeof(stat));
    return linux_copy_status(status);
}

static int64_t linux_statx(process_t *process, int64_t dirfd,
                           uint64_t path_pointer, uint32_t flags,
                           uint32_t mask, uint64_t output_pointer) {
    char path[LINUX_MAX_PATH];
    os_file_info_t info;
    linux_statx_t stat;
    uint32_t nvidia_minor;
    kstatus_t status;
    if ((flags & ~(uint32_t)(LINUX_AT_SYMLINK_NOFOLLOW |
                             LINUX_AT_EMPTY_PATH)) != 0U) return K_EINVAL;
    if (path_pointer == 0U && (flags & LINUX_AT_EMPTY_PATH) != 0U) {
        linux_fd_entry_t entry;
        linux_stat_t legacy;
        status = linux_fd_lookup(process, dirfd, &entry);
        if (status != K_OK) return status;
        if (entry.kind == LINUX_FD_NULL) {
            linux_null_stat(&legacy);
            status = K_OK;
        } else if (entry.kind == LINUX_FD_CONSOLE) {
            linux_console_stat(dirfd, &legacy);
            status = K_OK;
        } else if (entry.kind == LINUX_FD_NVIDIA) {
            linux_fd_entry_put(&entry);
            return K_EBADF;
        } else if (entry.kind == LINUX_FD_WINDOW) {
            linux_fd_entry_put(&entry);
            return K_EBADF;
        } else if (entry.kind == LINUX_FD_SOCKET ||
                   entry.kind == LINUX_FD_UNIX_SOCKET) {
            linux_zero(&legacy, sizeof(legacy));
            legacy.device = 1U;
            legacy.inode = (entry.kind == LINUX_FD_SOCKET ? 0x534F434BULL :
                            0x554E4958ULL) + (uint64_t)(uint32_t)dirfd;
            legacy.links = 1U;
            legacy.mode = 0140000U | 0777U;
            legacy.block_size = PAGE_SIZE;
            status = K_OK;
        } else if (entry.kind == LINUX_FD_PIPE &&
                   entry.private_data != 0) {
            os_file_info_t info;
            status = pipe_stat((pipe_endpoint_t *)entry.private_data, &info);
            if (status == K_OK) {
                linux_zero(&legacy, sizeof(legacy));
                legacy.device = 1U;
                legacy.inode = 0x50495045ULL + (uint64_t)(uint32_t)dirfd;
                legacy.links = 1U;
                legacy.mode = 0010000U | (info.mode & 07777U);
                legacy.block_size = PAGE_SIZE;
            }
        } else if (entry.kind == LINUX_FD_EPOLL ||
                   entry.kind == LINUX_FD_EVENTFD) {
            linux_zero(&legacy, sizeof(legacy));
            legacy.device = 1U;
            legacy.inode = (entry.kind == LINUX_FD_EPOLL ? 0x45504F4CULL :
                            0x45564446ULL) + (uint64_t)(uint32_t)dirfd;
            legacy.links = 1U;
            legacy.mode = 0020000U | 0600U;
            legacy.block_size = PAGE_SIZE;
        } else if (entry.kind == LINUX_FD_PROC && entry.private_data != 0) {
            linux_proc_stat((linux_proc_file_t *)entry.private_data, &legacy);
            status = K_OK;
        }
        else {
            status = linux_file_stat(entry.file, &legacy, 0U);
        }
        linux_fd_entry_put(&entry);
        if (status != K_OK) return status;
        linux_zero(&stat, sizeof(stat));
        stat.mask = mask != 0U ? mask : 0x00000FFFU;
        stat.block_size = (uint32_t)legacy.block_size;
        stat.link_count = (uint32_t)legacy.links;
        stat.mode = (uint16_t)legacy.mode;
        stat.inode = legacy.inode;
        stat.size = (uint64_t)legacy.size;
        stat.blocks = (uint64_t)legacy.blocks;
        stat.device_minor = (uint32_t)legacy.device;
        if (entry.kind == LINUX_FD_NULL) {
            stat.rdevice_major = 1U;
            stat.rdevice_minor = 3U;
        }
        return linux_copy_status(copy_to_user(
            (void __user *)(uintptr_t)output_pointer, &stat, sizeof(stat)));
    }
    status = linux_path_from_user(process, dirfd, path_pointer, path);
    if (status != K_OK) return status;
    {
        linux_proc_file_t *proc_file = 0;
        status = linux_proc_open_path(process, path, &proc_file);
        if (status == K_OK) {
            linux_stat_t legacy;
            linux_proc_stat(proc_file, &legacy);
            linux_proc_file_put(proc_file);
            linux_zero(&stat, sizeof(stat));
            stat.mask = mask != 0U ? mask : 0x00000FFFU;
            stat.block_size = (uint32_t)legacy.block_size;
            stat.link_count = (uint32_t)legacy.links;
            stat.mode = (uint16_t)legacy.mode;
            stat.inode = legacy.inode;
            stat.size = (uint64_t)legacy.size;
            stat.blocks = (uint64_t)legacy.blocks;
            stat.device_major = 0U;
            stat.device_minor = (uint32_t)legacy.device;
            return linux_copy_status(copy_to_user(
                (void __user *)(uintptr_t)output_pointer, &stat,
                sizeof(stat)));
        }
    }
    if (linux_exact_path(path, "/dev/null")) {
        linux_zero(&stat, sizeof(stat));
        stat.mask = mask != 0U ? mask : 0x00000FFFU;
        stat.block_size = PAGE_SIZE;
        stat.link_count = 1U;
        stat.mode = 0020000U | 0666U;
        stat.inode = 3U;
        stat.rdevice_major = 1U;
        stat.rdevice_minor = 3U;
        return linux_copy_status(copy_to_user(
            (void __user *)(uintptr_t)output_pointer, &stat, sizeof(stat)));
    }
    if (linux_nvidia_device_path(path, &nvidia_minor)) {
        linux_stat_t legacy;
        if (!nv_frontend_driver_ready()) return K_ENOENT;
        linux_nvidia_stat(nvidia_minor, &legacy);
        linux_zero(&stat, sizeof(stat));
        stat.mask = mask != 0U ? mask : 0x00000FFFU;
        stat.block_size = (uint32_t)legacy.block_size;
        stat.link_count = (uint32_t)legacy.links;
        stat.mode = (uint16_t)legacy.mode;
        stat.inode = legacy.inode;
        stat.size = (uint64_t)legacy.size;
        stat.blocks = (uint64_t)legacy.blocks;
        stat.device_major = 195U;
        stat.device_minor = nvidia_minor;
        return linux_copy_status(copy_to_user(
            (void __user *)(uintptr_t)output_pointer, &stat, sizeof(stat)));
    }
    status = linux_vfs_stat_path(path, flags, &info);
    if (status != K_OK) return status;
    linux_zero(&stat, sizeof(stat));
    stat.mask = mask != 0U ? mask : 0x00000FFFU;
    stat.block_size = PAGE_SIZE;
    stat.link_count = 1U;
    stat.mode = (uint16_t)linux_vfs_info_mode(&info);
    stat.inode = 1U;
    stat.size = info.size;
    stat.blocks = (info.size + 511U) / 512U;
    stat.device_major = 0U;
    stat.device_minor = 1U;
    status = copy_to_user((void __user *)(uintptr_t)output_pointer,
                          &stat, sizeof(stat));
    return linux_copy_status(status);
}

static int64_t linux_lseek(process_t *process, int64_t descriptor,
                           int64_t offset, uint32_t whence) {
    linux_fd_entry_t entry;
    uint64_t position = 0U;
    kstatus_t status = linux_fd_lookup(process, descriptor, &entry);
    if (status != K_OK) return status;
    if (entry.kind == LINUX_FD_NULL) {
        linux_fd_entry_put(&entry);
        return whence <= 2U ? 0 : K_EINVAL;
    }
    if (entry.kind == LINUX_FD_CONSOLE) {
        linux_fd_entry_put(&entry);
        return K_ESPIPE;
    }
    if (entry.kind == LINUX_FD_NVIDIA) {
        linux_fd_entry_put(&entry);
        return K_ESPIPE;
    }
    if (entry.kind == LINUX_FD_WINDOW) {
        linux_fd_entry_put(&entry);
        return K_ESPIPE;
    }
    if (entry.kind == LINUX_FD_PIPE || entry.kind == LINUX_FD_EPOLL ||
        entry.kind == LINUX_FD_EVENTFD ||
        entry.kind == LINUX_FD_UNIX_SOCKET) {
        linux_fd_entry_put(&entry);
        return K_ESPIPE;
    }
    if (entry.kind == LINUX_FD_PROC && entry.private_data != 0) {
        linux_proc_file_t *proc_file =
            (linux_proc_file_t *)entry.private_data;
        int64_t base;
        int64_t target;
        uint64_t limit = linux_proc_node_is_directory(proc_file->node) ?
                         linux_proc_directory_count(proc_file) :
                         proc_file->length;
        spinlock_lock(&proc_file->lock);
        if (proc_file->position > (uint64_t)INT64_MAX) {
            status = K_EOVERFLOW;
        } else if (whence == 0U) {
            target = offset;
            status = target < 0 || (uint64_t)target > limit ? K_EINVAL : K_OK;
            if (status == K_OK) proc_file->position = (uint64_t)target;
        } else if (whence == 1U) {
            base = (int64_t)proc_file->position;
            if ((offset > 0 && base > INT64_MAX - offset) ||
                (offset < 0 && base < INT64_MIN - offset)) {
                status = K_EOVERFLOW;
            } else {
                target = base + offset;
                status = target < 0 || (uint64_t)target > limit ? K_EINVAL :
                         K_OK;
                if (status == K_OK) proc_file->position = (uint64_t)target;
            }
        } else if (whence == 2U) {
            base = limit > (uint64_t)INT64_MAX ? INT64_MAX : (int64_t)limit;
            if ((offset > 0 && base > INT64_MAX - offset) ||
                (offset < 0 && base < INT64_MIN - offset)) {
                status = K_EOVERFLOW;
            } else {
                target = base + offset;
                status = target < 0 || (uint64_t)target > limit ? K_EINVAL :
                         K_OK;
                if (status == K_OK) proc_file->position = (uint64_t)target;
            }
        } else {
            status = K_EINVAL;
        }
        position = proc_file->position;
        spinlock_unlock(&proc_file->lock);
        linux_proc_file_put(proc_file);
        return status == K_OK ? (int64_t)position : status;
    }
    uint32_t native_whence = whence <= 2U ? whence : UINT32_MAX;
    status = vfs_seek(entry.file, offset, native_whence, &position);
    linux_fd_entry_put(&entry);
    return status == K_OK ? (int64_t)position : status;
}

static int64_t linux_fsync(process_t *process, int64_t descriptor) {
    linux_fd_entry_t entry;
    kstatus_t status = linux_fd_lookup(process, descriptor, &entry);
    if (status != K_OK) return status;
    if (entry.kind == LINUX_FD_CONSOLE) status = K_OK;
    else if (entry.kind == LINUX_FD_NULL ||
             entry.kind == LINUX_FD_NVIDIA || entry.kind == LINUX_FD_WINDOW ||
             entry.kind == LINUX_FD_PIPE || entry.kind == LINUX_FD_EPOLL ||
             entry.kind == LINUX_FD_EVENTFD ||
             entry.kind == LINUX_FD_UNIX_SOCKET)
        status = K_EBADF;
    else if (entry.kind == LINUX_FD_PROC) status = K_OK;
    else status = vfs_fsync(entry.file);
    linux_fd_entry_put(&entry);
    return status;
}

static int64_t linux_ftruncate(process_t *process, int64_t descriptor,
                               uint64_t length) {
    linux_fd_entry_t entry;
    kstatus_t status = linux_fd_lookup(process, descriptor, &entry);
    if (status != K_OK) {
        (void)kernel_serial_printf_serial_only(
            "OS_FTRUNCATE_LOOKUP_FAIL pid=%llu fd=%lld bytes=%llu status=%d\r\n",
            (unsigned long long)process->pid, (long long)descriptor,
            (unsigned long long)length, (int)status);
        return status;
    }
    status = entry.kind == LINUX_FD_NULL || entry.kind == LINUX_FD_CONSOLE ||
             entry.kind == LINUX_FD_NVIDIA ||
             entry.kind == LINUX_FD_WINDOW || entry.kind == LINUX_FD_PIPE ||
             entry.kind == LINUX_FD_EPOLL || entry.kind == LINUX_FD_EVENTFD ||
             entry.kind == LINUX_FD_UNIX_SOCKET ||
             entry.kind == LINUX_FD_PROC ?
             K_EINVAL :
             vfs_truncate_kernel(entry.file, length);
    if (status != K_OK) {
        (void)kernel_serial_printf_serial_only(
            "OS_FTRUNCATE_FAIL pid=%llu fd=%lld kind=%u bytes=%llu status=%d\r\n",
            (unsigned long long)process->pid, (long long)descriptor,
            (unsigned)entry.kind, (unsigned long long)length, (int)status);
    }
    linux_fd_entry_put(&entry);
    return status;
}

typedef struct linux_lock_wait {
    vnode_t *node;
    unsigned sequence;
} linux_lock_wait_t;

/* 等待谓词在调度器/队列锁下运行。此处只读取 atomic 代数：获取 fd 或记录锁会反转通知锁序。 */
static bool linux_record_lock_changed(void *context) {
    linux_lock_wait_t *wait = context;
    return atomic_load_explicit(&wait->node->record_lock_sequence,
                                memory_order_acquire) != wait->sequence;
}

static int64_t linux_fcntl_lock(process_t *process, int64_t descriptor,
                                const linux_fd_entry_t *entry,
                                uint32_t command, uint64_t pointer) {
    linux_flock_t lock;
    _Static_assert(sizeof(linux_flock_t) == 32, "x86-64 flock ABI");
    kstatus_t status = copy_from_user(&lock, (const void __user *)(uintptr_t)pointer,
                                      sizeof(lock));
    if (status != K_OK) return linux_copy_status(status);
    if (entry->kind != LINUX_FD_FILE || !entry->file || !entry->file->vnode)
        return K_EBADF;
    vnode_t *node = entry->file->vnode;
    if ((node->mode & 0170000U) != 0100000U) return K_EBADF;
    if (lock.type != LINUX_F_RDLCK && lock.type != LINUX_F_WRLCK &&
        lock.type != LINUX_F_UNLCK) return K_EINVAL;
    if (command == LINUX_F_GETLK && lock.type == LINUX_F_UNLCK) return K_EINVAL;
    if (command != LINUX_F_GETLK &&
        ((lock.type == LINUX_F_RDLCK && !(entry->file->flags & VFS_OPEN_READ)) ||
         (lock.type == LINUX_F_WRLCK && !(entry->file->flags & VFS_OPEN_WRITE))))
        return K_EBADF;
    uint64_t base = 0;
    if (lock.whence == 1) {
        status = vfs_seek(entry->file, 0, 1, &base);
        if (status != K_OK) return status;
    } else if (lock.whence == 2) {
        os_file_info_t info;
        status = vfs_file_stat(entry->file, &info);
        if (status != K_OK) return status;
        base = info.size;
    } else if (lock.whence != 0) return K_EINVAL;
    if (base > INT64_MAX) return K_EOVERFLOW;
    int64_t first;
    if (__builtin_add_overflow((int64_t)base, lock.start, &first)) return K_EOVERFLOW;
    if (first < 0) return K_EINVAL;
    int64_t last = INT64_MAX;
    if (lock.length > 0) {
        if (__builtin_add_overflow(first, lock.length - 1, &last)) return K_EOVERFLOW;
    } else if (lock.length < 0) {
        last = first - 1;
        if (__builtin_add_overflow(first, lock.length, &first)) return K_EOVERFLOW;
        if (first < 0) return K_EINVAL;
    }
    file_lock_range_t request = {process->pid, first, last, lock.type};
    for (;;) {
        spinlock_lock(&process->linux_fds.lock);
        const linux_fd_entry_t *current = &process->linux_fds.entries[(uint32_t)descriptor];
        if (current->kind != LINUX_FD_FILE || current->file != entry->file ||
            current->description != entry->description) {
            spinlock_unlock(&process->linux_fds.lock);
            return K_EBADF;
        }
        linux_lock_wait_t wait = {node, atomic_load_explicit(
            &node->record_lock_sequence, memory_order_acquire)};
        if (command == LINUX_F_GETLK) {
            file_lock_range_t conflict;
            status = file_lock_query(node, &request, &conflict);
            spinlock_unlock(&process->linux_fds.lock);
            if (status != K_OK) return status;
            lock.type = conflict.type;
            if (conflict.type != LINUX_F_UNLCK) {
                lock.whence = 0;
                lock.start = conflict.first;
                lock.length = conflict.last == INT64_MAX ? 0 : conflict.last - conflict.first + 1;
                lock.pid = (int32_t)conflict.owner;
            }
            return linux_copy_status(copy_to_user((void __user *)(uintptr_t)pointer,
                                                   &lock, sizeof(lock)));
        }
        status = file_lock_set(node, &request);
        uint32_t ticket = UINT32_MAX;
        if (status == K_EAGAIN && command == LINUX_F_SETLKW) {
            kstatus_t wait_status = file_lock_begin_wait(node, &request, &ticket);
            if (wait_status != K_OK) status = wait_status;
        }
        spinlock_unlock(&process->linux_fds.lock);
        if (status == K_OK && linux_record_lock_changed(&wait))
            wake_all(&node->record_lock_waitq);
        if (status != K_EAGAIN || command != LINUX_F_SETLKW) return status;
        status = wait_on_queue(&node->record_lock_waitq, linux_record_lock_changed,
                               &wait, OS_WAIT_INFINITE);
        file_lock_end_wait(ticket);
        if (status != K_OK) return status;
    }
}

static int64_t linux_fcntl(process_t *process, int64_t descriptor,
                           uint32_t command, uint64_t argument) {
    linux_fd_entry_t entry = {0};
    kstatus_t status = linux_fd_lookup(process, descriptor, &entry);
    if (status != K_OK) return status;
    int64_t result = K_EINVAL;
    switch (command) {
    case LINUX_F_GETLK:
    case LINUX_F_SETLK:
    case LINUX_F_SETLKW:
        result = linux_fcntl_lock(process, descriptor, &entry, command, argument);
        break;
    case LINUX_F_GETFD:
        result = (entry.flags & LINUX_O_CLOEXEC) != 0U ? LINUX_FD_CLOEXEC : 0;
        break;
    case LINUX_F_SETFD:
        result = linux_fd_set_flags(process, descriptor,
                                    (argument & LINUX_FD_CLOEXEC) != 0U ?
                                    LINUX_O_CLOEXEC : 0U, LINUX_O_CLOEXEC);
        break;
    case LINUX_F_GETFL:
        result = entry.flags & (LINUX_O_WRONLY | LINUX_O_RDWR |
                                LINUX_O_APPEND | LINUX_O_NONBLOCK |
                                LINUX_O_DIRECTORY);
        break;
    case LINUX_F_SETFL:
        result = linux_fd_set_flags(process, descriptor, (uint32_t)argument,
                                    LINUX_O_APPEND | LINUX_O_NONBLOCK);
        break;
    case LINUX_F_DUPFD:
        result = argument >= OS_LINUX_FD_LIMIT ? K_EINVAL :
                 linux_dup_min(process, descriptor, (uint32_t)argument, 0U);
        break;
    case LINUX_F_DUPFD_CLOEXEC:
        result = argument >= OS_LINUX_FD_LIMIT ? K_EINVAL :
                 linux_dup_min(process, descriptor, (uint32_t)argument,
                               LINUX_FD_CLOEXEC);
        break;
    default:
        break;
    }
    linux_fd_entry_put(&entry);
    return result;
}

static int64_t linux_mmap(process_t *process, uint64_t requested_address,
                          uint64_t length, uint64_t protection,
                          uint64_t flags, int64_t descriptor,
                          uint64_t offset) {
    /* glibc 会直接转发 fd 参数寄存器而不清除高位。Linux fget 接收 unsigned int，
     * 而非整个寄存器。保留低 32 位描述符，并通过常规查找拒绝负数/无效 fd，
     * 不要因为无害的高位而拒绝请求。 */
    descriptor = (int64_t)(int32_t)descriptor;
    vm_object_t *object = 0;
    linux_fd_entry_t entry = {0};
    vaddr_t address;
    uint64_t map_size;
    uint32_t vm_prot = VM_PROT_USER;
    uint32_t vm_flags;
    kstatus_t status;
    if (length == 0U || length > (uint64_t)SIZE_MAX ||
        length > UINT64_MAX - (PAGE_SIZE - 1ULL) ||
        (offset & (PAGE_SIZE - 1ULL)) != 0U) return K_EINVAL;
    map_size = (length + PAGE_SIZE - 1ULL) & ~(PAGE_SIZE - 1ULL);
    if ((protection & ~(uint64_t)(LINUX_PROT_READ | LINUX_PROT_WRITE |
                                  LINUX_PROT_EXEC)) != 0U ||
        (flags & ~(uint64_t)(LINUX_MAP_SHARED | LINUX_MAP_PRIVATE |
                             LINUX_MAP_FIXED | LINUX_MAP_ANONYMOUS |
                             LINUX_MAP_GROWSDOWN | LINUX_MAP_DENYWRITE |
                             LINUX_MAP_STACK | LINUX_MAP_FIXED_NOREPLACE |
                             LINUX_MAP_32BIT | LINUX_MAP_NORESERVE)) != 0U ||
        ((flags & LINUX_MAP_SHARED) != 0U &&
         (flags & LINUX_MAP_PRIVATE) != 0U) ||
        (flags & (LINUX_MAP_SHARED | LINUX_MAP_PRIVATE)) == 0U) {
        return K_EINVAL;
    }
    if ((protection & LINUX_PROT_READ) != 0U) vm_prot |= VM_PROT_READ;
    if ((protection & LINUX_PROT_WRITE) != 0U) vm_prot |= VM_PROT_WRITE;
    if ((protection & LINUX_PROT_EXEC) != 0U) vm_prot |= VM_PROT_EXEC;
    vm_flags = (flags & LINUX_MAP_SHARED) != 0U ? VM_MAP_SHARED : VM_MAP_PRIVATE;
    if ((flags & (LINUX_MAP_GROWSDOWN | LINUX_MAP_STACK)) != 0U) {
        vm_flags |= VM_MAP_STACK;
    }
    /* V8 初始化 JIT 代码区域时会使用 Linux 风格的 RWX 转换。
     * 此例外仅适用于 Linux 兼容映射；System 原生 VM 映射仍遵循 W^X。 */
    vm_flags |= VM_MAP_ALLOW_RWX;
    address = (vaddr_t)requested_address;
    /* 将稀缺的 MAP_32BIT 区域留给确实需要它的调用方。普通无提示分配（包括 V8/PartitionAlloc 的稀疏预留）
     * 不得在 pthread 栈分配前耗尽整个低地址区域。显式提示和固定加载器/设备映射仍使用指定地址。 */
    if (address == 0U &&
        (flags & (LINUX_MAP_32BIT | LINUX_MAP_FIXED |
                  LINUX_MAP_FIXED_NOREPLACE)) == 0U)
        address = 0x100000000ULL;
    /* 匿名 VM 对象本就按需分配物理页。NORESERVE 无需提前预留；FIXED 会忽略 Linux MAP_32BIT。 */
    if ((flags & LINUX_MAP_32BIT) != 0U &&
        (flags & (LINUX_MAP_FIXED | LINUX_MAP_FIXED_NOREPLACE)) == 0U)
        vm_flags |= VM_MAP_LOW32;
    if ((flags & LINUX_MAP_FIXED_NOREPLACE) != 0U) {
        if ((flags & LINUX_MAP_FIXED) != 0U) return K_EINVAL;
    }
    if ((flags & (LINUX_MAP_FIXED | LINUX_MAP_FIXED_NOREPLACE)) != 0U) {
        if (address == 0U || (address & (PAGE_SIZE - 1ULL)) != 0U ||
            !x86_is_canonical(address) || address > X86_64_USER_TOP ||
            map_size > X86_64_USER_TOP + 1ULL - address) return K_EINVAL;
        vm_flags |= VM_MAP_FIXED;
    }
    if ((flags & LINUX_MAP_ANONYMOUS) == 0U) {
        if (descriptor < 0 || offset > UINT64_MAX - map_size) return K_EBADF;
        status = linux_fd_lookup(process, descriptor, &entry);
        if (status != K_OK) return status;
        if (entry.kind == LINUX_FD_NVIDIA) {
            nv_frontend_mapping_t mapping = {0};
            if ((flags & LINUX_MAP_SHARED) == 0U) {
                linux_fd_entry_put(&entry);
                return K_EINVAL;
            }
            status = nv_frontend_mmap(
                (nv_frontend_file_t *)entry.private_data, offset, map_size,
                vm_prot, &mapping);
            linux_fd_entry_put(&entry);
            if (status != K_OK) return status;
            if (mapping.physical_pages != 0 || mapping.page_count != 0U) {
                status = vm_object_create_page_array(
                    mapping.physical_pages, mapping.page_count,
                    mapping.cache_mode, mapping.cookie,
                    nv_frontend_mapping_release, &object);
            } else {
                status = vm_object_create_device(
                    mapping.physical, mapping.length, mapping.cache_mode,
                    mapping.cookie, nv_frontend_mapping_release, &object);
            }
            if (status != K_OK) {
                nv_frontend_mapping_release(mapping.cookie);
                return status;
            }
        } else if (entry.kind != LINUX_FD_FILE || entry.file == 0 ||
            entry.file->vnode == 0) {
            linux_fd_entry_put(&entry);
            return K_EBADF;
        } else {
            uint64_t mapping_size = entry.file->vnode->size;
            if (mapping_size < offset + map_size)
                mapping_size = offset + map_size;
            status = vm_object_create_file(entry.file->vnode,
                                           vfs_vm_file_ops(), mapping_size,
                                           offset, (size_t)map_size, &object);
            linux_fd_entry_put(&entry);
            if (status != K_OK) return status;
        }
    }
    vm_space_t *space = process_vm_reference(process);
    status = (flags & LINUX_MAP_FIXED) != 0U ?
        vm_map_object_replace(space, object, &address, 0U,
                               (size_t)map_size, vm_prot, vm_flags) :
        vm_map_object(space, object, &address, 0U,
                       (size_t)map_size, vm_prot, vm_flags);
    vm_space_put(space);
    if (status == K_EBUSY && (flags & LINUX_MAP_FIXED_NOREPLACE) != 0U)
        status = LINUX_EEXIST;
    if (object != 0) vm_object_put(object);
#if OS_DEBUG_SERIAL && OS_LINUX_MAP_DIAGNOSTICS
    if (status == K_OK && protection == 0U &&
        map_size >= 0x100000000ULL) {
        linux_partition_diag_record(address, map_size);
    }
    if (g_linux_partition_mmap_debug_count < 128U &&
        (linux_partition_diag_range(requested_address, map_size) ||
         (status == K_OK && linux_partition_diag_range(address, map_size)))) {
        kernel_serial_printf_serial_only(
            "OS_LINUX_MMAP_PARTITION req=%llx len=%llx prot=%llx flags=%llx ret=%llx status=%lld\r\n",
            (unsigned long long)requested_address,
            (unsigned long long)map_size,
            (unsigned long long)protection,
            (unsigned long long)flags,
            (unsigned long long)address,
            (long long)status);
        ++g_linux_partition_mmap_debug_count;
    }
    if (g_linux_high_mmap_debug_count < 512U && status == K_OK &&
        address >= 0x100000000000ULL && map_size <= 0x10000000ULL) {
        kernel_serial_printf_serial_only(
            "OS_LINUX_MMAP_HIGH req=%llx len=%llx prot=%llx flags=%llx ret=%llx status=%lld\r\n",
            (unsigned long long)requested_address,
            (unsigned long long)map_size,
            (unsigned long long)protection,
            (unsigned long long)flags,
            (unsigned long long)address,
            (long long)status);
        ++g_linux_high_mmap_debug_count;
    }
    if (g_linux_mmap_debug_count < 32U &&
        (requested_address < 0x1000000ULL || map_size >= (1ULL << 30) ||
         status != K_OK)) {
        kernel_serial_printf_serial_only(
            "OS_LINUX_MMAP req=%llx len=%llx prot=%llx flags=%llx ret=%llx status=%lld\r\n",
            (unsigned long long)requested_address,
            (unsigned long long)map_size,
            (unsigned long long)protection,
            (unsigned long long)flags,
            (unsigned long long)address,
            (long long)status);
        ++g_linux_mmap_debug_count;
    }
#endif
    return status == K_OK ? (int64_t)address : status;
}

/*
 * System 管理窗口服务器，但 Linux 兼容模式的桌面客户端仍必须能创建自己的窗口。
 * 此处仅作为窄适配层：窗口分配、映射、所有权和拆除仍由现有 Window Server 操作完成，
 * 返回的整数仅是 Linux fd。
 */
static int64_t linux_window_create(process_t *process,
                                   uint64_t arguments_pointer) {
    linux_window_create_t arguments;
    window_server_window_t *window = 0;
    vm_space_t *space = 0;
    bool mapped = false;
    uint64_t mapped_address = 0U;
    uint64_t buffer_size = 0U;
    int64_t descriptor = -1;
    kstatus_t status;

    if (process == 0 || !linux_user_pointer(arguments_pointer,
                                             sizeof(arguments))) {
        return LINUX_EFAULT;
    }
    status = copy_from_user(
        &arguments, (const void __user *)(uintptr_t)arguments_pointer,
        sizeof(arguments));
    if (status != K_OK) return linux_copy_status(status);
#if OS_DEBUG_SERIAL
    if (g_linux_window_create_debug_count < 4U) {
        kernel_serial_printf_serial_only(
            "OS_LINUX_WINDOW_CREATE_BEGIN pid=%llu x=%d y=%d w=%u h=%u\r\n",
            (unsigned long long)(process != 0 ? process->pid : 0U),
            arguments.x, arguments.y, arguments.width, arguments.height);
    }
#endif
    if (arguments.x < 0 || arguments.y < 0 || arguments.width == 0U ||
        arguments.height == 0U ||
        (arguments.flags & ~(OS_WINDOW_VISIBLE | OS_WINDOW_RESIZABLE |
                             OS_WINDOW_CLIENT_DECORATIONS |
                             OS_WINDOW_PREMULTIPLIED_ALPHA |
                             OS_WINDOW_STANDARD_CAPTION_CONTROLS |
                             OS_WINDOW_SURFACE_EXCHANGE |
                             OS_WINDOW_EXPLICIT_MOVE_RESIZE)) != 0U ||
        arguments.reserved != 0U || arguments.buffer_size != 0U ||
        arguments.identifier != 0U ||
        (arguments.address != 0U &&
         ((arguments.address & (PAGE_SIZE - 1U)) != 0U ||
          !x86_is_canonical(arguments.address) ||
          arguments.address > X86_64_USER_TOP))) {
        return K_EINVAL;
    }

    status = window_server_create(process, arguments.x, arguments.y,
                                  arguments.width, arguments.height,
                                  arguments.flags, arguments.background,
                                  arguments.title, &window);
    if (status != K_OK) {
#if OS_DEBUG_SERIAL
        if (g_linux_window_create_debug_count < 4U) {
            kernel_serial_printf_serial_only(
                "OS_LINUX_WINDOW_CREATE_FAIL pid=%llu stage=create status=%d\r\n",
                (unsigned long long)(process != 0 ? process->pid : 0U),
                (int)status);
        }
#endif
        return status;
    }
    buffer_size = window_server_window_buffer_size(window);
    space = process_vm_reference(process);
    status = window_server_map_space(window, space, arguments.address,
                               &mapped_address);
    if (status != K_OK) goto fail;
    mapped = true;
    status = window_server_set_owner_address(window, mapped_address);
    if (status != K_OK) goto fail;
    if ((arguments.flags & OS_WINDOW_VISIBLE) != 0U) {
        status = window_server_set(window, arguments.x, arguments.y, 1U);
        if (status != K_OK) goto fail;
    }

    /* 为 fd 表单独持有一个引用；即使同进程线程同时关闭 fd，本地引用也要保留到发布完成。 */
    object_get(window);
    descriptor = linux_fd_install(
        process, -1, 0, window, LINUX_FD_WINDOW,
        LINUX_O_NONBLOCK | LINUX_O_CLOEXEC);
    if (descriptor < 0) {
        window_server_put(window);
        status = (kstatus_t)descriptor;
        goto fail;
    }

    arguments.address = mapped_address;
    arguments.buffer_size = buffer_size;
    arguments.identifier = window_server_window_identifier(window);
    status = copy_to_user(
        (void __user *)(uintptr_t)arguments_pointer, &arguments,
        sizeof(arguments));
    if (status != K_OK) {
        status = (kstatus_t)linux_copy_status(status);
        goto fail;
    }
#if OS_DEBUG_SERIAL
    if (g_linux_window_create_debug_count < 4U) {
        kernel_serial_printf_serial_only(
            "OS_LINUX_WINDOW_CREATE_OK pid=%llu fd=%lld id=%u addr=%llx size=%llu\r\n",
            (unsigned long long)(process != 0 ? process->pid : 0U),
            (long long)descriptor, arguments.identifier,
            (unsigned long long)arguments.address,
            (unsigned long long)arguments.buffer_size);
        g_linux_window_create_debug_count++;
    }
#endif
    window_server_put(window);
    vm_space_put(space);
    return descriptor;

fail:
    if (mapped) (void)vm_unmap(space, mapped_address, (size_t)buffer_size);
    (void)window_server_close_owned(process, window);
    if (descriptor >= 0) (void)linux_close(process, descriptor);
    window_server_put(window);
    vm_space_put(space);
    return status;
}

static kstatus_t linux_window_ioctl(process_t *process,
                                    window_server_window_t *window,
                                    uint32_t request, void *argument,
                                    uint32_t argument_size) {
    linux_window_command_t *command = (linux_window_command_t *)argument;
    if (process == 0 || window == 0 || command == 0 ||
        request != LINUX_WINDOW_IOCTL_COMMIT ||
        LINUX_WINDOW_IOCTL_NR(request) != LINUX_WINDOW_IOCTL_BASE ||
        argument_size != sizeof(*command) || command->reserved != 0U ||
        command->reserved2 != 0U) return LINUX_ENOTTY;
    switch (command->operation) {
    case LINUX_WINDOW_OPERATION_PRESENT:
        {
            kstatus_t status = window_server_present(
                process, window, command->address, command->length,
                command->stride, command->x, command->y,
                command->width, command->height);
#if OS_DEBUG_SERIAL && OS_ENABLE_FRAME_TRACE
            if (g_linux_window_present_debug_count < 4U) {
                kernel_serial_printf_serial_only(
                    "OS_LINUX_WINDOW_PRESENT status=%d addr=%llx len=%llu stride=%u w=%u h=%u\r\n",
                    (int)status, (unsigned long long)command->address,
                    (unsigned long long)command->length, command->stride,
                                     command->width, command->height);
                g_linux_window_present_debug_count++;
            }
#endif
            return status;
        }
    case LINUX_WINDOW_OPERATION_MOVE:
        if (command->width != 0U || command->height != 0U ||
            command->stride != 0U || command->address != 0U ||
            command->length != 0U) return K_EINVAL;
        return window_server_move(process, window, command->x, command->y);
    case LINUX_WINDOW_OPERATION_RESIZE:
        if (command->x != 0 || command->y != 0 || command->stride != 0U ||
            command->address != 0U || command->length != 0U ||
            command->width == 0U || command->height == 0U) return K_EINVAL;
        return window_server_resize(process, window, command->width,
                                    command->height);
    case LINUX_WINDOW_OPERATION_CLOSE:
        if (command->x != 0 || command->y != 0 || command->width != 0U ||
            command->height != 0U || command->stride != 0U ||
            command->address != 0U || command->length != 0U) return K_EINVAL;
        return window_server_close_owned(process, window);
    case LINUX_WINDOW_OPERATION_MINIMIZE:
        if (command->x != 0 || command->y != 0 || command->width > 1U ||
            command->height != 0U || command->stride != 0U ||
            command->address != 0U || command->length != 0U) return K_EINVAL;
        return window_server_set_minimized_owned(process, window, command->width);
    case LINUX_WINDOW_OPERATION_CAPTURE:
        if (command->x != 0 || command->y != 0 || command->width > 1U ||
            command->height != 0U || command->stride != 0U ||
            command->address != 0U || command->length != 0U) return K_EINVAL;
        return window_server_capture_owned(process, window, command->width);
    case LINUX_WINDOW_OPERATION_MAXIMIZE:
        if (command->x != 0 || command->y != 0 || command->width > 1U ||
            command->height != 0U || command->stride != 0U ||
            command->address != 0U || command->length != 0U) return K_EINVAL;
        return window_server_set_maximized_owned(process, window, command->width);
    case LINUX_WINDOW_OPERATION_FULLSCREEN:
        if (command->x != 0 || command->y != 0 || command->width > 1U ||
            command->height != 0U || command->stride != 0U ||
            command->address != 0U || command->length != 0U) return K_EINVAL;
        return window_server_set_fullscreen_owned(process, window, command->width);
    case LINUX_WINDOW_OPERATION_GEOMETRY:
        if (command->x != 0 || command->y != 0 || command->width != 0U ||
            command->height != 0U || command->stride != 0U ||
            command->address != 0U || command->length != 0U) return K_EINVAL;
        return window_server_geometry_owned(process, window,
            &command->x, &command->y, &command->width, &command->height, &command->stride);
    case LINUX_WINDOW_OPERATION_INTERACTIVE:
        if (command->x != 0 || command->y != 0 || command->height != 0U ||
            command->stride != 0U || command->address != 0U ||
            command->length != 0U) return K_EINVAL;
        return window_server_begin_interactive_owned(process, window, command->width);
    case LINUX_WINDOW_OPERATION_DECORATIONS:
        if (command->x != 0 || command->y != 0 || command->width > 1U ||
            command->height != 0U || command->stride != 0U ||
            command->address != 0U || command->length != 0U) return K_EINVAL;
        return window_server_set_decorations_owned(process, window, command->width);
    case LINUX_WINDOW_OPERATION_TITLE: {
        char title[32];
        if (command->x != 0 || command->y != 0 || command->width != 0U ||
            command->height != 0U || command->stride != 0U ||
            command->length != sizeof(title)) return K_EINVAL;
        if (!linux_user_pointer(command->address, sizeof(title))) return LINUX_EFAULT;
        kstatus_t status = copy_from_user(title,
            (const void __user *)(uintptr_t)command->address, sizeof(title));
        if (status != K_OK) return linux_copy_status(status);
        return window_server_title_owned(process,
            window_server_window_identifier(window), title);
    }
    case LINUX_WINDOW_OPERATION_VISIBILITY:
        if (command->x != 0 || command->y != 0 || command->width > 1U ||
            command->height > LINUX_WINDOW_VISIBILITY_INACTIVE ||
            (command->width == 0U && command->height != 0U) || command->stride != 0U ||
            command->address != 0U || command->length != 0U) return K_EINVAL;
        return window_server_set_visibility_owned(process, window,
            command->width | (command->height != 0U ? WINDOW_SERVER_SHOW_INACTIVE : 0U));
    default:
        return K_EINVAL;
    }
}

static int64_t linux_fionread(const linux_fd_entry_t *entry,
                             uint64_t argument_pointer) {
    pipe_endpoint_t *endpoint = 0;
    linux_unix_channel_t *channel = 0;
    uint64_t bytes = 0U;
    kstatus_t status;
    if (entry->kind == LINUX_FD_PIPE) {
        endpoint = (pipe_endpoint_t *)entry->private_data;
        status = pipe_pending_bytes(endpoint, &bytes);
    } else if (entry->kind == LINUX_FD_UNIX_SOCKET) {
        linux_unix_socket_t *socket = entry->private_data;
        linux_unix_socket_pin_io(socket, false, &endpoint, &channel);
        status = endpoint ? pipe_pending_bytes(endpoint, &bytes) : K_OK;
        linux_unix_socket_release_endpoint(&endpoint);
        linux_unix_socket_release_channel(&channel);
    } else {
        return LINUX_ENOTTY;
    }
    if (status != K_OK) return status;
    if (!linux_user_pointer(argument_pointer, sizeof(int32_t))) return LINUX_EFAULT;
    int32_t pending = bytes > INT32_MAX ? INT32_MAX : (int32_t)bytes;
    return copy_to_user((void __user *)(uintptr_t)argument_pointer,
                        &pending, sizeof(pending)) == K_OK ? K_OK : LINUX_EFAULT;
}

static kstatus_t linux_window_surface_ioctl(process_t *process,
    window_server_window_t *window, uint64_t argument_pointer) {
    os_surface_command_t command;
    if (!linux_user_pointer(argument_pointer, sizeof(command))) return LINUX_EFAULT;
    kstatus_t status = copy_from_user(&command,
        (const void __user *)(uintptr_t)argument_pointer, sizeof(command));
    if (status != K_OK) return linux_copy_status(status);
    if (command.reserved || command.frame.reserved) return K_EINVAL;
    switch (command.operation) {
    case OS_SURFACE_BEGIN:
        if (command.frame.token || command.frame.address || command.frame.capacity ||
            command.frame.width || command.frame.height || command.frame.stride ||
            command.frame.damage_x || command.frame.damage_y ||
            command.frame.damage_width || command.frame.damage_height)
            return K_EINVAL;
        status = window_server_surface_begin(process, window, &command.frame);
        if (status != K_OK) return status;
        status = copy_to_user((void __user *)(uintptr_t)argument_pointer,
                              &command, sizeof(command));
        if (status != K_OK) {
            /* 客户端从未收到 token：返回已获取的后缓冲区时若发生 EFAULT，不得留下租约。 */
            (void)window_server_surface_end(process, window, &command.frame, false);
        }
        return status == K_OK ? K_OK : linux_copy_status(status);
    case OS_SURFACE_COMMIT:
    case OS_SURFACE_CANCEL:
        return window_server_surface_end(process, window, &command.frame,
                                          command.operation == OS_SURFACE_COMMIT);
    default:
        return K_EINVAL;
    }
}

static int64_t linux_ioctl(process_t *process, int64_t descriptor,
                           uint32_t request, uint64_t argument_pointer) {
    linux_fd_entry_t entry;
    uint32_t argument_size = NV_FRONTEND_IOCTL_SIZE(request);
    void *argument;
    kstatus_t status;
    status = linux_fd_lookup(process, descriptor, &entry);
    if (status != K_OK) return status;
    if (request == 0x541bU) { /* Linux FIONREAD：旧式 ioctl，不编码数据大小。 */
        int64_t result = linux_fionread(&entry, argument_pointer);
        linux_fd_entry_put(&entry);
        return result;
    }
    if (entry.kind == LINUX_FD_WINDOW) {
        if (request == LINUX_WINDOW_IOCTL_SURFACE) {
            status = linux_window_surface_ioctl(process,
                (window_server_window_t *)entry.private_data, argument_pointer);
            linux_fd_entry_put(&entry);
            return status;
        }
        argument_size = LINUX_WINDOW_IOCTL_SIZE(request);
        if (request != LINUX_WINDOW_IOCTL_COMMIT ||
            argument_size != sizeof(linux_window_command_t)) {
            linux_fd_entry_put(&entry);
            return LINUX_ENOTTY;
        }
        if (!linux_user_pointer(argument_pointer, argument_size)) {
            linux_fd_entry_put(&entry);
            return LINUX_EFAULT;
        }
        linux_window_command_t command;
        status = copy_from_user(
            &command, (const void __user *)(uintptr_t)argument_pointer,
            sizeof(command));
        if (status == K_OK) {
            status = linux_window_ioctl(
                process, (window_server_window_t *)entry.private_data,
                request, &command, sizeof(command));
        }
        if (status == K_OK) {
            status = copy_to_user(
                (void __user *)(uintptr_t)argument_pointer, &command,
                sizeof(command));
        }
        linux_fd_entry_put(&entry);
        return status == K_OK ? K_OK : linux_copy_status(status);
    }
    if (entry.kind != LINUX_FD_NVIDIA || entry.private_data == 0) {
        linux_fd_entry_put(&entry);
        return LINUX_ENOTTY;
    }
    if (argument_size == 0U || argument_size > 16384U ||
        !linux_user_pointer(argument_pointer, argument_size)) {
        linux_fd_entry_put(&entry);
        return argument_size == 0U || argument_size > 16384U ?
               K_EINVAL : LINUX_EFAULT;
    }
    argument = kmalloc(argument_size, 0U);
    if (argument == 0) {
        linux_fd_entry_put(&entry);
        return K_ENOMEM;
    }
    status = copy_from_user(argument,
                            (const void __user *)(uintptr_t)argument_pointer,
                            argument_size);
    if (status == K_OK) {
        if (NV_FRONTEND_IOCTL_NR(request) == NV_FRONTEND_ESC_REGISTER_FD) {
            nv_frontend_register_fd_t *registration =
                (nv_frontend_register_fd_t *)argument;
            linux_fd_entry_t control_entry = {0};
            if (argument_size != sizeof(*registration) ||
                registration->ctl_fd < 0 ||
                !linux_fd_valid((int64_t)registration->ctl_fd)) {
                status = K_EINVAL;
            } else {
                status = linux_fd_lookup(
                    process, (int64_t)registration->ctl_fd, &control_entry);
                if (status == K_OK) {
                    if (control_entry.kind != LINUX_FD_NVIDIA ||
                        control_entry.private_data == 0) {
                        status = K_EBADF;
                    } else {
                        status = nv_frontend_register_fd(
                            (nv_frontend_file_t *)entry.private_data,
                            (nv_frontend_file_t *)control_entry.private_data);
                    }
                    linux_fd_entry_put(&control_entry);
                }
            }
        } else {
            status = nv_frontend_ioctl(
                (nv_frontend_file_t *)entry.private_data, request, argument,
                argument_size);
        }
    }
    if (status == K_OK) {
        status = copy_to_user((void __user *)(uintptr_t)argument_pointer,
                              argument, argument_size);
    }
    kfree(argument);
    linux_fd_entry_put(&entry);
    return status == K_ENOSYS ? LINUX_ENOTTY :
           (status == K_OK ? K_OK : linux_copy_status(status));
}

static int64_t linux_brk(process_t *process, uint64_t requested) {
    uint64_t old_brk;
    uint64_t base;
    uint64_t old_end;
    uint64_t new_end;
    vaddr_t address;
    if (process == 0) return K_EPERM;
    process_thread_lock(process);
    if (process->linux_brk_base == 0U) {
        process->linux_brk_base = 0x60000000ULL;
        process->linux_brk = process->linux_brk_base;
    }
    old_brk = process->linux_brk;
    base = process->linux_brk_base;
    if (requested < base || requested == old_brk ||
        requested > UINT64_MAX - (PAGE_SIZE - 1ULL)) {
        process_thread_unlock(process);
        return (int64_t)old_brk;
    }
    /* 元数据和 VM 必须描述同一映像。映射期间保留其快照，之后若发生 exit/exec/brk，则拒绝过期发布。 */
    vm_space_t *space = process->vm;
    if (space != 0) vm_space_get(space);
    process_thread_unlock(process);
    bool mapped = false;
    old_end = (old_brk + PAGE_SIZE - 1ULL) & ~(PAGE_SIZE - 1ULL);
    new_end = (requested + PAGE_SIZE - 1ULL) & ~(PAGE_SIZE - 1ULL);
    if (new_end > old_end) {
        uint64_t amount = new_end - old_end;
        address = (vaddr_t)old_end;
        kstatus_t status = vm_map_object(space, 0, &address, 0U,
                                          (size_t)amount,
                                          VM_PROT_USER | VM_PROT_READ |
                                          VM_PROT_WRITE,
                                          VM_MAP_PRIVATE | VM_MAP_FIXED);
        if (status != K_OK) {
            vm_space_put(space);
            return (int64_t)old_brk;
        }
        mapped = true;
    }
    bool committed = false;
    process_thread_lock(process);
    if (process->vm == space && process->linux_brk_base == base &&
        process->linux_brk == old_brk &&
        atomic_load_explicit(&process->state, memory_order_acquire) == PROCESS_RUNNING) {
        process->linux_brk = (vaddr_t)requested;
        committed = true;
    }
    process_thread_unlock(process);
    if (!committed && mapped)
        (void)vm_unmap(space, (vaddr_t)old_end, (size_t)(new_end - old_end));
    vm_space_put(space);
    return (int64_t)(committed ? requested : old_brk);
}

static kstatus_t linux_timespec_timeout(uint64_t pointer, bool absolute,
                                        bool realtime, uint64_t *timeout) {
    linux_timespec_t value;
    os_timespec_t now;
    uint64_t value_ns;
    if (timeout == 0) return K_EINVAL;
    if (pointer == 0U) {
        *timeout = OS_WAIT_INFINITE;
        return K_OK;
    }
    kstatus_t status = copy_from_user(&value,
        (const void __user *)(uintptr_t)pointer, sizeof(value));
    if (status != K_OK) return status;
    if (value.seconds < 0 || value.nanoseconds < 0 ||
        value.nanoseconds >= 1000000000LL ||
        (uint64_t)value.seconds > UINT64_MAX / 1000000000ULL) return K_EINVAL;
    uint64_t seconds = (uint64_t)value.seconds;
    if (seconds > (UINT64_MAX - (uint64_t)value.nanoseconds) / 1000000000ULL) {
        value_ns = UINT64_MAX;
    } else {
        value_ns = seconds * 1000000000ULL + (uint64_t)value.nanoseconds;
    }
    if (!absolute) {
        *timeout = value_ns;
        return K_OK;
    }
    status = kernel_clock_read(realtime ? OS_CLOCK_REALTIME :
                              OS_CLOCK_MONOTONIC, &now);
    if (status != K_OK) return status;
    uint64_t now_ns = now.seconds < 0 ? 0U :
        (uint64_t)now.seconds > UINT64_MAX / 1000000000ULL ? UINT64_MAX :
        (uint64_t)now.seconds * 1000000000ULL +
            (uint64_t)now.nanoseconds;
    *timeout = value_ns <= now_ns ? 0U : value_ns - now_ns;
    return K_OK;
}

static int64_t linux_futex(process_t *process, uint64_t address,
                           uint32_t operation, uint32_t expected,
                           uint64_t timeout_pointer, uint64_t bitset) {
    uint32_t command = operation & 0x7FU;
    uint32_t modifiers = operation & ~0x7FU;
    bool private_key = (modifiers & LINUX_FUTEX_PRIVATE_FLAG) != 0U;
    int64_t result = K_EINVAL;
    bool absolute_timeout = false;
    /* 低 7 位是 futex 命令，其余位为操作修饰符。Chromium 的 Abseil 等待器使用 WAIT_BITSET，
     * 并设置 private 和 realtime 修饰符，即使等待时间为无限。 */
    if ((modifiers & ~(LINUX_FUTEX_PRIVATE_FLAG |
                       LINUX_FUTEX_CLOCK_REALTIME)) != 0U ||
        address == 0U || (address & 3U) != 0U) {
        goto done;
    }
    if (command == LINUX_FUTEX_WAIT_BITSET) {
        if (bitset == 0U) goto done;
        absolute_timeout = true;
    } else if (command != LINUX_FUTEX_WAIT &&
               command != LINUX_FUTEX_WAKE) {
        result = K_ENOSYS;
        goto done;
    }
    if (command == LINUX_FUTEX_WAIT) {
        uint64_t timeout = OS_WAIT_INFINITE;
        kstatus_t status = linux_timespec_timeout(timeout_pointer,
            absolute_timeout,
            (modifiers & LINUX_FUTEX_CLOCK_REALTIME) != 0U, &timeout);
        if (status != K_OK) {
            result = linux_copy_status(status);
            goto done;
        }
        status = futex_wait_keyed(process,
            (uint32_t __user *)(uintptr_t)address, expected, timeout,
            private_key);
        result = status;
        goto done;
    }
    if (command == LINUX_FUTEX_WAIT_BITSET) {
        uint64_t timeout = OS_WAIT_INFINITE;
        kstatus_t status = linux_timespec_timeout(timeout_pointer, true,
            (modifiers & LINUX_FUTEX_CLOCK_REALTIME) != 0U, &timeout);
        if (status != K_OK) {
            result = linux_copy_status(status);
            goto done;
        }
        result = futex_wait_keyed(process,
            (uint32_t __user *)(uintptr_t)address, expected, timeout,
            private_key);
        goto done;
    }
    if (command == LINUX_FUTEX_WAKE) {
        uint32_t woken = 0U;
        kstatus_t status = futex_wake_keyed(process,
            (uint32_t __user *)(uintptr_t)address, expected, &woken,
            private_key);
        result = status == K_OK ? (int64_t)woken : status;
        goto done;
    }
    result = K_ENOSYS;
done:
#if OS_DEBUG_SERIAL
    /* 启动期间 wake 调用极其频繁。仅采样少量 wake，其余预算留给 WAIT/其他操作，
     * 避免渲染器故障被分配器唤醒掩盖。 */
    if (g_debug_linux_futex_logs < 256U &&
        (operation != (LINUX_FUTEX_WAKE | LINUX_FUTEX_PRIVATE_FLAG) ||
         g_debug_linux_futex_logs < 32U)) {
        kernel_serial_printf_serial_only(
            "OS_LINUX_FUTEX addr=%llx op=%x expected=%x timeout=%llx result=%lld\r\n",
            (unsigned long long)address, operation, expected,
            (unsigned long long)timeout_pointer, (long long)result);
        ++g_debug_linux_futex_logs;
    }
#endif
    return result;
}

void linux_thread_exit_cleanup(struct thread *raw_thread) {
    thread_t *thread = (thread_t *)raw_thread;
    if (thread == 0 || !thread->linux_clear_tid_pending ||
        thread->linux_clear_tid == 0U) return;
    vaddr_t address = thread->linux_clear_tid;
    thread->linux_clear_tid_pending = false;
    if (put_user_u32((uint32_t __user *)(uintptr_t)address, 0U) == K_OK) {
        uint32_t ignored = 0U;
        /* Linux clear_child_tid 会唤醒共享 FUTEX_WAKE 键。glibc 的 pthread_join 使用不带
         * FUTEX_PRIVATE_FLAG 的 WAIT_BITSET；若用私有 wake，只会清除字，而已休眠的 join 仍会阻塞。 */
        (void)futex_wake_keyed(thread->process,
                         (uint32_t __user *)(uintptr_t)address, 1U, &ignored,
                         false);
    }
}

bool linux_process_is(struct process *raw_process) {
    process_t *process = (process_t *)raw_process;
    return process != 0 && process->personality ==
           OS_PROCESS_PERSONALITY_LINUX_X86_64;
}

static bool linux_string_equal(const char *left, const char *right) {
    size_t index = 0U;
    if (left == 0 || right == 0) return false;
    while (left[index] != '\0' && right[index] != '\0') {
        if (left[index] != right[index]) return false;
        ++index;
    }
    return left[index] == right[index];
}

bool linux_elf_interpreter_path(const char *path) {
    /* 系统 Linux ABI 服务与应用私有 glibc 加载器都走适配层，原生加载器仍独立。 */
    if (linux_string_equal(path, "/lib/ld-linux-system.so.1")) return true;
    if (path == 0) return false;
    static const char prefix[] = "/usr/apps/";
    for (size_t index = 0U; index < sizeof(prefix) - 1U; ++index) {
        if (path[index] != prefix[index]) return false;
    }
    const char *app = path + sizeof(prefix) - 1U;
    const char *cursor = app;
    while ((*cursor >= 'a' && *cursor <= 'z') ||
           (*cursor >= 'A' && *cursor <= 'Z') ||
           (*cursor >= '0' && *cursor <= '9') || *cursor == '-' || *cursor == '_') ++cursor;
    return cursor != app && linux_string_equal(cursor, "/lib/ld-linux-x86-64.so.2");
}

static int64_t linux_arch_prctl(thread_t *thread, uint64_t command,
                                uint64_t argument) {
    if (thread == 0) return K_EPERM;
    if (command == LINUX_ARCH_SET_FS) {
        if (!x86_is_canonical((vaddr_t)argument) ||
            argument > X86_64_USER_TOP) return K_EINVAL;
        thread->arch.fs_base = (vaddr_t)argument;
        x86_set_user_fs_base((vaddr_t)argument);
        return K_OK;
    }
    if (command == LINUX_ARCH_GET_FS) {
        return copy_to_user((void __user *)(uintptr_t)argument,
                            &thread->arch.fs_base,
                            sizeof(thread->arch.fs_base)) == K_OK ?
               K_OK : LINUX_EFAULT;
    }
    return K_ENOSYS;
}

static int64_t linux_prctl(process_t *process, uint64_t option,
                           uint64_t argument0) {
    char name[16] = {0};
    if (process == 0) return K_EPERM;
    if (option == LINUX_PR_SET_NAME) {
        if (argument0 == 0U || !linux_user_pointer(argument0, sizeof(name)) ||
            copy_from_user(name,
                           (const void __user *)(uintptr_t)argument0,
                           sizeof(name)) != K_OK) return LINUX_EFAULT;
        /* System 只有一个进程名称，没有每线程独立且供调度器查看的 comm 字段。
         * 接受 Linux 操作并验证输入字节，使 Chromium 线程初始化能继续。 */
        return K_OK;
    }
    if (option == LINUX_PR_GET_NAME) {
        if (argument0 == 0U || !linux_user_pointer(argument0, sizeof(name))) {
            return LINUX_EFAULT;
        }
        linux_copy(name, process->name,
                   sizeof(name) < sizeof(process->name) ?
                   sizeof(name) : sizeof(process->name));
        return copy_to_user((void __user *)(uintptr_t)argument0, name,
                            sizeof(name)) == K_OK ? K_OK : LINUX_EFAULT;
    }
    return K_ENOSYS;
}

static int64_t linux_clone(process_t *process, thread_t *parent,
                           uint64_t flags, uint64_t child_stack,
                           uint64_t parent_tid, uint64_t child_tid,
                           uint64_t tls, arch_trap_frame_t *frame) {
    const uint64_t required = LINUX_CLONE_VM | LINUX_CLONE_SIGHAND |
                              LINUX_CLONE_THREAD;
    if ((flags & LINUX_CLONE_VM) == 0U) {
        return syscall_process_clone_linux(flags, child_stack, parent_tid,
                                            child_tid, (uint64_t)(uintptr_t)frame);
    }
    if (process == 0 || parent == 0 || frame == 0 ||
        (flags & required) != required || child_stack == 0U ||
        child_stack > X86_64_USER_TOP ||
        (child_stack & 15U) != 0U) return K_EINVAL;
    arch_trap_frame_t child_frame = *frame;
    child_frame.rax = 0U;
    child_frame.rsp = child_stack;
    child_frame.vector = 0U;
    child_frame.error_code = 0U;
    vaddr_t child_fs = (flags & LINUX_CLONE_SETTLS) != 0U ?
                       (vaddr_t)tls : (vaddr_t)parent->arch.fs_base;
    thread_t *child = 0;
    kstatus_t status = thread_create_user_from_frame(process, &child_frame,
                                                     child_fs, &child);
    if (status != K_OK) return status;
    x86_fp_state_clone_current(&parent->arch, &child->arch);
    child->signal_mask = parent->signal_mask;
    if ((flags & LINUX_CLONE_CHILD_CLEARTID) != 0U) {
        child->linux_clear_tid = (vaddr_t)child_tid;
        child->linux_clear_tid_pending = child_tid != 0U;
    }
    if ((flags & LINUX_CLONE_CHILD_SETTID) != 0U && child_tid != 0U) {
        status = put_user_u32((uint32_t __user *)(uintptr_t)child_tid,
                              (uint32_t)child->tid);
    }
    if (status == K_OK && (flags & LINUX_CLONE_PARENT_SETTID) != 0U &&
        parent_tid != 0U) {
        status = put_user_u32((uint32_t __user *)(uintptr_t)parent_tid,
                              (uint32_t)child->tid);
    }
    if (status == K_OK) status = thread_start(child);
    int64_t result = status == K_OK ? (int64_t)child->tid : status;
    if (status != K_OK) {
        (void)thread_terminate(child, status);
    }
    object_put(child);
    return result;
}

static int64_t linux_clock_gettime(uint64_t clock_id, uint64_t pointer) {
    linux_timespec_t value;
    os_timespec_t clock_value;
    kstatus_t status;
    clock_id = (uint32_t)clock_id;
    if (clock_id == 2U || clock_id == 3U) {
        thread_t *thread = sched_current_thread();
        if (thread == 0 || thread->process == 0) return K_EPERM;
        uint64_t runtime;
        if (clock_id == 3U) {
            runtime = __atomic_load_n(&thread->sched.runtime_ns, __ATOMIC_RELAXED);
        } else {
            os_task_info_t info;
            status = process_task_query(thread->process->pid, 0U, &info);
            if (status != K_OK) return status;
            runtime = info.runtime_ns;
        }
        value.seconds = (int64_t)(runtime / 1000000000ULL);
        value.nanoseconds = (int64_t)(runtime % 1000000000ULL);
        return copy_to_user((void __user *)(uintptr_t)pointer, &value,
                            sizeof(value)) == K_OK ? K_OK : LINUX_EFAULT;
    }
    /* 单调时钟源是未经校准的 TSC，不受 NTP 频率调整。因此此处 RAW 与其时钟源/纪元相同；
     * 不得读取 realtime 或获取可调整偏移。Abseil 会在 CycleClock 校准期间、call_once 标志完成前使用 RAW。 */
    if (clock_id == 4U) clock_id = OS_LINUX_CLOCK_MONOTONIC;
    /* Linux coarse 时钟也可以使用精度更高的时钟实现。 */
    if (clock_id == 5U) clock_id = OS_LINUX_CLOCK_REALTIME;
    if (clock_id == 6U) clock_id = OS_LINUX_CLOCK_MONOTONIC;
    if (clock_id != OS_LINUX_CLOCK_REALTIME &&
        clock_id != OS_LINUX_CLOCK_MONOTONIC) {
        return K_EINVAL;
    }
    status = kernel_clock_read((uint32_t)clock_id, &clock_value);
    if (status != K_OK) return status;
    value.seconds = clock_value.seconds;
    value.nanoseconds = clock_value.nanoseconds;
    return copy_to_user((void __user *)(uintptr_t)pointer, &value,
                        sizeof(value)) == K_OK ? K_OK : LINUX_EFAULT;
}

static int64_t linux_gettimeofday(uint64_t time_pointer,
                                  uint64_t timezone_pointer) {
    if (time_pointer != 0U) {
        os_timespec_t clock_value;
        kstatus_t status = kernel_clock_read(OS_CLOCK_REALTIME, &clock_value);
        if (status != K_OK) return status;
        /* Linux x86-64 timeval 包含两个有符号 64 位字段。 */
        int64_t value[2] = {clock_value.seconds,
                           clock_value.nanoseconds / 1000};
        if (copy_to_user((void __user *)(uintptr_t)time_pointer,
                         value, sizeof(value)) != K_OK) return LINUX_EFAULT;
    }
    if (timezone_pointer != 0U) {
        /* 来宾时钟为 UTC；旧版 timezone 结构使用 int 字段。 */
        int32_t timezone[2] = {0, 0};
        if (copy_to_user((void __user *)(uintptr_t)timezone_pointer,
                         timezone, sizeof(timezone)) != K_OK) return LINUX_EFAULT;
    }
    return K_OK;
}

static int64_t linux_uname(uint64_t pointer) {
    linux_utsname_t value;
    linux_zero(&value, sizeof(value));
    linux_copy(value.sysname, "System", sizeof("System"));
    linux_copy(value.nodename, "localhost", sizeof("localhost"));
    linux_copy(value.release, "0.1", sizeof("0.1"));
    linux_copy(value.version, "System Linux ABI", sizeof("System Linux ABI"));
    linux_copy(value.machine, "x86_64", sizeof("x86_64"));
    linux_copy(value.domainname, "", 1U);
    return copy_to_user((void __user *)(uintptr_t)pointer, &value,
                        sizeof(value)) == K_OK ? K_OK : LINUX_EFAULT;
}

static int64_t linux_getcwd(process_t *process, uint64_t pointer,
                            uint64_t size) {
    char cwd[PROCESS_CWD_CAPACITY];
    size_t length = 0U;
    if (process == 0 || size > (uint64_t)SIZE_MAX) return K_EINVAL;
    kstatus_t status = process_cwd_copy(process, cwd);
    if (status != K_OK) return status;
    while (length + 1U < sizeof(cwd) && cwd[length] != '\0') ++length;
    if (length + 1U > size) return LINUX_ERANGE;
    status = copy_to_user((void __user *)(uintptr_t)pointer, cwd, length + 1U);
    return status == K_OK ? (int64_t)(length + 1U) : LINUX_EFAULT;
}

static int64_t linux_chdir(process_t *process, uint64_t pointer) {
    char path[LINUX_MAX_PATH];
    char resolved[LINUX_MAX_PATH];
    os_file_info_t info;
    kstatus_t status = linux_path_from_user(process, LINUX_AT_FDCWD,
                                            pointer, path);
    if (status != K_OK) return status;
    status = vfs_resolve_path_kernel(path, true, false, resolved);
    if (status != K_OK) return status;
    status = vfs_stat_kernel(resolved, &info);
    if (status != K_OK) return status;
    if (info.type != OS_FILE_TYPE_DIRECTORY) return K_ENOTDIR;
    return process_cwd_set(process, resolved);
}

static int64_t linux_mkdir(process_t *process, int64_t dirfd,
                          uint64_t pointer, uint64_t mode) {
    char path[LINUX_MAX_PATH];
    char resolved[LINUX_MAX_PATH];
    kstatus_t status = linux_path_from_user(process, dirfd, pointer, path);
    if (status != K_OK) return status;
    size_t length = 0U;
    while (path[length] != '\0') ++length;
    /* mkdir 允许路径以分隔符结尾；原生 VFS 接收的是叶名称。 */
    while (length > 1U && path[length - 1U] == '/') path[--length] = '\0';
    if (length == 1U && path[0] == '/') return K_EEXIST;
    const char *target = path;
    if (vfs_path_needs_resolution(path)) {
        status = vfs_resolve_path_kernel(path, false, true, resolved);
        if (status != K_OK) return status;
        if (resolved[0] == '/' && resolved[1] == '\0') return K_EEXIST;
        target = resolved;
    }
    /* 原生 mode 为零表示使用默认值。需包含目录类型，确保 Linux 请求权限位为零时仍精确得到 mode 0000。 */
    return vfs_mkdir_kernel(target, 0040000U | ((uint32_t)mode & 0777U));
}

static int64_t linux_symlink(process_t *process, uint64_t target_pointer,
                            int64_t dirfd, uint64_t link_pointer) {
    char target[LINUX_MAX_PATH], path[LINUX_MAX_PATH], resolved[LINUX_MAX_PATH];
    kstatus_t status = linux_copy_string(target_pointer, target, sizeof(target));
    if (status != K_OK) return status;
    status = linux_path_from_user(process, dirfd, link_pointer, path);
    if (status != K_OK) return status;
    status = vfs_resolve_path_kernel(path, false, true, resolved);
    if (status != K_OK) return status;
    return vfs_symlink_kernel(target, resolved);
}

static int64_t linux_unlinkat(process_t *process, int64_t dirfd,
                             uint64_t pointer, uint32_t flags) {
    char path[LINUX_MAX_PATH], resolved[LINUX_MAX_PATH];
    if ((flags & ~LINUX_AT_REMOVEDIR) != 0U) return K_EINVAL;
    kstatus_t status = linux_path_from_user(process, dirfd, pointer, path);
    if (status != K_OK) return status;
    /* 跟随父路径链接，但不跟随叶节点：悬空链接也可以删除。VFS 会在删除边界处检查目标类型。 */
    status = vfs_resolve_path_kernel(path, false, false, resolved);
    if (status != K_OK) return status;
    if (resolved[0] == '/' && resolved[1] == '\0') return K_EBUSY;
    return vfs_remove_typed_kernel(resolved,
                                  (flags & LINUX_AT_REMOVEDIR) != 0U);
}

static int64_t linux_renameat(process_t *process, int64_t old_dirfd,
                             uint64_t old_pointer, int64_t new_dirfd,
                             uint64_t new_pointer) {
    char old_path[LINUX_MAX_PATH], new_path[LINUX_MAX_PATH];
    char old_resolved[LINUX_MAX_PATH], new_resolved[LINUX_MAX_PATH];
    kstatus_t status = linux_path_from_user(process, old_dirfd,
                                           old_pointer, old_path);
    if (status != K_OK) return status;
    status = linux_path_from_user(process, new_dirfd, new_pointer, new_path);
    if (status != K_OK) return status;
    /* 移动叶节点本身，包括悬空符号链接。解析两侧父目录，但允许目标不存在；VFS 会在命名空间锁下发布替换，
     * 并保留已有打开文件引用。 */
    status = vfs_resolve_path_kernel(old_path, false, false, old_resolved);
    if (status != K_OK) return status;
    status = vfs_resolve_path_kernel(new_path, false, true, new_resolved);
    if (status != K_OK) return status;
    if ((old_resolved[0] == '/' && old_resolved[1] == '\0') ||
        (new_resolved[0] == '/' && new_resolved[1] == '\0')) return K_EBUSY;
    return vfs_rename_kernel(old_resolved, new_resolved);
}

static int64_t linux_sysinfo(uint64_t pointer) {
    uint64_t total_pages, free_pages;
    linux_sysinfo_t info;
    os_timespec_t uptime;
    kstatus_t status = page_memory_get_stats(&total_pages, &free_pages);
    if (status != K_OK) return status;
    if (total_pages > UINT64_MAX / PAGE_SIZE || free_pages > total_pages)
        return K_EOVERFLOW;
    status = kernel_clock_read(OS_CLOCK_MONOTONIC, &uptime);
    if (status != K_OK) return status;
    /* 将不支持的记账字段和所有 ABI 填充清零，绝不把栈字节暴露给 glibc 的 sysconf(_SC_PHYS_PAGES) 转换。 */
    linux_zero(&info, sizeof(info));
    info.uptime = uptime.seconds;
    info.totalram = total_pages * PAGE_SIZE;
    info.freeram = free_pages * PAGE_SIZE;
    info.mem_unit = 1U;
    status = copy_to_user((void __user *)(uintptr_t)pointer, &info, sizeof(info));
    return linux_copy_status(status);
}

static int64_t linux_getrandom(uint64_t pointer, uint64_t length) {
    if (length > (uint64_t)INT64_MAX) return K_EOVERFLOW;
    return syscall_random_get(pointer, length, 0U, 0U, 0U, 0U);
}

static int64_t linux_prlimit64(uint64_t pid, uint32_t resource,
                               uint64_t new_pointer, uint64_t old_pointer) {
    linux_rlimit_t value;
    if (pid != 0U || old_pointer == 0U) return K_EINVAL;
    if (new_pointer != 0U) return K_ENOSYS;
    if (resource >= LINUX_RLIMIT_NLIMITS) return K_EINVAL;
    value.current = resource == LINUX_RLIMIT_STACK ?
        8ULL * 1024ULL * 1024ULL :
        resource == 7U ? OS_LINUX_FD_LIMIT : LINUX_RLIM_INFINITY;
    value.maximum = value.current;
    return copy_to_user((void __user *)(uintptr_t)old_pointer, &value,
                        sizeof(value)) == K_OK ? K_OK : LINUX_EFAULT;
}

static int64_t linux_sched_read(uint64_t pid, bool policy_query, uint64_t pointer) {
    int32_t tid = (int32_t)(uint32_t)pid;
    if (tid < 0) return K_EINVAL;
    thread_t *target = linux_thread_reference_by_tid((uint64_t)tid);
    if (target == 0) return K_ESRCH;
    uint8_t class_id = __atomic_load_n(&target->base_sched_class, __ATOMIC_RELAXED);
    uint8_t rt_priority = __atomic_load_n(&target->base_rt_priority, __ATOMIC_RELAXED);
    object_put(target);
    int32_t policy, priority = 0;
    if (class_id == SCHED_CLASS_FAIR) policy = 0; /* SCHED_OTHER */
    else if (class_id == SCHED_CLASS_IDLE) policy = 5; /* SCHED_IDLE */
    else if (class_id == SCHED_CLASS_RT && rt_priority < RT_PRIORITY_LEVELS) {
        policy = 2; /* Native RT queues rotate equal-priority peers: SCHED_RR. */
        priority = 99 - rt_priority; /* Native lower number means higher priority. */
    } else return K_ENOSYS;
    if (policy_query) return policy;
    return copy_to_user((void __user *)(uintptr_t)pointer, &priority,
                        sizeof(priority)) == K_OK ? K_OK : LINUX_EFAULT;
}

static int64_t linux_sched_getaffinity(uint64_t pid, uint64_t size,
                                       uint64_t pointer) {
    uint64_t mask[4];
    uint32_t cpus;
    int32_t tid = (int32_t)(uint32_t)pid;
    if (tid < 0 || size == 0U) return K_EINVAL;
    thread_t *target = linux_thread_reference_by_tid((uint64_t)tid);
    if (target == 0) return K_ESRCH;
    linux_zero(mask, sizeof(mask));
    cpus = x86_smp_discovered_count();
    if (cpus == 0U) cpus = 1U;
    if (cpus > 256U) cpus = 256U;
    size_t mask_bytes = ((cpus + 63U) / 64U) * sizeof(uint64_t);
    if (size < mask_bytes) {
        object_put(target);
        return K_EINVAL;
    }
    for (uint32_t word = 0U; word < (cpus + 63U) / 64U; ++word) {
        mask[word] = __atomic_load_n(&target->affinity.bits[word],
                                     __ATOMIC_RELAXED);
    }
    if ((cpus & 63U) != 0U)
        mask[cpus >> 6] &= (1ULL << (cpus & 63U)) - 1ULL;
    object_put(target);
    if (copy_to_user((void __user *)(uintptr_t)pointer, mask, mask_bytes) != K_OK) {
        return LINUX_EFAULT;
    }
    return (int64_t)mask_bytes;
}

static int64_t linux_sigaction(process_t *process, uint64_t signal,
                               uint64_t action_pointer, uint64_t old_pointer,
                               uint64_t signal_size) {
    linux_sigaction_t replacement;
    linux_sigaction_t previous;
    if (process == 0 || signal == 0U || signal > OS_SIGNAL_COUNT ||
        (signal_size != 8U && signal_size != 16U) ||
        signal == OS_SIGKILL || signal == OS_SIGSTOP) return K_EINVAL;
    spinlock_lock(&process->signal_lock);
    os_signal_action_t old = process->signal_actions[signal];
    spinlock_unlock(&process->signal_lock);
    linux_zero(&previous, sizeof(previous));
    previous.handler = old.handler;
    previous.restorer = old.restorer;
    previous.mask[0] = old.mask;
    previous.flags = (old.flags & OS_SIG_FLAG_RESETHAND) != 0U ?
                     0x80000000ULL : 0U;
    if (action_pointer != 0U) {
        kstatus_t status = copy_from_user(&replacement,
            (const void __user *)(uintptr_t)action_pointer,
            sizeof(replacement));
        if (status != K_OK) return LINUX_EFAULT;
        os_signal_action_t next = {
            .handler = replacement.handler,
            .mask = replacement.mask[0],
            .restorer = replacement.restorer,
            .flags = 0U,
            .reserved = 0U,
        };
        if ((replacement.flags & 0x80000000ULL) != 0U) {
            next.flags |= OS_SIG_FLAG_RESETHAND;
        }
        if ((replacement.flags & 0x40000000ULL) != 0U) {
            next.flags |= OS_SIG_FLAG_NODEFER;
        }
        spinlock_lock(&process->signal_lock);
        process->signal_actions[signal] = next;
        spinlock_unlock(&process->signal_lock);
    }
    if (old_pointer != 0U && copy_to_user(
            (void __user *)(uintptr_t)old_pointer, &previous,
            sizeof(previous)) != K_OK) return LINUX_EFAULT;
    return K_OK;
}

static int64_t linux_sigprocmask(thread_t *thread, uint64_t how,
                                 uint64_t set_pointer, uint64_t old_pointer,
                                 uint64_t signal_size) {
    uint64_t old_mask;
    uint64_t new_mask;
    if (thread == 0 || (signal_size != 8U && signal_size != 16U)) return K_EINVAL;
    old_mask = thread->signal_mask;
    if (set_pointer != 0U) {
        if (copy_from_user(&new_mask,
                (const void __user *)(uintptr_t)set_pointer,
                sizeof(new_mask)) != K_OK) return LINUX_EFAULT;
        if (how == OS_SIG_BLOCK) thread->signal_mask |= new_mask;
        else if (how == OS_SIG_UNBLOCK) thread->signal_mask &= ~new_mask;
        else if (how == OS_SIG_SETMASK) thread->signal_mask = new_mask;
        else return K_EINVAL;
        thread->signal_mask &= ~OS_SIGNAL_UNBLOCKABLE_MASK;
    }
    if (old_pointer != 0U && copy_to_user(
            (void __user *)(uintptr_t)old_pointer, &old_mask,
            sizeof(old_mask)) != K_OK) return LINUX_EFAULT;
    return K_OK;
}

static int64_t linux_readv(process_t *process, int64_t descriptor,
                           uint64_t vector_pointer, uint64_t count,
                           bool writing) {
    linux_iovec_t vectors[LINUX_MAX_IOV];
    if (count == 0U) return 0;
    if (count > LINUX_MAX_IOV || copy_from_user(
            vectors, (const void __user *)(uintptr_t)vector_pointer,
            (size_t)count * sizeof(vectors[0])) != K_OK) return LINUX_EFAULT;
    /* Reject an unrepresentable aggregate before any vector changes file
     * state. Checking each length alone does not protect signed total. */
    uint64_t requested = 0U;
    for (uint64_t index = 0U; index < count; ++index) {
        if (vectors[index].length > (uint64_t)INT64_MAX - requested)
            return K_EOVERFLOW;
        requested += vectors[index].length;
    }
    int64_t total = 0;
    for (uint64_t index = 0U; index < count; ++index) {
        int64_t result = writing ? linux_write(process, descriptor,
                                               vectors[index].base,
                                               vectors[index].length) :
                                   linux_read(process, descriptor,
                                              vectors[index].base,
                                              vectors[index].length);
        if (result < 0) return total != 0 ? total : result;
        total += result;
        if ((uint64_t)result != vectors[index].length) break;
    }
    return total;
}

static int64_t linux_nanosleep(uint32_t clock_id, uint32_t flags,
                               uint64_t request_pointer, uint64_t remaining_pointer) {
    linux_timespec_t request;
    linux_timespec_t remaining = {0};
    wait_queue_t sleep_queue;
    uint64_t delay_ns;
    kstatus_t status;

    /* glibc 的 nanosleep 通过 clock_nanosleep 进入；两种 ABI 必须共用真实阻塞路径。 */
    if ((clock_id != OS_LINUX_CLOCK_REALTIME && clock_id != OS_LINUX_CLOCK_MONOTONIC) ||
        (flags & ~1U) != 0U) return K_EINVAL;
    /* 绝对截止时间不读取或写入 remaining，即使传入的是无效地址。 */
    if (flags == 1U) remaining_pointer = 0U;
    if (copy_from_user(&request, (const void __user *)(uintptr_t)request_pointer,
                       sizeof(request)) != K_OK) return LINUX_EFAULT;
    if (request.seconds < 0 || request.nanoseconds < 0 ||
        request.nanoseconds >= 1000000000LL) return K_EINVAL;
    if ((uint64_t)request.seconds >
        (UINT64_MAX - (uint64_t)request.nanoseconds) / 1000000000ULL) {
        return K_EOVERFLOW;
    }
    if (request.seconds == 0 && request.nanoseconds == 0) {
        if (remaining_pointer != 0U && copy_to_user(
                (void __user *)(uintptr_t)remaining_pointer, &remaining,
                sizeof(remaining)) != K_OK) return LINUX_EFAULT;
        return K_OK;
    }

    delay_ns = (uint64_t)request.seconds * 1000000000ULL +
               (uint64_t)request.nanoseconds;
    if (flags == 1U) {
        os_timespec_t now;
        status = kernel_clock_read(clock_id, &now);
        if (status != K_OK) return status;
        /* 先比较秒和纳秒，再换算正差，避免实时纪元或大截止时间产生加减溢出。 */
        if (request.seconds < now.seconds ||
            (request.seconds == now.seconds && request.nanoseconds <= now.nanoseconds)) return K_OK;
        uint64_t seconds = (uint64_t)(request.seconds - now.seconds);
        if (request.nanoseconds < now.nanoseconds) --seconds;
        uint64_t nanos = request.nanoseconds >= now.nanoseconds ?
            (uint64_t)(request.nanoseconds - now.nanoseconds) :
            (uint64_t)(1000000000LL + request.nanoseconds - now.nanoseconds);
        delay_ns = seconds * 1000000000ULL + nanos;
    }
    /* 截止等待直接使用栈队列，避免每次短睡眠分配对象并争用定时器注册表。
     * 长睡眠分段为一小时，避免有限时长被通用等待核心的无限期哨兵或 TSC 饱和吞掉。 */
    wait_queue_init(&sleep_queue);
    do {
        uint64_t chunk = delay_ns < 3600000000000ULL ? delay_ns : 3600000000000ULL;
        status = wait_on_queue(&sleep_queue, linux_poll_never_ready, 0, chunk);
        if (status != K_ETIMEDOUT) break;
        delay_ns -= chunk;
    } while (delay_ns != 0U);
    if (status == K_ETIMEDOUT) status = K_OK;
    if (status != K_OK && remaining_pointer != 0U) {
        /* 沿用取消时返回完整请求的保守行为；精确的信号中断剩余时间仍需单独实现。 */
        remaining = request;
        if (copy_to_user((void __user *)(uintptr_t)remaining_pointer,
                         &remaining, sizeof(remaining)) != K_OK) {
            status = LINUX_EFAULT;
        }
    } else if (status == K_OK && remaining_pointer != 0U &&
               copy_to_user((void __user *)(uintptr_t)remaining_pointer,
                            &remaining, sizeof(remaining)) != K_OK) {
        status = LINUX_EFAULT;
    }
    return status;
}

static int64_t linux_proc_getdents64(linux_proc_file_t *file,
                                     uint64_t buffer, uint64_t size) {
    uint64_t total = 0U;
    if (file == 0 || !linux_proc_node_is_directory(file->node)) {
        return K_ENOTDIR;
    }
    for (;;) {
        uint64_t index;
        char name[64];
        uint8_t type;
        uint64_t inode;
        size_t name_length = 0U;
        size_t record_length;
        uint8_t record[128U];
        linux_dirent64_header_t header;
        kstatus_t status;

        spinlock_lock(&file->lock);
        index = file->position;
        if (index >= linux_proc_directory_count(file)) {
            spinlock_unlock(&file->lock);
            break;
        }
        linux_zero(name, sizeof(name));
        if (!linux_proc_directory_entry(file, index, name, sizeof(name),
                                         &type, &inode)) {
            spinlock_unlock(&file->lock);
            return K_EIO;
        }
        while (name_length + 1U < sizeof(name) && name[name_length] != '\0') {
            ++name_length;
        }
        record_length = sizeof(header) + name_length + 1U;
        record_length = (record_length + 7U) & ~((size_t)7U);
        if (record_length > sizeof(record) ||
            record_length > (size_t)(size - total)) {
            spinlock_unlock(&file->lock);
            return total != 0U ? (int64_t)total : K_EINVAL;
        }
        /* 下方会覆写头部和名称；这里只初始化对齐填充，即其余将暴露给用户态的字节。 */
        linux_zero(record + sizeof(header) + name_length + 1U,
                    record_length - sizeof(header) - name_length - 1U);
        header.inode = inode;
        header.offset = index + 1U;
        header.record_length = (uint16_t)record_length;
        header.type = type;
        linux_copy(record, &header, sizeof(header));
        linux_copy(record + sizeof(header), name, name_length + 1U);
        status = copy_to_user((void __user *)(uintptr_t)(buffer + total),
                              record, record_length);
        if (status == K_OK) {
            file->position = index + 1U;
            total += record_length;
        }
        spinlock_unlock(&file->lock);
        if (status != K_OK) return LINUX_EFAULT;
    }
    return (int64_t)total;
}

static int64_t linux_getdents64(process_t *process, int64_t descriptor,
                                uint64_t buffer, uint64_t size) {
    linux_fd_entry_t entry;
    uint64_t total = 0U;
    if (process == 0 || buffer == 0U || size > (uint64_t)SIZE_MAX ||
        size == 0U) return K_EINVAL;
    if (!x86_user_range_valid((void __user *)(uintptr_t)buffer, (size_t)size)) {
        return LINUX_EFAULT;
    }
    kstatus_t status = linux_fd_lookup(process, descriptor, &entry);
    if (status != K_OK) return status;
    if (entry.kind != LINUX_FD_FILE || entry.file == 0) {
        if (entry.kind == LINUX_FD_PROC) {
            int64_t result = linux_proc_getdents64(
                (linux_proc_file_t *)entry.private_data, buffer, size);
            linux_fd_entry_put(&entry);
            return result;
        }
        linux_fd_entry_put(&entry);
        return K_ENOTDIR;
    }

    status = vfs_file_position_lock(entry.file);
    if (status != K_OK) {
        linux_fd_entry_put(&entry);
        return status;
    }
    os_file_info_t batch[16U];
    uint32_t batch_count = 0U;
    uint32_t batch_cursor = 0U;
    uint64_t batch_start = 0U;
    for (;;) {
        os_file_info_t info;
        uint64_t index = entry.file->position;
        size_t name_length = 0U;
        size_t record_length;
        uint8_t record[128U];
        linux_dirent64_header_t header;

        if (index > UINT32_MAX) {
            status = K_EOVERFLOW;
            break;
        }
        if (batch_cursor >= batch_count || index != batch_start + batch_cursor) {
            batch_count = batch_cursor = 0U;
            batch_start = index;
            status = vfs_enumerate_file_batch(entry.file, (uint32_t)index,
                                              batch, 16U, &batch_count);
            if (status == K_OK && (batch_count == 0U || batch_count > 16U)) status = K_EIO;
        } else {
            status = K_OK;
        }
        if (status == K_ENOENT) {
            status = K_OK;
            break;
        }
        if (status != K_OK) break;
        info = batch[batch_cursor];
        while (name_length < OS_FILE_NAME_MAX && info.name[name_length] != '\0') {
            ++name_length;
        }
        if (name_length == 0U || name_length >= OS_FILE_NAME_MAX) {
            status = K_EIO;
            break;
        }
        record_length = sizeof(header) + name_length + 1U;
        record_length = (record_length + 7U) & ~((size_t)7U);
        if (record_length > sizeof(record)) {
            status = K_EOVERFLOW;
            break;
        }
        if (record_length > (size_t)(size - total)) {
            if (total == 0U) status = K_EINVAL;
            break;
        }
        /* Header and name are overwritten below; initialize only alignment
         * padding, the remaining bytes that will be exposed to userspace. */
        linux_zero(record + sizeof(header) + name_length + 1U,
                    record_length - sizeof(header) - name_length - 1U);
        header.inode = index + 1U;
        header.offset = index + 1U;
        header.record_length = (uint16_t)record_length;
        header.type = info.type == OS_FILE_TYPE_DIRECTORY ? 4U :
                      (info.type == OS_FILE_TYPE_FIFO ? 1U : 8U);
        linux_copy(record, &header, sizeof(header));
        linux_copy(record + sizeof(header), info.name, name_length + 1U);
        status = copy_to_user((void __user *)(uintptr_t)(buffer + total),
                              record, record_length);
        if (status != K_OK) {
            status = LINUX_EFAULT;
            break;
        }
        total += record_length;
        entry.file->position = index + 1U;
        ++batch_cursor;
    }
    vfs_file_position_unlock(entry.file);
    linux_fd_entry_put(&entry);
    return status == K_OK ? (int64_t)total : status;
}

int64_t linux_syscall_dispatch(arch_trap_frame_t *frame) {
    thread_t *thread = sched_current_thread();
    process_t *process = thread != 0 ? thread->process : 0;
    uint64_t number;
    if (frame == 0 || process == 0 || !linux_process_is(process)) {
        return K_EPERM;
    }
    number = frame->rax;
    /* personality 切换后仍保留 System 仅串口诊断通道。libc stderr 后备路径使用此保留的原生调用号，
     * 因为继承的描述符是 System 调试句柄，不是 Linux 文件描述符。 */
    if (number == OS_SYS_DEBUG_WRITE) {
        return syscall_debug_write(frame->rdi, frame->rsi, frame->rdx,
                                   frame->r10, frame->r8, frame->r9);
    }
    switch (number) {
    case LINUX_SYS_READ:
        return linux_read(process, (int64_t)frame->rdi, frame->rsi, frame->rdx);
    case LINUX_SYS_WRITE:
        return linux_write(process, (int64_t)frame->rdi, frame->rsi, frame->rdx);
    case LINUX_SYS_READLINK:
        return linux_readlinkat(process, LINUX_AT_FDCWD, frame->rdi,
                                frame->rsi, frame->rdx);
    case LINUX_SYS_READLINKAT:
        return linux_readlinkat(process, (int64_t)frame->rdi, frame->rsi,
                                frame->rdx, frame->r10);
    case LINUX_SYS_OPEN:
        return linux_openat(process, LINUX_AT_FDCWD, frame->rdi, frame->rsi,
                            frame->rdx);
    case LINUX_SYS_CREAT:
        /* creat(path, mode) 等价于只写打开并创建/截断，权限来自第二参数。 */
        return linux_openat(process, LINUX_AT_FDCWD, frame->rdi,
                            LINUX_O_WRONLY | LINUX_O_CREAT | LINUX_O_TRUNC,
                            frame->rsi);
    case LINUX_SYS_OPENAT:
        return linux_openat(process, (int64_t)frame->rdi, frame->rsi,
                            frame->rdx, frame->r10);
    case LINUX_SYS_PIPE:
    case LINUX_SYS_PIPE2:
        return linux_pipe_create(process,
                                  number == LINUX_SYS_PIPE2 ?
                                  (uint32_t)frame->rsi : 0U, frame->rdi);
    case LINUX_SYS_EVENTFD:
    case LINUX_SYS_EVENTFD2:
        return linux_eventfd_create(
            process, frame->rdi,
            number == LINUX_SYS_EVENTFD ? 0U : (uint32_t)frame->rsi);
    case LINUX_SYS_EPOLL_CREATE:
        return frame->rdi == 0U ? K_EINVAL : linux_epoll_create(process, 0U);
    case LINUX_SYS_EPOLL_CREATE1:
        return linux_epoll_create(process, (uint32_t)frame->rdi);
    case LINUX_SYS_EPOLL_CTL: {
        int64_t result = linux_epoll_ctl(
            process, (int64_t)frame->rdi, (uint32_t)frame->rsi,
            (int64_t)frame->rdx, frame->r10);
#if OS_DEBUG_SERIAL
        linux_debug_socket_io(5U, (int64_t)frame->rdi, result);
#endif
        return result;
    }
    case LINUX_SYS_EPOLL_WAIT:
    case LINUX_SYS_EPOLL_PWAIT: {
        int64_t result = linux_epoll_wait(
            process, (int64_t)frame->rdi, frame->rsi,
            (int64_t)frame->rdx, (int64_t)frame->r10);
#if OS_DEBUG_SERIAL
        linux_debug_socket_io(6U, (int64_t)frame->rdi, result);
#endif
        return result;
    }
    case LINUX_SYS_SOCKET:
        return linux_socket_create(process, frame->rdi, frame->rsi,
                                   frame->rdx);
    case LINUX_SYS_SOCKETPAIR:
        return linux_socketpair(process, frame->rdi, frame->rsi,
                                frame->rdx, frame->r10);
    case LINUX_SYS_BIND:
        return linux_socket_bind(process, (int64_t)frame->rdi, frame->rsi,
                                 frame->rdx);
    case LINUX_SYS_CONNECT:
        return linux_socket_connect(process, (int64_t)frame->rdi, frame->rsi,
                                    frame->rdx);
    case LINUX_SYS_LISTEN:
        return linux_socket_listen(process, (int64_t)frame->rdi, frame->rsi);
    case LINUX_SYS_ACCEPT:
        return linux_socket_accept(process, (int64_t)frame->rdi, frame->rsi,
                                   frame->rdx, 0U);
    case LINUX_SYS_ACCEPT4:
        return linux_socket_accept(process, (int64_t)frame->rdi, frame->rsi,
                                   frame->rdx, (uint32_t)frame->r10);
    case LINUX_SYS_SENDTO: {
        linux_fd_entry_t entry = {0};
        kstatus_t status = linux_fd_lookup(process, (int64_t)frame->rdi,
                                            &entry);
        int64_t result;
        if (status != K_OK) {
            result = status;
        } else if (entry.kind == LINUX_FD_UNIX_SOCKET) {
            result = linux_unix_sendto(&entry, frame->rsi, frame->rdx,
                                       (uint32_t)frame->r10, frame->r8,
                                       frame->r9);
        } else if (entry.kind == LINUX_FD_SOCKET &&
                   entry.private_data != 0 &&
                   ((socket_t *)entry.private_data)->object.type ==
                       KOBJECT_TYPE_SOCKET) {
            result = linux_socket_send((socket_t *)entry.private_data, &entry,
                                       frame->rsi, frame->rdx,
                                       (uint32_t)frame->r10, frame->r8,
                                       frame->r9);
        } else {
            result = K_ENOTSOCK;
        }
        if (status == K_OK) linux_fd_entry_put(&entry);
#if OS_DEBUG_SERIAL
        linux_debug_socket_io(1U, (int64_t)frame->rdi, result);
#endif
        return result;
    }
    case LINUX_SYS_RECVFROM: {
        linux_fd_entry_t entry = {0};
        kstatus_t status = linux_fd_lookup(process, (int64_t)frame->rdi,
                                            &entry);
        int64_t result;
        if (status != K_OK) {
            result = status;
        } else if (entry.kind == LINUX_FD_UNIX_SOCKET) {
            result = linux_unix_recvfrom(&entry, frame->rsi, frame->rdx,
                                         (uint32_t)frame->r10, frame->r8,
                                         frame->r9);
        } else if (entry.kind == LINUX_FD_SOCKET &&
                   entry.private_data != 0 &&
                   ((socket_t *)entry.private_data)->object.type ==
                       KOBJECT_TYPE_SOCKET) {
            result = linux_socket_receive((socket_t *)entry.private_data,
                                          &entry, frame->rsi, frame->rdx,
                                          (uint32_t)frame->r10, frame->r8,
                                          frame->r9);
        } else {
            result = K_ENOTSOCK;
        }
#if OS_DEBUG_SERIAL
        if (entry.kind == LINUX_FD_SOCKET &&
            g_debug_inet_recv_logs++ < 64U) {
            kernel_serial_printf_serial_only(
                "OS_INET_RECVFROM pid=%llu fd=%d flags=%x result=%d\r\n",
                (unsigned long long)process->pid, (int)frame->rdi,
                (unsigned int)frame->r10, (int)result);
        }
#endif
        if (status == K_OK) linux_fd_entry_put(&entry);
#if OS_DEBUG_SERIAL
        if (((uint32_t)frame->r10 & LINUX_MSG_PEEK) != 0U &&
            g_debug_socket_peek_logs++ < 64U) {
            kernel_serial_printf_serial_only(
                "OS_SOCKET_PEEK pid=%llu fd=%d flags=%x result=%d\r\n",
                (unsigned long long)process->pid, (int)frame->rdi,
                (unsigned int)frame->r10, (int)result);
        }
        linux_debug_socket_io(2U, (int64_t)frame->rdi, result);
#endif
        return result;
    }
    case LINUX_SYS_SENDMSG: {
        int64_t result = linux_sendmsg(process, (int64_t)frame->rdi,
                                       frame->rsi, (uint32_t)frame->rdx);
#if OS_DEBUG_SERIAL
        linux_debug_socket_io(3U, (int64_t)frame->rdi, result);
#endif
        return result;
    }
    case LINUX_SYS_RECVMSG: {
        int64_t result = linux_recvmsg(process, (int64_t)frame->rdi,
                                       frame->rsi, (uint32_t)frame->rdx);
#if OS_DEBUG_SERIAL
        linux_debug_socket_io(4U, (int64_t)frame->rdi, result);
#endif
        return result;
    }
    case LINUX_SYS_SHUTDOWN:
        return linux_socket_shutdown(process, (int64_t)frame->rdi,
                                     frame->rsi);
    case LINUX_SYS_GETSOCKNAME: {
        linux_fd_entry_t entry;
        socket_t *socket;
        kstatus_t status = linux_socket_entry(process, (int64_t)frame->rdi,
                                               &entry, &socket);
        if (status != K_OK) return status;
        status = linux_socket_copy_name(socket, false, frame->rsi,
                                        frame->rdx);
#if OS_DEBUG_SERIAL
        if (g_debug_socket_name_logs < 128U) {
            kernel_serial_printf_serial_only(
                "OS_SOCKET_NAME pid=%llu fd=%d peer=0 status=%d\r\n",
                (unsigned long long)process->pid, (int)frame->rdi, status);
            ++g_debug_socket_name_logs;
        }
#endif
        linux_fd_entry_put(&entry);
        return status;
    }
    case LINUX_SYS_GETPEERNAME: {
        linux_fd_entry_t entry;
        socket_t *socket;
        kstatus_t status = linux_socket_entry(process, (int64_t)frame->rdi,
                                               &entry, &socket);
        if (status != K_OK) return status;
        status = linux_socket_copy_name(socket, true, frame->rsi,
                                        frame->rdx);
#if OS_DEBUG_SERIAL
        if (g_debug_socket_name_logs < 128U) {
            kernel_serial_printf_serial_only(
                "OS_SOCKET_NAME pid=%llu fd=%d peer=1 status=%d\r\n",
                (unsigned long long)process->pid, (int)frame->rdi, status);
            ++g_debug_socket_name_logs;
        }
#endif
        linux_fd_entry_put(&entry);
        return status;
    }
    case LINUX_SYS_SETSOCKOPT:
        return linux_socket_setsockopt(process, (int64_t)frame->rdi,
                                       (uint32_t)frame->rsi,
                                       (uint32_t)frame->rdx, frame->r10,
                                       frame->r8);
    case LINUX_SYS_GETSOCKOPT:
        return linux_socket_getsockopt(process, (int64_t)frame->rdi,
                                       (uint32_t)frame->rsi,
                                       (uint32_t)frame->rdx, frame->r10,
                                       frame->r8);
    case LINUX_SYS_POLL: {
        int64_t result = linux_poll(process, frame->rdi, frame->rsi,
                                    (int64_t)frame->rdx);
#if OS_DEBUG_SERIAL
        linux_debug_socket_io(7U, (int64_t)frame->rsi, result);
#endif
        return result;
    }
    case LINUX_SYS_WINDOW_CREATE:
        return linux_window_create(process, frame->rdi);
    case LINUX_SYS_DISPLAY_GET_INFO:
        if (!linux_user_pointer(frame->rdi, sizeof(os_display_info_t))) {
            return LINUX_EFAULT;
        }
        return syscall_display_get_info(frame->rdi, 0U, 0U, 0U, 0U, 0U);
    case LINUX_SYS_CLOSE:
        return linux_close(process, (int64_t)frame->rdi);
    case LINUX_SYS_STAT:
    case LINUX_SYS_LSTAT:
        return linux_stat_path(process, LINUX_AT_FDCWD, frame->rdi,
                               number == LINUX_SYS_LSTAT ? LINUX_AT_SYMLINK_NOFOLLOW : 0U,
                               frame->rsi);
    case LINUX_SYS_FSTAT:
        return linux_fstat(process, (int64_t)frame->rdi, frame->rsi);
    case LINUX_SYS_STATFS:
        return linux_statfs(process, frame->rdi, frame->rsi);
    case LINUX_SYS_FSTATFS:
        return linux_fstatfs(process, (int64_t)frame->rdi, frame->rsi);
    case LINUX_SYS_NEWFSTATAT:
        return linux_stat_path(process, (int64_t)frame->rdi, frame->rsi,
                               frame->r10, frame->rdx);
    case LINUX_SYS_STATX:
        return linux_statx(process, (int64_t)frame->rdi, frame->rsi,
                           (uint32_t)frame->rdx, (uint32_t)frame->r10,
                           frame->r8);
    case LINUX_SYS_LSEEK:
        return linux_lseek(process, (int64_t)frame->rdi, (int64_t)frame->rsi,
                           (uint32_t)frame->rdx);
    case LINUX_SYS_MMAP: {
        int64_t result = linux_mmap(process, frame->rdi, frame->rsi,
                                    frame->rdx, frame->r10,
                                    (int64_t)frame->r8, frame->r9);
        if (result < 0 && linux_process_is_chrome_image(process) &&
            g_linux_chrome_vm_fail_logs < 32U) {
            linux_chrome_vm_failure("MMAP", process, frame->rdi,
                                    frame->rsi, frame->rdx, frame->r10,
                                    result);
            ++g_linux_chrome_vm_fail_logs;
        }
        if (result < 0 && g_linux_mmap_fail_logs < 32U) {
            kernel_serial_printf_serial_only(
                "OS_DIAG_MMAP_FAIL pid=%llx addr=%llx len=%llx prot=%llx flags=%llx fd=%lld off=%llx status=%lld\r\n",
                (unsigned long long)process->pid,
                (unsigned long long)frame->rdi,
                (unsigned long long)frame->rsi,
                (unsigned long long)frame->rdx,
                (unsigned long long)frame->r10,
                (long long)(int64_t)frame->r8,
                (unsigned long long)frame->r9,
                (long long)result);
            ++g_linux_mmap_fail_logs;
        }
        return result;
    }
    case LINUX_SYS_MPROTECT: {
        if ((frame->rdi & (PAGE_SIZE - 1ULL)) != 0U ||
            frame->rsi > UINT64_MAX - (PAGE_SIZE - 1ULL)) return K_EINVAL;
        if (frame->rsi == 0U) return K_OK;
        uint32_t prot = VM_PROT_USER;
        if ((frame->rdx & LINUX_PROT_READ) != 0U) prot |= VM_PROT_READ;
        if ((frame->rdx & LINUX_PROT_WRITE) != 0U) prot |= VM_PROT_WRITE;
        if ((frame->rdx & LINUX_PROT_EXEC) != 0U) prot |= VM_PROT_EXEC;
        vm_space_t *space = process_vm_reference(process);
        uint64_t rounded = (frame->rsi + PAGE_SIZE - 1ULL) & ~(PAGE_SIZE - 1ULL);
        kstatus_t status = vm_protect_allow_rwx(
            space, (vaddr_t)frame->rdi, (size_t)rounded, prot);
        vm_space_put(space);
        if (status != K_OK && linux_process_is_chrome_image(process) &&
            g_linux_chrome_vm_fail_logs < 32U) {
            linux_chrome_vm_failure("MPROTECT", process, frame->rdi,
                                    frame->rsi, frame->rdx, 0U,
                                    (int64_t)status);
            ++g_linux_chrome_vm_fail_logs;
        }
#if OS_DEBUG_SERIAL && OS_LINUX_MAP_DIAGNOSTICS
        if (g_linux_partition_mprotect_debug_count < 128U &&
            linux_partition_diag_range(frame->rdi, frame->rsi)) {
            kernel_serial_printf_serial_only(
                "OS_LINUX_MPROTECT_PARTITION addr=%llx len=%llx prot=%llx status=%lld\r\n",
                (unsigned long long)frame->rdi,
                (unsigned long long)frame->rsi,
                (unsigned long long)frame->rdx,
                (long long)status);
            ++g_linux_partition_mprotect_debug_count;
        }
#endif
        return status;
    }
    case LINUX_SYS_MUNMAP: {
        if (frame->rsi == 0U || frame->rsi > UINT64_MAX - (PAGE_SIZE - 1ULL) ||
            (frame->rdi & (PAGE_SIZE - 1ULL)) != 0U) return K_EINVAL;
        uint64_t rounded = (frame->rsi + PAGE_SIZE - 1ULL) & ~(PAGE_SIZE - 1ULL);
        vm_space_t *space = process_vm_reference(process);
        kstatus_t status = vm_unmap(space, (vaddr_t)frame->rdi,
                                    (size_t)rounded);
        vm_space_put(space);
        return status;
    }
    case LINUX_SYS_BRK:
        return linux_brk(process, frame->rdi);
    case LINUX_SYS_MADVISE: {
        vm_space_t *space = process_vm_reference(process);
        kstatus_t status = vm_advise(space, (vaddr_t)frame->rdi,
                                     (size_t)frame->rsi,
                                     (uint32_t)frame->rdx);
        vm_space_put(space);
        return status;
    }
    case LINUX_SYS_DUP:
        return linux_dup(process, (int64_t)frame->rdi, -1, 0U);
    case LINUX_SYS_DUP2:
        return linux_dup(process, (int64_t)frame->rdi, (int64_t)frame->rsi, 0U);
    case LINUX_SYS_DUP3:
        if ((frame->rdx & ~LINUX_O_CLOEXEC) != 0U || frame->rdi == frame->rsi) {
            return K_EINVAL;
        }
        return linux_dup(process, (int64_t)frame->rdi, (int64_t)frame->rsi,
                         (uint32_t)((frame->rdx & LINUX_O_CLOEXEC) != 0U ?
                                    LINUX_FD_CLOEXEC : 0U));
    case LINUX_SYS_FCNTL:
        return linux_fcntl(process, (int64_t)frame->rdi, (uint32_t)frame->rsi,
                           frame->rdx);
    case LINUX_SYS_FSYNC:
    case LINUX_SYS_FDATASYNC:
        return linux_fsync(process, (int64_t)frame->rdi);
    case LINUX_SYS_FTRUNCATE:
        return linux_ftruncate(process, (int64_t)frame->rdi, frame->rsi);
    case LINUX_SYS_PREAD64:
    case LINUX_SYS_PWRITE64:
        return linux_positioned_io(process, (int64_t)frame->rdi, frame->rsi,
                                   frame->rdx, (int64_t)frame->r10,
                                   number == LINUX_SYS_PWRITE64);
    case LINUX_SYS_READV:
        return linux_readv(process, (int64_t)frame->rdi, frame->rsi,
                           frame->rdx, false);
    case LINUX_SYS_WRITEV:
        return linux_readv(process, (int64_t)frame->rdi, frame->rsi,
                           frame->rdx, true);
    case LINUX_SYS_ACCESS:
    case LINUX_SYS_FACCESSAT2: {
        char path[LINUX_MAX_PATH];
        os_file_info_t info;
        uint32_t nvidia_minor;
        kstatus_t status = number == LINUX_SYS_ACCESS ?
            linux_path_from_user(process, LINUX_AT_FDCWD, frame->rdi, path) :
            linux_path_from_user(process, (int64_t)frame->rdi, frame->rsi, path);
        if (status != K_OK) return status;
        if (linux_nvidia_device_path(path, &nvidia_minor)) {
            (void)nvidia_minor;
            return nv_frontend_driver_ready() ? K_OK : K_ENOENT;
        }
        if (linux_exact_path(path, "/dev/null")) return K_OK;
        status = vfs_stat_kernel(path, &info);
        return status;
    }
    case LINUX_SYS_GETPID:
        return (int64_t)process->pid;
    case LINUX_SYS_GETTID:
        if (thread == 0 || process == 0) return K_EPERM;
        /* Linux 将线程组 leader 的 TID 作为 getpid() 返回值。System 从同一全局 ID 流分配进程和线程对象，
         * 因此仅转换该 leader，不修改调度器公开的原生 ID。 */
        return (int64_t)linux_thread_visible_tid(thread);
    case LINUX_SYS_GETUID:
    case LINUX_SYS_GETGID:
    case LINUX_SYS_GETEUID:
    case LINUX_SYS_GETEGID:
        return 0;
    case LINUX_SYS_GETRESUID:
    case LINUX_SYS_GETRESGID:
        return linux_getresids(frame->rdi, frame->rsi, frame->rdx);
    case LINUX_SYS_EXIT:
    case LINUX_SYS_EXIT_GROUP:
#if OS_DEBUG_SERIAL
        if (g_linux_exit_debug_count++ < 4U) {
            uint32_t process_state;
            uint32_t process_thread_count;
            uint64_t user_stack[8] = {0};
            uint64_t caller_stack[8] = {0};
            uint64_t abort_stack[8] = {0};
            uint64_t verbose_stack[8] = {0};
            uint64_t exit_unwind[12] = {0};
            process_thread_lock(process);
            process_state = atomic_load_explicit(&process->state,
                                                 memory_order_acquire);
            process_thread_count = process->thread_count;
            process_thread_unlock(process);
            (void)copy_from_user(user_stack,
                                 (const void __user *)(uintptr_t)frame->rsp,
                                 sizeof(user_stack));
            (void)copy_from_user(caller_stack,
                                 (const void __user *)(uintptr_t)frame->rbp,
                                 sizeof(caller_stack));
            uint64_t unwind_frame = frame->rbp;
            for (uint32_t unwind_index = 0U;
                 unwind_index < 12U && unwind_frame != 0U;
                 ++unwind_index) {
                uint64_t frame_words[2] = {0};
                if (copy_from_user(
                        frame_words,
                        (const void __user *)(uintptr_t)unwind_frame,
                        sizeof(frame_words)) != K_OK ||
                    frame_words[1] < 0x10000U ||
                    frame_words[1] > X86_64_USER_TOP) {
                    break;
                }
                exit_unwind[unwind_index] = frame_words[1];
                if (frame_words[0] <= unwind_frame ||
                    frame_words[0] - unwind_frame > 16ULL * 1024ULL * 1024ULL) {
                    break;
                }
                unwind_frame = frame_words[0];
            }
            if (caller_stack[6] != 0U) {
                (void)copy_from_user(
                    abort_stack,
                    (const void __user *)(uintptr_t)caller_stack[6],
                    sizeof(abort_stack));
            }
            if (abort_stack[0] != 0U) {
                (void)copy_from_user(
                    verbose_stack,
                    (const void __user *)(uintptr_t)abort_stack[0],
                    sizeof(verbose_stack));
            }
            kernel_serial_printf_serial_only(
                "OS_LINUX_EXIT_TRACE pid=%llu tid=%llu state=%u threads=%u "
                "nr=%llu rip=%llx rsp=%llx rbp=%llx "
                "stack0=%llx stack1=%llx stack2=%llx stack3=%llx "
                "stack4=%llx stack5=%llx stack6=%llx stack7=%llx code=%u\r\n",
                (unsigned long long)process->pid,
                (unsigned long long)thread->tid,
                process_state, process_thread_count,
                (unsigned long long)number,
                (unsigned long long)frame->rip,
                (unsigned long long)frame->rsp,
                (unsigned long long)frame->rbp,
                (unsigned long long)user_stack[0],
                (unsigned long long)user_stack[1],
                (unsigned long long)user_stack[2],
                (unsigned long long)user_stack[3],
                (unsigned long long)user_stack[4],
                (unsigned long long)user_stack[5],
                (unsigned long long)user_stack[6],
                (unsigned long long)user_stack[7],
                (unsigned int)frame->rdi);
            kernel_serial_printf_serial_only(
                "OS_LINUX_EXIT_CHAIN c0=%llx c1=%llx c2=%llx c3=%llx "
                "c4=%llx c5=%llx c6=%llx c7=%llx\r\n",
                (unsigned long long)caller_stack[0],
                (unsigned long long)caller_stack[1],
                (unsigned long long)caller_stack[2],
                (unsigned long long)caller_stack[3],
                (unsigned long long)caller_stack[4],
                (unsigned long long)caller_stack[5],
                (unsigned long long)caller_stack[6],
                (unsigned long long)caller_stack[7]);
            kernel_serial_printf_serial_only(
                "OS_LINUX_EXIT_UNWIND u0=%llx u1=%llx u2=%llx "
                "u3=%llx u4=%llx u5=%llx u6=%llx u7=%llx "
                "u8=%llx u9=%llx u10=%llx u11=%llx\r\n",
                (unsigned long long)exit_unwind[0],
                (unsigned long long)exit_unwind[1],
                (unsigned long long)exit_unwind[2],
                (unsigned long long)exit_unwind[3],
                (unsigned long long)exit_unwind[4],
                (unsigned long long)exit_unwind[5],
                (unsigned long long)exit_unwind[6],
                (unsigned long long)exit_unwind[7],
                (unsigned long long)exit_unwind[8],
                (unsigned long long)exit_unwind[9],
                (unsigned long long)exit_unwind[10],
                (unsigned long long)exit_unwind[11]);
            kernel_serial_printf_serial_only(
                "OS_LINUX_ABORT_CHAIN a0=%llx a1=%llx a2=%llx a3=%llx "
                "a4=%llx a5=%llx a6=%llx a7=%llx\r\n",
                (unsigned long long)abort_stack[0],
                (unsigned long long)abort_stack[1],
                (unsigned long long)abort_stack[2],
                (unsigned long long)abort_stack[3],
                (unsigned long long)abort_stack[4],
                (unsigned long long)abort_stack[5],
                (unsigned long long)abort_stack[6],
                (unsigned long long)abort_stack[7]);
            kernel_serial_printf_serial_only(
                "OS_LINUX_VERBOSE_CHAIN v0=%llx v1=%llx v2=%llx v3=%llx "
                "v4=%llx v5=%llx v6=%llx v7=%llx\r\n",
                (unsigned long long)verbose_stack[0],
                (unsigned long long)verbose_stack[1],
                (unsigned long long)verbose_stack[2],
                (unsigned long long)verbose_stack[3],
                (unsigned long long)verbose_stack[4],
                (unsigned long long)verbose_stack[5],
                (unsigned long long)verbose_stack[6],
                (unsigned long long)verbose_stack[7]);
        }
#endif
        if (number == LINUX_SYS_EXIT) thread_exit((int64_t)frame->rdi);
        process_exit((int64_t)frame->rdi);
    case LINUX_SYS_CLONE:
        return linux_clone(process, thread, frame->rdi, frame->rsi, frame->rdx,
                           frame->r10, frame->r8, frame);
    case LINUX_SYS_FORK:
        return syscall_process_clone_linux(OS_SIGCHLD, 0U, 0U, 0U,
                                            (uint64_t)(uintptr_t)frame);
    case LINUX_SYS_WAIT4:
        if ((frame->rdx & ~(uint64_t)OS_PROCESS_WAIT_NOHANG) != 0U)
            return K_EINVAL;
        /* 尚未实现 rusage 记账；绝不伪造数据。 */
        if (frame->r10 != 0U) return K_ENOSYS;
        return syscall_process_wait(frame->rdi, frame->rdx, frame->rsi,
                                     0U, 0U, 0U);
    case LINUX_SYS_PRCTL:
        return linux_prctl(process, frame->rdi, frame->rsi);
    case LINUX_SYS_ARCH_PRCTL:
        return linux_arch_prctl(thread, frame->rdi, frame->rsi);
    case LINUX_SYS_FUTEX:
        return linux_futex(process, frame->rdi, (uint32_t)frame->rsi,
                           (uint32_t)frame->rdx, frame->r10, frame->r9);
    case LINUX_SYS_CLOCK_GETTIME:
        return linux_clock_gettime(frame->rdi, frame->rsi);
    case LINUX_SYS_GETTIMEOFDAY:
        return linux_gettimeofday(frame->rdi, frame->rsi);
    case LINUX_SYS_GETRANDOM:
        return linux_getrandom(frame->rdi, frame->rsi);
    case LINUX_SYS_SYSINFO:
        return linux_sysinfo(frame->rdi);
    case LINUX_SYS_UNAME:
        return linux_uname(frame->rdi);
    case LINUX_SYS_GETCWD:
        return linux_getcwd(process, frame->rdi, frame->rsi);
    case LINUX_SYS_CHDIR:
        return linux_chdir(process, frame->rdi);
    case LINUX_SYS_RENAME:
        return linux_renameat(process, LINUX_AT_FDCWD, frame->rdi,
                              LINUX_AT_FDCWD, frame->rsi);
    case LINUX_SYS_RENAMEAT:
        return linux_renameat(process, (int64_t)frame->rdi, frame->rsi,
                              (int64_t)frame->rdx, frame->r10);
    case LINUX_SYS_UNLINK:
        return linux_unlinkat(process, LINUX_AT_FDCWD, frame->rdi, 0U);
    case LINUX_SYS_RMDIR:
        return linux_unlinkat(process, LINUX_AT_FDCWD, frame->rdi,
                              LINUX_AT_REMOVEDIR);
    case LINUX_SYS_UNLINKAT:
        return linux_unlinkat(process, (int64_t)frame->rdi, frame->rsi,
                              (uint32_t)frame->rdx);
    case LINUX_SYS_MKDIR:
        return linux_mkdir(process, LINUX_AT_FDCWD, frame->rdi, frame->rsi);
    case LINUX_SYS_MKDIRAT:
        return linux_mkdir(process, (int64_t)frame->rdi, frame->rsi, frame->rdx);
    case LINUX_SYS_SYMLINK:
        return linux_symlink(process, frame->rdi, LINUX_AT_FDCWD, frame->rsi);
    case LINUX_SYS_SYMLINKAT:
        return linux_symlink(process, frame->rdi, (int64_t)frame->rsi, frame->rdx);
    case LINUX_SYS_PRLIMIT64:
        return linux_prlimit64(frame->rdi, (uint32_t)frame->rsi, frame->rdx,
                               frame->r10);
    case LINUX_SYS_SCHED_GETAFFINITY:
        return linux_sched_getaffinity(frame->rdi, frame->rsi, frame->rdx);
    case LINUX_SYS_SCHED_GETPARAM:
        return linux_sched_read(frame->rdi, false, frame->rsi);
    case LINUX_SYS_SCHED_GETSCHEDULER:
        return linux_sched_read(frame->rdi, true, 0U);
    case LINUX_SYS_SET_TID_ADDRESS:
        thread->linux_clear_tid = (vaddr_t)frame->rdi;
        thread->linux_clear_tid_pending = frame->rdi != 0U;
        return (int64_t)linux_thread_visible_tid(thread);
    case LINUX_SYS_SET_ROBUST_LIST:
        if (frame->rsi != 24U ||
            (frame->rdi != 0U && !linux_user_pointer(frame->rdi, 24U))) {
            return K_EINVAL;
        }
        thread->linux_robust_list = (vaddr_t)frame->rdi;
        thread->linux_robust_length = frame->rsi;
        return K_OK;
    case LINUX_SYS_RSEQ:
        return K_ENOSYS;
    case LINUX_SYS_RT_SIGACTION:
        return linux_sigaction(process, frame->rdi, frame->rsi, frame->rdx,
                               frame->r10);
    case LINUX_SYS_RT_SIGPROCMASK:
        return linux_sigprocmask(thread, frame->rdi, frame->rsi, frame->rdx,
                                 frame->r10);
    case LINUX_SYS_IOCTL:
        return linux_ioctl(process, (int64_t)frame->rdi, (uint32_t)frame->rsi,
                            frame->rdx);
    case LINUX_SYS_NANOSLEEP:
        return linux_nanosleep(OS_LINUX_CLOCK_REALTIME, 0U, frame->rdi, frame->rsi);
    case LINUX_SYS_CLOCK_NANOSLEEP:
        return linux_nanosleep((uint32_t)frame->rdi, (uint32_t)frame->rsi,
                               frame->rdx, frame->r10);
    case LINUX_SYS_SCHED_YIELD:
        schedule();
        return K_OK;
    case LINUX_SYS_GETDENTS64:
        return linux_getdents64(process, (int64_t)frame->rdi, frame->rsi,
                                frame->rdx);
    case LINUX_SYS_EXECVE:
        return process_exec(process, (const char __user *)(uintptr_t)frame->rdi,
                            (const char __user *const __user *)(uintptr_t)frame->rsi,
                            (const char __user *const __user *)(uintptr_t)frame->rdx,
                            0);
    default:
        return K_ENOSYS;
    }
}
