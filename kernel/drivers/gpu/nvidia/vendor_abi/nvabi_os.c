#include "nvabi_internal.h"

#include <arch/x86_64/paging.h>
#include <arch/x86_64/smp.h>
#include <kernel/nv_frontend.h>

#include "../rm/include/nv_rm_driver.h"
#include "../rm/include/nv_rm_vendor_bridge.h"

static nv_port_registry_t g_nvabi_registry;
static atomic_bool g_nvabi_registry_initialized = ATOMIC_VAR_INIT(false);

nv_state_t *NV_API_CALL nv_get_ctl_state(void)
{
    /* Linux 厂商实现会返回其控制设备中嵌入的状态。System 将该设备保持为私有，
     * 因此 RM 桥接层会通过不透明的 OS 适配器结果公开相同的实时状态。 */
    return (nv_state_t *)nv_rm_vendor_ctl_state();
}

void NV_API_CALL nv_get_screen_info(
    nv_state_t *nv, NvU64 *base, NvU32 *width, NvU32 *height,
    NvU32 *depth, NvU32 *pitch, NvU64 *size)
{
    nv_rm_screen_info_t info;

    (void)nv;
    if (base != NULL) *base = 0U;
    if (width != NULL) *width = 0U;
    if (height != NULL) *height = 0U;
    if (depth != NULL) *depth = 0U;
    if (pitch != NULL) *pitch = 0U;
    if (size != NULL) *size = 0U;

    if (!nv_rm_driver_get_screen_info(&info))
        return;

    if (base != NULL) *base = info.physical_base;
    if (width != NULL) *width = info.width;
    if (height != NULL) *height = info.height;
    if (depth != NULL) *depth = info.depth;
    if (pitch != NULL) *pitch = info.pitch;
    if (size != NULL) *size = info.size;
}

typedef struct nvabi_mutex {
    uint32_t magic;
    /*
     * 595.84 Linux OS 接口使用 struct semaphore（down/up）实现 os_*mutex，
     * 并非跟踪所有者的互斥锁。RM 可能在不同于获取者的完成上下文中释放它。
     */
    nv_port_semaphore_t semaphore;
} nvabi_mutex_t;

typedef struct nvabi_semaphore {
    uint32_t magic;
    nv_port_semaphore_t semaphore;
} nvabi_semaphore_t;

typedef struct nvabi_rwlock {
    uint32_t magic;
    nv_port_rwlock_t lock;
} nvabi_rwlock_t;

typedef struct nvabi_spinlock {
    uint32_t magic;
    nv_port_spinlock_t lock;
} nvabi_spinlock_t;

typedef struct nvabi_wait_queue {
    uint32_t magic;
    nv_port_completion_t completion;
} nvabi_wait_queue_t;

#define NVABI_MUTEX_MAGIC 0x4D555458U
#define NVABI_SEMAPHORE_MAGIC 0x53454D41U
#define NVABI_RWLOCK_MAGIC 0x52574C4BU
#define NVABI_SPINLOCK_MAGIC 0x5350494EU
#define NVABI_WAIT_QUEUE_MAGIC 0x57414954U

static bool nvabi_magic(const uint32_t *magic, uint32_t expected,
                        const char *function) {
    if (magic == NULL || *magic != expected) {
        nvabi_log_invalid(function, "os-sync");
        return false;
    }
    return true;
}

static thread_t *nvabi_current_thread(void) {
    return sched_current_thread();
}

static process_t *nvabi_current_process(void) {
    thread_t *thread = nvabi_current_thread();
    return thread == NULL ? NULL : thread->process;
}

NV_STATUS NV_API_CALL nvabi_file_private_lookup(NvS32 fd, NvBool ctl,
                                                 nv_file_private_t **out,
                                                 void **os_private) {
    process_t *process = nvabi_current_process();
    nv_frontend_file_t *file = NULL;
    linux_fd_entry_t entry;

    if (out == NULL || os_private == NULL)
        return NV_ERR_INVALID_ARGUMENT;
    *out = NULL;
    *os_private = NULL;
    if (process == NULL || fd < 0 || fd >= (NvS32)OS_LINUX_FD_LIMIT)
        return NV_ERR_INVALID_ARGUMENT;

    spinlock_lock(&process->linux_fds.lock);
    entry = process->linux_fds.entries[(uint32_t)fd];
    if (entry.kind == LINUX_FD_NVIDIA && entry.private_data != NULL) {
        file = (nv_frontend_file_t *)entry.private_data;
        nv_frontend_file_get(file);
    }
    spinlock_unlock(&process->linux_fds.lock);

    if (file == NULL || nv_frontend_file_is_control(file) !=
        (ctl != NV_FALSE)) {
        if (file != NULL)
            nv_frontend_file_put(file);
        return NV_ERR_INVALID_ARGUMENT;
    }
    *out = (nv_file_private_t *)nv_frontend_file_vendor_private(file);
    if (*out == NULL) {
        nv_frontend_file_put(file);
        return NV_ERR_INVALID_ARGUMENT;
    }
    *os_private = file;
    return NV_OK;
}

