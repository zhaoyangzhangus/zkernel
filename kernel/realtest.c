#include <kernel/console.h>
#include <kernel/fat32.h>
#include <kernel/bootinfo.h>
#include <kernel/realtest.h>
#include <kernel/vfs.h>
#include <arch/x86_64/cpu.h>
#include <arch/x86_64/reboot.h>

#include <stdatomic.h>

#ifndef OS_REALTEST
#define OS_REALTEST 0
#endif

#if OS_REALTEST

#define REALTEST_LOG_CAPACITY (256U * 1024U)
#define REALTEST_WRITE_CHUNK  4096U
#define REALTEST_STATE_CAPACITY 8192U
#define REALTEST_HARD_TIMEOUT_NS 300000000000ULL

static char g_realtest_log[REALTEST_LOG_CAPACITY];
static char g_realtest_chunk[REALTEST_WRITE_CHUNK];
static size_t g_realtest_length;
static size_t g_realtest_flushed;
static bool g_realtest_overflow;
static bool g_realtest_flush_busy;
static bool g_realtest_sealed;
static file_t *g_realtest_file;
static OS_FAT32_FILE *g_realtest_raw_file;
static OS_FAT32 *g_realtest_raw_filesystem;
static uint64_t g_realtest_raw_file_offset;
static EFI_SET_VARIABLE g_realtest_set_variable;
static bool g_realtest_runtime_services_enabled;
static bool g_realtest_specific_failure_state;
static uint32_t g_realtest_flush_count;
static atomic_flag g_realtest_lock = ATOMIC_FLAG_INIT;
static char g_realtest_state[REALTEST_STATE_CAPACITY];
static size_t g_realtest_state_length;
/* 独立运行映像会将静态原子变量初始化为零；此工具链提供 ATOMIC_FLAG_INIT，但不提供 ATOMIC_VAR_INIT。 */
static atomic_bool g_realtest_timeout_active;
static atomic_bool g_realtest_timeout_rebooting;
static _Atomic uint64_t g_realtest_timeout_deadline_tsc;

static const EFI_GUID g_realtest_variable_guid = {
    0x4C495445U, 0x4F53U, 0x5254U,
    {0x45U, 0x53U, 0x54U, 0x43U, 0x55U, 0x52U, 0x4EU, 0x31U}
};
static CHAR16 g_realtest_variable_name[] = {
    'L', 'i', 't', 'e', 'O', 'S', 'R', 'e', 'a', 'l', 'T', 'e', 's', 't',
    'S', 't', 'a', 't', 'e', 0
};

static uint64_t realtest_lock(void) {
    uint64_t flags;
    __asm__ volatile ("pushfq; popq %0" : "=r"(flags) : : "memory");
    __asm__ volatile ("cli" : : : "memory");
    while (atomic_flag_test_and_set_explicit(&g_realtest_lock,
                                             memory_order_acquire)) {
        __asm__ volatile ("pause");
    }
    return flags;
}

static void realtest_unlock(uint64_t flags) {
    atomic_flag_clear_explicit(&g_realtest_lock, memory_order_release);
    if ((flags & (1ULL << 9)) != 0U) {
        __asm__ volatile ("sti" : : : "memory");
    }
}

static void append_text(char *buffer, size_t *length, size_t capacity,
                        const char *text) {
    if (buffer == 0 || length == 0 || text == 0 ||
        capacity == 0U || *length >= capacity) return;
    while (*text != '\0' && *length < capacity - 1U) {
        buffer[(*length)++] = *text++;
    }
    buffer[*length] = '\0';
}

static void append_hex(char *buffer, size_t *length, size_t capacity,
                       uint64_t value) {
    if (buffer == 0 || length == 0 || capacity == 0U ||
        *length >= capacity) return;
    static const char digits[] = "0123456789ABCDEF";
    append_text(buffer, length, capacity, "0x");
    for (int shift = 60; shift >= 0 && *length < capacity - 1U;
         shift -= 4) {
        buffer[(*length)++] = digits[(value >> (uint32_t)shift) & 0xFU];
    }
    buffer[*length] = '\0';
}

