#include "internal.h"

#include <arch/x86_64/cpu.h>
#include <kernel/process.h>
#include <kernel/sched.h>
#include <kernel/console.h>
#include <kernel/perf.h>
#include <kernel/telemetry.h>
#include "../process/internal.h"

#ifndef OS_DEBUG_SERIAL
#define OS_DEBUG_SERIAL 0
#endif

#ifndef OS_REALTEST
#define OS_REALTEST 0
#endif

#if OS_DEBUG_SERIAL || OS_REALTEST
#define VM_FAULT_TELEMETRY_ENABLED 1
#else
#define VM_FAULT_TELEMETRY_ENABLED 0
#endif

#define VM_FAULT_HISTOGRAM_BUCKETS 64U

typedef struct vm_slow_fault_diagnostic {
    uint64_t pid, tid, address, error, total_ns, fault_ns, around_ns;
} vm_slow_fault_diagnostic_t;
static volatile vm_slow_fault_diagnostic_t g_vm_slow_faults[32];
static atomic_flag g_vm_slow_fault_guard = ATOMIC_FLAG_INIT;
static volatile uint64_t g_vm_slow_fault_process;
static void vm_record_slow_fault(uint64_t pid, uint64_t tid, uint64_t address,
                                 uint32_t error, uint64_t started,
                                 uint64_t resolved, uint64_t finished) {
    uint64_t elapsed = finished - started;
    if (x86_boot_cpu_features.tsc_hz == 0U ||
        elapsed < x86_boot_cpu_features.tsc_hz / 100U) return;
    if (atomic_flag_test_and_set_explicit(&g_vm_slow_fault_guard,
                                          memory_order_acquire)) return;
    for (size_t index = 0U; index < 32U; ++index) {
        if (g_vm_slow_faults[index].tid != 0U &&
            g_vm_slow_faults[index].tid != tid) continue;
        uint64_t ns = x86_tsc_to_ns(elapsed);
        if (ns > g_vm_slow_faults[index].total_ns)
            g_vm_slow_faults[index] = (vm_slow_fault_diagnostic_t){
                pid, tid, address, error, ns,
                x86_tsc_to_ns(resolved - started),
                x86_tsc_to_ns(finished - resolved)};
        break;
    }
    atomic_flag_clear_explicit(&g_vm_slow_fault_guard, memory_order_release);
}

#define VM_FAULT_DIAG_COM1_DATA   0x3F8U
#define VM_FAULT_DIAG_COM1_STATUS 0x3FDU

static atomic_flag g_vm_fault_diag_lock = ATOMIC_FLAG_INIT;

static void vm_fault_diag_write_char(char value) {
    uint16_t status_port = VM_FAULT_DIAG_COM1_STATUS;
    uint16_t data_port = VM_FAULT_DIAG_COM1_DATA;
    for (uint32_t attempt = 0U; attempt < 100000U; ++attempt) {
        uint8_t ready;
        __asm__ volatile ("inb %w1, %0" : "=a"(ready) : "d"(status_port));
        if ((ready & 0x20U) != 0U) break;
        __asm__ volatile ("pause");
    }
    __asm__ volatile ("outb %0, %w1" : : "a"((uint8_t)value), "d"(data_port));
}

static void vm_fault_diag_write_text(const char *text) {
    while (*text != '\0') vm_fault_diag_write_char(*text++);
}

static void vm_fault_diag_write_hex(uint64_t value) {
    static const char digits[] = "0123456789abcdef";
    vm_fault_diag_write_text("0x");
    for (int shift = 60; shift >= 0; shift -= 4) {
        vm_fault_diag_write_char(digits[(value >> (uint32_t)shift) & 0xFU]);
    }
}

static void vm_fault_diag_emit(thread_t *thread, const vm_fault_info_t *fault,
                               uint32_t cpu_error, kstatus_t status,
                               vm_area_t *area, uint64_t file_offset,
                               uint32_t object_type) {
    /* 在串口输出和进程销毁释放页面前获取快照。无法取得统计信息时应明确报错，
     * 不能将其报告为可用内存为零。 */
    uint64_t total_pages = 0U, free_pages = 0U;
    kstatus_t memory_status = page_memory_get_stats(&total_pages, &free_pages);
    while (atomic_flag_test_and_set_explicit(&g_vm_fault_diag_lock,
                                              memory_order_acquire)) {
        __asm__ volatile ("pause");
    }
    vm_fault_diag_write_text("OS_VM_USER_FAULT pid=");
    vm_fault_diag_write_hex(thread->process->pid);
    vm_fault_diag_write_text(" address=");
    vm_fault_diag_write_hex(fault->address);
    vm_fault_diag_write_text(" access=");
    vm_fault_diag_write_hex(fault->access);
    vm_fault_diag_write_text(" error=");
    vm_fault_diag_write_hex(cpu_error);
    vm_fault_diag_write_text(" status=");
    vm_fault_diag_write_hex((uint64_t)status);
    vm_fault_diag_write_text(" area=");
    vm_fault_diag_write_hex(area != 0 ? area->start : 0U);
    vm_fault_diag_write_char('-');
    vm_fault_diag_write_hex(area != 0 ? area->end : 0U);
    vm_fault_diag_write_text(" prot=");
    vm_fault_diag_write_hex(area != 0 ? area->prot : 0U);
    vm_fault_diag_write_text(" object=");
    vm_fault_diag_write_hex(object_type);
    vm_fault_diag_write_text(" file_offset=");
    vm_fault_diag_write_hex(file_offset);
    vm_fault_diag_write_text(" memory_status=");
    vm_fault_diag_write_hex((uint64_t)memory_status);
    vm_fault_diag_write_text(" total_pages=");
    vm_fault_diag_write_hex(total_pages);
    vm_fault_diag_write_text(" free_pages=");
    vm_fault_diag_write_hex(free_pages);
    vm_fault_diag_write_text("\r\n");
    atomic_flag_clear_explicit(&g_vm_fault_diag_lock, memory_order_release);
}