nv_file_private_t *NV_API_CALL nv_get_file_private(NvS32 fd, NvBool ctl,
                                                    void **os_private) {
    nv_file_private_t *file_private = NULL;
    if (nvabi_file_private_lookup(fd, ctl, &file_private, os_private) != NV_OK)
        return NULL;
    return file_private;
}

void NV_API_CALL nv_put_file_private(void *os_private) {
    if (os_private != NULL)
        nv_frontend_file_put((nv_frontend_file_t *)os_private);
}

static size_t nvabi_append_char(char *destination, size_t capacity,
                                size_t used, char value) {
    if (destination != NULL && capacity != 0U && used + 1U < capacity) {
        destination[used] = value;
    }
    return used + 1U;
}

static size_t nvabi_append_string(char *destination, size_t capacity,
                                  size_t used, const char *value) {
    if (value == NULL) value = "(null)";
    while (*value != '\0') {
        used = nvabi_append_char(destination, capacity, used, *value++);
    }
    return used;
}

static size_t nvabi_append_unsigned(char *destination, size_t capacity,
                                    size_t used, NvU64 value, NvU32 base,
                                    bool upper) {
    /* 64 位十进制数需要 20 位，比十六进制表示所需的 16 位更多。 */
    char digits[sizeof(NvU64) * 3U];
    size_t count = 0U;
    const char *alphabet = upper ? "0123456789ABCDEF" : "0123456789abcdef";

    do {
        digits[count++] = alphabet[value % base];
        value /= base;
    } while (value != 0U && count < sizeof(digits));
    while (count != 0U) used = nvabi_append_char(
        destination, capacity, used, digits[--count]);
    return used;
}

/* 有界格式化程序足以满足厂商诊断 ABI 的需求。它特意只支持标量和字符串转换；
 * 不支持的格式扩展会按原样输出，不会被静默丢弃。 */
static int nvabi_vsnprintf(char *destination, NvU32 capacity,
                           const char *format, va_list arguments) {
    size_t used = 0U;
    va_list ap;

    if (format == NULL || (destination == NULL && capacity != 0U)) return -1;
    va_copy(ap, arguments);
    while (*format != '\0') {
        if (*format != '%') {
            used = nvabi_append_char(destination, capacity, used, *format++);
            continue;
        }
        ++format;
        if (*format == '%') {
            used = nvabi_append_char(destination, capacity, used, *format++);
            continue;
        }
        while (*format == '-' || *format == '+' || *format == ' ' ||
               *format == '#' || *format == '0') ++format;
        while (*format >= '0' && *format <= '9') ++format;
        if (*format == '.') {
            ++format;
            while (*format >= '0' && *format <= '9') ++format;
        }
        bool long_value = false;
        if (*format == 'l') {
            long_value = true;
            ++format;
            if (*format == 'l') ++format;
        }
        if (*format == 'z') {
            long_value = true;
            ++format;
        }
        switch (*format) {
        case 'c':
            used = nvabi_append_char(destination, capacity, used,
                                      (char)va_arg(ap, int));
            break;
        case 's':
            used = nvabi_append_string(destination, capacity, used,
                                        va_arg(ap, const char *));
            break;
        case 'd':
        case 'i': {
            NvS64 value = long_value ? va_arg(ap, NvS64) : (NvS64)va_arg(ap, int);
            if (value < 0) {
                used = nvabi_append_char(destination, capacity, used, '-');
                value = -value;
            }
            used = nvabi_append_unsigned(destination, capacity, used,
                                          (NvU64)value, 10U, false);
            break;
        }
        case 'u':
            used = nvabi_append_unsigned(destination, capacity, used,
                                          long_value ? va_arg(ap, NvU64) :
                                          (NvU64)va_arg(ap, unsigned int),
                                          10U, false);
            break;
        case 'x':
        case 'X':
            used = nvabi_append_unsigned(destination, capacity, used,
                                          long_value ? va_arg(ap, NvU64) :
                                          (NvU64)va_arg(ap, unsigned int),
                                          16U, *format == 'X');
            break;
        case 'p':
            used = nvabi_append_string(destination, capacity, used, "0x");
            used = nvabi_append_unsigned(destination, capacity, used,
                                          (NvU64)(uintptr_t)va_arg(ap, void *),
                                          16U, false);
            break;
        default:
            used = nvabi_append_char(destination, capacity, used, '%');
            if (*format != '\0') used = nvabi_append_char(
                destination, capacity, used, *format);
            break;
        }
        if (*format != '\0') ++format;
    }
    va_end(ap);
    if (destination != NULL && capacity != 0U) {
        destination[used < (size_t)capacity - 1U ? used : capacity - 1U] = '\0';
    }
    return used > (size_t)INT32_MAX ? INT32_MAX : (int)used;
}

static atomic_uint_fast64_t g_nvabi_post_event_unsupported_count = ATOMIC_VAR_INIT(0);

