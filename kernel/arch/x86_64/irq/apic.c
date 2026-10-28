#include "arch/x86_64/apic.h"
#include "arch/x86_64/cpu.h"
#include "arch/x86_64/paging.h"
#include <arch/x86_64/smp.h>
#include <kernel/sched.h>
#include <kernel/process.h>
#include <uapi/process.h>
#include <kernel/deferred.h>
#include <kernel/timer.h>
#include <kernel/wait.h>
#include <kernel/watchdog.h>
#include <kernel/e1000.h>
#include <kernel/nvme_core.h>
#include <kernel/realtest.h>

#define LAPIC_ID_REGISTER            0x020U
#define LAPIC_VERSION_REGISTER       0x030U
#define LAPIC_SPURIOUS_REGISTER      0x0F0U
#define LAPIC_EOI_REGISTER           0x0B0U
#define LAPIC_LVT_TIMER_REGISTER     0x320U
#define LAPIC_ICR_LOW_REGISTER       0x300U
#define LAPIC_ICR_HIGH_REGISTER      0x310U
#define LAPIC_TIMER_DIVIDE_REGISTER  0x3E0U
#define LAPIC_TIMER_INITIAL_REGISTER 0x380U
#define LAPIC_TIMER_CURRENT_REGISTER 0x390U
#define LAPIC_ENABLE_BIT             (1U << 8)
#define LAPIC_TIMER_PERIODIC_BIT     (1U << 17)
#define LAPIC_TIMER_MASKED_BIT       (1U << 16)
#define LAPIC_ICR_DELIVERY_PENDING   (1U << 12)
#define LAPIC_ICR_LEVEL_ASSERT       (1U << 14)
#define LAPIC_ICR_TRIGGER_LEVEL      (1U << 15)
#define LAPIC_DELIVERY_INIT          (5U << 8)
#define LAPIC_DELIVERY_STARTUP       (6U << 8)
#define LAPIC_KERNEL_VIRTUAL_BASE    (X86_64_KERNEL_AUX_BASE + 0x00100000ULL)
#define IA32_APIC_BASE_MSR            0x1BU
#define IA32_APIC_GLOBAL_ENABLE       (1ULL << 11)

static volatile UINT32 *g_lapic = (volatile UINT32 *)(uintptr_t)OS_LAPIC_BASE;
static volatile UINT64 g_tick_count;
static BOOLEAN g_initialized;
static UINT32 g_timer_initial_count;
#define LAPIC_SCHED_PERIOD_NS 2000000ULL
#define LAPIC_CALIBRATION_NS 5000000ULL
/*
 * 定时器到期、等待队列超时和看门狗超时都会获取普通内核锁，并可能唤醒线程。它们不得
 * 在 LAPIC 硬中断中运行：否则中断若在 CPU 持有此类锁时到达，就会递归获取同一把锁并
 * 永久自旋。
 *
 * 此处有意使用单个合并式维护项。它与设备轮询无关：xHCI 仍完全由其 MSI-X 处理程序驱动。
 */
static atomic_bool g_timer_maintenance_pending;
static atomic_bool g_timer_maintenance_queued;

static void lapic_schedule_timer_maintenance(void);

static void lapic_timer_maintenance_work(void *argument) {
    uint64_t now_tsc;
    (void)argument;

    /* 检查截止时间前，先消费触发此工作的时钟节拍。 */
    (void)atomic_exchange_explicit(&g_timer_maintenance_pending, false,
                                   memory_order_acq_rel);
    now_tsc = x86_read_tsc();
    timer_poll(now_tsc);
    (void)watchdog_poll(now_tsc);
    wait_poll_timeouts(now_tsc);

    /*
    * 检查 pending 前先发布完成状态。若此处发生中断竞争，它要么自行排入后继工作，
    * 要么保留 pending 标志供下方调用处理；不会丢失超时节拍。
     */
    atomic_store_explicit(&g_timer_maintenance_queued, false,
                          memory_order_release);
    if (atomic_load_explicit(&g_timer_maintenance_pending,
                             memory_order_acquire)) {
        lapic_schedule_timer_maintenance();
    }
}

static void lapic_schedule_timer_maintenance(void) {
    bool expected = false;
    atomic_store_explicit(&g_timer_maintenance_pending, true,
                          memory_order_release);
    if (!atomic_compare_exchange_strong_explicit(&g_timer_maintenance_queued,
                                                 &expected, true,
                                                 memory_order_acq_rel,
                                                 memory_order_acquire)) {
        return;
    }
    if (!deferred_try_schedule(lapic_timer_maintenance_work, 0)) {
        /* 保持 pending 标志；下一个周期性定时器中断会重试。 */
        atomic_store_explicit(&g_timer_maintenance_queued, false,
                              memory_order_release);
    }
}