static void vm_write_repair_diag(const char *stage, const vm_fault_info_t *fault,
                                 const vm_area_t *area, uint64_t page_address,
                                 uint64_t object_index, uint64_t pte_flags,
                                 kstatus_t status) {
    thread_t *thread = sched_current_thread();
    process_t *process = thread != 0 ? thread->process : 0;
    (void)kernel_serial_printf_serial_only(
        "OS_VM_WRITE_REPAIR stage=%s pid=%llx va=%llx page=%llx "
        "index=%llx area_flags=%llx cow=%u pte_flags=%llx cpu_error=%llx "
        "status=%lld\r\n",
        stage != 0 ? stage : "unknown",
        (unsigned long long)(process != 0 ? process->pid : 0U),
        (unsigned long long)(fault != 0 ? (uint64_t)fault->address : 0U),
        (unsigned long long)page_address,
        (unsigned long long)object_index,
        (unsigned long long)(area != 0 ? area->flags : 0U),
        area != 0 && (area->flags & VM_AREA_COW) != 0U ? 1U : 0U,
        (unsigned long long)pte_flags,
        (unsigned long long)(fault != 0 ? fault->cpu_error : 0U),
        (long long)status);
}

enum vm_fault_counter {
    VM_FAULT_COUNT_TOTAL,
    VM_FAULT_COUNT_USER,
    VM_FAULT_COUNT_KERNEL,
    VM_FAULT_COUNT_NOT_PRESENT,
    VM_FAULT_COUNT_PROTECTION,
    VM_FAULT_COUNT_ANON,
    VM_FAULT_COUNT_SHARED,
    VM_FAULT_COUNT_FILE,
    VM_FAULT_COUNT_COW,
    VM_FAULT_COUNT_STACK,
    VM_FAULT_COUNT_WINDOW_SURFACE,
    VM_FAULT_COUNT_MAX,
};

enum vm_fault_stage {
    VM_FAULT_STAGE_VMA,
    VM_FAULT_STAGE_BACKING,
    VM_FAULT_STAGE_MAPPING,
    VM_FAULT_STAGE_REMAINING,
    VM_FAULT_STAGE_TOTAL,
    VM_FAULT_STAGE_MAX,
};

typedef struct vm_fault_trace {
    uint64_t start_tsc;
    uint64_t vma_ready_tsc;
    uint64_t backing_ready_tsc;
    uint64_t pte_ready_tsc;
    uint64_t end_tsc;
    uint32_t object_type;
    uint32_t object_flags;
    uint32_t area_flags;
    bool cow;
} vm_fault_trace_t;

static kstatus_t vm_handle_fault_impl(vm_space_t *space,
                                      const vm_fault_info_t *fault,
                                      vm_fault_trace_t *trace,
                                      bool *surface_hint);
static void vm_fault_around(vm_space_t *space, const vm_fault_info_t *fault);

/* 只在持有 map_lock 时读取区域。提示用于避开普通缺页的第二次加锁查找；
 * 窗口预取仍会重新验证当前映射，不依赖此快照的存续时间。 */
static bool vm_area_fault_around_eligible(const vm_area_t *area) {
    return area != 0 && (area->flags & VM_MAP_STACK) == 0U &&
           area->object != 0 && area->object->type == VM_OBJECT_SHARED &&
           (area->object->flags & VM_OBJECT_FLAG_WINDOW_SURFACE) != 0U;
}


#if VM_FAULT_TELEMETRY_ENABLED
static atomic_uint_fast64_t g_vm_fault_counts[VM_FAULT_COUNT_MAX];
static atomic_uint_fast64_t g_vm_fault_stage_totals[VM_FAULT_STAGE_MAX];
static atomic_uint_fast64_t
    g_vm_fault_stage_histograms[VM_FAULT_STAGE_MAX][VM_FAULT_HISTOGRAM_BUCKETS];
static atomic_bool g_vm_fault_reported;

static uint32_t vm_fault_histogram_bucket(uint64_t value) {
    uint32_t bucket = 0U;
    while (value > 1U && bucket + 1U < VM_FAULT_HISTOGRAM_BUCKETS) {
        value >>= 1U;
        ++bucket;
    }
    return bucket;
}

static void vm_fault_record_stage(enum vm_fault_stage stage,
                                  uint64_t elapsed) {
    uint32_t bucket;
    atomic_fetch_add_explicit(&g_vm_fault_stage_totals[stage], elapsed,
                              memory_order_relaxed);
    bucket = vm_fault_histogram_bucket(elapsed);
    atomic_fetch_add_explicit(&g_vm_fault_stage_histograms[stage][bucket], 1U,
                              memory_order_relaxed);
}

static uint64_t vm_fault_histogram_quantile(enum vm_fault_stage stage,
                                            uint64_t samples,
                                            uint32_t percentile) {
    uint64_t target;
    uint64_t seen = 0U;

    if (samples == 0U) return 0U;
    target = ((samples * percentile) + 99U) / 100U;
    if (target == 0U) target = 1U;
    for (uint32_t bucket = 0U; bucket < VM_FAULT_HISTOGRAM_BUCKETS;
         ++bucket) {
        seen += atomic_load_explicit(
            &g_vm_fault_stage_histograms[stage][bucket],
            memory_order_relaxed);
        if (seen >= target) {
            return bucket + 1U >= 64U ? UINT64_MAX :
                   (1ULL << (bucket + 1U)) - 1U;
        }
    }
    return UINT64_MAX;
}

