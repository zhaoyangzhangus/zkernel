#include "idt.h"

#include "../../lib/printf.h"
#include "../../mm/page_fault.h"

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

/*
 * 前三个字段在 ring0->ring0 exception 中总是存在。
 * rsp/ss 只在 privilege level 改变时由 CPU 压栈，本处理器不读取它们。
 */
typedef struct {
    uint64_t rip;
    uint64_t cs;
    uint64_t rflags;
    uint64_t rsp;
    uint64_t ss;
} interrupt_frame_t;

static idt_entry_t g_idt[256] __attribute__((aligned(16)));

static uint16_t read_cs(void)
{
    uint16_t cs;
    __asm__ volatile("mov %%cs, %0" : "=r"(cs));
    return cs;
}

static uint64_t read_cr2(void)
{
    uint64_t value;
    __asm__ volatile("mov %%cr2, %0" : "=r"(value));
    return value;
}

static inline void debug_char(char c)
{
    __asm__ volatile("outb %0, $0xe9" :: "a"((uint8_t)c));
}

static void debug_text(const char *s)
{
    while (*s != '\0')
        debug_char(*s++);
}

static void halt_forever(void)
{
    __asm__ volatile("cli");
    for (;;)
        __asm__ volatile("hlt");
}

__attribute__((interrupt))
static void page_fault_isr(interrupt_frame_t *frame, uint64_t error_code)
{
    debug_char('P');

    uint64_t address = read_cr2();

    if (page_fault_handle(address, error_code)) {
        debug_char('R');
        return;
    }

    debug_char('U');
    printf("[#PF] unhandled va=%p err=0x%llx rip=%p\n",
           (void *)(uintptr_t)address,
           (unsigned long long)error_code,
           (void *)(uintptr_t)frame->rip);
    halt_forever();
}

__attribute__((interrupt))
static void double_fault_isr(interrupt_frame_t *frame, uint64_t error_code)
{
    (void)frame;
    (void)error_code;
    debug_text("\n[#DF]\n");
    halt_forever();
}

__attribute__((interrupt))
static void general_protection_isr(interrupt_frame_t *frame,
                                   uint64_t error_code)
{
    (void)frame;
    (void)error_code;
    debug_text("\n[#GP]\n");
    halt_forever();
}

static void set_gate(uint32_t vector, void (*handler)(void))
{
    uint64_t address = (uint64_t)(uintptr_t)handler;
    idt_entry_t *gate = &g_idt[vector];

    gate->offset_low = (uint16_t)address;
    gate->selector = read_cs();
    gate->ist = 0;
    gate->type_attr = 0x8E; /* present, DPL0, 64-bit interrupt gate */
    gate->offset_mid = (uint16_t)(address >> 16);
    gate->offset_high = (uint32_t)(address >> 32);
    gate->zero = 0;
}

void idt_init(void)
{
    for (uint32_t i = 0; i < 256; ++i)
        g_idt[i] = (idt_entry_t){0};

    /*
     * GCC interrupt attribute 生成 exception prologue/epilogue，并自动
     * 丢弃 #PF 的 error code 后 iretq。KCFLAGS 已启用
     * -mgeneral-regs-only 和 -mno-red-zone。
     */
    set_gate(8,  (void (*)(void))double_fault_isr);
    set_gate(13, (void (*)(void))general_protection_isr);
    set_gate(14, (void (*)(void))page_fault_isr);

    idtr_t idtr = {
        .limit = (uint16_t)(sizeof(g_idt) - 1),
        .base = (uint64_t)(uintptr_t)g_idt,
    };

    __asm__ volatile("lidt %0" :: "m"(idtr) : "memory");
}