static UINT64 lapic_read_msr(UINT32 index) {
    UINT32 low;
    UINT32 high;
    __asm__ volatile ("rdmsr" : "=a"(low), "=d"(high) : "c"(index));
    return ((UINT64)high << 32) | low;
}

static VOID lapic_write_msr(UINT32 index, UINT64 value) {
    __asm__ volatile ("wrmsr" : : "c"(index), "a"((UINT32)value),
                      "d"((UINT32)(value >> 32)) : "memory");
}

static VOID lapic_write(UINT32 offset, UINT32 value) {
    g_lapic[offset / sizeof(UINT32)] = value;
}

static UINT32 lapic_read(UINT32 offset) {
    return g_lapic[offset / sizeof(UINT32)];
}

static BOOLEAN lapic_wait_icr_idle(VOID) {
    uint64_t start = x86_read_tsc();
    uint64_t ticks = x86_timeout_ns_to_tsc(1000000ULL);
    uint64_t deadline = ticks > UINT64_MAX - start ? UINT64_MAX : start + ticks;
    while ((lapic_read(LAPIC_ICR_LOW_REGISTER) & LAPIC_ICR_DELIVERY_PENDING) != 0) {
        if ((int64_t)(x86_read_tsc() - deadline) >= 0) return 0;
        __asm__ volatile ("pause");
    }
    return 1;
}

static BOOLEAN lapic_send_ipi(UINT32 apic_id, UINT32 command) {
    if (!g_initialized || apic_id > 0xFFU) return 0;

    /*
    * ICR_HIGH 与 ICR_LOW 构成一个逻辑事务。特别是，当此 CPU 正在执行 TLB shootdown 时，
    * 定时器中断可能排入延迟工作，并由其发送重新调度 IPI。不得让本地中断在两次 MMIO
    * 写入之间覆盖目标地址。可能耗时的投递等待应放在关闭中断的临界区之外；获取该区后
    * 还需重新检查，以防等待期间中断发送了另一条 IPI。
     */
    UINT64 interrupt_flags;
    for (;;) {
        if (!lapic_wait_icr_idle()) return 0;
        __asm__ volatile ("pushfq; popq %0; cli"
                          : "=r"(interrupt_flags) : : "memory");
        if ((lapic_read(LAPIC_ICR_LOW_REGISTER) &
             LAPIC_ICR_DELIVERY_PENDING) == 0U) {
            break;
        }
        if ((interrupt_flags & (1ULL << 9)) != 0U) {
            __asm__ volatile ("sti" : : : "memory");
        }
    }
    lapic_write(LAPIC_ICR_HIGH_REGISTER, apic_id << 24);
    lapic_write(LAPIC_ICR_LOW_REGISTER, command);
    if ((interrupt_flags & (1ULL << 9)) != 0U) {
        __asm__ volatile ("sti" : : : "memory");
    }
    return lapic_wait_icr_idle();
}

BOOLEAN lapic_send_init(UINT32 apic_id) {
    return lapic_send_ipi(apic_id, LAPIC_DELIVERY_INIT |
                          LAPIC_ICR_LEVEL_ASSERT | LAPIC_ICR_TRIGGER_LEVEL);
}

BOOLEAN lapic_send_init_deassert(UINT32 apic_id) {
    return lapic_send_ipi(apic_id,
                          LAPIC_DELIVERY_INIT | LAPIC_ICR_TRIGGER_LEVEL);
}

BOOLEAN lapic_send_startup(UINT32 apic_id, UINT8 vector) {
    if (vector == 0) return 0;
    return lapic_send_ipi(apic_id, LAPIC_DELIVERY_STARTUP | vector);
}

BOOLEAN lapic_send_fixed(UINT32 apic_id, UINT8 vector) {
    if (vector < 32U) return 0;
    return lapic_send_ipi(apic_id, vector);
}

/* 使用已初始化的 TSC 校准分频后的 APIC 时钟。固定计数没有可移植的时间单位
 *（在 QEMU 中 1,000,000 表示 16 ms）。屏蔽的一次性定时器可避免校准过程发布定时器工作。 */
