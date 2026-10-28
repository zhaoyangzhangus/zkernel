#include <arch/x86_64/acpi.h>
#include <arch/x86_64/apic.h>
#include <arch/x86_64/cpu.h>
#include <arch/x86_64/paging.h>
#include <arch/x86_64/smp.h>
#include <kernel/mm.h>
#include <kernel/realtest.h>
#include <kernel/sched.h>
#include <arch/x86_64/syscall_internal.h>
#include "internal.h"

enum {
    AP_STATE_OFFLINE = 0,
    AP_STATE_BOOTING,
    AP_STATE_STARTED,
    AP_STATE_ONLINE,
    AP_STATE_FAILED,
};

enum {
    SMP_BSP_FAIL_NONE = 0U,
    SMP_BSP_FAIL_BAD_INPUT = 1U,
    SMP_BSP_FAIL_TRAMPOLINE_LAYOUT = 2U,
    SMP_BSP_FAIL_ROOT_TABLE = 3U,
    SMP_BSP_FAIL_TRAMPOLINE_MAP = 4U,
    SMP_BSP_FAIL_BSP_BIND = 5U,
    SMP_BSP_FAIL_APIC_MODE = 6U,
    SMP_BSP_FAIL_AP_STACK = 7U,
    SMP_BSP_FAIL_PREPARE_SECONDARY = 8U,
    SMP_BSP_FAIL_SEND_INIT = 9U,
    SMP_BSP_FAIL_SEND_SIPI1 = 10U,
    SMP_BSP_FAIL_SEND_SIPI2 = 11U,
    SMP_BSP_FAIL_WAIT_AP = 12U,
    SMP_BSP_FAIL_FINAL_COUNT = 13U,
    SMP_BSP_FAIL_SEND_INIT_DEASSERT = 14U,
};

enum {
    SMP_AP_STEP_NONE = 0U,
    SMP_AP_STEP_ENTERED = 101U,
    SMP_AP_STEP_PLATFORM = 102U,
    SMP_AP_STEP_FEATURES = 103U,
    SMP_AP_STEP_CPU_INIT = 104U,
    SMP_AP_STEP_SYSCALL_INIT = 105U,
    SMP_AP_STEP_LAPIC_INIT = 106U,
    SMP_AP_STEP_CPU_LOCAL = 107U,
    SMP_AP_STEP_STARTED = 108U,
};

typedef struct __attribute__((packed)) {
    uint64_t cr3;
    uint64_t stack_top;
    uint64_t entry;
    uint32_t cpu_index;
    uint32_t apic_id;
    uint64_t generation;
} x86_ap_trampoline_parameters_t;

_Static_assert(sizeof(x86_ap_trampoline_parameters_t) == 40U,
               "AP trampoline parameter layout");
_Static_assert(__builtin_offsetof(x86_ap_trampoline_parameters_t, cpu_index) == 24U,
               "AP trampoline cpu index offset");

extern const uint8_t x86_ap_trampoline_start[];
extern const uint8_t x86_ap_trampoline_parameters[];
extern const uint8_t x86_ap_trampoline_end[];

static atomic_uint g_ap_states[MAX_CPUS];
static void *g_ap_stacks[MAX_CPUS];
static atomic_uint g_started_count;
static atomic_uint g_online_count;
static atomic_bool g_release_aps;
static atomic_bool g_enable_ap_timers;
static atomic_uint g_ap_timer_enabled_count;
static atomic_uint_fast64_t g_ipi_acknowledgements[MAX_CPUS];
static atomic_bool g_reschedule_pending[MAX_CPUS];
static atomic_uint_fast64_t g_online_words[MAX_CPUS / 64U];
static uint32_t g_discovered_count;

static atomic_uint g_ap_boot_steps[MAX_CPUS];
static atomic_uint g_smp_fail_cpu;
static atomic_uint g_smp_fail_apic;
static atomic_uint g_smp_fail_bsp_step;