void nvabi_log_unsupported(const char *function, const char *caller_stage) {
    uint64_t count = 0U;
    /* 此回调仍不受支持，不能伪装成通知成功。视频队列可能调用数千次；每次同步输出到
     * UART 会遮蔽其他诊断信息并阻塞完成处理。保留最初八条报告，之后按累计次数为
     * 2 的幂输出摘要。回调上下文中不分配内存、不休眠，也不获取日志锁。 */
    if (function != NULL && nvabi_string_compare(function, "nv_post_event") == 0) {
        count = atomic_fetch_add_explicit(&g_nvabi_post_event_unsupported_count,
                                         1U, memory_order_relaxed) + 1U;
        if (count > 8U && (count & (count - 1U)) != 0U) return;
    }
    kernel_serial_write("OS_NVIDIA_ABI_UNSUPPORTED ");
    kernel_serial_write(function == NULL ? "<unknown>" : function);
    kernel_serial_write(" STAGE=");
    kernel_serial_write(caller_stage == NULL ? "vendor_abi" : caller_stage);
    if (count != 0U) {
        char text[24];
        size_t used = nvabi_append_unsigned(text, sizeof(text), 0U,
                                            count, 10U, false);
        text[used < sizeof(text) ? used : sizeof(text) - 1U] = '\0';
        kernel_serial_write(" COUNT=");
        kernel_serial_write(text);
    }
    kernel_serial_write("\r\n");
}

void nvabi_log_invalid(const char *function, const char *caller_stage) {
    kernel_serial_write("OS_NVIDIA_ABI_INVALID ");
    kernel_serial_write(function == NULL ? "<unknown>" : function);
    kernel_serial_write(" STAGE=");
    kernel_serial_write(caller_stage == NULL ? "vendor_abi" : caller_stage);
    kernel_serial_write("\r\n");
}

NV_STATUS NV_API_CALL os_alloc_mem(void **address, NvU64 size) {
    if (address == NULL || size == 0U || size > (NvU64)SIZE_MAX) {
        return nvabi_invalid_status("os_alloc_mem", "os-memory");
    }
    *address = kmalloc((size_t)size, 0U);
    return *address == NULL ? NV_ERR_NO_MEMORY : NV_OK;
}

void NV_API_CALL os_free_mem(void *address) {
    if (address != NULL) kfree(address);
}

NV_STATUS NV_API_CALL os_match_mmap_offset(
    void *pAllocPrivate, NvU64 offset, NvU64 *pPageIndex) {
    nvabi_page_allocation_t *allocation =
        (nvabi_page_allocation_t *)pAllocPrivate;

    if (pPageIndex == NULL || allocation == NULL ||
        allocation->magic != NVABI_PAGE_ALLOCATION_MAGIC ||
        allocation->page_count == 0U ||
        allocation->memory.page_count != allocation->page_count ||
        allocation->memory.phys_pages == NULL ||
        allocation->memory.page_objects == NULL) {
        nvabi_log_invalid("os_match_mmap_offset", "os-memory-map");
        return NV_ERR_INVALID_ARGUMENT;
    }

    for (NvU64 index = 0U; index < (NvU64)allocation->page_count; ++index) {
        NvU64 page_address;
        if (allocation->contiguous) {
            if (index > UINT64_MAX / NVABI_PAGE_SIZE ||
                allocation->memory.phys_pages[0] > UINT64_MAX -
                    index * NVABI_PAGE_SIZE) {
                nvabi_log_invalid("os_match_mmap_offset", "os-memory-map");
                return NV_ERR_INVALID_ARGUMENT;
            }
            page_address = allocation->memory.phys_pages[0] +
                           index * NVABI_PAGE_SIZE;
        } else {
            page_address = allocation->memory.phys_pages[(uint32_t)index];
        }
        if (offset == page_address) {
            *pPageIndex = index;
            return NV_OK;
        }
    }

    return NV_ERR_OBJECT_NOT_FOUND;
}

NV_STATUS NV_API_CALL os_get_system_time(NvU32 *seconds, NvU32 *microseconds) {
    uint64_t now;
    if (seconds == NULL || microseconds == NULL) {
        return nvabi_invalid_status("os_get_system_time", "os-time");
    }
    now = nv_port_time_now_ns();
    *seconds = (NvU32)((now / 1000000000ULL) & UINT32_MAX);
    *microseconds = (NvU32)((now % 1000000000ULL) / 1000ULL);
    return NV_OK;
}

NvU64 NV_API_CALL os_get_monotonic_time_ns(void) {
    return nv_port_time_now_ns();
}

NvU64 NV_API_CALL os_get_monotonic_time_ns_hr(void) {
    return nv_port_time_now_ns();
}

NvU64 NV_API_CALL os_get_monotonic_tick_resolution_ns(void) {
    return 1ULL;
}

/*
 * Linux 会在 RM 调用期间持有带引用计数的 struct pid。System 没有 PID 命名空间，
 * 因此进程对象就是对应的稳定标识。获取时增加对象引用，put 时准确释放该引用；
 * 这样即可遵守生命周期约定，无需另造 PID 对象或与命名空间无关的成功值。
 */
void *NV_API_CALL os_get_pid_info(void) {
    process_t *process = nvabi_current_process();
    if (process == NULL) return NULL;
    object_get(process);
    return process;
}

void NV_API_CALL os_put_pid_info(void *pid_info) {
    if (pid_info != NULL) object_put(pid_info);
}