static void vm_fault_emit_stage(const char *name, enum vm_fault_stage stage) {
    static const uint32_t percentiles[] = {50U, 90U, 95U, 99U, 100U};
    char metric[64];
    uint64_t samples = atomic_load_explicit(
        &g_vm_fault_stage_totals[stage], memory_order_relaxed);
    uint64_t stage_samples = 0U;

    for (uint32_t bucket = 0U; bucket < VM_FAULT_HISTOGRAM_BUCKETS;
         ++bucket) {
        stage_samples += atomic_load_explicit(
            &g_vm_fault_stage_histograms[stage][bucket],
            memory_order_relaxed);
    }
    for (uint32_t index = 0U; index < sizeof(percentiles) / sizeof(percentiles[0]);
         ++index) {
        const char *suffix = index == 0U ? "p50_tsc" :
                             index == 1U ? "p90_tsc" :
                             index == 2U ? "p95_tsc" :
                             index == 3U ? "p99_tsc" : "max_tsc";
        uint64_t value = vm_fault_histogram_quantile(
            stage, stage_samples, percentiles[index]);
        size_t offset = 0U;
        while (name[offset] != '\0' && offset + 1U < sizeof(metric)) {
            metric[offset] = name[offset];
            ++offset;
        }
        if (offset + 1U < sizeof(metric)) metric[offset++] = '.';
        for (size_t suffix_index = 0U;
             suffix[suffix_index] != '\0' && offset + 1U < sizeof(metric);
             ++suffix_index) {
            metric[offset++] = suffix[suffix_index];
        }
        metric[offset] = '\0';
        kernel_perf_emit_value(metric, value);
    }
    char average_metric[64];
    size_t offset = 0U;
    while (name[offset] != '\0' && offset + 1U < sizeof(average_metric)) {
        average_metric[offset] = name[offset];
        ++offset;
    }
    if (offset + 1U < sizeof(average_metric)) average_metric[offset++] = '.';
    const char average_suffix[] = "average_tsc";
    for (size_t suffix_index = 0U;
         average_suffix[suffix_index] != '\0' &&
             offset + 1U < sizeof(average_metric);
         ++suffix_index) {
        average_metric[offset++] = average_suffix[suffix_index];
    }
    average_metric[offset] = '\0';
    kernel_perf_emit_value(average_metric,
                           stage_samples == 0U ? 0U : samples / stage_samples);
    if (stage == VM_FAULT_STAGE_TOTAL) {
        kernel_perf_emit_value(
            "page_fault_avg_ns",
            x86_boot_cpu_features.tsc_hz == 0U || stage_samples == 0U ? 0U :
                x86_tsc_to_ns(samples / stage_samples));
        kernel_perf_emit_value(
            "page_fault_p95_ns",
            x86_boot_cpu_features.tsc_hz == 0U ? 0U :
                x86_tsc_to_ns(vm_fault_histogram_quantile(
                    stage, stage_samples, 95U)));
        kernel_perf_emit_value(
            "page_fault_p99_ns",
            x86_boot_cpu_features.tsc_hz == 0U ? 0U :
                x86_tsc_to_ns(vm_fault_histogram_quantile(
                    stage, stage_samples, 99U)));
    }
}
#endif

void vm_fault_telemetry_reset(void) {
#if VM_FAULT_TELEMETRY_ENABLED
    atomic_store_explicit(&g_vm_fault_reported, false, memory_order_relaxed);
    for (uint32_t index = 0U; index < VM_FAULT_COUNT_MAX; ++index) {
        atomic_store_explicit(&g_vm_fault_counts[index], 0U,
                              memory_order_relaxed);
    }
    for (uint32_t stage = 0U; stage < VM_FAULT_STAGE_MAX; ++stage) {
        atomic_store_explicit(&g_vm_fault_stage_totals[stage], 0U,
                              memory_order_relaxed);
        for (uint32_t bucket = 0U; bucket < VM_FAULT_HISTOGRAM_BUCKETS;
             ++bucket) {
            atomic_store_explicit(&g_vm_fault_stage_histograms[stage][bucket],
                                  0U, memory_order_relaxed);
        }
    }
#endif
}

void vm_fault_telemetry_report(void) {
#if VM_FAULT_TELEMETRY_ENABLED
    static const char *const names[VM_FAULT_COUNT_MAX] = {
        "page_fault_total",
        "page_fault_user",
        "page_fault_kernel",
        "page_fault_not_present",
        "page_fault_protection",
        "page_fault_anon",
        "page_fault_shared",
        "page_fault_file",
        "page_fault_cow",
        "page_fault_stack",
        "page_fault_window_surface",
    };
    static const char *const stages[VM_FAULT_STAGE_MAX] = {
        "page_fault.vma_lookup",
        "page_fault.backing",
        "page_fault.mapping",
        "page_fault.remaining",
        "page_fault.total",
    };

    if (atomic_exchange_explicit(&g_vm_fault_reported, true,
                                 memory_order_acq_rel)) return;
    for (uint32_t index = 0U; index < VM_FAULT_COUNT_MAX; ++index) {
        kernel_perf_emit_value(
            names[index],
            atomic_load_explicit(&g_vm_fault_counts[index],
                                 memory_order_relaxed));
    }
    for (uint32_t stage = 0U; stage < VM_FAULT_STAGE_MAX; ++stage) {
        vm_fault_emit_stage(stages[stage], (enum vm_fault_stage)stage);
    }
#endif
}

