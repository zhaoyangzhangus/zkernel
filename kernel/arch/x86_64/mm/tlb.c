#include <arch/x86_64/acpi.h>
#include <arch/x86_64/apic.h>
#include <arch/x86_64/cpu.h>
#include <arch/x86_64/paging.h>
#include <arch/x86_64/smp.h>
#include <kernel/mm.h>
#include <kernel/sched.h>
#include <kernel/spinlock.h>
#include <kernel/console.h>

/*
 * 当前实现串行化 shootdown 请求。这样页表写入者可以严格执行：发布 PTE 变更、
 * 远端失效、等待确认、最后复用物理页。以后可替换为每 CPU 请求队列而不改变接口。
 */
static spinlock_t g_tlb_shootdown_lock;
static atomic_uint g_tlb_owner_cpu;
static atomic_uint_fast64_t g_tlb_generation;
static atomic_uint_fast64_t g_tlb_acknowledgement[MAX_CPUS];
static atomic_bool g_tlb_waiting[MAX_CPUS];
static vaddr_t g_tlb_address;
static paddr_t g_tlb_root;
static uint16_t g_tlb_pcid;
static bool g_tlb_full_root;

/* 即使普通启动关闭了常规 COM1 跟踪，也要保留罕见 shootdown 故障的信息。Chrome 会将
 * MADV_DONTNEED 故障转成分配器硬性 CHECK，因此需要记录目标 CPU 和地址以供诊断。 */
static void tlb_failure_putc(char value) {
    for (uint32_t attempt = 0U; attempt < 100000U; ++attempt) {
        uint8_t status;
        __asm__ volatile ("inb %w1, %0" : "=a"(status) : "d"(0x3FDU));
        if ((status & 0x20U) != 0U) break;
        __asm__ volatile ("pause");
    }
    __asm__ volatile ("outb %0, %w1" : : "a"((uint8_t)value), "d"(0x3F8U));
}

static void tlb_failure_text(const char *text) {
    if (text == 0) return;
    while (*text != '\0') tlb_failure_putc(*text++);
}

static void tlb_failure_hex(uint64_t value) {
    static const char digits[] = "0123456789abcdef";
    tlb_failure_text("0x");
    for (int shift = 60; shift >= 0; shift -= 4) {
        tlb_failure_putc(digits[(value >> (uint32_t)shift) & 0xFU]);
    }
}

static void tlb_failure_report(const char *reason, uint32_t owner,
                               uint32_t target, uint32_t apic_id,
                               uint64_t generation, uint64_t acknowledgement,
                               paddr_t root, vaddr_t address) {
    tlb_failure_text("OS_TLB_");
    tlb_failure_text(reason);
    tlb_failure_text(" owner=");
    tlb_failure_hex(owner);
    tlb_failure_text(" target=");
    tlb_failure_hex(target);
    tlb_failure_text(" apic=");
    tlb_failure_hex(apic_id);
    tlb_failure_text(" gen=");
    tlb_failure_hex(generation);
    tlb_failure_text(" ack=");
    tlb_failure_hex(acknowledgement);
    tlb_failure_text(" root=");
    tlb_failure_hex(root.value);
    tlb_failure_text(" va=");
    tlb_failure_hex((uint64_t)address);
    uint32_t state = UINT32_MAX, runnable = 0;
    uint64_t tid = 0, waits = 0;
    uint32_t lock = 0, lock_owner = UINT32_MAX, lock_waiter = UINT32_MAX;
    if (sched_debug_cpu(target, &state, &tid, &runnable)) {
        tlb_failure_text(" target_tid=");
        tlb_failure_hex(tid);
        tlb_failure_text(" target_state=");
        tlb_failure_hex(state);
        tlb_failure_text(" runnable=");
        tlb_failure_hex(runnable);
    }
    x86_page_table_debug_state(&lock, &lock_owner, &lock_waiter, &waits);
    tlb_failure_text(" pt_lock=");
    tlb_failure_hex(lock);
    tlb_failure_text(" pt_owner=");
    tlb_failure_hex(lock_owner);
    tlb_failure_text(" pt_waiter=");
    tlb_failure_hex(lock_waiter);
    tlb_failure_text(" pt_waits=");
    tlb_failure_hex(waits);
    tlb_failure_text("\r\n");
}

typedef struct {
    uint64_t pcid;
    uint64_t address;
} invpcid_descriptor_t;