NV_STATUS NV_API_CALL os_find_ns_pid(void *pid_info, NvU32 *ns_pid) {
    process_t *process = (process_t *)pid_info;
    if (process == NULL || ns_pid == NULL) {
        return nvabi_invalid_status("os_find_ns_pid", "os-process");
    }
    if (atomic_load_explicit(&process->state, memory_order_acquire) ==
        PROCESS_DEAD) {
        *ns_pid = 0U;
        return NV_ERR_OBJECT_NOT_FOUND;
    }
    *ns_pid = (NvU32)process->pid;
    return *ns_pid == 0U ? NV_ERR_OBJECT_NOT_FOUND : NV_OK;
}

NvBool NV_API_CALL os_is_init_ns(void) {
    /* System 当前只公开一个进程/PID 命名空间。 */
    return NV_TRUE;
}

static NvBool nvabi_current_process_has_nvidia_fd(void) {
    thread_t *thread = nvabi_current_thread();
    process_t *process = thread == NULL ? NULL : thread->process;
    bool found = false;

    if (process == NULL) return NV_FALSE;
    spinlock_lock(&process->linux_fds.lock);
    for (NvU32 descriptor = 0U;
         descriptor < OS_LINUX_FD_LIMIT; ++descriptor) {
        const linux_fd_entry_t *entry =
            &process->linux_fds.entries[descriptor];
        if (entry->kind == LINUX_FD_NVIDIA && entry->private_data != NULL) {
            found = true;
            break;
        }
    }
    spinlock_unlock(&process->linux_fds.lock);
    return found ? NV_TRUE : NV_FALSE;
}

NvBool NV_API_CALL nv_is_gpu_accessible(nv_state_t *nv) {
    (void)nv;
    /* 不可变的 Linux 实现会检查当前任务的 files_struct 中是否有 NVIDIA 文件。System
     * 在其 Linux personality 文件描述符表上保持相同的可观察规则；这不是就绪状态的
     * 快捷判断，也不会向没有真实 fd 的进程授予访问权限。 */
    return nvabi_current_process_has_nvidia_fd();
}

NV_STATUS NV_API_CALL os_delay(NvU32 milliseconds) {
    nv_port_mdelay(milliseconds);
    return NV_OK;
}

NV_STATUS NV_API_CALL os_delay_us(NvU32 microseconds) {
    nv_port_udelay(microseconds);
    return NV_OK;
}

NvU64 NV_API_CALL os_get_cpu_frequency(void) {
    return x86_boot_cpu_features.tsc_hz;
}

NvU32 NV_API_CALL os_get_current_process(void) {
    process_t *process = nvabi_current_process();
    return process == NULL ? 0U : (NvU32)process->pid;
}

void NV_API_CALL os_get_current_process_name(char *name, NvU32 capacity) {
    process_t *process = nvabi_current_process();
    const char *source = process == NULL ? "kernel" : process->name;
    if (name == NULL || capacity == 0U) return;
    NvU32 index = 0U;
    while (index + 1U < capacity && source[index] != '\0') {
        name[index] = source[index];
        ++index;
    }
    name[index] = '\0';
}

NV_STATUS NV_API_CALL os_get_current_thread(NvU64 *thread_id) {
    thread_t *thread = nvabi_current_thread();
    if (thread_id == NULL) return nvabi_invalid_status(
        "os_get_current_thread", "os-thread");
    if (thread == NULL) return nvabi_unsupported_status(
        "os_get_current_thread", "os-thread");
    *thread_id = thread->tid;
    return NV_OK;
}

char *NV_API_CALL os_string_copy(char *destination, const char *source) {
    if (destination == NULL || source == NULL) return NULL;
    size_t index = 0U;
    do {
        destination[index] = source[index];
    } while (source[index++] != '\0');
    return destination;
}

NvU32 NV_API_CALL os_string_length(const char *source) {
    return (NvU32)nvabi_strlen(source);
}

NvU32 NV_API_CALL os_strtoul(const char *source, char **end,
                             NvU32 base) {
    NvU64 value = 0U;
    const char *cursor = source;
    NvU32 radix = base == 0U ? 10U : base;
    if (source == NULL || (radix != 10U && radix != 16U && radix != 8U)) {
        if (end != NULL) *end = (char *)source;
        return 0U;
    }
    while (*cursor == ' ' || *cursor == '\t') ++cursor;
    if (radix == 16U && cursor[0] == '0' &&
        (cursor[1] == 'x' || cursor[1] == 'X')) cursor += 2;
    while (*cursor != '\0') {
        NvU32 digit;
        if (*cursor >= '0' && *cursor <= '9') digit = (NvU32)(*cursor - '0');
        else if (*cursor >= 'a' && *cursor <= 'f') digit =
            (NvU32)(*cursor - 'a' + 10);
        else if (*cursor >= 'A' && *cursor <= 'F') digit =
            (NvU32)(*cursor - 'A' + 10);
        else break;
        if (digit >= radix || value > (UINT64_MAX - digit) / radix) break;
        value = value * radix + digit;
        ++cursor;
    }
    if (end != NULL) *end = (char *)cursor;
    return (NvU32)value;
}

NvS32 NV_API_CALL os_string_compare(const char *left, const char *right) {
    return (NvS32)nvabi_string_compare(left, right);
}

NvS32 NV_API_CALL os_vsnprintf(char *destination, NvU32 capacity,
                               const char *format, va_list arguments) {
    return (NvS32)nvabi_vsnprintf(destination, capacity, format, arguments);
}