#if VM_FAULT_TELEMETRY_ENABLED
static void vm_fault_telemetry_record(const vm_fault_info_t *fault,
                                      bool from_user,
                                      const vm_fault_trace_t *trace) {
    if (fault == 0 || trace == 0 || trace->start_tsc == 0U) return;

    atomic_fetch_add_explicit(&g_vm_fault_counts[VM_FAULT_COUNT_TOTAL], 1U,
                              memory_order_relaxed);
    atomic_fetch_add_explicit(
        &g_vm_fault_counts[from_user ? VM_FAULT_COUNT_USER :
                           VM_FAULT_COUNT_KERNEL],
        1U, memory_order_relaxed);
    atomic_fetch_add_explicit(
        &g_vm_fault_counts[(fault->cpu_error & 1U) == 0U ?
                           VM_FAULT_COUNT_NOT_PRESENT :
                           VM_FAULT_COUNT_PROTECTION],
        1U, memory_order_relaxed);
    switch (trace->object_type) {
        case VM_OBJECT_ANON:
            atomic_fetch_add_explicit(&g_vm_fault_counts[VM_FAULT_COUNT_ANON],
                                      1U, memory_order_relaxed);
            break;
        case VM_OBJECT_SHARED:
            atomic_fetch_add_explicit(&g_vm_fault_counts[VM_FAULT_COUNT_SHARED],
                                      1U, memory_order_relaxed);
            break;
        case VM_OBJECT_FILE:
            atomic_fetch_add_explicit(&g_vm_fault_counts[VM_FAULT_COUNT_FILE],
                                      1U, memory_order_relaxed);
            break;
        default:
            break;
    }
    if (trace->cow) {
        atomic_fetch_add_explicit(&g_vm_fault_counts[VM_FAULT_COUNT_COW], 1U,
                                  memory_order_relaxed);
    }
    if ((trace->area_flags & VM_MAP_STACK) != 0U) {
        atomic_fetch_add_explicit(&g_vm_fault_counts[VM_FAULT_COUNT_STACK], 1U,
                                  memory_order_relaxed);
    }
    if ((trace->object_flags & VM_OBJECT_FLAG_WINDOW_SURFACE) != 0U) {
        atomic_fetch_add_explicit(
            &g_vm_fault_counts[VM_FAULT_COUNT_WINDOW_SURFACE], 1U,
            memory_order_relaxed);
    }
    if (trace->vma_ready_tsc >= trace->start_tsc) {
        vm_fault_record_stage(VM_FAULT_STAGE_VMA,
                              trace->vma_ready_tsc - trace->start_tsc);
    }
    if (trace->backing_ready_tsc >= trace->vma_ready_tsc &&
        trace->backing_ready_tsc != 0U) {
        vm_fault_record_stage(VM_FAULT_STAGE_BACKING,
                              trace->backing_ready_tsc -
                                  trace->vma_ready_tsc);
    }
    if (trace->pte_ready_tsc >= trace->backing_ready_tsc &&
        trace->pte_ready_tsc != 0U) {
        vm_fault_record_stage(VM_FAULT_STAGE_MAPPING,
                              trace->pte_ready_tsc -
                                  trace->backing_ready_tsc);
    }
    if (trace->end_tsc >= trace->pte_ready_tsc && trace->pte_ready_tsc != 0U) {
        vm_fault_record_stage(VM_FAULT_STAGE_REMAINING,
                              trace->end_tsc - trace->pte_ready_tsc);
    }
    if (trace->end_tsc >= trace->start_tsc) {
        vm_fault_record_stage(VM_FAULT_STAGE_TOTAL,
                              trace->end_tsc - trace->start_tsc);
    }
}
#endif

#if OS_DEBUG_SERIAL
static void vm_fault_write_diagnostic(const char *reason,
                                      const vm_fault_info_t *fault,
                                      const vm_area_t *area,
                                      uint64_t existing_flags) {
    kernel_serial_printf_serial_only(
        "OS_DIAG_VM_FAULT %s ADDRESS=%llx ACCESS=%x AREA_START=%llx "
        "AREA_END=%llx PROT=%x FLAGS=%x PTE=%llx\r\n",
        reason,
        (unsigned long long)fault->address,
        fault->access,
        (unsigned long long)(area != 0 ? area->start : 0U),
        (unsigned long long)(area != 0 ? area->end : 0U),
        area != 0 ? area->prot : 0U,
        area != 0 ? area->flags : 0U,
        (unsigned long long)existing_flags);
}
#endif

bool vm_handle_current_fault(vaddr_t address, uint32_t cpu_error,
                             bool from_user, bool from_uaccess) {
    thread_t *thread;
    vm_fault_info_t fault;
    kstatus_t status;
#if VM_FAULT_TELEMETRY_ENABLED
    vm_fault_trace_t trace = {0};
#endif

    if ((!from_user && !from_uaccess) ||
        address < VM_USER_BASE || address >= VM_USER_END) {
        return false;
    }

    thread = sched_current_thread();
    if (thread == 0 || thread->object.type != KOBJECT_TYPE_THREAD ||
        thread->process == 0) {
        return false;
    }
    vm_space_t *space = process_vm_reference(thread->process);
    if (space == 0) return false;

    bool surface_hint = false;
    fault.address = address;
    fault.access = (cpu_error & (1U << 4)) != 0U ? VM_PROT_EXEC :
                   (cpu_error & (1U << 1)) != 0U ? VM_PROT_WRITE :
                                                  VM_PROT_READ;
    fault.cpu_error = cpu_error;
    uint64_t diagnostic_started = x86_read_tsc();
 #if VM_FAULT_TELEMETRY_ENABLED
    trace.start_tsc = telemetry_timestamp();
    status = vm_handle_fault_impl(space, &fault, &trace, &surface_hint);
    trace.end_tsc = telemetry_timestamp();
    vm_fault_telemetry_record(&fault, from_user, &trace);
#else
    status = vm_handle_fault_impl(space, &fault, 0, &surface_hint);
#endif
    uint64_t diagnostic_resolved = x86_read_tsc();
    if (status != K_OK && from_user) {
        uint64_t area_start = 0U;
        uint64_t area_end = 0U;
        uint64_t object_offset = 0U;
        uint32_t area_prot = 0U;
        uint32_t object_type = 0U;
        map_lock(space);
        vm_area_t *area = find_area(space, address);
        if (area != 0) {
            area_start = area->start;
            area_end = area->end;
            area_prot = area->prot;
            if (area->object != 0) {
                object_type = area->object->type;
                if (area->object->type == VM_OBJECT_FILE) {
                    object_offset = area->object->u.file.file_offset +
                                    area->object_offset + address - area->start;
                }
            }
        }
        map_unlock(space);
        vm_area_t area_snapshot = {
            .start = area_start,
            .end = area_end,
            .prot = area_prot,
        };
        vm_fault_diag_emit(thread, &fault, cpu_error, status,
                           area != 0 ? &area_snapshot : 0,
                           object_offset, object_type);
    }
    if (status == K_OK && surface_hint && (cpu_error & 1U) == 0U) {
        vm_fault_around(space, &fault);
    }
    /* 单独跟踪主线程：执行文件 I/O 的子线程不能掩盖浏览器 UI 的缺页延迟。
     * 此路径中不记录日志，也不分配内存。 */
    if (thread->tid == thread->process->linux_thread_group_leader_tid ||
        thread->process->pid == g_vm_slow_fault_process)
        vm_record_slow_fault(thread->process->pid, thread->tid, address,
                              cpu_error, diagnostic_started,
                              diagnostic_resolved, x86_read_tsc());
    vm_space_put(space);
    return status == K_OK;
}