static void smp_record_bsp_failure(uint32_t cpu_index,
                                   uint32_t apic_id,
                                   uint32_t step) {
    atomic_store_explicit(&g_smp_fail_cpu, cpu_index, memory_order_relaxed);
    atomic_store_explicit(&g_smp_fail_apic, apic_id, memory_order_relaxed);
    atomic_store_explicit(&g_smp_fail_bsp_step, step, memory_order_release);
    realtest_mark_number("SMP_FAIL_CPU", cpu_index);
    realtest_mark_number("SMP_FAIL_APIC", apic_id);
    realtest_mark_number("SMP_FAIL_BSP_STEP", step);
    if (cpu_index < MAX_CPUS) {
        realtest_mark_number(
            "SMP_FAIL_AP_STEP",
            atomic_load_explicit(&g_ap_boot_steps[cpu_index],
                                 memory_order_acquire));
    }
}

static void bytes_zero(void *memory, size_t size) {
    uint8_t *bytes = (uint8_t *)memory;
    while (size-- != 0) *bytes++ = 0;
}

static void bytes_copy(void *destination, const void *source, size_t size) {
    uint8_t *out = (uint8_t *)destination;
    const uint8_t *in = (const uint8_t *)source;
    while (size-- != 0) *out++ = *in++;
}

static void tsc_delay(uint64_t nanoseconds) {
    uint64_t start = x86_read_tsc();
    uint64_t ticks = x86_timeout_ns_to_tsc(nanoseconds);
    uint64_t deadline = ticks > UINT64_MAX - start ? UINT64_MAX : start + ticks;
    while ((int64_t)(x86_read_tsc() - deadline) < 0) __asm__ volatile ("pause");
}

static bool wait_for_ap(uint32_t cpu_index, uint64_t timeout_ns) {
    uint64_t start = x86_read_tsc();
    uint64_t ticks = x86_timeout_ns_to_tsc(timeout_ns);
    uint64_t deadline = ticks > UINT64_MAX - start ? UINT64_MAX : start + ticks;
    for (;;) {
        unsigned state = atomic_load_explicit(&g_ap_states[cpu_index],
                                              memory_order_acquire);
        if (state == AP_STATE_STARTED || state == AP_STATE_ONLINE) return true;
        if (state == AP_STATE_FAILED || (int64_t)(x86_read_tsc() - deadline) >= 0) {
            return false;
        }
        __asm__ volatile ("pause");
    }
}

static uint32_t current_legacy_apic_id(void) {
    uint32_t eax;
    uint32_t ebx;
    uint32_t ecx;
    uint32_t edx;
    __asm__ volatile ("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
                      : "a"(1U), "c"(0U));
    (void)eax;
    (void)ecx;
    (void)edx;
    return ebx >> 24;
}

static bool ap_features_match(void) {
    uint32_t eax;
    uint32_t ebx;
    uint32_t ecx;
    uint32_t edx;
    uint32_t maximum_basic;
    uint32_t maximum_extended;
    __asm__ volatile ("cpuid" : "=a"(maximum_basic), "=b"(ebx),
                      "=c"(ecx), "=d"(edx) : "a"(0U), "c"(0U));
    __asm__ volatile ("cpuid" : "=a"(maximum_extended), "=b"(ebx),
                      "=c"(ecx), "=d"(edx) : "a"(0x80000000U), "c"(0U));
    if (maximum_extended < 0x80000001U) return !x86_boot_cpu_features.nx;
    __asm__ volatile ("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
                      : "a"(0x80000001U), "c"(0U));
    if (((edx & (1U << 20)) != 0) != x86_boot_cpu_features.nx) return false;
    if (maximum_basic >= 7U) {
        __asm__ volatile ("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
                          : "a"(7U), "c"(0U));
        if (((ebx & (1U << 7)) != 0) != x86_boot_cpu_features.smep ||
            ((ebx & (1U << 20)) != 0) != x86_boot_cpu_features.smap) return false;
    } else if (x86_boot_cpu_features.smep || x86_boot_cpu_features.smap) {
        return false;
    }
    if (maximum_basic >= 1U) {
        __asm__ volatile ("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
                          : "a"(1U), "c"(0U));
        if (((ecx & (1U << 17)) != 0) != x86_boot_cpu_features.pcid) return false;
    } else if (x86_boot_cpu_features.pcid) {
        return false;
    }
    if (maximum_extended >= 0x80000008U) {
        __asm__ volatile ("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
                          : "a"(0x80000008U), "c"(0U));
        if ((uint8_t)eax != x86_boot_cpu_features.phys_bits ||
            (uint8_t)(eax >> 8) != x86_boot_cpu_features.virt_bits) return false;
    }
    return true;
}