NvS32 NV_API_CALL os_snprintf(char *destination, NvU32 capacity,
                              const char *format, ...) {
    va_list arguments;
    NvS32 result;
    va_start(arguments, format);
    result = os_vsnprintf(destination, capacity, format, arguments);
    va_end(arguments);
    return result;
}

void NV_API_CALL os_log_error(const char *format, va_list arguments) {
    char buffer[512];
    if (format == NULL) {
        nvabi_log_invalid("os_log_error", "os-log");
        return;
    }
    (void)os_vsnprintf(buffer, sizeof(buffer), format, arguments);
    kernel_serial_write(buffer);
    kernel_serial_write("\r\n");
}

void *NV_API_CALL os_mem_copy(void *destination, const void *source, NvU32 size) {
    return nvabi_memcpy(destination, source, size);
}

NV_STATUS NV_API_CALL os_memcpy_from_user(void *destination,
                                          const void *source, NvU32 size) {
    return nvabi_status_from_kstatus(nv_port_copy_from_user(
        destination, source, size));
}

NV_STATUS NV_API_CALL os_memcpy_to_user(void *destination, const void *source,
                                        NvU32 size) {
    return nvabi_status_from_kstatus(nv_port_copy_to_user(
        destination, source, size));
}

void *NV_API_CALL os_mem_set(void *destination, NvU8 value, NvU32 size) {
    return nvabi_memset(destination, value, size);
}

NvS32 NV_API_CALL os_mem_cmp(const NvU8 *left, const NvU8 *right, NvU32 size) {
    return (NvS32)nvabi_memcmp(left, right, size);
}

NV_STATUS NV_API_CALL os_flush_cpu_cache_all(void) {
    nv_port_cache_flush(NULL, 0U);
    nv_port_mb();
    return NV_OK;
}

NV_STATUS NV_API_CALL os_flush_user_cache(void) {
    nv_port_cache_flush(NULL, 0U);
    nv_port_mb();
    return NV_OK;
}

void NV_API_CALL os_flush_cpu_write_combine_buffer(void) {
    nv_port_wc_drain();
}

NvU8 NV_API_CALL os_io_read_byte(NvU32 port) {
    NvU8 value;
    __asm__ volatile ("inb %1, %0" : "=a"(value) : "Nd"((NvU16)port));
    return value;
}

NvU16 NV_API_CALL os_io_read_word(NvU32 port) {
    NvU16 value;
    __asm__ volatile ("inw %1, %0" : "=a"(value) : "Nd"((NvU16)port));
    return value;
}

NvU32 NV_API_CALL os_io_read_dword(NvU32 port) {
    NvU32 value;
    __asm__ volatile ("inl %1, %0" : "=a"(value) : "Nd"((NvU16)port));
    return value;
}

void NV_API_CALL os_io_write_byte(NvU32 port, NvU8 value) {
    __asm__ volatile ("outb %0, %1" : : "a"(value), "Nd"((NvU16)port));
}

void NV_API_CALL os_io_write_word(NvU32 port, NvU16 value) {
    __asm__ volatile ("outw %0, %1" : : "a"(value), "Nd"((NvU16)port));
}

void NV_API_CALL os_io_write_dword(NvU32 port, NvU32 value) {
    __asm__ volatile ("outl %0, %1" : : "a"(value), "Nd"((NvU16)port));
}

NvBool NV_API_CALL os_is_administrator(void) {
    return NV_TRUE;
}

NvBool NV_API_CALL os_check_access(RsAccessRight accessRight) {
    (void)accessRight;
    return NV_TRUE;
}

void NV_API_CALL os_dbg_init(void) {}

void NV_API_CALL os_dbg_breakpoint(void) {
    nvabi_unsupported_void("os_dbg_breakpoint", "os-debug");
}

void NV_API_CALL os_dbg_set_level(NvU32 level) {
    (void)level;
}

NvU32 NV_API_CALL os_get_cpu_count(void) {
    NvU32 count = x86_smp_discovered_count();
    return count == 0U ? 1U : count;
}

NvU32 NV_API_CALL os_get_cpu_number(void) {
    return x86_current_cpu_index();
}

void NV_API_CALL os_disable_console_access(void) {
    nvabi_unsupported_void("os_disable_console_access", "os-console");
}

void NV_API_CALL os_enable_console_access(void) {
    nvabi_unsupported_void("os_enable_console_access", "os-console");
}

NV_STATUS NV_API_CALL os_registry_init(void) {
    bool expected = false;
    if (atomic_compare_exchange_strong_explicit(
            &g_nvabi_registry_initialized, &expected, true,
            memory_order_acq_rel, memory_order_acquire)) {
        nv_port_registry_init(&g_nvabi_registry);
    }
    return NV_OK;
}

NvU64 NV_API_CALL os_get_max_user_va(void) {
    return X86_64_USER_TOP;
}

NV_STATUS NV_API_CALL os_schedule(void) {
    schedule();
    return NV_OK;
}