static kstatus_t vm_handle_fault_impl(vm_space_t *space,
                                      const vm_fault_info_t *fault,
                                      vm_fault_trace_t *trace,
                                      bool *surface_hint) {
    if (space == 0 || fault == 0 || fault->address >= VM_USER_END) return K_EINVAL;
retry_fault:
    if (surface_hint != 0) *surface_hint = false;
    map_lock(space);
    vm_area_t *area = find_area(space, fault->address);
    if (area == 0 || (area->flags & VM_MAP_GUARD) != 0 || area->object == 0) {
        map_unlock(space);
        return K_EACCES;
    }
    if (surface_hint != 0)
        *surface_hint = vm_area_fault_around_eligible(area);
    if (trace != 0) {
        trace->vma_ready_tsc = telemetry_timestamp();
        trace->object_type = area->object->type;
        trace->object_flags = area->object->flags;
        trace->area_flags = area->flags;
        if ((area->object->flags & VM_OBJECT_FLAG_WINDOW_SURFACE) != 0U) {
            atomic_fetch_add_explicit(&area->object->fault_count, 1U,
                                      memory_order_relaxed);
        }
    }
    uint32_t access = fault->access == 0 ? VM_PROT_READ : fault->access;
    if (((access & VM_PROT_READ) != 0 && (area->prot & VM_PROT_READ) == 0) ||
        ((access & VM_PROT_WRITE) != 0 && (area->prot & VM_PROT_WRITE) == 0) ||
        ((access & VM_PROT_EXEC) != 0 && (area->prot & VM_PROT_EXEC) == 0)) {
#if OS_DEBUG_SERIAL
        /* Chromium 的 ProtectedMemory 只读探测也会触发已映射页面的写缺页。
         * 这是预期的 uaccess 修复路径，因此不要因诊断 UART 写入而串行化启动流程。 */
        if ((fault->cpu_error & 1U) == 0U) {
            vm_fault_write_diagnostic("DENY", fault, area, 0U);
        }
#endif
        map_unlock(space);
        return K_EACCES;
    }
    if (area->object->type != VM_OBJECT_ANON &&
        area->object->type != VM_OBJECT_SHARED &&
        area->object->type != VM_OBJECT_FILE &&
        area->object->type != VM_OBJECT_DEVICE &&
        area->object->type != VM_OBJECT_PAGE_ARRAY) {
        map_unlock(space);
        return K_ENOSYS;
    }
    uint64_t page_address = fault->address & ~(PAGE_SIZE - 1ULL);
    uint64_t object_index =
        (area->object_offset + page_address - area->start) >> PAGE_SHIFT;
    paddr_t existing;
    uint64_t existing_flags = 0;
    kstatus_t translated = x86_translate_page(space->root_table,
                                              (vaddr_t)page_address,
                                              &existing, &existing_flags);
    if (area->object->type == VM_OBJECT_DEVICE) {
        if (translated == K_OK) {
            map_unlock(space);
            return K_OK;
        }
        uint64_t byte_offset = object_index << PAGE_SHIFT;
        if (byte_offset >= area->object->u.device.length ||
            area->object->u.device.phys.value > UINT64_MAX - byte_offset) {
            map_unlock(space);
            return K_EINVAL;
        }
        if (trace != 0) trace->backing_ready_tsc = telemetry_timestamp();
        kstatus_t device_status = x86_map_page(
            space->root_table, (vaddr_t)page_address,
            paddr_make(area->object->u.device.phys.value + byte_offset),
            hardware_flags(area->prot, false), area->object->u.device.cache_mode);
        if (trace != 0) trace->pte_ready_tsc = telemetry_timestamp();
        if (device_status == K_OK) {
            atomic_fetch_add_explicit(&space->rss_pages, 1U, memory_order_relaxed);
            atomic_fetch_add_explicit(&space->tlb_generation, 1U,
                                      memory_order_release);
        }
        map_unlock(space);
        return device_status;
    }
    if (area->object->type == VM_OBJECT_PAGE_ARRAY) {
        uint64_t physical;
        if (translated == K_OK) {
            map_unlock(space);
            return K_OK;
        }
        if (object_index >= area->object->u.page_array.page_count) {
            map_unlock(space);
            return K_EINVAL;
        }
        physical = area->object->u.page_array.pages[object_index];
        if ((physical & (PAGE_SIZE - 1ULL)) != 0U ||
            physical > UINT64_MAX - PAGE_SIZE) {
            map_unlock(space);
            return K_EINVAL;
        }
        if (trace != 0) trace->backing_ready_tsc = telemetry_timestamp();
        kstatus_t page_array_status = x86_map_page(
            space->root_table, (vaddr_t)page_address,
            paddr_make(physical), hardware_flags(area->prot, false),
            area->object->u.page_array.cache_mode);
        if (trace != 0) trace->pte_ready_tsc = telemetry_timestamp();
        if (page_array_status == K_OK) {
            atomic_fetch_add_explicit(&space->rss_pages, 1U,
                                      memory_order_relaxed);
            atomic_fetch_add_explicit(&space->tlb_generation, 1U,
                                      memory_order_release);
        }
        map_unlock(space);
        return page_array_status;
    }
    page_t *page = 0;
    bool cow = (area->flags & VM_AREA_COW) != 0;
    if (trace != 0) trace->cow = cow;
    bool private_file = area->private_object != 0 &&
                        area->object->type == VM_OBJECT_FILE &&
                        (area->flags & VM_MAP_PRIVATE) != 0;
    if (translated == K_OK) {
        if ((access & VM_PROT_WRITE) == 0) {
            map_unlock(space);
            return K_OK;
        }
        /* 逐页处理 COW；已重新映射为可写的页面无需再次处理。 */
        if (x86_page_entry_writable(existing_flags)) {
            /* 另一颗 CPU 可能已在未发送 IPI 的情况下升级同一物理页。
             * 重试前先移除本 CPU 过期的只读映射。 */
            __asm__ volatile ("invlpg (%0)" : :
                              "r"(page_address) : "memory");
            map_unlock(space);
            return K_OK;
        }

        kstatus_t status;
        if (private_file) {
            page = anon_page_lookup(area->private_object, object_index);
            if (page == 0) {
                status = private_file_shadow_page(area, object_index, existing, &page);
            } else if (cow) {
                status = anon_page_cow(area->private_object, object_index, &page);
            } else {
                status = K_OK;
            }
        } else {
            if (!cow) {
                /* VMA 可写，因此过期的只读 PTE 属于仍需修复的权限更新。 */
                status = x86_protect_page(
                    space->root_table, (vaddr_t)page_address,
                    hardware_flags(area->prot, false), X86_CACHE_WB);
                if (status != K_OK) {
                    vm_write_repair_diag("PROTECT", fault, area,
                                         page_address, object_index,
                                         existing_flags, status);
                }
                map_unlock(space);
                return status;
            }
            status = anon_page_cow(area->object, object_index, &page);
        }
        if (status != K_OK) {
            (void)kernel_serial_printf_serial_only(
                "OS_VM_WRITE_REPAIR_FAIL pid=%llx va=%llx "
                "object=%llx index=%llx cow=%u private_file=%u "
                "pte=%llx pte_flags=%llx status=%lld\r\n",
                (unsigned long long)(sched_current_thread() != 0 &&
                    sched_current_thread()->process != 0 ?
                    sched_current_thread()->process->pid : 0U),
                (unsigned long long)(uint64_t)fault->address,
                (unsigned long long)(uintptr_t)area->object,
                (unsigned long long)object_index, cow ? 1U : 0U,
                private_file ? 1U : 0U,
                (unsigned long long)existing.value,
                (unsigned long long)existing_flags, (long long)status);
            map_unlock(space);
            return status;
        }
        if (trace != 0) trace->backing_ready_tsc = telemetry_timestamp();
        if (page_to_phys(page).value == existing.value) {
            /* 子进程释放引用后，COW 只需提升页面权限。不要拆除或重新分配页表。 */
            status = x86_protect_page(space->root_table,
                (vaddr_t)page_address, hardware_flags(area->prot, false),
                X86_CACHE_WB);
            if (trace != 0) trace->pte_ready_tsc = telemetry_timestamp();
            if (status == K_OK) {
                atomic_fetch_add_explicit(&space->tlb_generation, 1U,
                                          memory_order_release);
            }
            map_unlock(space);
            return status;
        }
        status = x86_unmap_page(space->root_table, (vaddr_t)page_address, 0);
        if (status != K_OK) {
            vm_write_repair_diag("UNMAP", fault, area, page_address,
                                 object_index, existing_flags, status);
            map_unlock(space);
            return status;
        }
        status = x86_map_page(space->root_table, (vaddr_t)page_address,
                              page_to_phys(page), hardware_flags(area->prot, false),
                              X86_CACHE_WB);
        if (status != K_OK) {
            vm_write_repair_diag("COW_MAP", fault, area, page_address,
                                 object_index, existing_flags, status);
        }
        if (trace != 0) trace->pte_ready_tsc = telemetry_timestamp();
        atomic_fetch_add_explicit(&space->tlb_generation, 1U, memory_order_release);
        map_unlock(space);
        return status;
    }

    /*
    * 后备存储回调可能获取 vnode/后端锁并执行 I/O。VFS 用户拷贝路径可能在持有相同锁时触发缺页，因此
    * 绝不能在持有 map_lock 时进入这些回调。解锁前固定两个对象并快照 VMA；回调返回后重新验证，因为
    * 期间可能已执行 unmap/protect/split。
     */
    vm_area_t area_snapshot = *area;
    uint64_t advice_generation = atomic_load_explicit(
        &space->advice_generation, memory_order_acquire);
    vm_object_t *object = area->object;
    vm_object_t *private_object = area->private_object;
    vm_object_get(object);
    vm_object_get(private_object);
    const vm_file_ops_t *file_ops =
        object->type == VM_OBJECT_FILE ? object->u.file.ops : 0;
    void *file_mapping =
        object->type == VM_OBJECT_FILE ? object->u.file.mapping : 0;
    uint64_t file_page_index = object->type == VM_OBJECT_FILE ?
        (object->u.file.file_offset >> PAGE_SHIFT) + object_index : 0U;
    map_unlock(space);

    bool page_pinned = false;
    kstatus_t status;
    if (private_file) {
        page = anon_page_lookup(private_object, object_index);
        if (page != 0) {
            status = K_OK;
        } else {
            status = file_ops == 0 || file_ops->page_get == 0 ||
                     file_mapping == 0 ? K_EIO :
                     file_ops->page_get(file_mapping, file_page_index, &page);
            if (status == K_OK && page == 0) status = K_EIO;
            if (status == K_OK && (access & VM_PROT_WRITE) != 0) {
                status = private_file_shadow_page(&area_snapshot, object_index,
                                                  page_to_phys(page), &page);
            }
        }
    } else if (object->type == VM_OBJECT_FILE) {
        status = file_ops == 0 || file_ops->page_get == 0 ||
                 file_mapping == 0 ? K_EIO :
                 file_ops->page_get(file_mapping, file_page_index, &page);
    } else {
        status = anon_page_get(object, object_index, true, true, &page);
        page_pinned = status == K_OK;
    }

    if (status == K_OK && page == 0) status = K_EIO;
    if (status != K_OK) {
        vm_object_put(private_object);
        vm_object_put(object);
        return status;
    }

    vm_object_t *page_owner = 0;
    if (private_file && (access & VM_PROT_WRITE) != 0U) {
        page_owner = private_object;
    } else if (object->type == VM_OBJECT_ANON ||
               object->type == VM_OBJECT_SHARED) {
        page_owner = object;
    }
    if (page_owner != 0 && !page_pinned)
        page_pinned = anon_page_pin(page_owner, object_index, page);
    if (page_owner != 0 && !page_pinned) {
        vm_object_put(private_object);
        vm_object_put(object);
        goto retry_fault;
    }

    map_lock(space);
    area = find_area(space, fault->address);
    if (atomic_load_explicit(&space->advice_generation,
                             memory_order_acquire) != advice_generation ||
        area == 0 || area->object != object ||
        area->private_object != private_object ||
        area->start != area_snapshot.start ||
        area->end != area_snapshot.end ||
        area->object_offset != area_snapshot.object_offset ||
        area->prot != area_snapshot.prot ||
        area->flags != area_snapshot.flags) {
        /* 无锁缺页可能在 munmap 已丢弃此片段后才填充后备页。映射拥有的索引只有一个虚拟所有者；仅发生
         * split/protect 时保留页面，丢弃孤立结果。固定引用会使采样页面在此回收期间保持有效。 */
        anon_page_node_t *retired = 0;
        if (page_owner != 0 &&
            (page_owner->flags & VM_OBJECT_FLAG_MAP_OWNED) != 0U &&
            (area == 0 ||
             (area->object != page_owner && area->private_object != page_owner) ||
             ((area->object_offset + page_address - area->start) >> PAGE_SHIFT)
                 != object_index))
            (void)anon_page_retire_range(page_owner, object_index,
                                         object_index + 1U, &retired);
        map_unlock(space);
        anon_page_release_retired(retired);
        if (page_pinned) anon_page_unpin(page);
        vm_object_put(private_object);
        vm_object_put(object);
        goto retry_fault;
    }

    /* 后备 I/O 执行期间，并发缺页可能已安装此页面。 */
    if (x86_translate_page(space->root_table, (vaddr_t)page_address,
                           &existing, &existing_flags) == K_OK) {
        map_unlock(space);
        if (page_pinned) anon_page_unpin(page);
        vm_object_put(private_object);
        vm_object_put(object);
        goto retry_fault;
    }
    if (trace != 0) trace->backing_ready_tsc = telemetry_timestamp();
    uint32_t page_flags = vm_area_page_flags(area, page_address, area->prot);
    status = x86_map_page(space->root_table, (vaddr_t)page_address,
                          page_to_phys(page), page_flags, X86_CACHE_WB);
    if (status != K_OK) {
        vm_write_repair_diag("POPULATE", fault, area, page_address,
                             object_index, existing_flags, status);
    }
    if (trace != 0) trace->pte_ready_tsc = telemetry_timestamp();
    if (status == K_OK) {
        atomic_fetch_add_explicit(&space->rss_pages, 1U, memory_order_relaxed);
        atomic_fetch_add_explicit(&space->tlb_generation, 1U,
                                  memory_order_release);
    }
    map_unlock(space);
    if (page_pinned) anon_page_unpin(page);

    if (status == K_OK && object->type == VM_OBJECT_FILE &&
        (area_snapshot.prot & VM_PROT_WRITE) != 0 &&
        (area_snapshot.flags & VM_MAP_SHARED) != 0 &&
        file_ops != 0 && file_ops->page_mark_dirty != 0) {
        file_ops->page_mark_dirty(file_mapping, file_page_index);
    }
    vm_object_put(private_object);
    vm_object_put(object);
    return status;
}