__noreturn void x86_smp_ap_entry(uint32_t cpu_index) {
    const x86_acpi_platform_t *platform = x86_acpi_platform();
    bool valid = true;
    uint32_t apic_id = UINT32_MAX;
    bool ap_timer_enabled = false;

    if (cpu_index < MAX_CPUS) {
        atomic_store_explicit(&g_ap_boot_steps[cpu_index],
                              SMP_AP_STEP_ENTERED, memory_order_release);
    }

    if (platform == 0 || cpu_index >= platform->cpu_count ||
        platform->cpus[cpu_index].apic_id != current_legacy_apic_id()) {
        valid = false;
    } else {
        apic_id = platform->cpus[cpu_index].apic_id;
        atomic_store_explicit(&g_ap_boot_steps[cpu_index],
                              SMP_AP_STEP_PLATFORM, memory_order_release);
    }

    if (valid) {
        if (!ap_features_match()) valid = false;
        else atomic_store_explicit(&g_ap_boot_steps[cpu_index],
                                   SMP_AP_STEP_FEATURES, memory_order_release);
    }

    if (valid) {
        if (!x86_cpu_init_secondary(cpu_index, apic_id)) {
            valid = false;
        } else {
            x86_paging_arch_init_secondary();
            atomic_store_explicit(&g_ap_boot_steps[cpu_index],
                                  SMP_AP_STEP_CPU_INIT, memory_order_release);
        }
    }

    if (valid) {
        uint64_t stack_top = (uint64_t)(uintptr_t)g_ap_stacks[cpu_index] +
                             X86_AP_BOOT_STACK_SIZE;
        if (!syscall_init(stack_top)) valid = false;
        else atomic_store_explicit(&g_ap_boot_steps[cpu_index],
                                   SMP_AP_STEP_SYSCALL_INIT, memory_order_release);
    }

    if (valid) {
        if (!lapic_init_secondary(apic_id, 32U, 1000000U)) valid = false;
        else atomic_store_explicit(&g_ap_boot_steps[cpu_index],
                                   SMP_AP_STEP_LAPIC_INIT, memory_order_release);
    }

    if (valid) {
        if (x86_cpu_local_current() == 0) valid = false;
        else atomic_store_explicit(&g_ap_boot_steps[cpu_index],
                                   SMP_AP_STEP_CPU_LOCAL, memory_order_release);
    }

    if (valid) {
        atomic_store_explicit(&g_ap_boot_steps[cpu_index],
                              SMP_AP_STEP_STARTED, memory_order_release);
        atomic_fetch_add_explicit(&g_started_count, 1U, memory_order_relaxed);
    }
    /* STARTED 是 BSP 的 acquire 屏障。先发布计数和诊断步骤，否则最后一个 AP 可能会在
     * 计数可见前报告就绪，导致 BSP 错误地判定最终计数检查失败。 */
    atomic_store_explicit(&g_ap_states[cpu_index],
                          valid ? AP_STATE_STARTED : AP_STATE_FAILED,
                          memory_order_release);

    if (valid) {
        /* BSP 建立全部运行队列后才发布 online，避免调度器观察到半初始化 CPU。 */
        while (!atomic_load_explicit(&g_release_aps, memory_order_acquire)) {
            __asm__ volatile ("pause");
        }
        x86_cpu_local_t *local = x86_cpu_local_current();
        local->Flags |= X86_CPU_LOCAL_ONLINE;
        atomic_fetch_or_explicit(&g_online_words[cpu_index >> 6],
                                 1ULL << (cpu_index & 63U), memory_order_release);
        atomic_fetch_add_explicit(&g_online_count, 1U, memory_order_release);
        atomic_store_explicit(&g_ap_states[cpu_index], AP_STATE_ONLINE,
                              memory_order_release);
        for (;;) {
            if (!ap_timer_enabled) {
                if (!atomic_load_explicit(&g_enable_ap_timers,
                                          memory_order_acquire)) {
                    __asm__ volatile ("sti; hlt; cli" : : : "memory");
                    continue;
                }
                lapic_enable_timer();
                atomic_fetch_add_explicit(&g_ap_timer_enabled_count, 1U,
                                          memory_order_release);
                ap_timer_enabled = true;
            }
            /* 进入 HLT 前也要检查电平触发请求。 */
            if (x86_smp_take_reschedule_request() ||
                sched_runnable_count() != 0U) {
                (void)sched_try_run_ready();
            }
            __asm__ volatile ("sti; hlt" : : : "memory");
            sched_finish_switch();
            /*
             * IPI 是低延迟唤醒路径，但不能成为正确性的唯一条件：
             * 若就绪线程恰好在 IPI 投递边界入队，空闲 CPU 仍必须主动
             * 扫描一次本地运行队列，避免跨 CPU 线程永久饥饿。
             */
            if (x86_smp_take_reschedule_request() ||
                sched_runnable_count() != 0U) {
                (void)sched_try_run_ready();
            }
            __asm__ volatile ("cli" : : : "memory");
        }
    }
    for (;;) __asm__ volatile ("cli; hlt");
}

