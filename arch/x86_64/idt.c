#include "idt.h"

#include "../../lib/printf.h"
#include "../../mm/page_fault.h"

#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint16_t offset_low;
    uint16_t selector;
    uint8_t ist;
    uint8_t type_attr;
    uint16_t offset_mid;
    uint32_t offset_high;
    uint32_t zero;
} __attribute__((packed)) idt_entry_t;

typedef struct {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed)) idtr_t;

typedef struct {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed)) gdtr_t;

typedef struct {
    idt_handler_t handler;
    void *context;
} handler_slot_t;

#define GDT_KERNEL_CODE 0x08U
#define GDT_KERNEL_DATA 0x10U

static uint64_t g_gdt[3] __attribute__((aligned(16))) = {
    UINT64_C(0x0000000000000000),
    UINT64_C(0x00AF9A000000FFFF),
    UINT64_C(0x00CF92000000FFFF),
};

static idt_entry_t g_idt[IDT_VECTOR_COUNT] __attribute__((aligned(16)));
static handler_slot_t g_handlers[IDT_VECTOR_COUNT];

/*
 * bit=1 表示向量已占用。
 * 初始化后：
 *   0x00..0x1F CPU exceptions       = occupied
 *   0x20..0xEF dynamic IRQ pool     = free
 *   0xF0..0xFF kernel-reserved      = occupied
 */
static uint64_t g_vector_bitmap[4];

/* 由 interrupt_stubs.S 提供，每个元素对应一个独立 vector stub。 */
extern void (*const idt_stub_table[IDT_VECTOR_COUNT])(void);

static void gdt_init(void)
{
    gdtr_t gdtr = {
        .limit = (uint16_t)(sizeof(g_gdt) - 1),
        .base = (uint64_t)(uintptr_t)g_gdt,
    };

    __asm__ volatile(
        "lgdt %0\n\t"
        "movw $0x10, %%ax\n\t"
        "movw %%ax, %%ds\n\t"
        "movw %%ax, %%es\n\t"
        "movw %%ax, %%ss\n\t"
        "pushq $0x08\n\t"
        "leaq 1f(%%rip), %%rax\n\t"
        "pushq %%rax\n\t"
        "lretq\n\t"
        "1:\n\t"
        :
        : "m"(gdtr)
        : "rax", "memory");
}

static uint64_t read_cr2(void)
{
    uint64_t value;
    __asm__ volatile("mov %%cr2, %0" : "=r"(value));
    return value;
}

static void halt_forever(void)
{
    __asm__ volatile("cli");
    for (;;)
        __asm__ volatile("hlt");
}

static void set_gate(uint32_t vector, void (*handler)(void))
{
    uint64_t address = (uint64_t)(uintptr_t)handler;
    idt_entry_t *gate = &g_idt[vector];

    gate->offset_low = (uint16_t)address;
    gate->selector = GDT_KERNEL_CODE;
    gate->ist = 0;
    gate->type_attr = 0x8E; /* present, DPL0, 64-bit interrupt gate */
    gate->offset_mid = (uint16_t)(address >> 16);
    gate->offset_high = (uint32_t)(address >> 32);
    gate->zero = 0;
}

static inline uint64_t vector_bit(uint8_t vector)
{
    return UINT64_C(1) << (vector & 63U);
}

static inline uint32_t vector_word(uint8_t vector)
{
    return (uint32_t)vector >> 6;
}

static bool vector_is_claimed(uint8_t vector)
{
    uint64_t word =
        __atomic_load_n(&g_vector_bitmap[vector_word(vector)],
                        __ATOMIC_ACQUIRE);
    return (word & vector_bit(vector)) != 0;
}

static uint64_t dynamic_mask(uint32_t word)
{
    switch (word) {
    case 0:
        return UINT64_C(0xFFFFFFFF00000000); /* 0x20..0x3F */
    case 1:
    case 2:
        return UINT64_MAX;                   /* 0x40..0xBF */
    case 3:
        return UINT64_C(0x0000FFFFFFFFFFFF); /* 0xC0..0xEF */
    default:
        return 0;
    }
}

bool idt_vector_alloc(uint8_t *out_vector)
{
    if (out_vector == NULL)
        return false;

    for (uint32_t wi = 0; wi < 4; ++wi) {
        uint64_t allowed = dynamic_mask(wi);

        for (;;) {
            uint64_t old =
                __atomic_load_n(&g_vector_bitmap[wi], __ATOMIC_ACQUIRE);
            uint64_t free_bits = ~old & allowed;

            if (free_bits == 0)
                break;

            unsigned bit = (unsigned)__builtin_ctzll(free_bits);
            uint64_t desired = old | (UINT64_C(1) << bit);

            if (__atomic_compare_exchange_n(&g_vector_bitmap[wi],
                                            &old, desired,
                                            false,
                                            __ATOMIC_ACQ_REL,
                                            __ATOMIC_ACQUIRE)) {
                *out_vector = (uint8_t)((wi << 6) | bit);
                return true;
            }
        }
    }

    return false;
}