static void shootdown_lock(void) {
    uint32_t cpu_index = x86_current_cpu_index();
    uint64_t saved_flags = 0;
    bool enabled_delivery = false;
    /*
    * 所有者等待远端确认时会发布 g_tlb_waiting。持有全局 shootdown 锁期间，定时器节拍
    * 不得将此 CPU 切换到其他线程；否则新线程会观察到过期的 TLB 等待状态，导致之后的
    * 每次 VM 操作都像是永久阻塞。
     */
    sched_preempt_disable();
    while (atomic_exchange_explicit(&g_tlb_shootdown_lock.state, 1U,
                                     memory_order_acquire) != 0U) {
        if (!enabled_delivery) {
            __asm__ volatile ("pushfq; popq %0" : "=r"(saved_flags) : : "memory");
            if (cpu_index < MAX_CPUS) {
                atomic_store_explicit(&g_tlb_waiting[cpu_index], true,
                                      memory_order_release);
            }
            /* 目标 CPU 可能正在等待同一把锁；必须允许 TLB IPI 进来完成确认。 */
            __asm__ volatile ("sti" : : : "memory");
            enabled_delivery = true;
        }
        __asm__ volatile ("pause");
    }
    atomic_store_explicit(&g_tlb_owner_cpu, cpu_index, memory_order_release);
    if (enabled_delivery) {
        if ((saved_flags & (1ULL << 9)) == 0) {
            __asm__ volatile ("cli" : : : "memory");
        }
        if (cpu_index < MAX_CPUS) {
            atomic_store_explicit(&g_tlb_waiting[cpu_index], false,
                                  memory_order_release);
        }
    }
}

static void shootdown_unlock(void) {
    atomic_store_explicit(&g_tlb_owner_cpu, UINT32_MAX,
                          memory_order_release);
    atomic_store_explicit(&g_tlb_shootdown_lock.state, 0U, memory_order_release);
    sched_preempt_enable();
}

static void invalidate_local_page(vaddr_t address) {
    __asm__ volatile ("invlpg (%0)" : : "r"((uint64_t)address) : "memory");
}

static uint16_t root_pcid(paddr_t root) {
    page_t *root_page = phys_to_page(root);
    return root_page != 0 && root_page->owner == PAGE_OWNER_PAGETABLE ?
           (uint16_t)root_page->private_data : 0U;
}

static void invalidate_target_page(paddr_t root, uint16_t pcid, vaddr_t address) {
    /* INVLPG 可直接作用于活动地址空间，并使全局转换失效。仅当用户根页表未激活时才
     * 需要 INVPCID。 */
    if ((uint64_t)address > X86_64_USER_TOP ||
        x86_current_root_table().value == root.value) {
        invalidate_local_page(address);
        return;
    }
    if (x86_boot_cpu_features.pcid && x86_boot_cpu_features.invpcid) {
        invpcid_descriptor_t descriptor = {(uint64_t)pcid, (uint64_t)address};
        /* INVPCID type 0：只失效一个线性地址下的一个 PCID。 */
        __asm__ volatile ("invpcid %0, %1" : : "m"(descriptor), "r"(0ULL)
                          : "memory");
        return;
    }
    (void)root;
    invalidate_local_page(address);
}

static void invalidate_target_root(paddr_t root, uint16_t pcid) {
    if (x86_current_root_table().value == root.value) {
        uint64_t cr3;
        __asm__ volatile ("mov %%cr3, %0" : "=r"(cr3));
        cr3 &= ~(1ULL << 63); /* 不请求 PCID 无刷新切换。 */
        __asm__ volatile ("mov %0, %%cr3" : : "r"(cr3) : "memory");
    } else if (x86_boot_cpu_features.pcid && x86_boot_cpu_features.invpcid) {
        invpcid_descriptor_t descriptor = {(uint64_t)pcid, 0U};
        __asm__ volatile ("invpcid %0, %1" : : "m"(descriptor), "r"(1ULL)
                          : "memory");
    }
    /* 未启用 PCID 时，切回未活动的根页表会刷新其页表项。 */
}

static bool send_tlb_ipi_with_retry(uint32_t apic_id) {
    /* ICR 可能正被本地 APIC 的定时器/其它 IPI 占用，短暂失败时重试。 */
    for (uint32_t attempt = 0; attempt < 8U; ++attempt) {
        if (lapic_send_fixed(apic_id, X86_TLB_IPI_VECTOR)) return true;
        for (uint32_t pause = 0; pause < 256U; ++pause) {
            __asm__ volatile ("pause");
        }
    }
    return false;
}