bool x86_smp_start_aps(const OS_BOOT_INFO *boot_info) {
    const x86_acpi_platform_t *platform = x86_acpi_platform();
    realtest_mark("SMP_START_BEGIN");
    atomic_init(&g_smp_fail_cpu, UINT32_MAX);
    atomic_init(&g_smp_fail_apic, UINT32_MAX);
    atomic_init(&g_smp_fail_bsp_step, SMP_BSP_FAIL_NONE);
    for (uint32_t diag_i = 0; diag_i < MAX_CPUS; ++diag_i) {
        atomic_init(&g_ap_boot_steps[diag_i], SMP_AP_STEP_NONE);
    }
    if (boot_info == 0 || platform == 0 || platform->cpu_count == 0 ||
        boot_info->ApTrampolineSize != PAGE_SIZE ||
        (boot_info->ApTrampolineBase & (PAGE_SIZE - 1ULL)) != 0 ||
        boot_info->ApTrampolineBase < PAGE_SIZE ||
        boot_info->ApTrampolineBase > 0xFF000ULL) {
        smp_record_bsp_failure(UINT32_MAX, UINT32_MAX, SMP_BSP_FAIL_BAD_INPUT);
        return false;
    }
    realtest_mark_number("SMP_CPU_COUNT", platform->cpu_count);

    size_t blob_size = (size_t)(x86_ap_trampoline_end - x86_ap_trampoline_start);
    size_t parameter_offset = (size_t)(x86_ap_trampoline_parameters -
                                       x86_ap_trampoline_start);
    if (blob_size == 0 || blob_size > PAGE_SIZE ||
        parameter_offset > blob_size - sizeof(x86_ap_trampoline_parameters_t)) {
        smp_record_bsp_failure(UINT32_MAX, UINT32_MAX,
                               SMP_BSP_FAIL_TRAMPOLINE_LAYOUT);
        return false;
    }
    paddr_t root = x86_current_root_table();
    if (root.value == 0 || root.value > UINT32_MAX) {
        smp_record_bsp_failure(UINT32_MAX, UINT32_MAX, SMP_BSP_FAIL_ROOT_TABLE);
        return false;
    }
    uint8_t *page = (uint8_t *)phys_to_direct(paddr_make(boot_info->ApTrampolineBase));
    if (page == 0) {
        smp_record_bsp_failure(UINT32_MAX, UINT32_MAX, SMP_BSP_FAIL_TRAMPOLINE_MAP);
        return false;
    }
    bytes_zero(page, PAGE_SIZE);
    bytes_copy(page, x86_ap_trampoline_start, blob_size);
    x86_ap_trampoline_parameters_t *parameters =
        (x86_ap_trampoline_parameters_t *)(void *)(page + parameter_offset);

    uint32_t bsp_index = UINT32_MAX;
    for (uint32_t i = 0; i < platform->cpu_count; ++i) {
        if (platform->cpus[i].apic_id == platform->bsp_apic_id) {
            bsp_index = i;
            break;
        }
    }
    if (bsp_index == UINT32_MAX ||
        !x86_cpu_bind_bootstrap(bsp_index, platform->bsp_apic_id)) {
        smp_record_bsp_failure(bsp_index, platform->bsp_apic_id,
                               SMP_BSP_FAIL_BSP_BIND);
        return false;
    }
    realtest_mark_number("SMP_BSP_INDEX", bsp_index);
    realtest_mark_number("SMP_BSP_APIC", platform->bsp_apic_id);

    g_discovered_count = platform->cpu_count;
    atomic_init(&g_started_count, 1U);
    atomic_init(&g_online_count, 1U);
    atomic_init(&g_release_aps, false);
    atomic_init(&g_enable_ap_timers, false);
    atomic_init(&g_ap_timer_enabled_count, 0U);
    for (uint32_t word = 0; word < MAX_CPUS / 64U; ++word) {
        atomic_init(&g_online_words[word], 0);
    }
    atomic_fetch_or_explicit(&g_online_words[bsp_index >> 6],
                             1ULL << (bsp_index & 63U), memory_order_relaxed);
    for (uint32_t i = 0; i < platform->cpu_count; ++i) {
        atomic_init(&g_ap_states[i], platform->cpus[i].apic_id == platform->bsp_apic_id ?
                                      AP_STATE_STARTED : AP_STATE_OFFLINE);
        atomic_init(&g_ipi_acknowledgements[i], 0);
        atomic_init(&g_reschedule_pending[i], false);
        g_ap_stacks[i] = 0;
    }

    uint8_t vector = (uint8_t)(boot_info->ApTrampolineBase >> PAGE_SHIFT);
    uint64_t generation = 1U;
    bool trace_protocol = true;
    for (uint32_t i = 0; i < platform->cpu_count; ++i) {
        uint32_t apic_id = platform->cpus[i].apic_id;
        if (apic_id == platform->bsp_apic_id) continue;
        bool trace_this_ap = trace_protocol;
        trace_protocol = false;
        realtest_mark_number("SMP_AP_BEGIN", i);
        realtest_mark_number("SMP_APIC_ID", apic_id);
        if (apic_id > 0xFFU || platform->cpus[i].x2apic) {
            smp_record_bsp_failure(i, apic_id, SMP_BSP_FAIL_APIC_MODE);
            return false;
        }
        void *stack = vmalloc(X86_AP_BOOT_STACK_SIZE);
        if (stack == 0) {
            smp_record_bsp_failure(i, apic_id, SMP_BSP_FAIL_AP_STACK);
            return false;
        }
        g_ap_stacks[i] = stack;
        if (trace_this_ap) {
            realtest_mark_number("SMP_AP_STACK_READY", i);
        }
        uint64_t stack_top = (uint64_t)(uintptr_t)stack + X86_AP_BOOT_STACK_SIZE;
        if (!x86_cpu_prepare_secondary(i, apic_id, stack_top)) {
            smp_record_bsp_failure(i, apic_id, SMP_BSP_FAIL_PREPARE_SECONDARY);
            return false;
        }
        if (trace_this_ap) {
            realtest_mark_number("SMP_AP_CONTEXT_READY", i);
        }
        parameters->cr3 = root.value;
        parameters->stack_top = stack_top;
        parameters->entry = (uint64_t)(uintptr_t)&x86_smp_ap_entry;
        parameters->cpu_index = i;
        parameters->apic_id = apic_id;
        parameters->generation = generation++;
        atomic_store_explicit(&g_ap_states[i], AP_STATE_BOOTING, memory_order_release);
        atomic_thread_fence(memory_order_seq_cst);

        if (!lapic_send_init(apic_id)) {
            smp_record_bsp_failure(i, apic_id, SMP_BSP_FAIL_SEND_INIT);
            return false;
        }
        if (trace_this_ap) {
            realtest_mark_number("SMP_AP_INIT_ASSERTED", i);
        }
        tsc_delay(10000000ULL);
        /* 遵循 x86 Linux 的 INIT 顺序：发送 STARTUP IPI 前必须撤销电平触发的 INIT。 */
        if (!lapic_send_init_deassert(apic_id)) {
            smp_record_bsp_failure(i, apic_id,
                                   SMP_BSP_FAIL_SEND_INIT_DEASSERT);
            return false;
        }
        if (trace_this_ap) {
            realtest_mark_number("SMP_AP_INIT_DEASSERTED", i);
        }
        atomic_thread_fence(memory_order_seq_cst);
        if (!lapic_send_startup(apic_id, vector)) {
            smp_record_bsp_failure(i, apic_id, SMP_BSP_FAIL_SEND_SIPI1);
            return false;
        }
        if (trace_this_ap) {
            realtest_mark_number("SMP_AP_SIPI1_SENT", i);
        }
        if (!wait_for_ap(i, 200000ULL)) {
            if (!lapic_send_startup(apic_id, vector)) {
                smp_record_bsp_failure(i, apic_id, SMP_BSP_FAIL_SEND_SIPI2);
                return false;
            }
            if (trace_this_ap) {
                realtest_mark_number("SMP_AP_SIPI2_SENT", i);
            }
            if (!wait_for_ap(i, 1000000000ULL)) {
                smp_record_bsp_failure(i, apic_id, SMP_BSP_FAIL_WAIT_AP);
                return false;
            }
        }
        realtest_mark_number("SMP_AP_STARTED", i);
    }
    if (atomic_load_explicit(&g_started_count, memory_order_acquire) !=
        platform->cpu_count) {
        smp_record_bsp_failure(UINT32_MAX, UINT32_MAX, SMP_BSP_FAIL_FINAL_COUNT);
        return false;
    }
    realtest_mark("SMP_START_OK");
    return true;
}