static UINT32 lapic_calibrate_timer(UINT8 timer_vector, UINT32 fallback) {
    uint64_t window = x86_timeout_ns_to_tsc(LAPIC_CALIBRATION_NS);
    if (window == 0U) return fallback;
    lapic_write(LAPIC_LVT_TIMER_REGISTER,
                ((UINT32)timer_vector & 0xFFU) | LAPIC_TIMER_MASKED_BIT);
    lapic_write(LAPIC_TIMER_INITIAL_REGISTER, UINT32_MAX);
    uint64_t started = x86_read_tsc();
    UINT32 first = lapic_read(LAPIC_TIMER_CURRENT_REGISTER);
    uint64_t ended = started;
    for (uint32_t polls = 0U; polls < 1000000U; ++polls) {
        ended = x86_read_tsc();
        if (ended < started || ended - started >= window) break;
        __asm__ volatile("pause" : : : "memory");
    }
    UINT32 last = lapic_read(LAPIC_TIMER_CURRENT_REGISTER);
    if (ended < started || ended - started < window || first <= last)
        return fallback;
    uint64_t elapsed_ns = x86_tsc_to_ns(ended - started);
    if (elapsed_ns < LAPIC_CALIBRATION_NS || elapsed_ns > 100000000ULL)
        return fallback;
    /* 32 位 APIC 时间差乘以 2 ms 不会导致 uint64_t 溢出。 */
    uint64_t count = (uint64_t)(first - last) * LAPIC_SCHED_PERIOD_NS / elapsed_ns;
    if (count == 0U || count > UINT32_MAX) return fallback;
    return (UINT32)count;
}

BOOLEAN lapic_init(UINT8 timer_vector, UINT32 initial_count) {
    if (g_initialized || timer_vector < 32U || initial_count == 0) return 0;
    /* 读取版本寄存器，确保恒等映射下的 LAPIC MMIO 区可访问。 */
    if (lapic_read(LAPIC_VERSION_REGISTER) == 0) return 0;
    lapic_write(LAPIC_SPURIOUS_REGISTER, LAPIC_ENABLE_BIT | 0xFFU);
    lapic_write(LAPIC_TIMER_DIVIDE_REGISTER, 0x3U);
    g_timer_initial_count = lapic_calibrate_timer(timer_vector, initial_count);
    lapic_write(LAPIC_LVT_TIMER_REGISTER,
                ((UINT32)timer_vector & 0xFFU) | LAPIC_TIMER_PERIODIC_BIT);
    atomic_init(&g_timer_maintenance_pending, false);
    atomic_init(&g_timer_maintenance_queued, false);
    lapic_write(LAPIC_TIMER_INITIAL_REGISTER, g_timer_initial_count);
    g_tick_count = 0;
    g_initialized = 1;
    return 1;
}

BOOLEAN lapic_init_secondary(UINT32 expected_apic_id, UINT8 timer_vector,
                                    UINT32 initial_count) {
    if (expected_apic_id > 0xFFU || timer_vector < 32U || initial_count == 0 ||
        !__atomic_load_n(&g_initialized, __ATOMIC_ACQUIRE)) return 0;

    UINT64 apic_base = lapic_read_msr(IA32_APIC_BASE_MSR);
    if ((apic_base & IA32_APIC_GLOBAL_ENABLE) == 0) {
        lapic_write_msr(IA32_APIC_BASE_MSR, apic_base | IA32_APIC_GLOBAL_ENABLE);
    }
    if (lapic_read(LAPIC_VERSION_REGISTER) == 0 ||
        (lapic_read(LAPIC_ID_REGISTER) >> 24) != expected_apic_id) return 0;

    /* IF 在运行队列建立前保持为零，因此可以先编程周期定时器再发布 online。 */
    lapic_write(LAPIC_SPURIOUS_REGISTER, LAPIC_ENABLE_BIT | 0xFFU);
    lapic_write(LAPIC_TIMER_DIVIDE_REGISTER, 0x3U);
    lapic_write(LAPIC_LVT_TIMER_REGISTER,
                ((UINT32)timer_vector & 0xFFU) | LAPIC_TIMER_PERIODIC_BIT |
                LAPIC_TIMER_MASKED_BIT);
    /* 所有 AP 都使用 BSP 校准出的周期，而非回退参数。 */
    lapic_write(LAPIC_TIMER_INITIAL_REGISTER, g_timer_initial_count);
    lapic_write(LAPIC_EOI_REGISTER, 0);
    return 1;
}

VOID lapic_enable_timer(void) {
    if (!g_initialized) return;
    lapic_write(LAPIC_LVT_TIMER_REGISTER,
                lapic_read(LAPIC_LVT_TIMER_REGISTER) &
                ~LAPIC_TIMER_MASKED_BIT);
}