NV_STATUS NV_API_CALL os_alloc_spinlock(void **lock) {
    if (lock == NULL) return nvabi_invalid_status(
        "os_alloc_spinlock", "os-sync");
    nvabi_spinlock_t *value = (nvabi_spinlock_t *)kzalloc(
        sizeof(*value), 0U);
    if (value == NULL) return NV_ERR_NO_MEMORY;
    value->magic = NVABI_SPINLOCK_MAGIC;
    nv_port_spin_init(&value->lock);
    *lock = value;
    return NV_OK;
}

void NV_API_CALL os_free_spinlock(void *lock) {
    nvabi_spinlock_t *value = (nvabi_spinlock_t *)lock;
    if (!nvabi_magic(value == NULL ? NULL : &value->magic,
                     NVABI_SPINLOCK_MAGIC, "os_free_spinlock")) return;
    value->magic = 0U;
    kfree(value);
}

NvU64 NV_API_CALL os_acquire_spinlock(void *lock) {
    nvabi_spinlock_t *value = (nvabi_spinlock_t *)lock;
    if (!nvabi_magic(value == NULL ? NULL : &value->magic,
                     NVABI_SPINLOCK_MAGIC, "os_acquire_spinlock")) return 0U;
    return nv_port_spin_lock_irqsave(&value->lock);
}

void NV_API_CALL os_release_spinlock(void *lock, NvU64 flags) {
    nvabi_spinlock_t *value = (nvabi_spinlock_t *)lock;
    if (!nvabi_magic(value == NULL ? NULL : &value->magic,
                     NVABI_SPINLOCK_MAGIC, "os_release_spinlock")) return;
    nv_port_spin_unlock_irqrestore(&value->lock, flags);
}

NV_STATUS NV_API_CALL os_alloc_mutex(void **mutex) {
    if (mutex == NULL) return nvabi_invalid_status(
        "os_alloc_mutex", "os-sync");
    nvabi_mutex_t *value = (nvabi_mutex_t *)kzalloc(sizeof(*value), 0U);
    if (value == NULL) return NV_ERR_NO_MEMORY;
    value->magic = NVABI_MUTEX_MAGIC;
    nv_port_semaphore_init(&value->semaphore, 1U);
    *mutex = value;
    return NV_OK;
}

void NV_API_CALL os_free_mutex(void *mutex) {
    nvabi_mutex_t *value = (nvabi_mutex_t *)mutex;
    if (!nvabi_magic(value == NULL ? NULL : &value->magic,
                     NVABI_MUTEX_MAGIC, "os_free_mutex")) return;
    value->magic = 0U;
    kfree(value);
}

NV_STATUS NV_API_CALL os_acquire_mutex(void *mutex) {
    nvabi_mutex_t *value = (nvabi_mutex_t *)mutex;
    if (!nvabi_magic(value == NULL ? NULL : &value->magic,
                     NVABI_MUTEX_MAGIC, "os_acquire_mutex")) {
        return NV_ERR_INVALID_ARGUMENT;
    }
    return nvabi_status_from_kstatus(nv_port_semaphore_down(
        &value->semaphore, UINT64_MAX));
}

NV_STATUS NV_API_CALL os_cond_acquire_mutex(void *mutex) {
    nvabi_mutex_t *value = (nvabi_mutex_t *)mutex;
    if (!nvabi_magic(value == NULL ? NULL : &value->magic,
                     NVABI_MUTEX_MAGIC, "os_cond_acquire_mutex")) {
        return NV_ERR_INVALID_ARGUMENT;
    }
    kstatus_t status = nv_port_semaphore_down(&value->semaphore, 0U);
    return status == K_ETIMEDOUT ? NV_ERR_TIMEOUT_RETRY :
                                   nvabi_status_from_kstatus(status);
}

void NV_API_CALL os_release_mutex(void *mutex) {
    nvabi_mutex_t *value = (nvabi_mutex_t *)mutex;
    if (!nvabi_magic(value == NULL ? NULL : &value->magic,
                     NVABI_MUTEX_MAGIC, "os_release_mutex")) return;
    nv_port_semaphore_up(&value->semaphore);
}

void *NV_API_CALL os_alloc_semaphore(NvU32 initial_count) {
    nvabi_semaphore_t *value = (nvabi_semaphore_t *)kzalloc(
        sizeof(*value), 0U);
    if (value == NULL) return NULL;
    value->magic = NVABI_SEMAPHORE_MAGIC;
    nv_port_semaphore_init(&value->semaphore, initial_count);
    return value;
}

void NV_API_CALL os_free_semaphore(void *semaphore) {
    nvabi_semaphore_t *value = (nvabi_semaphore_t *)semaphore;
    if (!nvabi_magic(value == NULL ? NULL : &value->magic,
                     NVABI_SEMAPHORE_MAGIC, "os_free_semaphore")) return;
    value->magic = 0U;
    kfree(value);
}

NV_STATUS NV_API_CALL os_acquire_semaphore(void *semaphore) {
    nvabi_semaphore_t *value = (nvabi_semaphore_t *)semaphore;
    if (!nvabi_magic(value == NULL ? NULL : &value->magic,
                     NVABI_SEMAPHORE_MAGIC, "os_acquire_semaphore")) {
        return NV_ERR_INVALID_ARGUMENT;
    }
    return nvabi_status_from_kstatus(nv_port_semaphore_down(
        &value->semaphore, UINT64_MAX));
}