uint32_t x86_smp_started_count(void) {
    return atomic_load_explicit(&g_started_count, memory_order_acquire);
}

uint32_t x86_smp_discovered_count(void) { return g_discovered_count; }

void x86_smp_get_start_diag(x86_smp_start_diag_t *diag) {
    if (diag == 0) return;

    uint32_t cpu_index =
        atomic_load_explicit(&g_smp_fail_cpu, memory_order_acquire);
    diag->cpu_index = cpu_index;
    diag->apic_id =
        atomic_load_explicit(&g_smp_fail_apic, memory_order_acquire);
    diag->bsp_step =
        atomic_load_explicit(&g_smp_fail_bsp_step, memory_order_acquire);
    diag->started_count =
        atomic_load_explicit(&g_started_count, memory_order_acquire);
    diag->discovered_count = g_discovered_count;

    if (cpu_index < MAX_CPUS) {
        diag->ap_step =
            atomic_load_explicit(&g_ap_boot_steps[cpu_index], memory_order_acquire);
        diag->ap_state =
            atomic_load_explicit(&g_ap_states[cpu_index], memory_order_acquire);
    } else {
        diag->ap_step = SMP_AP_STEP_NONE;
        diag->ap_state = AP_STATE_OFFLINE;
    }
}

bool x86_smp_cpu_online(uint32_t cpu_index) {
    if (cpu_index >= g_discovered_count || cpu_index >= MAX_CPUS) return false;
    uint64_t word = atomic_load_explicit(&g_online_words[cpu_index >> 6],
                                         memory_order_acquire);
    return (word & (1ULL << (cpu_index & 63U))) != 0;
}