bool x86_tlb_shootdown_active(void) {
    uint32_t cpu_index = x86_current_cpu_index();
    return cpu_index < MAX_CPUS &&
           atomic_load_explicit(&g_tlb_waiting[cpu_index], memory_order_acquire);
}

bool x86_tlb_cpu_waiting(uint32_t cpu_index) {
    return cpu_index < MAX_CPUS &&
           atomic_load_explicit(&g_tlb_waiting[cpu_index],
                                memory_order_acquire);
}

void x86_tlb_wait_begin(void) {
    uint32_t cpu_index = x86_current_cpu_index();
    if (cpu_index < MAX_CPUS) {
        atomic_store_explicit(&g_tlb_waiting[cpu_index], true,
                              memory_order_release);
    }
}

void x86_tlb_wait_end(void) {
    uint32_t cpu_index = x86_current_cpu_index();
    if (cpu_index < MAX_CPUS) {
        atomic_store_explicit(&g_tlb_waiting[cpu_index], false,
                              memory_order_release);
    }
}

void x86_tlb_debug_state(uint32_t *lock_state, uint32_t *owner_cpu,
                         uint32_t *waiting_mask, uint32_t *ack_mask,
                         uint64_t *generation) {
    uint64_t current_generation = atomic_load_explicit(&g_tlb_generation,
                                                       memory_order_acquire);
    uint32_t waiting = 0U;
    uint32_t acknowledged = 0U;
    for (uint32_t cpu_index = 0; cpu_index < MAX_CPUS; ++cpu_index) {
        if (cpu_index < 32U &&
            atomic_load_explicit(&g_tlb_waiting[cpu_index],
                                 memory_order_acquire)) {
            waiting |= 1U << cpu_index;
        }
        if (cpu_index < 32U &&
            atomic_load_explicit(&g_tlb_acknowledgement[cpu_index],
                                 memory_order_acquire) == current_generation) {
            acknowledged |= 1U << cpu_index;
        }
    }
    if (lock_state != 0) {
        *lock_state = atomic_load_explicit(&g_tlb_shootdown_lock.state,
                                           memory_order_acquire);
    }
    if (owner_cpu != 0) {
        *owner_cpu = atomic_load_explicit(&g_tlb_owner_cpu,
                                          memory_order_acquire);
    }
    if (waiting_mask != 0) *waiting_mask = waiting;
    if (ack_mask != 0) *ack_mask = acknowledged;
    if (generation != 0) *generation = current_generation;
}

static uint64_t tlb_enable_ipi_delivery(uint32_t cpu_index) {
    uint64_t flags;
    __asm__ volatile ("pushfq; popq %0" : "=r"(flags) : : "memory");
    if (cpu_index < MAX_CPUS) {
        atomic_store_explicit(&g_tlb_waiting[cpu_index], true, memory_order_release);
    }
    /* IF=0 的调用者可能持有 vmalloc 等不可抢占锁；远端 TLB IPI 不需要
       本 CPU 开中断，因此这里不能为了等待 shootdown 而打开定时器抢占。 */
    return flags;
}

static void tlb_restore_interrupt_state(uint32_t cpu_index, uint64_t flags) {
    if (cpu_index < MAX_CPUS) {
        atomic_store_explicit(&g_tlb_waiting[cpu_index], false, memory_order_release);
    }
    if ((flags & (1ULL << 9)) == 0) {
        __asm__ volatile ("cli" : : : "memory");
    }
}

static void tlb_finish_shootdown(uint32_t cpu_index, uint64_t flags) {
    tlb_restore_interrupt_state(cpu_index, flags);
    shootdown_unlock();
    /* TLB 期间延迟的 reschedule 必须在释放 shootdown 锁后立即消费。 */
        /* 重调度请求在后续定时器或空闲路径中消费，避免切换引导栈。 */
}

void x86_tlb_ipi_interrupt(void) {
    uint64_t generation = atomic_load_explicit(&g_tlb_generation,
                                               memory_order_acquire);
    vaddr_t address = g_tlb_address;
    paddr_t root = g_tlb_root;
    if (g_tlb_full_root) invalidate_target_root(root, g_tlb_pcid);
    else invalidate_target_page(root, g_tlb_pcid, address);
    uint32_t cpu_index = x86_current_cpu_index();
    if (generation != 0 && cpu_index < MAX_CPUS) {
        atomic_store_explicit(&g_tlb_acknowledgement[cpu_index], generation,
                              memory_order_release);
    }
    lapic_end_of_interrupt();
}