static void append_u32(char *buffer, size_t *length, size_t capacity,
                       uint32_t value) {
    if (buffer == 0 || length == 0 || capacity == 0U ||
        *length >= capacity) return;
    char digits[10];
    uint32_t count = 0U;
    do {
        digits[count++] = (char)('0' + value % 10U);
        value /= 10U;
    } while (value != 0U && count < sizeof(digits));
    while (count != 0U && *length < capacity - 1U) {
        buffer[(*length)++] = digits[--count];
    }
    buffer[*length] = '\0';
}

bool realtest_enabled(void) {
    return true;
}

void realtest_timeout_start(void) {
    uint64_t now_tsc;
    uint64_t timeout_tsc;

    /* x86_timeout_ns_to_tsc() 要求 CPU 频率已初始化。CPU 检测先于高半区运行时续接执行，但仍保留此检查，
     * 避免异常早期故障导致看门狗触发除零错误。 */
    if (x86_boot_cpu_features.tsc_hz == 0U) {
        atomic_store_explicit(&g_realtest_timeout_active, false,
                              memory_order_release);
        return;
    }
    now_tsc = x86_read_tsc();
    timeout_tsc = x86_timeout_ns_to_tsc(REALTEST_HARD_TIMEOUT_NS);
    if (timeout_tsc == 0U || timeout_tsc == UINT64_MAX ||
        timeout_tsc > UINT64_MAX - now_tsc) {
        atomic_store_explicit(&g_realtest_timeout_active, false,
                              memory_order_release);
        return;
    }
    atomic_store_explicit(&g_realtest_timeout_rebooting, false,
                          memory_order_release);
    atomic_store_explicit(&g_realtest_timeout_deadline_tsc,
                          now_tsc + timeout_tsc, memory_order_release);
    atomic_store_explicit(&g_realtest_timeout_active, true,
                          memory_order_release);
}

void realtest_timeout_stop(void) {
    atomic_store_explicit(&g_realtest_timeout_active, false,
                          memory_order_release);
}

bool realtest_timeout_poll(uint64_t now_tsc) {
    uint64_t deadline;

    if (!atomic_load_explicit(&g_realtest_timeout_active,
                              memory_order_acquire) ||
        atomic_load_explicit(&g_realtest_timeout_rebooting,
                             memory_order_acquire)) {
        return false;
    }
    deadline = atomic_load_explicit(&g_realtest_timeout_deadline_tsc,
                                    memory_order_acquire);
    if ((int64_t)(now_tsc - deadline) < 0) return false;
    return atomic_exchange_explicit(&g_realtest_timeout_rebooting, true,
                                    memory_order_acq_rel) == false;
}

__attribute__((noreturn)) void realtest_timeout_reboot(void) {
    /* 此函数可能从 LAPIC 硬中断调用。不要调用 kernel_serial_write_serial_only()：它会写入 FAT 日志，
     * 并获取可能由被中断线程持有的 real-test 锁。启用串口调试时，应使用受保护的原始 COM1 路径。 */
    atomic_store_explicit(&g_realtest_timeout_active, false,
                          memory_order_release);
    kernel_serial_write_guarded(
        "OS_REALTEST_TIMEOUT_FAIL\r\n"
        "OS_REALTEST_TIMEOUT_REBOOTING\r\n");
    x86_system_reboot();
}

void realtest_boot_info(const struct os_boot_info *info) {
    const OS_BOOT_INFO *boot_info = (const OS_BOOT_INFO *)info;
    EFI_RUNTIME_SERVICES *services;
    if (boot_info == 0 || boot_info->RuntimeServices == 0 ||
        boot_info->RuntimeServices > UINT32_MAX) return;
    services = (EFI_RUNTIME_SERVICES *)(uintptr_t)boot_info->RuntimeServices;
    if (services->SetVariable == 0 ||
        (UINT64)(uintptr_t)services->SetVariable > UINT32_MAX) return;
    g_realtest_set_variable = services->SetVariable;
    g_realtest_runtime_services_enabled = true;
}