kstatus_t vm_handle_fault(vm_space_t *space, const vm_fault_info_t *fault) {
    return vm_handle_fault_impl(space, fault, 0, 0);
}

static kstatus_t vm_populate_range_internal(vm_space_t *space,
                                             vaddr_t start,
                                             size_t length,
                                             bool fault_around) {
    vm_area_t *area;
    vm_object_t *object;
    uint64_t rounded_length;
    uint64_t end;
    uint64_t object_relative = 0U;

    if (space == 0 || length == 0U ||
        ((uint64_t)start & (PAGE_SIZE - 1ULL)) != 0U ||
        (uint64_t)length > UINT64_MAX - (PAGE_SIZE - 1ULL)) {
        return K_EINVAL;
    }
    rounded_length = ((uint64_t)length + PAGE_SIZE - 1ULL) &
                     ~(PAGE_SIZE - 1ULL);
    if (rounded_length == 0U ||
        !range_valid((uint64_t)start, rounded_length, &end)) {
        return K_EINVAL;
    }

    map_lock(space);
    area = find_area(space, (uint64_t)start);
    if (area != 0 && area->object != 0 &&
        (uint64_t)start >= area->start) {
        object_relative = (uint64_t)start - area->start;
    }
    if (area == 0 || (area->flags & VM_MAP_GUARD) != 0U ||
        area->object == 0 || end > area->end ||
        area->object_offset > area->object->size ||
        object_relative > area->object->size - area->object_offset ||
        rounded_length > area->object->size - area->object_offset -
                         object_relative) {
        map_unlock(space);
        return K_EINVAL;
    }
    object = area->object;
    bool window_surface = object->type == VM_OBJECT_SHARED &&
        (object->flags & VM_OBJECT_FLAG_WINDOW_SURFACE) != 0U;
    /* 自动预取缺页不得分配冷的匿名/共享页面。只有窗口表面选择主动分配后备页。在此映射锁下重新检查：
     * vm_fault_around 采样的 VMA 可能已被替换。显式 vm_populate_range 调用仍保留原有分配语义。 */
    if (fault_around &&
        (!window_surface || (area->flags & VM_MAP_STACK) != 0U)) {
        map_unlock(space);
        return K_OK;
    }
    vm_object_get(object);

    if (window_surface) {
        uint32_t page_flags = hardware_flags(area->prot, false);
        uint64_t populated = 0;
        kstatus_t result = K_OK;
        for (uint64_t address = (uint64_t)start; address < end;
             address += PAGE_SIZE) {
            paddr_t existing;
            kstatus_t lookup = x86_translate_page(
                space->root_table, (vaddr_t)address, &existing, 0);
            if (lookup == K_OK) continue;
            if (lookup != K_ENOENT) {
                result = lookup;
                break;
            }

            page_t *page = 0;
            uint64_t object_index =
                (area->object_offset + address - area->start) >> PAGE_SHIFT;
            kstatus_t status = anon_page_get(object, object_index, true, false, &page);
            if (status == K_OK) {
                status = x86_map_page(space->root_table, (vaddr_t)address,
                                      page_to_phys(page), page_flags,
                                      X86_CACHE_WB);
            }
            if (status != K_OK) {
                result = status;
                break;
            }
            ++populated;
        }
        /* 整个填充事务都由 map_lock 保护。仅统计已安装页面，包括失败前成功安装的部分；释放本批次的映射锁
         * 前只发布一次代数。 */
        if (populated != 0U) {
            atomic_fetch_add_explicit(&space->rss_pages, populated,
                                      memory_order_relaxed);
            atomic_fetch_add_explicit(&space->tlb_generation, 1U,
                                      memory_order_release);
            atomic_fetch_add_explicit(&object->prefaulted_pages, populated,
                                      memory_order_relaxed);
            if (fault_around) {
                atomic_fetch_add_explicit(&object->fault_around_pages, populated,
                                          memory_order_relaxed);
            }
        }
        map_unlock(space);
        vm_object_put(object);
        return result;
    }
    map_unlock(space);

    for (uint64_t address = (uint64_t)start; address < end;
         address += PAGE_SIZE) {
        paddr_t existing;
        kstatus_t lookup = x86_translate_page(
            space->root_table, (vaddr_t)address, &existing, 0);
        if (lookup == K_OK) continue;
        if (lookup != K_ENOENT) {
            vm_object_put(object);
            return lookup;
        }

        vm_fault_info_t fault = {
            .address = (vaddr_t)address,
            .access = VM_PROT_READ,
            .cpu_error = 0U,
        };
        kstatus_t status = vm_handle_fault(space, &fault);
        if (status != K_OK) {
            vm_object_put(object);
            return status;
        }
        if ((object->flags & VM_OBJECT_FLAG_WINDOW_SURFACE) != 0U) {
            atomic_fetch_add_explicit(&object->prefaulted_pages, 1U,
                                      memory_order_relaxed);
            if (fault_around) {
                atomic_fetch_add_explicit(&object->fault_around_pages, 1U,
                                          memory_order_relaxed);
            }
        }
    }
    vm_object_put(object);
    return K_OK;
}