BOOLEAN lapic_use_kernel_mapping(VOID) {
    paddr_t root = x86_current_root_table();
    paddr_t physical = paddr_make(OS_LAPIC_BASE);
    kstatus_t status = x86_map_page(root, (vaddr_t)LAPIC_KERNEL_VIRTUAL_BASE,
                                    physical, X86_PAGE_WRITE | X86_PAGE_GLOBAL,
                                    X86_CACHE_UC);
    if (status != K_OK && status != K_EBUSY) return 0;
    paddr_t translated;
    uint64_t flags;
    if (x86_translate_page(root, (vaddr_t)LAPIC_KERNEL_VIRTUAL_BASE,
                           &translated, &flags) != K_OK ||
        (translated.value & ~(PAGE_SIZE - 1ULL)) != OS_LAPIC_BASE) return 0;
    g_lapic = (volatile UINT32 *)(uintptr_t)LAPIC_KERNEL_VIRTUAL_BASE;
    return 1;
}

VOID lapic_end_of_interrupt(void) {
    if (g_initialized) lapic_write(LAPIC_EOI_REGISTER, 0);
}

UINT64 lapic_tick_count(void) {
    return __atomic_load_n(&g_tick_count, __ATOMIC_RELAXED);
}

UINT64 lapic_timer_interrupt(OS_INTERRUPT_CONTEXT *context) {
    (void)context;
    __atomic_add_fetch(&g_tick_count, 1ULL, __ATOMIC_RELAXED);
    /* 设备硬中断只排队；USB 事件和网络包在普通内核上下文处理。 */
    /* xHCI 和 E1000 输入由中断驱动；定时器中断绝不轮询设备。 */
    (void)nvme_schedule_deferred_poll();
    lapic_schedule_timer_maintenance();
    lapic_end_of_interrupt();
    uint64_t now_tsc = x86_read_tsc();
    sched_trace_wake_stall(now_tsc);
    /* deadline.S 在硬件 RIP/CS/RFLAGS 帧之前保存 15 个 GPR。 */
    const uint64_t *saved = (const uint64_t *)context;
    sched_trace_inbox_stall(now_tsc, saved != 0 ? saved[15] : 0U,
                            saved != 0 ? saved[17] : 0U);
    if (realtest_timeout_poll(now_tsc))
        realtest_timeout_reboot();
    /* 页表锁与 vmalloc 锁持有期间只接收 TLB IPI，不能在此重入调度。 */
    if (x86_tlb_shootdown_active()) return 0;
    /*
    * SYSCALL 的 C 代码以 IF=1 运行，以便接收 TLB IPI。定时器不得抢占该 C 调用链：
    * 它的返回帧位于当前线程的系统调用栈上，并由系统调用存根恢复。
     */
    if (x86_syscall_active_fast()) return 0;
    sched_tick(x86_tsc_to_ns(now_tsc));
    /* EOI 已完成，且 CPL3 中断发生时没有内核临界区处于活动状态。
     * 此处也要处理 SIGKILL：卡住的用户循环可能永远不会发起系统调用。 */
    if (saved != 0 && (saved[16] & 3U) == 3U && !sched_preempt_disabled()) {
        thread_t *thread = sched_current_thread();
        if (thread != 0 && (__atomic_load_n(&thread->flags, __ATOMIC_ACQUIRE) & THREAD_FLAG_TERMINATE_REQUEST))
            thread_exit(-(int64_t)OS_SIGKILL);
        const uint64_t kill_bit = 1ULL << (OS_SIGKILL - 1U);
        if (thread != 0 && (__atomic_load_n(&thread->signal_pending, __ATOMIC_ACQUIRE) & kill_bit)) {
            __atomic_fetch_and(&thread->signal_pending, ~kill_bit, __ATOMIC_ACQ_REL);
            process_exit_signal(OS_SIGKILL);
        }
    }
    /* 在系统调用期间收到的 IPI 会保留待处理请求。远端唤醒仍在收件箱中，尚未计入
     * nr_running，因此系统调用返回后的普通时间片检查无法发现它们。仅在安全边界消费
     * 请求；映射/分配器临界区禁用抢占期间不得丢失请求。 */
    if (!sched_preempt_disabled() && x86_smp_take_reschedule_request()) {
        sched_handle_reschedule_request();
        return 0;
    }
    if (sched_tick_should_preempt()) schedule();
    return 0;
}