bool idt_vector_alloc_at(uint8_t vector)
{
    if (vector < IDT_DYNAMIC_FIRST || vector > IDT_DYNAMIC_LAST)
        return false;

    uint32_t wi = vector_word(vector);
    uint64_t bit = vector_bit(vector);
    uint64_t old =
        __atomic_fetch_or(&g_vector_bitmap[wi], bit, __ATOMIC_ACQ_REL);

    return (old & bit) == 0;
}

bool idt_vector_free(uint8_t vector)
{
    if (vector < IDT_DYNAMIC_FIRST || vector > IDT_DYNAMIC_LAST)
        return false;

    idt_handler_t handler =
        __atomic_load_n(&g_handlers[vector].handler, __ATOMIC_ACQUIRE);
    if (handler != NULL)
        return false;

    uint32_t wi = vector_word(vector);
    uint64_t bit = vector_bit(vector);
    uint64_t old =
        __atomic_fetch_and(&g_vector_bitmap[wi], ~bit, __ATOMIC_ACQ_REL);

    return (old & bit) != 0;
}

bool idt_handler_register(uint8_t vector,
                          idt_handler_t handler,
                          void *context)
{
    if (handler == NULL ||
        vector < IDT_EXCEPTION_COUNT ||
        !vector_is_claimed(vector))
        return false;

    idt_handler_t expected = NULL;

    /*
     * context 先发布，handler 用 release CAS 最后发布。
     * 一个 vector 同时只允许一个 handler。
     */
    __atomic_store_n(&g_handlers[vector].context, context, __ATOMIC_RELAXED);

    if (!__atomic_compare_exchange_n(&g_handlers[vector].handler,
                                     &expected, handler,
                                     false,
                                     __ATOMIC_RELEASE,
                                     __ATOMIC_ACQUIRE))
        return false;

    return true;
}

bool idt_handler_unregister(uint8_t vector)
{
    if (vector < IDT_EXCEPTION_COUNT ||
        !vector_is_claimed(vector))
        return false;

    idt_handler_t old =
        __atomic_exchange_n(&g_handlers[vector].handler,
                            NULL, __ATOMIC_ACQ_REL);

    /*
     * context 故意不立即清零：若另一个 CPU 已 acquire 到旧 handler，
     * 它仍需要看到对应 context。调用者仍需保证 context 的生命周期覆盖
     * 可能正在执行的旧 handler。
     */
    return old != NULL;
}

static void dispatch_exception(idt_frame_t *frame)
{
    if (frame->vector == 14U) {
        uint64_t address = read_cr2();

        if (page_fault_handle(address, frame->error_code))
            return;

        printf("[#PF] unhandled va=%p err=0x%llx rip=%p\n",
               (void *)(uintptr_t)address,
               (unsigned long long)frame->error_code,
               (void *)(uintptr_t)frame->rip);
        halt_forever();
    }

    printf("[EXC] vector=%lu err=0x%llx rip=%p cs=0x%llx\n",
           (unsigned long)frame->vector,
           (unsigned long long)frame->error_code,
           (void *)(uintptr_t)frame->rip,
           (unsigned long long)frame->cs);
    halt_forever();
}

void idt_dispatch(idt_frame_t *frame)
{
    if (frame == NULL || frame->vector >= IDT_VECTOR_COUNT) {
        printf("[IDT] invalid interrupt frame\n");
        halt_forever();
    }

    uint8_t vector = (uint8_t)frame->vector;

    if (vector < IDT_EXCEPTION_COUNT) {
        dispatch_exception(frame);
        return;
    }

    idt_handler_t handler =
        __atomic_load_n(&g_handlers[vector].handler, __ATOMIC_ACQUIRE);

    if (handler != NULL) {
        void *context =
            __atomic_load_n(&g_handlers[vector].context, __ATOMIC_RELAXED);
        handler(frame, context);
        return;
    }

    /*
     * Local APIC spurious vector 不需要 EOI，未注册时直接返回。
     * 其它未注册硬件向量说明 routing/driver 状态有问题，立即暴露。
     */
    if (vector == IDT_SPURIOUS_VECTOR)
        return;

    printf("[IRQ] unhandled vector=0x%x rip=%p\n",
           vector, (void *)(uintptr_t)frame->rip);
    halt_forever();
}

void idt_init(void)
{
    gdt_init();

    for (uint32_t i = 0; i < IDT_VECTOR_COUNT; ++i) {
        g_idt[i] = (idt_entry_t){0};
        g_handlers[i].handler = NULL;
        g_handlers[i].context = NULL;
        set_gate(i, idt_stub_table[i]);
    }

    __atomic_store_n(&g_vector_bitmap[0],
                     UINT64_C(0x00000000FFFFFFFF),
                     __ATOMIC_RELAXED);
    __atomic_store_n(&g_vector_bitmap[1], 0, __ATOMIC_RELAXED);
    __atomic_store_n(&g_vector_bitmap[2], 0, __ATOMIC_RELAXED);
    __atomic_store_n(&g_vector_bitmap[3],
                     UINT64_C(0xFFFF000000000000),
                     __ATOMIC_RELAXED);

    idtr_t idtr = {
        .limit = (uint16_t)(sizeof(g_idt) - 1),
        .base = (uint64_t)(uintptr_t)g_idt,
    };

    __asm__ volatile("lidt %0" :: "m"(idtr) : "memory");
}