bool x86_smp_cpu_started(uint32_t cpu_index) {
    if (cpu_index >= g_discovered_count || cpu_index >= MAX_CPUS) return false;
    unsigned state = atomic_load_explicit(&g_ap_states[cpu_index], memory_order_acquire);
    return state == AP_STATE_STARTED || state == AP_STATE_ONLINE;
}

bool x86_smp_release_aps(void) {
    if (g_discovered_count == 0 ||
        atomic_load_explicit(&g_started_count, memory_order_acquire) !=
        g_discovered_count) return false;
    atomic_store_explicit(&g_release_aps, true, memory_order_release);

    uint64_t start = x86_read_tsc();
    uint64_t ticks = x86_timeout_ns_to_tsc(1000000000ULL);
    uint64_t deadline = ticks > UINT64_MAX - start ? UINT64_MAX : start + ticks;
    while (atomic_load_explicit(&g_online_count, memory_order_acquire) !=
           g_discovered_count) {
        if ((int64_t)(x86_read_tsc() - deadline) >= 0) return false;
        __asm__ volatile ("pause");
    }
    return true;
}

bool x86_smp_enable_ap_timers(void) {
    const x86_acpi_platform_t *platform = x86_acpi_platform();
    uint32_t target;
    if (platform == 0 || g_discovered_count != platform->cpu_count ||
        atomic_load_explicit(&g_online_count, memory_order_acquire) !=
            g_discovered_count) {
        return false;
    }
    target = g_discovered_count > 0U ? g_discovered_count - 1U : 0U;
    atomic_store_explicit(&g_enable_ap_timers, true, memory_order_release);
    realtest_checkpoint("SMP_AP_TIMER_ENABLE_ARMED");
    for (uint32_t cpu_index = 0U; cpu_index < g_discovered_count; ++cpu_index) {
        if (platform->cpus[cpu_index].apic_id == platform->bsp_apic_id) continue;
        if (!x86_smp_request_reschedule(cpu_index)) return false;
    }
    realtest_checkpoint("SMP_AP_TIMER_ENABLE_REQUESTS_OK");

    uint64_t start = x86_read_tsc();
    uint64_t ticks = x86_timeout_ns_to_tsc(1000000000ULL);
    uint64_t deadline = ticks > UINT64_MAX - start ? UINT64_MAX : start + ticks;
    while (atomic_load_explicit(&g_ap_timer_enabled_count,
                                memory_order_acquire) < target) {
        if ((int64_t)(x86_read_tsc() - deadline) >= 0) return false;
        __asm__ volatile ("pause");
    }
    realtest_checkpoint("SMP_AP_TIMER_ENABLE_WAIT_OK");
    return true;
}

