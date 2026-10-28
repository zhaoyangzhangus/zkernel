#include <arch/x86_64/interrupt.h>
#include <arch/x86_64/uaccess.h>
#include <arch/x86_64/paging.h>
#include <arch/x86_64/syscall_internal.h>
#include <kernel/process.h>
#include <kernel/realtest.h>
#include <kernel/sched.h>
#include <kernel/crash_dump.h>
#include <kernel/console.h>

#define COM1_DATA   0x3F8U
#define COM1_STATUS 0x3FDU

static volatile uint64_t g_exception_count[32];
static volatile bool g_ud_self_test;
static volatile uint32_t g_uaccess_fault_count;

static uint8_t port_in8(uint16_t port) {
    uint8_t value;
    __asm__ volatile ("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

static void port_out8(uint16_t port, uint8_t value) {
    __asm__ volatile ("outb %0, %1" : : "a"(value), "Nd"(port));
}

static void exception_write(const char *text) {
    while (*text != '\0') {
        for (uint32_t tries = 0; tries < 100000U &&
             (port_in8(COM1_STATUS) & 0x20U) == 0; ++tries) { }
        port_out8(COM1_DATA, (uint8_t)*text++);
    }
}

static void exception_write_hex(uint64_t value) {
    static const char digits[] = "0123456789ABCDEF";
    exception_write("0x");
    for (int shift = 60; shift >= 0; shift -= 4) {
        port_out8(COM1_DATA, (uint8_t)digits[(value >> (uint32_t)shift) & 0xFU]);
    }
}

static uint64_t read_cr2(void) {
    uint64_t value;
    __asm__ volatile ("mov %%cr2, %0" : "=r"(value));
    return value;
}

static const char *exception_name(uint64_t vector) {
    static const char *const names[32] = {
        "#DE", "#DB", "NMI", "#BP", "#OF", "#BR", "#UD", "#NM",
        "#DF", "保留", "#TS", "#NP", "#SS", "#GP", "#PF", "保留",
        "#MF", "#AC", "#MC", "#XM", "#VE", "#CP", "保留", "保留",
        "保留", "保留", "保留", "保留", "#HV", "#VC", "#SX", "保留",
    };
    return vector < 32U ? names[vector] : "IRQ";
}

static bool exception_is_user_process_fault(
    const arch_trap_frame_t *frame) {
    return frame != 0 && frame->vector < 32U &&
           (frame->cs & 3U) == 3U && frame->vector != 2U &&
           frame->vector != 3U;
}

/* 未处理的同步故障会使整个地址空间失效，而不只是单个工作线程。若故障线程承担了 GPU
 * 服务或其他进程级角色，让同进程的其他线程继续运行会使 IPC 和互斥锁等待者永久受阻。
 * 已解决的延迟缺页、UAccess 修复和调试器陷阱不会到达此处。 */
static __noreturn void exception_exit_user_process(uint64_t vector) {
    uint32_t signal;
    switch (vector) {
    case 0U:  /* 除法错误 */
    case 16U: /* x87 浮点异常 */
    case 19U: /* SIMD 浮点异常 */
        signal = OS_SIGFPE;
        break;
    case 6U:
        signal = OS_SIGILL;
        break;
    case 17U:
        signal = OS_SIGBUS;
        break;
    default:
        signal = OS_SIGSEGV;
        break;
    }
    /* 中断门已清除 IF。此路径仅处理用户态故障且不会返回，也不持有缺页处理锁；但拆除
     * 过程可能等待远端调度器和页表 shootdown，因此必须确保它们的 IPI 可送达。与已解决
     * 的故障不同，此路径不会进入 SWAPGS 收尾代码。 */
    __asm__ volatile ("sti" : : : "memory");
    process_exit_signal(signal);
}

static void record_user_opcode(const arch_trap_frame_t *frame) {
    uint8_t bytes[8] = {0};
    if (frame == 0 || (frame->cs & 3U) != 3U ||
        !x86_user_range_valid((const void __user *)(uintptr_t)frame->rip,
                              sizeof(bytes)) ||
        copy_from_user(bytes,
                       (const void __user *)(uintptr_t)frame->rip,
                       sizeof(bytes)) != K_OK) {
        return;
    }
    kernel_serial_write("OS_REALTEST_OPCODE");
    for (uint32_t index = 0U; index < sizeof(bytes); ++index) {
        kernel_serial_write(" ");
        kernel_serial_write_u32(bytes[index]);
    }
    kernel_serial_write("\r\n");
}

static bool exception_process_is_chrome(const process_t *process) {
    static const char expected[] = "chrome";
    if (process == 0) return false;
    for (size_t index = 0U; index < sizeof(expected); ++index) {
        if (process->name[index] != expected[index]) return false;
    }
    return true;
}

/* 保留足够的 Ring3 状态，以便识别可恢复的用户故障，而无需进入通用崩溃转储路径。 */
static void record_user_context(const arch_trap_frame_t *frame,
                                const thread_t *thread) {
    uint64_t stack[8] = {0};

    if (frame == 0) return;

    exception_write("OS_USER_REGS pid=");
    exception_write_hex(
        thread != 0 && thread->process != 0 ?
            thread->process->pid : 0U);
    exception_write(" rsp=");
    exception_write_hex(frame->rsp);
    exception_write(" rbp=");
    exception_write_hex(frame->rbp);
    exception_write(" rax=");
    exception_write_hex(frame->rax);
    exception_write(" rbx=");
    exception_write_hex(frame->rbx);
    exception_write(" rcx=");
    exception_write_hex(frame->rcx);
    exception_write(" rdx=");
    exception_write_hex(frame->rdx);
    exception_write(" rsi=");
    exception_write_hex(frame->rsi);
    exception_write(" rdi=");
    exception_write_hex(frame->rdi);
    exception_write(" r8=");
    exception_write_hex(frame->r8);
    exception_write(" r9=");
    exception_write_hex(frame->r9);
    exception_write(" r10=");
    exception_write_hex(frame->r10);
    exception_write(" r11=");
    exception_write_hex(frame->r11);
    exception_write(" r12=");
    exception_write_hex(frame->r12);
    exception_write(" r13=");
    exception_write_hex(frame->r13);
    exception_write(" r14=");
    exception_write_hex(frame->r14);
    exception_write(" r15=");
    exception_write_hex(frame->r15);
    exception_write("\r\n");

    if (!x86_user_range_valid((const void __user *)(uintptr_t)frame->rsp,
                              sizeof(stack)) ||
        copy_from_user(stack,
                       (const void __user *)(uintptr_t)frame->rsp,
                       sizeof(stack)) != K_OK) {
        return;
    }

    exception_write("OS_USER_STACK rsp=");
    exception_write_hex(frame->rsp);
    for (uint32_t index = 0U; index < sizeof(stack) / sizeof(stack[0]);
         ++index) {
        exception_write(" ");
        exception_write_hex(stack[index]);
    }
    exception_write("\r\n");

    /* Chrome 执行 UD2 时，R15 通常指向致命检查消息。保留一段有界的短文本样本，不要
     * 假设它属于某种特定库结构；映像已剥离符号，这类假设可能掩盖真正故障的子系统。 */
    if (frame->vector != 6U || !exception_process_is_chrome(
            thread != 0 ? thread->process : 0)) return;
    exception_write("OS_CHROME_UD2_R15_TEXT ptr=");
    exception_write_hex(frame->r15);
    exception_write(" text=");
    for (uint64_t offset = 0U; offset < 64U; offset += sizeof(uint64_t)) {
        uint64_t word = 0U;
        uint64_t address = frame->r15 + offset;
        if (address < frame->r15 ||
            !x86_user_range_valid((const void __user *)(uintptr_t)address,
                                  sizeof(word)) ||
            copy_from_user(&word, (const void __user *)(uintptr_t)address,
                           sizeof(word)) != K_OK) {
            break;
        }
        for (uint32_t byte = 0U; byte < sizeof(word); ++byte) {
            uint8_t value = (uint8_t)(word >> (byte * 8U));
            if (value == 0U) {
                exception_write("\r\n");
                return;
            }
            char printable[2] = {
                value >= 0x20U && value <= 0x7EU ? (char)value : '.', '\0'
            };
            exception_write(printable);
        }
    }
    exception_write("\r\n");
}

static void exception_stop_exiting_user(bool from_user, process_t *process) {
    /* 正在运行的同进程线程可能在下一个系统调用边界前触发陷阱。退出状态已经发布，
     * 不要再为即将终止的进程填充或重试用户映射。内核/UAccess 故障仍走常规修复路径。 */
    if (from_user && process != 0 &&
        atomic_load_explicit(&process->state, memory_order_acquire) ==
            PROCESS_EXITING) {
        /* 与致命进程终止相同的不可返回用户态边界。 */
        __asm__ volatile ("sti" : : : "memory");
        thread_exit(process->exit_code);
    }
}

static bool exception_resolve_page_fault(const arch_trap_frame_t *frame,
                                         vaddr_t address, bool from_user,
                                         bool from_uaccess) {
    /* 中断门会清除 IF。未加锁系统调用中的用户拷贝故障也可能执行后备 I/O；此处保持
     * IF=0 会阻塞设备和唤醒 IPI。仅在已识别的 UAccess 位置且系统调用调度保护仍有效时，
     * 恢复调用方原有的开中断状态。持锁、TLB 和内核故障仍遵循原有中断门策略。异常收尾
     * 前（尤其是 SWAPGS 前）始终恢复 IF=0。 */
    bool allow_interrupts = (frame->rflags & (1ULL << 9)) != 0U &&
        (from_user || (from_uaccess && x86_syscall_active_fast() &&
                       !sched_preempt_disabled() && !x86_tlb_shootdown_active()));
    if (allow_interrupts) __asm__ volatile ("sti" : : : "memory");
    bool handled = vm_handle_current_fault(address, (uint32_t)frame->error_code,
                                            from_user, from_uaccess);
    if (allow_interrupts) __asm__ volatile ("cli" : : : "memory");
    return handled;
}

void x86_interrupt_dispatch(arch_trap_frame_t *frame) {
    if (frame == 0) {
        realtest_record_exception(0U, 0U, 0U, 0U, false);
        if (realtest_enabled()) realtest_finish_failure();
        exception_write("OS_EXCEPTION_BAD_FRAME\r\n");
        for (;;) __asm__ volatile ("cli; hlt");
    }
    if (frame->vector < 32U) ++g_exception_count[frame->vector];

    /* 合法懒分配先解决并重试；无效 UAccess 再通过修复表返回稳定错误码。 */
    if (frame->vector == 14U) {
        uint64_t fault_address = read_cr2();
        bool from_user = (frame->cs & 3U) == 3U;
        bool from_uaccess = x86_uaccess_fault_site(frame);
        bool handled_fault;
        thread_t *fault_thread = sched_current_thread();
        process_t *fault_process = fault_thread != 0 ? fault_thread->process : 0;
        exception_stop_exiting_user(from_user, fault_process);
        handled_fault = exception_resolve_page_fault(frame,
                                                (vaddr_t)fault_address,
                                                from_user, from_uaccess);
        if (from_uaccess && (frame->error_code & 1U) == 0U &&
            !handled_fault &&
            fault_process != 0 && linux_process_is(fault_process) &&
            g_uaccess_fault_count < 256U) {
            exception_write("OS_UACCESS_FAULT pid=");
            exception_write_hex(fault_process->pid);
            exception_write(" error=");
            exception_write_hex(frame->error_code);
            exception_write(" rip=");
            exception_write_hex(frame->rip);
            exception_write(" cr2=");
            exception_write_hex(fault_address);
            exception_write(" rdi=");
            exception_write_hex(frame->rdi);
            exception_write(" rsi=");
            exception_write_hex(frame->rsi);
            exception_write(" rdx=");
            exception_write_hex(frame->rdx);
            exception_write(" rax=");
            exception_write_hex(frame->rax);
            exception_write("\r\n");
            ++g_uaccess_fault_count;
        }
        if (handled_fault || x86_uaccess_fixup(frame)) return;
        if ((frame->cs & 3U) == 3U) {
            thread_t *thread = sched_current_thread();
            if (thread != 0 && thread->object.type == KOBJECT_TYPE_THREAD &&
                thread->process != 0) {
                exception_write("OS_USER_PAGE_FAULT error=");
                exception_write_hex(frame->error_code);
                exception_write(" rip=");
                exception_write_hex(frame->rip);
                exception_write(" cr2=");
                exception_write_hex(fault_address);
                exception_write("\r\n");
                record_user_context(frame, thread);
                record_user_opcode(frame);
                exception_exit_user_process(frame->vector);
            }
        }
    }
    if (frame->vector == 3U) return;
    if (frame->vector == 6U && g_ud_self_test) {
        frame->rip += 2U; /* 跳过本自检使用的 UD2。 */
        return;
    }
    if (exception_is_user_process_fault(frame)) {
        thread_t *thread = sched_current_thread();
        if (thread != 0 && thread->object.type == KOBJECT_TYPE_THREAD &&
            thread->process != 0) {
            exception_write("OS_USER_EXCEPTION vector=");
            exception_write_hex(frame->vector);
            exception_write(" pid=");
            exception_write_hex(thread->process->pid);
            exception_write(" tid=");
            exception_write_hex(thread->tid);
            exception_write(" rip=");
            exception_write_hex(frame->rip);
            exception_write("\r\n");
            record_user_context(frame, thread);
            record_user_opcode(frame);
            exception_exit_user_process(frame->vector);
        }
    }

    crash_dump_capture(frame, frame->vector == 14U ? read_cr2() : 0U);
    exception_write("OS_FATAL_EXCEPTION ");
    exception_write(exception_name(frame->vector));
    exception_write(" vector=");
    exception_write_hex(frame->vector);
    exception_write(" error=");
    exception_write_hex(frame->error_code);
    exception_write(" rip=");
    exception_write_hex(frame->rip);
    if (frame->vector == 14U) {
        exception_write(" cr2=");
        exception_write_hex(read_cr2());
    }
    exception_write("\r\n");
    record_user_opcode(frame);
    realtest_record_exception(frame->vector, frame->error_code,
                                     frame->rip, frame->vector == 14U ?
                                     read_cr2() : 0U, frame->vector == 14U);
    if (realtest_enabled()) realtest_finish_failure();
    for (;;) __asm__ volatile ("cli; hlt");
}

bool x86_exception_self_test(void) {
    arch_trap_frame_t user_frame = {0};
    user_frame.cs = 3U;
    user_frame.vector = 0U;
    if (!exception_is_user_process_fault(&user_frame)) return false;
    user_frame.vector = 2U;
    if (exception_is_user_process_fault(&user_frame)) return false;
    user_frame.vector = 3U;
    if (exception_is_user_process_fault(&user_frame)) return false;
    user_frame.vector = 13U;
    user_frame.cs = 0U;
    if (exception_is_user_process_fault(&user_frame)) return false;

    uint64_t breakpoint_before = g_exception_count[3];
    uint64_t invalid_opcode_before = g_exception_count[6];
    __asm__ volatile ("int3" : : : "memory");
    g_ud_self_test = true;
    __asm__ volatile ("ud2" : : : "memory");
    g_ud_self_test = false;
    return g_exception_count[3] == breakpoint_before + 1U &&
           g_exception_count[6] == invalid_opcode_before + 1U;
}
