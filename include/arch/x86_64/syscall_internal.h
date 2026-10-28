#pragma once

#include <arch/x86_64/context.h>
#include <arch/x86_64/cpu.h>
#include <kernel/base.h>
#include <uapi/syscall.h>

/* 系统调用入口及其调用方共用的私有架构约定。 */
typedef x86_cpu_local_t syscall_cpu_local_t;
typedef arch_trap_frame_t syscall_frame_t;

_Static_assert(__builtin_offsetof(syscall_cpu_local_t, UserStack) == 0U,
               "syscall cpu-local UserStack offset");
_Static_assert(__builtin_offsetof(syscall_cpu_local_t, KernelStack) == 8U,
               "syscall cpu-local KernelStack offset");
_Static_assert(__builtin_offsetof(syscall_cpu_local_t, KernelResumeStack) == 16U,
               "syscall cpu-local KernelResumeStack offset");
_Static_assert(__builtin_offsetof(syscall_cpu_local_t, ReturnToKernel) == 24U,
               "syscall cpu-local ReturnToKernel offset");
_Static_assert(__builtin_offsetof(syscall_cpu_local_t, Self) == 40U,
               "syscall cpu-local Self offset");
_Static_assert(__builtin_offsetof(syscall_cpu_local_t, CpuIndex) == 48U,
               "syscall cpu-local CpuIndex offset");
_Static_assert(__builtin_offsetof(syscall_cpu_local_t, UserEntries) == 64U,
               "syscall cpu-local UserEntries offset");

extern syscall_cpu_local_t syscall_cpu_local;
extern void syscall_entry(void);
extern void arch_enter_user(uint64_t instruction_pointer,
                                   uint64_t stack_pointer);

bool syscall_init(uint64_t kernel_stack_top);
void x86_syscall_set_kernel_stack(uint64_t kernel_stack_top);
int64_t syscall_dispatch(arch_trap_frame_t *frame);
int64_t syscall_deliver_pending_signal(arch_trap_frame_t *frame,
                                       int64_t status);
uint32_t syscall_thread_create_stage(void);
int x86_syscall_return_mode(arch_trap_frame_t *frame);
uint32_t x86_syscall_return_progress(uint32_t cpu_index);
uint64_t x86_syscall_return_number(uint32_t cpu_index);
uint64_t x86_syscall_return_rip(uint32_t cpu_index);
uint32_t x86_socket_return_progress(uint32_t cpu_index);
uint32_t x86_socket_return_active(uint32_t cpu_index);
uint64_t x86_socket_return_rip(uint32_t cpu_index);
bool x86_validate_user_frame(const arch_trap_frame_t *frame);
__noreturn void x86_syscall_bad_frame(void);
bool syscall_frame_self_test(void);
