#ifndef __KERNEL_ARCH_X86_64_IDT_H__
#define __KERNEL_ARCH_X86_64_IDT_H__

#include <stdbool.h>
#include <stdint.h>

#define IDT_VECTOR_COUNT         256U
#define IDT_EXCEPTION_COUNT      32U
#define IDT_DYNAMIC_FIRST        0x20U
#define IDT_DYNAMIC_LAST         0xEFU
#define IDT_KERNEL_FIRST         0xF0U
#define IDT_KERNEL_LAST          0xFFU
#define IDT_SPURIOUS_VECTOR      0xFFU

/*
 * interrupt_stubs.S 保存全部通用寄存器后形成的统一 frame。
 * user_rsp/user_ss 只在 CPL 变化时存在；只有 (cs & 3) != 0 时才能读取。
 */
typedef struct {
    uint64_t rax;
    uint64_t rcx;
    uint64_t rdx;
    uint64_t rbx;
    uint64_t rbp;
    uint64_t rsi;
    uint64_t rdi;
    uint64_t r8;
    uint64_t r9;
    uint64_t r10;
    uint64_t r11;
    uint64_t r12;
    uint64_t r13;
    uint64_t r14;
    uint64_t r15;

    uint64_t vector;
    uint64_t error_code;

    uint64_t rip;
    uint64_t cs;
    uint64_t rflags;

    uint64_t user_rsp;
    uint64_t user_ss;
} idt_frame_t;

typedef void (*idt_handler_t)(const idt_frame_t *frame, void *context);

void idt_init(void);

/*
 * 设备 IRQ/MSI/MSI-X 动态向量池：0x20..0xEF。
 * 0x00..0x1F 固定给 CPU exception；
 * 0xF0..0xFF 预留给 kernel IPI/timer/spurious 等内部用途。
 */
bool idt_vector_alloc(uint8_t *out_vector);
bool idt_vector_alloc_at(uint8_t vector);
bool idt_vector_free(uint8_t vector);

/*
 * 动态向量必须先 alloc/alloc_at；0xF0..0xFF 已由 idt_init() 预留，
 * 可直接注册 kernel 内部 handler。
 */
bool idt_handler_register(uint8_t vector,
                          idt_handler_t handler,
                          void *context);
bool idt_handler_unregister(uint8_t vector);

/* interrupt_stubs.S 的统一 C dispatcher。 */
void idt_dispatch(idt_frame_t *frame);

#endif