void realtest_mark(const char *state) {
    char data[REALTEST_STATE_CAPACITY];
    size_t state_length = 0U;
    if (!g_realtest_runtime_services_enabled || g_realtest_set_variable == 0 ||
        state == 0) return;
    while (state[state_length] != '\0') {
        ++state_length;
    }
    if (state_length + 2U >= sizeof(g_realtest_state) ||
        g_realtest_state_length + state_length + 2U >=
            sizeof(g_realtest_state)) return;
    for (size_t index = 0U; index < state_length; ++index) {
        g_realtest_state[g_realtest_state_length++] = state[index];
    }
    g_realtest_state[g_realtest_state_length++] = '\r';
    g_realtest_state[g_realtest_state_length++] = '\n';
    for (size_t index = 0U; index < g_realtest_state_length; ++index) {
        data[index] = g_realtest_state[index];
    }
    (void)g_realtest_set_variable(
        g_realtest_variable_name, (EFI_GUID *)&g_realtest_variable_guid,
        EFI_VARIABLE_NON_VOLATILE | EFI_VARIABLE_BOOTSERVICE_ACCESS |
            EFI_VARIABLE_RUNTIME_ACCESS,
        (UINTN)g_realtest_state_length, data);
}

void realtest_checkpoint(const char *state) {
    if (state == 0) return;
    realtest_mark(state);
    realtest_capture(state);
    realtest_capture("\r\n");
    (void)realtest_flush();
}

void realtest_checkpoint_deferred(const char *state) {
    if (state == 0) return;
    /* RM/GSP 初始化时可能开启设备中断，也可能持有厂商锁。此路径仅操作内存：UEFI SetVariable 和 FAT/VFS
     * 同步会在厂商调用返回后，由运行时检查点执行。 */
    realtest_capture(state);
    realtest_capture("\r\n");
}

void realtest_mark_number(const char *prefix, uint32_t value) {
    char state[128] = {0};
    size_t length = 0U;
    append_text(state, &length, sizeof(state), prefix != 0 ? prefix : "STATE");
    append_text(state, &length, sizeof(state), "=");
    append_u32(state, &length, sizeof(state), value);
    realtest_mark(state);
}

void realtest_clear_failure_state(void) {
    g_realtest_specific_failure_state = false;
}

void realtest_mark_value(const char *prefix, uint32_t value) {
    char state[128] = {0};
    size_t length = 0U;
    if (g_realtest_specific_failure_state) return;
    append_text(state, &length, sizeof(state), prefix != 0 ? prefix : "STATE");
    append_text(state, &length, sizeof(state), "=");
    append_u32(state, &length, sizeof(state), value);
    g_realtest_specific_failure_state = true;
    realtest_mark(state);
}

void realtest_mark_xhci_control(const char *kind, uint32_t error,
                                       uint32_t slot, uint32_t port,
                                       uint32_t length, uint32_t event_index,
                                       uint32_t event_cycle) {
    char state[256] = {0};
    size_t state_length = 0U;
    if (g_realtest_specific_failure_state) return;
    append_text(state, &state_length, sizeof(state), "FAILURE_XHCI_EP0_");
    append_text(state, &state_length, sizeof(state), kind != 0 ? kind : "ERROR");
    append_text(state, &state_length, sizeof(state), " error=");
    append_u32(state, &state_length, sizeof(state), error);
    append_text(state, &state_length, sizeof(state), " slot=");
    append_u32(state, &state_length, sizeof(state), slot);
    append_text(state, &state_length, sizeof(state), " port=");
    append_u32(state, &state_length, sizeof(state), port);
    append_text(state, &state_length, sizeof(state), " length=");
    append_u32(state, &state_length, sizeof(state), length);
    append_text(state, &state_length, sizeof(state), " event_index=");
    append_u32(state, &state_length, sizeof(state), event_index);
    append_text(state, &state_length, sizeof(state), " event_cycle=");
    append_u32(state, &state_length, sizeof(state), event_cycle);
    g_realtest_specific_failure_state = true;
    realtest_mark(state);
}

void realtest_disable_runtime_services(void) {
    g_realtest_runtime_services_enabled = false;
    g_realtest_set_variable = 0;
}