void x86_smp_ipi_interrupt(void) {
    uint32_t cpu_index = x86_current_cpu_index();
    bool reschedule = false;

    if (cpu_index < g_discovered_count && cpu_index < MAX_CPUS) {
        uint64_t acknowledgements =
            atomic_load_explicit(&g_ipi_acknowledgements[cpu_index],
                                 memory_order_relaxed);
        atomic_store_explicit(&g_ipi_acknowledgements[cpu_index],
                              acknowledgements + 1U,
                              memory_order_release);

        /*
         * idle CPU 的 reschedule IPI 只负责唤醒 HLT，不在中断栈上直接完成
         * 首次 idle -> thread context switch。
         *
         * AP 启动 idle 循环和 scheduler_idle_main() 都会在 HLT 返回后通过
         * x86_smp_take_reschedule_request() + sched_try_run_ready() 消费请求。
         * BSP 若正在 runtime self-test 驱动循环，也会持续调用 schedule()。
         *
         * 保留 pending 很重要：若这里先 exchange(false)，HLT 返回后的 idle
         * 路径会看不到请求，从而把首次运行完全依赖于中断内 schedule() 的时序。
         */
        /*
         * BSP 启动续执行路径也会将逻辑 idle 线程设为 queue.current，但不会执行
         * scheduler_idle_main()。此处同时处理 idle 循环和启动路径的请求；
         * 否则本地 IRQ 唤醒可能使 READY 工作线程滞留在队列中。
         */
        reschedule = atomic_exchange_explicit(
            &g_reschedule_pending[cpu_index], false,
            memory_order_acq_rel);
    }

    lapic_end_of_interrupt();

    if (reschedule) {
        scheduler_trace_reschedule_interrupt(cpu_index);
        /*
         * 系统调用执行期间保留该请求。TLB IPI 使用单独的向量，仍可正常处理；
         * 系统调用退出路径会在其栈帧完整后消费此请求。
         */
        if (x86_syscall_active_fast()) {
            if (cpu_index < g_discovered_count && cpu_index < MAX_CPUS) {
                atomic_store_explicit(&g_reschedule_pending[cpu_index], true,
                                      memory_order_release);
            }
            return;
        }
        /* TLB shootdown 期间不能重入调度；保留请求，由下一次时钟继续处理。 */
        if (x86_tlb_shootdown_active() || sched_preempt_disabled()) {
            if (cpu_index < g_discovered_count && cpu_index < MAX_CPUS) {
                atomic_store_explicit(&g_reschedule_pending[cpu_index], true,
                                      memory_order_release);
            }
        } else {
            sched_handle_reschedule_request();
        }
    }
}