NV_STATUS NV_API_CALL os_cond_acquire_semaphore(void *semaphore) {
    nvabi_semaphore_t *value = (nvabi_semaphore_t *)semaphore;
    if (!nvabi_magic(value == NULL ? NULL : &value->magic,
                     NVABI_SEMAPHORE_MAGIC, "os_cond_acquire_semaphore")) {
        return NV_ERR_INVALID_ARGUMENT;
    }
    return nvabi_status_from_kstatus(nv_port_semaphore_down(
        &value->semaphore, 0U));
}

NV_STATUS NV_API_CALL os_release_semaphore(void *semaphore) {
    nvabi_semaphore_t *value = (nvabi_semaphore_t *)semaphore;
    if (!nvabi_magic(value == NULL ? NULL : &value->magic,
                     NVABI_SEMAPHORE_MAGIC, "os_release_semaphore")) {
        return NV_ERR_INVALID_ARGUMENT;
    }
    nv_port_semaphore_up(&value->semaphore);
    return NV_OK;
}

void *NV_API_CALL os_alloc_rwlock(void) {
    nvabi_rwlock_t *value = (nvabi_rwlock_t *)kzalloc(sizeof(*value), 0U);
    if (value == NULL) return NULL;
    value->magic = NVABI_RWLOCK_MAGIC;
    nv_port_rwlock_init(&value->lock);
    return value;
}

void NV_API_CALL os_free_rwlock(void *lock) {
    nvabi_rwlock_t *value = (nvabi_rwlock_t *)lock;
    if (!nvabi_magic(value == NULL ? NULL : &value->magic,
                     NVABI_RWLOCK_MAGIC, "os_free_rwlock")) return;
    value->magic = 0U;
    kfree(value);
}

NV_STATUS NV_API_CALL os_acquire_rwlock_read(void *lock) {
    nvabi_rwlock_t *value = (nvabi_rwlock_t *)lock;
    if (!nvabi_magic(value == NULL ? NULL : &value->magic,
                     NVABI_RWLOCK_MAGIC, "os_acquire_rwlock_read")) {
        return NV_ERR_INVALID_ARGUMENT;
    }
    nv_port_rwlock_read_lock(&value->lock);
    return NV_OK;
}

NV_STATUS NV_API_CALL os_acquire_rwlock_write(void *lock) {
    nvabi_rwlock_t *value = (nvabi_rwlock_t *)lock;
    if (!nvabi_magic(value == NULL ? NULL : &value->magic,
                     NVABI_RWLOCK_MAGIC, "os_acquire_rwlock_write")) {
        return NV_ERR_INVALID_ARGUMENT;
    }
    nv_port_rwlock_write_lock(&value->lock);
    return NV_OK;
}

NV_STATUS NV_API_CALL os_cond_acquire_rwlock_read(void *lock) {
    nvabi_rwlock_t *value = (nvabi_rwlock_t *)lock;
    if (!nvabi_magic(value == NULL ? NULL : &value->magic,
                     NVABI_RWLOCK_MAGIC, "os_cond_acquire_rwlock_read")) {
        return NV_ERR_INVALID_ARGUMENT;
    }
    return nvabi_unsupported_status("os_cond_acquire_rwlock_read", "os-sync");
}

NV_STATUS NV_API_CALL os_cond_acquire_rwlock_write(void *lock) {
    nvabi_rwlock_t *value = (nvabi_rwlock_t *)lock;
    if (!nvabi_magic(value == NULL ? NULL : &value->magic,
                     NVABI_RWLOCK_MAGIC, "os_cond_acquire_rwlock_write")) {
        return NV_ERR_INVALID_ARGUMENT;
    }
    return nvabi_unsupported_status("os_cond_acquire_rwlock_write", "os-sync");
}

void NV_API_CALL os_release_rwlock_read(void *lock) {
    nvabi_rwlock_t *value = (nvabi_rwlock_t *)lock;
    if (!nvabi_magic(value == NULL ? NULL : &value->magic,
                     NVABI_RWLOCK_MAGIC, "os_release_rwlock_read")) return;
    nv_port_rwlock_read_unlock(&value->lock);
}

void NV_API_CALL os_release_rwlock_write(void *lock) {
    nvabi_rwlock_t *value = (nvabi_rwlock_t *)lock;
    if (!nvabi_magic(value == NULL ? NULL : &value->magic,
                     NVABI_RWLOCK_MAGIC, "os_release_rwlock_write")) return;
    nv_port_rwlock_write_unlock(&value->lock);
}

NvBool NV_API_CALL os_semaphore_may_sleep(void) {
    return NV_TRUE;
}

NV_STATUS NV_API_CALL os_get_version_info(os_version_info *info) {
    static const char version[] = "System NVIDIA ABI 595.84";
    static const char date[] = __DATE__ " " __TIME__;
    if (info == NULL) return nvabi_invalid_status(
        "os_get_version_info", "os-version");
    info->os_major_version = 1U;
    info->os_minor_version = 0U;
    info->os_build_number = 0U;
    info->os_build_version_str = version;
    info->os_build_date_plus_str = date;
    return NV_OK;
}