static bool tlb_shootdown(paddr_t root, vaddr_t virtual_address, bool full_root) {
    if (root.value == 0 || !x86_is_canonical((uint64_t)virtual_address)) return false;

    /*
     * Shootdown 失败由页表调用者回滚 PTE。不要把一次 ACK 延迟升级为
     * 永久全局 poisoned 状态；否则一次偶发超时会让之后所有
     * unmap/protect 都永久返回 K_EIO。
     *
     * 请求由 g_tlb_shootdown_lock 串行化，IPI handler 总是读取当前
     * generation/address。旧 IPI 延迟到达最多造成一次额外 invalidation；
     * 对已经回滚的 PTE 是无害的。
     */
    shootdown_lock();

    uint32_t current_cpu = x86_current_cpu_index();
    uint64_t interrupt_flags = tlb_enable_ipi_delivery(current_cpu);
    bool targets[MAX_CPUS] = {false};

    uint64_t generation = atomic_load_explicit(&g_tlb_generation,
                                               memory_order_relaxed) + 1U;
    if (generation == 0) generation = 1U;
    g_tlb_root = root;
    g_tlb_address = virtual_address;
    g_tlb_pcid = root_pcid(root);
    g_tlb_full_root = full_root;
    atomic_store_explicit(&g_tlb_generation, generation, memory_order_release);
    if (full_root) invalidate_target_root(root, g_tlb_pcid);
    else invalidate_target_page(root, g_tlb_pcid, virtual_address);

    const x86_acpi_platform_t *platform = x86_acpi_platform();
    if (platform != 0) {
        bool user_address = (uint64_t)virtual_address <= X86_64_USER_TOP;

        /*
         * 用户地址只需要失效此刻正在执行同一 root 的 CPU。
         * 已切出的 CPU 下次 MOV CR3 进入该 PCID 时会自行刷新；内核地址仍
         * 广播全部 online CPU，因为内核半区共享且可能使用 GLOBAL PTE。
         *
         * 发送和等待共用同一个 targets 快照，避免两个循环之间发生调度后
         * 等待一个从未发送本 generation 的 CPU。
         */
        for (uint32_t cpu_index = 0; cpu_index < platform->cpu_count; ++cpu_index) {
            if (cpu_index == current_cpu || cpu_index >= MAX_CPUS ||
                !x86_smp_cpu_online(cpu_index)) continue;
            if (user_address && !sched_cpu_uses_root(cpu_index, root)) continue;
            targets[cpu_index] = true;
            if (!send_tlb_ipi_with_retry(platform->cpus[cpu_index].apic_id)) {
                tlb_failure_report("SEND_FAIL", current_cpu, cpu_index,
                                   platform->cpus[cpu_index].apic_id,
                                   generation, 0U, root, virtual_address);
                tlb_finish_shootdown(current_cpu, interrupt_flags);
                return false;
            }
        }

        uint64_t start = x86_read_tsc();
        uint64_t ticks = x86_timeout_ns_to_tsc(1000000000ULL);
        uint64_t deadline = ticks > UINT64_MAX - start ? UINT64_MAX : start + ticks;
        for (uint32_t cpu_index = 0; cpu_index < platform->cpu_count; ++cpu_index) {
            if (cpu_index >= MAX_CPUS || !targets[cpu_index]) continue;
            while (atomic_load_explicit(&g_tlb_acknowledgement[cpu_index],
                                        memory_order_acquire) != generation) {
                if ((int64_t)(x86_read_tsc() - deadline) >= 0) {
                    tlb_failure_report(
                        "ACK_TIMEOUT", current_cpu, cpu_index,
                        platform->cpus[cpu_index].apic_id, generation,
                        atomic_load_explicit(
                            &g_tlb_acknowledgement[cpu_index],
                            memory_order_relaxed), root, virtual_address);
                    tlb_finish_shootdown(current_cpu, interrupt_flags);
                    return false;
                }
                __asm__ volatile ("pause");
            }
        }
    }
    tlb_finish_shootdown(current_cpu, interrupt_flags);
    return true;
}

bool x86_tlb_shootdown_page(paddr_t root, vaddr_t virtual_address) {
    return tlb_shootdown(root, virtual_address, false);
}

bool x86_tlb_shootdown_root(paddr_t root) {
    return tlb_shootdown(root, 0U, true);
}

bool x86_tlb_shootdown_self_test(void) {
    /* 未映射地址上的 INVLPG 没有副作用，但仍会完整经过所有 online CPU。 */
    return x86_tlb_shootdown_page(x86_current_root_table(), (vaddr_t)0x7000U);
}
