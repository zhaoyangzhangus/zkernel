#include <kernel/irq.h>
#include <kernel/perf.h>
#include <kernel/spinlock.h>
#include <kernel/telemetry.h>
#include <arch/x86_64/apic.h>
#include <arch/x86_64/context.h>

typedef struct irq_slot {
    irq_handler_t handler;
    void *context;
} irq_slot_t;

static spinlock_t g_irq_lock;
static atomic_uint g_irq_initialized;
static irq_slot_t g_irq_slots[IRQ_VECTOR_LAST + 1U];

static void irq_initialize(void) {
    /* 下方的 release 发布操作会使该表可供读取者使用。
     * 稳态 IRQ 分发不得写入与初始化代码共享的缓存行。 */
    if (atomic_load_explicit(&g_irq_initialized, memory_order_acquire) == 2U) return;
    unsigned expected = 0U;
    if (atomic_compare_exchange_strong_explicit(&g_irq_initialized, &expected, 1U,
                                                memory_order_acq_rel,
                                                memory_order_acquire)) {
        atomic_init(&g_irq_lock.state, 0U);
        for (uint32_t i = 0; i <= IRQ_VECTOR_LAST; ++i) {
            g_irq_slots[i].handler = 0;
            g_irq_slots[i].context = 0;
        }
        atomic_store_explicit(&g_irq_initialized, 2U, memory_order_release);
        return;
    }
    while (atomic_load_explicit(&g_irq_initialized, memory_order_acquire) != 2U) {
        __asm__ volatile ("pause");
    }
}

static uint64_t irq_lock(void) {
    uint64_t flags;
    /* 分发程序会在中断上下文中读取此表。初始化和加锁前必须屏蔽本地中断，
     * 首次发布时也不例外。 */
    __asm__ volatile ("pushfq; popq %0; cli" : "=r"(flags) : : "memory");
    irq_initialize();
    spinlock_lock(&g_irq_lock);
    return flags;
}

static void irq_unlock(uint64_t flags) {
    spinlock_unlock(&g_irq_lock);
    if ((flags & (1ULL << 9)) != 0U) {
        __asm__ volatile ("sti" : : : "memory");
    }
}

kstatus_t irq_register(uint8_t vector, irq_handler_t handler, void *context) {
    if (vector < IRQ_VECTOR_FIRST || vector > IRQ_VECTOR_LAST || handler == 0) {
        return K_EINVAL;
    }
    uint64_t flags = irq_lock();
    irq_slot_t *slot = &g_irq_slots[vector];
    if (slot->handler != 0) {
        irq_unlock(flags);
        return K_EBUSY;
    }
    slot->handler = handler;
    slot->context = context;
    irq_unlock(flags);
    return K_OK;
}

kstatus_t irq_unregister(uint8_t vector, irq_handler_t handler, void *context) {
    if (vector < IRQ_VECTOR_FIRST || vector > IRQ_VECTOR_LAST || handler == 0) {
        return K_EINVAL;
    }
    uint64_t flags = irq_lock();
    irq_slot_t *slot = &g_irq_slots[vector];
    if (slot->handler != handler || slot->context != context) {
        irq_unlock(flags);
        return K_ENOENT;
    }
    slot->handler = 0;
    slot->context = 0;
    irq_unlock(flags);
    return K_OK;
}

void x86_irq_dispatch(struct arch_trap_frame *frame) {
    if (frame == 0 || frame->vector < IRQ_VECTOR_FIRST ||
        frame->vector > IRQ_VECTOR_LAST) {
        lapic_end_of_interrupt();
        return;
    }
    uint8_t vector = (uint8_t)frame->vector;
    uint64_t flags = irq_lock();
    irq_handler_t handler = g_irq_slots[vector].handler;
    void *context = g_irq_slots[vector].context;
    irq_unlock(flags);
    if (handler != 0) handler(vector, frame, context);
    lapic_end_of_interrupt();
}

static void irq_self_test_handler(uint8_t vector, struct arch_trap_frame *frame,
                                  void *context) {
    (void)vector;
    (void)frame;
    uint32_t *hits = (uint32_t *)context;
    if (hits != 0) ++*hits;
}

bool irq_core_self_test(void) {
    uint32_t hits = 0;
    if (irq_register(48U, irq_self_test_handler, &hits) != K_OK ||
        irq_register(48U, irq_self_test_handler, &hits) != K_EBUSY) return false;
    struct arch_trap_frame frame = {0};
    frame.vector = 48U;
    uint64_t benchmark_start = telemetry_timestamp();
    x86_irq_dispatch(&frame);
    kernel_perf_emit_scope("io.irq_handler", benchmark_start);
    if (hits != 1U || irq_unregister(48U, irq_self_test_handler, &hits) != K_OK ||
        irq_unregister(48U, irq_self_test_handler, &hits) != K_ENOENT) return false;
    return true;
}