static bool raw_file_ready(OS_FAT32 *filesystem) {
    os_file_info_t info = {0};
    if (filesystem == 0 || g_realtest_file != 0 || g_realtest_raw_file != 0 ||
        !fat32_stat_path(filesystem, "log/realtest.log", &info) ||
        info.type != OS_FILE_TYPE_REGULAR) {
        return false;
    }
    if (!fat32_open(filesystem, "log/realtest.log",
                           &g_realtest_raw_file)) {
        return false;
    }
    g_realtest_raw_filesystem = filesystem;
    g_realtest_raw_file_offset = info.size;
    return true;
}

void realtest_capture(const char *text) {
    uint64_t flags;
    if (text == 0) return;
    flags = realtest_lock();
    if (g_realtest_sealed) {
        realtest_unlock(flags);
        return;
    }
    while (*text != '\0') {
        if (g_realtest_length + 1U >= sizeof(g_realtest_log)) {
            g_realtest_overflow = true;
            break;
        }
        g_realtest_log[g_realtest_length++] = *text++;
    }
    g_realtest_log[g_realtest_length] = '\0';
    realtest_unlock(flags);
}

static bool open_realtest_file(void) {
    static const char *const paths[] = {
        "/log/realtest.log",
    };
    /* 调度器和设备工作线程启动后，早期 FAT 句柄仍是依赖最少的日志路径。继续使用它，不要改用可能共享
     * 页缓存锁的 VFS 文件。 */
    if (g_realtest_raw_filesystem != 0) return true;
    if (g_realtest_file != 0) return true;
    for (uint32_t index = 0U; index < sizeof(paths) / sizeof(paths[0]);
         ++index) {
        file_t *file = 0;
        if (vfs_open_kernel(paths[index],
                            VFS_OPEN_WRITE | VFS_OPEN_CREATE | VFS_OPEN_APPEND,
                            0644U, &file) == K_OK) {
            g_realtest_file = file;
            return true;
        }
    }
    return false;
}

bool realtest_flush(void) {
    bool success = true;
    uint64_t flags;
    if (g_realtest_file == 0 && g_realtest_raw_filesystem == 0) return false;
    flags = realtest_lock();
    if (g_realtest_flush_busy || g_realtest_flushed >= g_realtest_length) {
        realtest_unlock(flags);
        return true;
    }
    g_realtest_flush_busy = true;
    realtest_unlock(flags);
    ++g_realtest_flush_count;
    realtest_mark_number("REALTEST_FLUSH_BEGIN", g_realtest_flush_count);

    for (;;) {
        size_t offset;
        size_t count;
        uint64_t written = 0U;
        bool write_ok;
        flags = realtest_lock();
        if (g_realtest_flushed >= g_realtest_length) {
            realtest_unlock(flags);
            break;
        }
        offset = g_realtest_flushed;
        count = g_realtest_length - offset;
        if (count > sizeof(g_realtest_chunk)) count = sizeof(g_realtest_chunk);
        for (size_t index = 0U; index < count; ++index) {
            g_realtest_chunk[index] = g_realtest_log[offset + index];
        }
        realtest_unlock(flags);

        if (g_realtest_file != 0) {
            write_ok = vfs_write_kernel(g_realtest_file, g_realtest_chunk, count,
                                        &written) == K_OK;
        } else if (g_realtest_raw_file != 0) {
            uint32_t raw_written = 0U;
            write_ok = fat32_write_file(
                g_realtest_raw_file, g_realtest_raw_file_offset + offset,
                g_realtest_chunk, (uint32_t)count, &raw_written);
            written = raw_written;
        } else {
            uint32_t raw_written = 0U;
            write_ok = fat32_write_path(
                g_realtest_raw_filesystem, "log/realtest.log",
                g_realtest_raw_file_offset + offset, g_realtest_chunk,
                (uint32_t)count, &raw_written);
            written = raw_written;
        }
        if (!write_ok || written != count) {
            kernel_serial_write_serial_only(
                "OS_REALTEST_FLUSH_WRITE_FAIL\r\n");
            success = false;
            break;
        }
        flags = realtest_lock();
        if (g_realtest_flushed == offset) g_realtest_flushed += count;
        realtest_unlock(flags);
    }
    if (success) {
        realtest_mark_number("REALTEST_FLUSH_WRITE_OK",
                                    g_realtest_flush_count);
        realtest_mark_number("REALTEST_FLUSH_SYNC_BEGIN",
                                    g_realtest_flush_count);
        bool sync_ok = g_realtest_file != 0 ?
            vfs_fsync(g_realtest_file) == K_OK :
            fat32_sync(g_realtest_raw_filesystem);
        if (!sync_ok) {
            kernel_serial_write_serial_only(
                "OS_REALTEST_FLUSH_SYNC_FAIL\r\n");
            success = false;
        } else {
            realtest_mark_number("REALTEST_FLUSH_DONE",
                                        g_realtest_flush_count);
        }
    }
    flags = realtest_lock();
    g_realtest_flush_busy = false;
    realtest_unlock(flags);
    return success && !g_realtest_overflow;
}