kstatus_t vm_populate_range(vm_space_t *space, vaddr_t start, size_t length) {
    return vm_populate_range_internal(space, start, length, false);
}

#ifndef VM_FAULT_AROUND_PAGES
#define VM_FAULT_AROUND_PAGES 16U
#endif
#if VM_FAULT_AROUND_PAGES != 16 && VM_FAULT_AROUND_PAGES != 32
#error "VM_FAULT_AROUND_PAGES must be 16 or 32"
#endif

static void vm_fault_around(vm_space_t *space, const vm_fault_info_t *fault) {
    vm_area_t *area;
    uint64_t page_address;
    uint64_t start;
    uint64_t end;
    uint64_t object_offset;
    uint64_t object_end;
    uint64_t page_count;

    if (space == 0 || fault == 0 ||
        (fault->cpu_error & 1U) != 0U) return;
    page_address = fault->address & ~(PAGE_SIZE - 1ULL);
    if (page_address > UINT64_MAX - PAGE_SIZE) return;
    start = page_address + PAGE_SIZE;

    map_lock(space);
    area = find_area(space, page_address);
    if (!vm_area_fault_around_eligible(area) ||
        start >= area->end || area->object_offset > area->object->size ||
        page_address < area->start) {
        map_unlock(space);
        return;
    }
    object_offset = area->object_offset + page_address - area->start;
    if (object_offset >= area->object->size) {
        map_unlock(space);
        return;
    }
    object_end = area->end;
    if (area->object->size - object_offset < object_end - page_address) {
        object_end = page_address + area->object->size - object_offset;
    }
    end = object_end;
    page_count = (end - start) >> PAGE_SHIFT;
    if (page_count > VM_FAULT_AROUND_PAGES - 1U) {
        page_count = VM_FAULT_AROUND_PAGES - 1U;
    }
    map_unlock(space);

    if (page_count != 0U) {
        (void)vm_populate_range_internal(
            space, (vaddr_t)start,
            (size_t)(page_count * PAGE_SIZE), true);
    }
}