NV_STATUS NV_API_CALL os_get_is_openrm(NvBool *is_openrm) {
    if (is_openrm == NULL) return nvabi_invalid_status(
        "os_get_is_openrm", "os-version");
    *is_openrm = NV_TRUE;
    return NV_OK;
}

NvBool NV_API_CALL os_is_bif_reset_supported(void *context) {
    (void)context;
    return nvabi_unsupported_bool("os_is_bif_reset_supported", "os-pci");
}

NvBool NV_API_CALL os_is_isr(void) {
    return NV_FALSE;
}

NvBool NV_API_CALL os_pat_supported(void) {
    return NV_TRUE;
}

void NV_API_CALL os_dump_stack(void) {
    nvabi_unsupported_void("os_dump_stack", "os-debug");
}

NvBool NV_API_CALL os_is_efi_enabled(void) {
    return NV_TRUE;
}

NvBool NV_API_CALL os_is_xen_dom0(void) {
    return NV_FALSE;
}

NvBool NV_API_CALL os_is_vgx_hyper(void) {
    return NV_FALSE;
}

NV_STATUS NV_API_CALL os_inject_vgx_msi(NvU16 vector, NvU64 address,
                                        NvU32 data) {
    (void)vector;
    (void)address;
    (void)data;
    return nvabi_unsupported_status("os_inject_vgx_msi", "os-virtualization");
}

NvBool NV_API_CALL os_is_grid_supported(void) {
    return NV_FALSE;
}

NvU32 NV_API_CALL os_get_grid_csp_support(void) {
    return 0U;
}

void NV_API_CALL os_bug_check(NvU32 code, const char *message) {
    (void)code;
    kernel_serial_write("OS_NVIDIA_ABI_BUG_CHECK ");
    kernel_serial_write(message == NULL ? "<null>" : message);
    kernel_serial_write(" STAGE=os-debug\r\n");
}

NV_STATUS NV_API_CALL os_get_euid(NvU32 *euid) {
    if (euid == NULL) return nvabi_invalid_status("os_get_euid", "os-process");
    *euid = 0U;
    return NV_OK;
}

NvU32 NV_API_CALL os_get_current_process_flags(void) {
    return nvabi_current_process() == NULL ? OS_CURRENT_PROCESS_FLAG_KERNEL_THREAD :
                                             OS_CURRENT_PROCESS_FLAG_NONE;
}

NV_STATUS NV_API_CALL os_alloc_wait_queue(os_wait_queue **queue) {
    if (queue == NULL) return nvabi_invalid_status(
        "os_alloc_wait_queue", "os-wait");
    nvabi_wait_queue_t *value = (nvabi_wait_queue_t *)kzalloc(
        sizeof(*value), 0U);
    if (value == NULL) return NV_ERR_NO_MEMORY;
    value->magic = NVABI_WAIT_QUEUE_MAGIC;
    nv_port_completion_init(&value->completion);
    *queue = (os_wait_queue *)value;
    return NV_OK;
}

void NV_API_CALL os_free_wait_queue(os_wait_queue *queue) {
    nvabi_wait_queue_t *value = (nvabi_wait_queue_t *)queue;
    if (!nvabi_magic(value == NULL ? NULL : &value->magic,
                     NVABI_WAIT_QUEUE_MAGIC, "os_free_wait_queue")) return;
    value->magic = 0U;
    kfree(value);
}

void NV_API_CALL os_wait_uninterruptible(os_wait_queue *queue) {
    nvabi_wait_queue_t *value = (nvabi_wait_queue_t *)queue;
    if (!nvabi_magic(value == NULL ? NULL : &value->magic,
                     NVABI_WAIT_QUEUE_MAGIC, "os_wait_uninterruptible")) return;
    if (nv_port_completion_wait(&value->completion, UINT64_MAX) != K_OK) {
        nvabi_log_invalid("os_wait_uninterruptible", "os-wait");
    }
}

void NV_API_CALL os_wait_interruptible(os_wait_queue *queue) {
    os_wait_uninterruptible(queue);
}

void NV_API_CALL os_wake_up(os_wait_queue *queue) {
    nvabi_wait_queue_t *value = (nvabi_wait_queue_t *)queue;
    if (!nvabi_magic(value == NULL ? NULL : &value->magic,
                     NVABI_WAIT_QUEUE_MAGIC, "os_wake_up")) return;
    nv_port_completion_signal(&value->completion);
}

NV_STATUS NV_API_CALL os_get_tegra_platform(NvU32 *platform) {
    (void)platform;
    return nvabi_unsupported_status("os_get_tegra_platform", "os-tegra");
}

NV_STATUS NV_API_CALL os_enable_pci_req_atomics(
    void *handle, enum os_pci_req_atomics_type type) {
    (void)handle;
    (void)type;
    return nvabi_unsupported_status("os_enable_pci_req_atomics", "os-pci");
}

void NV_API_CALL os_pci_trigger_flr(void *handle) {
    (void)handle;
    nvabi_unsupported_void("os_pci_trigger_flr", "os-pci");
}

NvU64 NV_API_CALL os_get_reclaimable_memory_usage(void) {
    nvabi_log_unsupported("os_get_reclaimable_memory_usage", "os-memory");
    return UINT64_MAX;
}

NvBool NV_API_CALL os_is_nvswitch_present(void) {
    return NV_FALSE;
}