static void seal_realtest_capture(void) {
    uint64_t flags = realtest_lock();
    g_realtest_sealed = true;
    realtest_unlock(flags);
}

void realtest_fat_ready(struct OS_FAT32 *filesystem) {
    OS_FAT32 *fat = (OS_FAT32 *)filesystem;
    /* Windows 测试框架会在复制 ESP 前删除旧日志。趁 FAT 仍是唯一存储所有者时创建文件，避免运行时日志
     * 写入器回退到 VFS/页缓存路径。 */
    (void)fat32_create_path(fat, "log", 1);
    if (!raw_file_ready(fat)) {
        if (!fat32_create_path(fat, "log/realtest.log", 0) ||
            !raw_file_ready(fat)) {
            realtest_mark("OS_REALTEST_FAT_LOG_CREATE_FAIL");
            return;
        }
        realtest_mark("OS_REALTEST_FAT_LOG_CREATED");
    }
    realtest_capture("OS_REALTEST_FAT_READY\r\n");
    (void)realtest_flush();
}

void realtest_fat_lost(const char *reason) {
    if (g_realtest_raw_filesystem == 0 || g_realtest_file != 0) return;
    realtest_capture("OS_REALTEST_FAT_LOST ");
    realtest_capture(reason != 0 ? reason : "unknown");
    realtest_capture("\r\n");
    (void)realtest_flush();
    if (g_realtest_raw_file != 0) {
        (void)fat32_close(g_realtest_raw_file);
        g_realtest_raw_file = 0;
    }
    g_realtest_raw_filesystem = 0;
    g_realtest_raw_file_offset = 0U;
}

void realtest_filesystem_ready(void) {
    bool flushed;
    if (g_realtest_file != 0 || g_realtest_raw_file != 0 ||
        g_realtest_raw_filesystem != 0) {
        realtest_capture("OS_REALTEST_LOG_READY\r\n");
        flushed = realtest_flush();
        if (flushed) realtest_disable_runtime_services();
        realtest_timeout_start();
        return;
    }
    if (!open_realtest_file()) return;
    realtest_capture("OS_REALTEST_LOG_READY\r\n");
    flushed = realtest_flush();
    if (flushed) realtest_disable_runtime_services();
    realtest_timeout_start();
}

void realtest_record_failure(const char *file, uint32_t line) {
    char record[256] = {0};
    char state[256] = {0};
    size_t length = 0U;
    size_t state_length = 0U;
    append_text(record, &length, sizeof(record), "OS_REALTEST_FAIL");
    append_text(state, &state_length, sizeof(state), "FAILURE");
    if (file != 0) {
        append_text(record, &length, sizeof(record), " loc=");
        append_text(record, &length, sizeof(record), file);
        append_text(record, &length, sizeof(record), ":");
        append_u32(record, &length, sizeof(record), line);
        append_text(state, &state_length, sizeof(state), " ");
        append_text(state, &state_length, sizeof(state), file);
        append_text(state, &state_length, sizeof(state), ":");
        append_u32(state, &state_length, sizeof(state), line);
    }
    append_text(record, &length, sizeof(record), "\r\n");
    if (!g_realtest_specific_failure_state) realtest_mark(state);
    realtest_capture(record);
}