bool x86_smp_ipi_self_test(void) {
    const x86_acpi_platform_t *platform = x86_acpi_platform();
    if (platform == 0 || platform->cpu_count != g_discovered_count ||
        atomic_load_explicit(&g_online_count, memory_order_acquire) !=
        g_discovered_count) return false;

    for (uint32_t cpu_index = 0; cpu_index < platform->cpu_count; ++cpu_index) {
        uint32_t apic_id = platform->cpus[cpu_index].apic_id;
        if (apic_id == platform->bsp_apic_id) continue;
        uint64_t before = atomic_load_explicit(&g_ipi_acknowledgements[cpu_index],
                                               memory_order_acquire);
        if (!lapic_send_fixed(apic_id, X86_SMP_IPI_VECTOR)) return false;

        uint64_t start = x86_read_tsc();
        uint64_t ticks = x86_timeout_ns_to_tsc(100000000ULL);
        uint64_t deadline = ticks > UINT64_MAX - start ? UINT64_MAX : start + ticks;
        while (atomic_load_explicit(&g_ipi_acknowledgements[cpu_index],
                                    memory_order_acquire) == before) {
            if ((int64_t)(x86_read_tsc() - deadline) >= 0) return false;
            __asm__ volatile ("pause");
        }
    }
    return true;
}

bool x86_smp_request_reschedule(uint32_t cpu_index) {
    const x86_acpi_platform_t *platform = x86_acpi_platform();
    if (platform == 0 || cpu_index >= platform->cpu_count ||
        !x86_smp_cpu_online(cpu_index)) return false;

    /*
    * 大多数唤醒突发都发生在已有请求待处理时。
    * 常见情况下避免无条件执行 LOCK XCHG。
     *
    * 生产者到达此函数前已发布运行队列更新。若目标 CPU 并发消费了旧请求，
    * 它会在该更新发布后重新调度；否则下面的 CAS 会安装新请求并发送 IPI。
     */
    if (atomic_load_explicit(&g_reschedule_pending[cpu_index],
                             memory_order_relaxed)) {
        scheduler_trace_reschedule_request(cpu_index, false);
        return true;
    }

    bool expected = false;
    if (!atomic_compare_exchange_strong_explicit(
            &g_reschedule_pending[cpu_index],
            &expected,
            true,
            memory_order_release,
            memory_order_relaxed)) {
        scheduler_trace_reschedule_request(cpu_index, false);
        return true;
    }

    scheduler_trace_reschedule_request(cpu_index, true);
    uint64_t send_started = scheduler_trace_reschedule_begin();
    for (uint32_t attempt = 0; attempt < 4U; ++attempt) {
        if (lapic_send_fixed(platform->cpus[cpu_index].apic_id,
                                    X86_SMP_IPI_VECTOR)) {
            scheduler_trace_reschedule_send(cpu_index, send_started, true, attempt+1U);
            return true;
        }
        __asm__ volatile ("pause");
    }

    /* pending 保持置位；目标 CPU 的时钟/空闲轮询作为后备处理路径。 */
    scheduler_trace_reschedule_send(cpu_index, send_started, false, 4U);
    return false;
}

bool x86_smp_take_reschedule_request(void) {
    uint32_t cpu_index = x86_current_cpu_index();
    if (cpu_index >= g_discovered_count || cpu_index >= MAX_CPUS) return false;

    /*
    * 面向绝大多数空闲轮询场景的快速路径。
    * 若生产者在此次读取后发生竞争，仍会发送 IPI；空闲循环还会通过可运行快照
    * 再次检查这一条件。
     */
    if (!atomic_load_explicit(&g_reschedule_pending[cpu_index],
                              memory_order_relaxed)) {
        return false;
    }

    return atomic_exchange_explicit(&g_reschedule_pending[cpu_index], false,
                                    memory_order_acq_rel);
}

bool x86_smp_reschedule_pending(uint32_t cpu_index) {
    if (cpu_index >= g_discovered_count || cpu_index >= MAX_CPUS) {
        return false;
    }

    return atomic_load_explicit(&g_reschedule_pending[cpu_index],
                                memory_order_acquire);
}

bool x86_smp_remote_user_self_test(void) {
    if (g_discovered_count <= 1U) return true;
    uint32_t current_cpu = x86_current_cpu_index();
    for (uint32_t cpu_index = 0; cpu_index < g_discovered_count; ++cpu_index) {
        if (cpu_index != current_cpu && x86_smp_cpu_online(cpu_index) &&
            x86_cpu_user_entry_count(cpu_index) != 0) return true;
    }
    return false;
}