void realtest_record_exception(uint64_t vector, uint64_t error_code,
                                      uint64_t rip, uint64_t fault_address,
                                      bool has_fault_address) {
    char record[256] = {0};
    size_t length = 0U;
    append_text(record, &length, sizeof(record),
                "OS_REALTEST_EXCEPTION vector=");
    append_hex(record, &length, sizeof(record), vector);
    append_text(record, &length, sizeof(record), " error=");
    append_hex(record, &length, sizeof(record), error_code);
    append_text(record, &length, sizeof(record), " rip=");
    append_hex(record, &length, sizeof(record), rip);
    if (has_fault_address) {
        append_text(record, &length, sizeof(record), " cr2=");
        append_hex(record, &length, sizeof(record), fault_address);
    }
    append_text(record, &length, sizeof(record), "\r\n");
    realtest_capture(record);
}

void realtest_finish_success(void) {
    /* 真实硬件部署采用启动后返回的探测方式。复位前持久化最终结果，使主机收集器无需进入桌面即可同时确认
     * 硬件结果和返回 Windows 的状态。 */
    realtest_timeout_stop();
    realtest_mark("OS_REALTEST_PASS");
    realtest_capture("OS_REALTEST_PASS\r\n");
    (void)realtest_flush();
    kernel_serial_write_serial_only("OS_REALTEST_PASS\r\n");
    realtest_capture("OS_REALTEST_REBOOTING\r\n");
    (void)realtest_flush();
    kernel_serial_write_serial_only("OS_REALTEST_REBOOTING\r\n");
    x86_system_reboot();
}

void realtest_finish_failure(void) {
    realtest_timeout_stop();
    realtest_mark("OS_REALTEST_FAIL");
    realtest_capture("OS_REALTEST_FAIL\r\n");
    seal_realtest_capture();
    (void)realtest_flush();
    kernel_serial_write_serial_only("OS_REALTEST_FAIL\r\n");
    x86_system_reboot();
}

#else

bool realtest_enabled(void) { return false; }
void realtest_boot_info(const struct os_boot_info *info) {
    (void)info;
}
void realtest_mark(const char *state) { (void)state; }
void realtest_checkpoint(const char *state) { (void)state; }
void realtest_checkpoint_deferred(const char *state) { (void)state; }
void realtest_mark_number(const char *prefix, uint32_t value) {
    (void)prefix;
    (void)value;
}
void realtest_clear_failure_state(void) { }
void realtest_mark_value(const char *prefix, uint32_t value) {
    (void)prefix;
    (void)value;
}
void realtest_mark_xhci_control(const char *kind, uint32_t error,
                                       uint32_t slot, uint32_t port,
                                       uint32_t length, uint32_t event_index,
                                       uint32_t event_cycle) {
    (void)kind;
    (void)error;
    (void)slot;
    (void)port;
    (void)length;
    (void)event_index;
    (void)event_cycle;
}
void realtest_disable_runtime_services(void) { }
void realtest_capture(const char *text) { (void)text; }
void realtest_fat_ready(struct OS_FAT32 *filesystem) {
    (void)filesystem;
}
void realtest_fat_lost(const char *reason) { (void)reason; }
void realtest_filesystem_ready(void) { }
bool realtest_flush(void) { return false; }
void realtest_record_failure(const char *file, uint32_t line) {
    (void)file;
    (void)line;
}
void realtest_record_exception(uint64_t vector, uint64_t error_code,
                                      uint64_t rip, uint64_t fault_address,
                                      bool has_fault_address) {
    (void)vector;
    (void)error_code;
    (void)rip;
    (void)fault_address;
    (void)has_fault_address;
}
void realtest_timeout_start(void) { }
void realtest_timeout_stop(void) { }
bool realtest_timeout_poll(uint64_t now_tsc) {
    (void)now_tsc;
    return false;
}
void realtest_timeout_reboot(void) {
    x86_system_reboot();
    __builtin_unreachable();
}
void realtest_finish_success(void) { }
void realtest_finish_failure(void) {
    x86_system_reboot();
    __builtin_unreachable();
}

#endif
