#include <arch/x86_64/syscall_internal.h>
#include <arch/x86_64/paging.h>
#include <arch/x86_64/cpu.h>
#include <arch/x86_64/smp.h>
#include <arch/x86_64/uaccess.h>
#include <kernel/futex.h>
#include <kernel/console.h>
#include <kernel/debug_stage.h>
#include <kernel/completion_port.h>
#include <kernel/message_port.h>
#include <kernel/timer.h>
#include <kernel/deferred.h>
#include <kernel/mm.h>
#include <kernel/kmem.h>
#include <kernel/vm.h>
#include <kernel/shared_section.h>
#include <kernel/process.h>
#include <kernel/linux_abi.h>

typedef struct syscall_delay_diagnostic {
    uint64_t pid, tid, number, argument0, argument1, result, duration_ns;
} syscall_delay_diagnostic_t;
static volatile syscall_delay_diagnostic_t g_syscall_delays[64];
static atomic_flag g_syscall_delay_guard = ATOMIC_FLAG_INIT;
/* 可选地由调试器指定进程；否则仅记录进程组 leader。 */
static volatile uint64_t g_syscall_delay_process;
typedef struct syscall_cost_diagnostic {
    uint64_t number, count, total_ns, maximum_ns;
} syscall_cost_diagnostic_t;
static volatile syscall_cost_diagnostic_t g_syscall_costs[128];
static atomic_flag g_syscall_cost_guard = ATOMIC_FLAG_INIT;
static volatile uint64_t g_syscall_cost_tid;
static uint64_t g_syscall_cost_dropped;

/* Linux 资源准入的有界失败计数器。无参数、路径、分配、时间戳或 UART 输出；刻意不统计
 * EAGAIN/ENOENT。列顺序：ENOMEM、EBUSY、EMFILE、ENOSPC、EIO、EINVAL、EACCES、EPERM。
 * 排查浏览器重新加载失败时应查看增量，而非总数。 */
static atomic_ullong g_syscall_resource_errors[512][8];

static void syscall_record_resource_error(uint64_t number, int64_t status) {
    unsigned column;
    if (number >= sizeof(g_syscall_resource_errors) /
                  sizeof(g_syscall_resource_errors[0])) return;
    switch (status) {
    case K_ENOMEM: column = 0U; break;
    case K_EBUSY: column = 1U; break;
    case K_EMFILE: column = 2U; break;
    case K_ENOSPC: column = 3U; break;
    case K_EIO: column = 4U; break;
    case K_EINVAL: column = 5U; break;
    case K_EACCES: column = 6U; break;
    case K_EPERM: column = 7U; break;
    default: return;
    }
    atomic_fetch_add_explicit(&g_syscall_resource_errors[number][column], 1ULL,
                              memory_order_relaxed);
}

/* 可选的有界墙钟时间（包含等待），不是 CPU 使用率。交互路径绝不获取阻塞锁，也不记录用户参数。 */
static void syscall_record_cost(uint64_t tid, uint64_t number,
                                uint64_t elapsed) {
    if (tid == 0U || tid != g_syscall_cost_tid) return;
    if (atomic_flag_test_and_set_explicit(&g_syscall_cost_guard,
                                          memory_order_acquire)) {
        __atomic_fetch_add(&g_syscall_cost_dropped, 1U, __ATOMIC_RELAXED);
        return;
    }
    uint64_t ns = x86_tsc_to_ns(elapsed);
    bool stored = false;
    for (size_t index = 0U; index < 128U; ++index) {
        if (g_syscall_costs[index].count != 0U &&
            g_syscall_costs[index].number != number) continue;
        g_syscall_costs[index].number = number;
        ++g_syscall_costs[index].count;
        g_syscall_costs[index].total_ns += ns;
        if (ns > g_syscall_costs[index].maximum_ns)
            g_syscall_costs[index].maximum_ns = ns;
        stored = true;
        break;
    }
    if (!stored)
        __atomic_fetch_add(&g_syscall_cost_dropped, 1U, __ATOMIC_RELAXED);
    atomic_flag_clear_explicit(&g_syscall_cost_guard, memory_order_release);
}
static bool syscall_measure_linux_delay(const thread_t *thread) {
    if (thread == 0 || thread->process == 0) return false;
    /* 指定进程后替代默认的仅 leader 过滤条件。否则无关的渲染器和服务等待会填满有界采样表。 */
    return g_syscall_delay_process != 0U ?
        thread->process->pid == g_syscall_delay_process :
        thread->tid == thread->process->linux_thread_group_leader_tid;
}
static void syscall_record_delay(uint64_t pid, uint64_t tid, uint64_t number,
                                 uint64_t argument0, uint64_t argument1,
                                 int64_t result, uint64_t elapsed) {
    if (x86_boot_cpu_features.tsc_hz == 0U ||
        elapsed < x86_boot_cpu_features.tsc_hz / 100U) return;
    if (atomic_flag_test_and_set_explicit(&g_syscall_delay_guard,
                                          memory_order_acquire)) return;
    for (size_t index = 0U; index < 64U; ++index) {
        if (g_syscall_delays[index].tid != 0U &&
            (g_syscall_delays[index].tid != tid ||
             g_syscall_delays[index].number != number)) continue;
        uint64_t ns = x86_tsc_to_ns(elapsed);
        if (ns > g_syscall_delays[index].duration_ns)
            g_syscall_delays[index] = (syscall_delay_diagnostic_t){
                pid, tid, number, argument0, argument1, (uint64_t)result, ns};
        break;
    }
    atomic_flag_clear_explicit(&g_syscall_delay_guard, memory_order_release);
}
#include <kernel/vfs.h>
#include <kernel/socket.h>
#include <kernel/net_manager.h>
#include <kernel/e1000.h>
#include <kernel/device.h>
#include <kernel/io.h>
#include <kernel/audio.h>
#include <kernel/hda.h>
#include <kernel/xhci.h>
#include <kernel/gpu.h>
#include <kernel/display.h>
#include <kernel/input.h>
#include <kernel/window_server.h>
#include <uapi/mm.h>
#include <uapi/io.h>
#include <uapi/ipc.h>
#include <uapi/process.h>
#include <uapi/signal.h>
#include <uapi/socket.h>
#include <uapi/device.h>
#include <uapi/file.h>
#include <uapi/audio.h>
#include <uapi/wait.h>
#include <uapi/gpu.h>
#include <uapi/display.h>
#include <uapi/input.h>
#include <uapi/window.h>
#include <uapi/network.h>

#include "internal.h"




#define IA32_EFER               0xC0000080U
#define IA32_STAR               0xC0000081U
#define IA32_LSTAR              0xC0000082U
#define IA32_FMASK              0xC0000084U
#define IA32_GS_BASE            0xC0000101U
#define IA32_KERNEL_GS_BASE     0xC0000102U

#define EFER_SYSCALL_ENABLE     (1ULL << 0)
#define RFLAGS_CARRY            (1ULL << 0)
#define RFLAGS_FIXED            (1ULL << 1)
#define RFLAGS_TRAP             (1ULL << 8)
#define RFLAGS_INTERRUPT        (1ULL << 9)
#define RFLAGS_DIRECTION        (1ULL << 10)
#define RFLAGS_IOPL             (3ULL << 12)
#define RFLAGS_NESTED_TASK      (1ULL << 14)
#define RFLAGS_RESUME           (1ULL << 16)
#define RFLAGS_VIRTUAL_8086     (1ULL << 17)
#define RFLAGS_ALIGNMENT_CHECK  (1ULL << 18)
#define RFLAGS_ID                (1ULL << 21)

/*
 * RFLAGS 中未定义位不能带入 SYSRETQ/IRETQ。
 *
 * 用户态可以保留算术标志、TF/IF/DF、RF、AC 和 ID；IOPL、NT、VM、VIF、VIP
 * 等位必须由返回路径拒绝。其余保留位若被伪造，不能让处理器在返回时解释
 * 出未定义状态。
 */
#define RFLAGS_USER_ALLOWED     ((1ULL << 0) | (1ULL << 1) | (1ULL << 2) | \
                                 (1ULL << 4) | (1ULL << 6) | (1ULL << 7) | \
                                 RFLAGS_TRAP | RFLAGS_INTERRUPT | \
                                 RFLAGS_DIRECTION | (1ULL << 11) | \
                                 RFLAGS_RESUME | RFLAGS_ALIGNMENT_CHECK | \
                                 RFLAGS_ID)

#define USER_ADDRESS_MIN        0x0000000000010000ULL
#define USER_ADDRESS_END        0x0000800000000000ULL
#define USER_CODE_SELECTOR      0x23ULL
#define USER_DATA_SELECTOR      0x1BULL
#define SYSCALL_TABLE_SIZE      (OS_SYS_DESKTOP_WALLPAPER + 1U)

typedef int64_t (*syscall_handler_t)(uint64_t, uint64_t, uint64_t,
                                     uint64_t, uint64_t, uint64_t);

syscall_cpu_local_t syscall_cpu_local;
volatile uint32_t g_syscall_return_progress[MAX_CPUS];
volatile uint64_t g_syscall_return_number[MAX_CPUS];
volatile uint64_t g_syscall_return_rip[MAX_CPUS];
volatile uint32_t g_socket_return_progress[MAX_CPUS];
volatile uint32_t g_socket_return_active[MAX_CPUS];
volatile uint64_t g_socket_return_rip[MAX_CPUS];

static uint64_t read_msr(uint32_t index) {
    uint32_t low;
    uint32_t high;
    __asm__ volatile ("rdmsr" : "=a"(low), "=d"(high) : "c"(index));
    return ((uint64_t)high << 32) | low;
}

static void write_msr(uint32_t index, uint64_t value) {
    __asm__ volatile ("wrmsr" : : "a"((uint32_t)value), "d"((uint32_t)(value >> 32)),
                      "c"(index) : "memory");
}

static bool user_address_valid(uint64_t address) {
    return address >= USER_ADDRESS_MIN && address < USER_ADDRESS_END;
}

bool versioned_header_valid(const os_versioned_header_t *header,
                            size_t required_size) {
    return header != 0 && header->size >= required_size &&
           header->version == OS_SYSCALL_ABI_VERSION && header->flags == 0;
}

process_t *current_process(void) {
    thread_t *thread = sched_current_thread();
    if (thread == 0 || thread->object.type != KOBJECT_TYPE_THREAD ||
        thread->process == 0) return 0;
    return thread->process;
}





static int64_t sys_debug_query(uint64_t query, uint64_t output, uint64_t output_size,
                               uint64_t unused3, uint64_t unused4, uint64_t unused5) {
    (void)output;
    (void)output_size;
    (void)unused3;
    (void)unused4;
    (void)unused5;
    return query == 0U ? (int64_t)OS_SYSCALL_ABI_VERSION : K_EINVAL;
}

/* 稀疏编号仍用直接索引表，分派复杂度固定为 O(1)。 */
static const syscall_handler_t g_syscall_table[SYSCALL_TABLE_SIZE] = {
    [OS_SYS_THREAD_EXIT] = syscall_thread_exit,
    [OS_SYS_PROCESS_EXIT] = syscall_process_exit,
    [OS_SYS_LINUX_ACTIVATE] = syscall_linux_activate,
    [OS_SYS_MEMORY_INFO] = syscall_memory_info,
    [OS_SYS_TASK_QUERY] = syscall_task_query,
    [OS_SYS_CPU_GET_USAGE] = syscall_cpu_get_usage,
    [OS_SYS_PROCESS_TERMINATE] = syscall_process_terminate,
    [OS_SYS_THREAD_TERMINATE] = syscall_thread_terminate,
    [OS_SYS_THREAD_CREATE] = syscall_thread_create,
    [OS_SYS_PROCESS_CREATE] = syscall_process_create,
    [OS_SYS_PROCESS_EXEC] = syscall_process_exec,
    [OS_SYS_PROCESS_INFO] = syscall_process_info,
    [OS_SYS_PROCESS_FORK] = syscall_process_fork,
    [OS_SYS_PROCESS_WAIT] = syscall_process_wait,
    [OS_SYS_PROCESS_ENUMERATE] = syscall_process_enumerate,
    [OS_SYS_THREAD_ENUMERATE] = syscall_thread_enumerate,
    [OS_SYS_THREAD_CONTEXT] = syscall_thread_context,
    [OS_SYS_THREAD_SIGNAL] = syscall_thread_signal,
    [OS_SYS_VM_MAP] = syscall_vm_map,
    [OS_SYS_VM_UNMAP] = syscall_vm_unmap,
    [OS_SYS_VM_PROTECT] = syscall_vm_protect,
    [OS_SYS_VM_SHARE] = syscall_vm_share,
    [OS_SYS_VM_SYNC] = syscall_vm_sync,
    [OS_SYS_VM_ADVISE] = syscall_vm_advise,
    [OS_SYS_HANDLE_CLOSE] = syscall_handle_close,
    [OS_SYS_HANDLE_DUP] = syscall_handle_dup,
    [OS_SYS_HANDLE_GET_FLAGS] = syscall_handle_get_flags,
    [OS_SYS_HANDLE_SET_FLAGS] = syscall_handle_set_flags,
    [OS_SYS_DEVICE_OPEN] = syscall_device_open,
    [OS_SYS_DEVICE_CONTROL] = syscall_device_control,
    [OS_SYS_DEVICE_ENUMERATE] = syscall_device_enumerate,
    [OS_SYS_PORT_CREATE] = syscall_port_create,
    [OS_SYS_PORT_SEND] = syscall_port_send,
    [OS_SYS_PORT_RECEIVE] = syscall_port_receive,
    [OS_SYS_COMPLETION_WAIT] = syscall_completion_wait,
    [OS_SYS_CLOCK_GET] = syscall_clock_get,
    [OS_SYS_TIMER_CREATE] = syscall_timer_create,
    [OS_SYS_CLOCK_SET] = syscall_clock_set,
    [OS_SYS_RANDOM_GET] = syscall_random_get,
    [OS_SYS_SYSTEM_POWER] = syscall_system_power,
    [OS_SYS_EXCEPTION_RETURN] = syscall_signal_return,
    [OS_SYS_SIGNAL_ACTION] = syscall_signal_action,
    [OS_SYS_SIGNAL_MASK] = syscall_signal_mask,
    [OS_SYS_SIGNAL_SEND] = syscall_signal_send,
    [OS_SYS_WAIT_ONE] = syscall_wait_one,
    [OS_SYS_WAIT_MANY] = syscall_wait_many,
    [OS_SYS_FUTEX_WAIT] = syscall_futex_wait,
    [OS_SYS_FUTEX_WAKE] = syscall_futex_wake,
    [OS_SYS_FILE_OPEN] = syscall_file_open,
    [OS_SYS_FILE_ENUMERATE] = syscall_file_enumerate,
    [OS_SYS_FILE_ENUMERATE_BATCH] = syscall_file_enumerate_batch,
    [OS_SYS_FILE_SEEK] = syscall_file_seek,
    [OS_SYS_FILE_STAT] = syscall_file_stat,
    [OS_SYS_FILE_TRUNCATE] = syscall_file_truncate,
    [OS_SYS_FILE_REMOVE] = syscall_file_remove,
    [OS_SYS_FILE_MKDIR] = syscall_file_mkdir,
    [OS_SYS_FILE_CHDIR] = syscall_file_chdir,
    [OS_SYS_FILE_GETCWD] = syscall_file_getcwd,
    [OS_SYS_FILE_FSTAT] = syscall_file_fstat,
    [OS_SYS_FILE_RENAME] = syscall_file_rename,
    [OS_SYS_FILE_READ] = syscall_file_read,
    [OS_SYS_FILE_WRITE] = syscall_file_write,
    [OS_SYS_FILE_FSYNC] = syscall_file_fsync,
    [OS_SYS_PIPE_CREATE] = syscall_pipe_create,
    [OS_SYS_PIPE_READ] = syscall_pipe_read,
    [OS_SYS_PIPE_WRITE] = syscall_pipe_write,
    [OS_SYS_IO_SUBMIT] = syscall_io_submit,
    [OS_SYS_IO_CANCEL] = syscall_io_cancel,
    [OS_SYS_SOCKET_CREATE] = syscall_socket_create,
    [OS_SYS_SOCKET_BIND] = syscall_socket_bind,
    [OS_SYS_SOCKET_CONNECT] = syscall_socket_connect,
    [OS_SYS_SOCKET_LISTEN] = syscall_socket_listen,
    [OS_SYS_SOCKET_ACCEPT] = syscall_socket_accept,
    [OS_SYS_SOCKET_SEND] = syscall_socket_send,
    [OS_SYS_SOCKET_RECV] = syscall_socket_recv,
    [OS_SYS_SOCKET_SEND_ASYNC] = syscall_socket_send_async,
    [OS_SYS_SOCKET_BIND6] = syscall_socket_bind6,
    [OS_SYS_SOCKET_CONNECT6] = syscall_socket_connect6,
    [OS_SYS_SOCKET_SEND6] = syscall_socket_send6,
    [OS_SYS_SOCKET_RECV6] = syscall_socket_recv6,
    [OS_SYS_SOCKET_SEND_ASYNC6] = syscall_socket_send_async6,
    [OS_SYS_SOCKET_GET_INFO] = syscall_socket_get_info,
    [OS_SYS_SOCKET_GET_OPTION] = syscall_socket_get_option,
    [OS_SYS_SOCKET_SET_OPTION] = syscall_socket_set_option,
    [OS_SYS_SOCKET_SHUTDOWN] = syscall_socket_shutdown,
    [OS_SYS_NET_GET_STATUS] = syscall_net_get_status,
    [OS_SYS_NET_ENUM_ADAPTER] = syscall_net_enum_adapter,
    [OS_SYS_NET_SET_IPV4] = syscall_net_set_ipv4,
    [OS_SYS_NET_SUBSCRIBE] = syscall_net_subscribe,
    [OS_SYS_GPU_CREATE_CTX] = syscall_gpu_create_context,
    [OS_SYS_GPU_GET_INFO] = syscall_gpu_get_info,
    [OS_SYS_DESKTOP_WALLPAPER] = syscall_desktop_wallpaper,
    [OS_SYS_GPU_ALLOC] = syscall_gpu_alloc,
    [OS_SYS_GPU_MAP] = syscall_gpu_map,
    [OS_SYS_GPU_SUBMIT] = syscall_gpu_submit,
    [OS_SYS_GPU_WAIT_FENCE] = syscall_gpu_wait_fence,
    [OS_SYS_DISPLAY_GET_INFO] = syscall_display_get_info,
    [OS_SYS_THEME_SETTINGS] = syscall_theme_settings,
    [OS_SYS_DISPLAY_COMMIT] = syscall_display_commit,
    [OS_SYS_FONT_CACHE] = syscall_font_cache,
    [OS_SYS_IMAGE_INFO] = syscall_image_info,
    [OS_SYS_IMAGE_DECODE] = syscall_image_decode,
    [OS_SYS_INPUT_READ] = syscall_input_read,
    [OS_SYS_WINDOW_REGISTER_MANAGER] = syscall_window_register_manager,
    [OS_SYS_WINDOW_CREATE] = syscall_window_create,
    [OS_SYS_WINDOW_ENUMERATE] = syscall_window_enumerate,
    [OS_SYS_WINDOW_MAP] = syscall_window_map,
    [OS_SYS_WINDOW_SET] = syscall_window_set,
    [OS_SYS_WINDOW_FOCUS] = syscall_window_focus,
    [OS_SYS_WINDOW_INPUT_READ] = syscall_window_input_read,
    [OS_SYS_WINDOW_INPUT_DISPATCH] = syscall_window_input_dispatch,
    [OS_SYS_WINDOW_EVENT_READ] = syscall_window_event_read,
    [OS_SYS_WINDOW_UPDATE] = syscall_window_update,
    [OS_SYS_WINDOW_TITLE] = syscall_window_title,
    [OS_SYS_WINDOW_CONTROL_TREE] = syscall_window_control_tree,
    [OS_SYS_CLIPBOARD_SET] = syscall_clipboard_set,
    [OS_SYS_CLIPBOARD_GET] = syscall_clipboard_get,
    [OS_SYS_AUDIO_OPEN] = syscall_audio_open,
    [OS_SYS_AUDIO_CONTROL] = syscall_audio_control,
    [OS_SYS_AUDIO_SETTINGS] = syscall_audio_settings,
    [OS_SYS_DEBUG_QUERY] = sys_debug_query,
    [OS_SYS_DEBUG_WRITE] = syscall_debug_write,
};

bool syscall_init(uint64_t kernel_stack_top) {
    if (kernel_stack_top == 0 || (kernel_stack_top & 0xFULL) != 0) return 0;
    syscall_cpu_local_t *local = x86_cpu_local_current();
    if (local == 0) return 0;
    local->UserStack = 0;
    local->KernelStack = kernel_stack_top;
    local->KernelResumeStack = 0;
    local->ReturnToKernel = 0;
    local->UserExitSeen = 0;

    /*
     * SYSRET 在长模式下用 STAR 高半值加 8/16 装载 SS/CS。
     * GDT 的用户数据段基址为 0x18、用户代码段基址为 0x20，
     * 因而 STAR 高半值必须为 0x13，才能得到带 RPL3 的
     * SS=0x1B、CS=0x23。使用 0x10 会产生 SS=0x18，之后的
     * 定时器 iretq 会把它视为错误的返回帧。
     */
    write_msr(IA32_STAR, (0x13ULL << 48) | (0x08ULL << 32));
    write_msr(IA32_LSTAR, (uint64_t)(uintptr_t)&syscall_entry);
    write_msr(IA32_FMASK, RFLAGS_DIRECTION | RFLAGS_INTERRUPT | RFLAGS_TRAP |
                             RFLAGS_ALIGNMENT_CHECK | RFLAGS_NESTED_TASK);
    /* 内核态始终使用 CPU-local GS；swapgs 后用户态得到零基址。 */
    write_msr(IA32_GS_BASE, (uint64_t)(uintptr_t)local);
    write_msr(IA32_KERNEL_GS_BASE, 0);
    write_msr(IA32_EFER, read_msr(IA32_EFER) | EFER_SYSCALL_ENABLE);
    return 1;
}

void x86_syscall_set_kernel_stack(uint64_t kernel_stack_top) {
    if (kernel_stack_top != 0 && (kernel_stack_top & 15U) == 0) {
        syscall_cpu_local_t *local = x86_cpu_local_current();
        if (local != 0) local->KernelStack = kernel_stack_top;
    }
}

void x86_syscall_init(void) {
    uint64_t stack;
    __asm__ volatile ("mov %%rsp, %0" : "=r"(stack));
    (void)syscall_init(stack & ~0xFULL);
}

static int64_t syscall_complete_dispatch(thread_t *thread, int64_t status) {
    if (thread != 0 && (__atomic_load_n(&thread->flags, __ATOMIC_ACQUIRE) & THREAD_FLAG_TERMINATE_REQUEST))
        thread_exit(-(int64_t)OS_SIGKILL);
    /* 处理程序已释放锁并提交 exec/信号栈帧。在普通 C 续执行路径中调度，使保存的栈能像阻塞系统调用一样
     * 正常恢复；不要从汇编恢复桩中调度。SyscallActive 会由上下文切换保存/恢复。 */
    if (thread != 0 && thread->sched_class != SCHED_CLASS_IDLE &&
        !thread->exec_pending && !sched_preempt_disabled() &&
        !x86_tlb_shootdown_active() &&
        (x86_smp_take_reschedule_request() || sched_tick_should_preempt())) {
        /* 在 FAIR 任务间选择前记入系统调用执行尾段。仅排空邮箱不会因 FAIR 唤醒 FAIR 而抢占；
         * 时钟也刻意不记入活动系统调用的执行时间。本地唤醒只发布 need_resched，不设置 IPI 待处理位。 */
        schedule();
    }
    return status;
}

int64_t syscall_dispatch(arch_trap_frame_t *frame) {
    thread_t *executing_thread;
    uint64_t number;
    bool native_escape;
    if (frame == 0 || !user_address_valid(frame->rip) ||
        !user_address_valid(frame->rsp)) return K_EACCES;
    executing_thread = sched_current_thread();
    number = frame->rax;
    native_escape = (number & OS_SYSCALL_NATIVE_FLAG) != 0U;
    number &= ~OS_SYSCALL_NATIVE_FLAG;
    if (executing_thread != 0 && executing_thread->process != 0 &&
        linux_process_is(executing_thread->process) && !native_escape) {
        const uint64_t group_kill_bit = 1ULL << (OS_SIGKILL - 1U);
        uint64_t pending = __atomic_fetch_and(
            &executing_thread->signal_pending, ~group_kill_bit,
            __ATOMIC_ACQ_REL);
        if ((pending & group_kill_bit) != 0U) {
            process_exit_signal(OS_SIGKILL);
        }
        bool measure = syscall_measure_linux_delay(executing_thread);
        uint64_t argument0 = frame->rdi, argument1 = frame->rsi;
        bool measure_cost = g_syscall_cost_tid != 0U &&
                            executing_thread->tid == g_syscall_cost_tid;
        uint64_t started = (measure || measure_cost) ? x86_read_tsc() : 0U;
        int64_t status = linux_syscall_dispatch(frame);
        uint64_t elapsed = (measure || measure_cost) ? x86_read_tsc() - started : 0U;
        if (status < 0) syscall_record_resource_error(number, status);
        if (measure_cost)
            syscall_record_cost(executing_thread->tid, number, elapsed);
        if (measure)
            syscall_record_delay(executing_thread->process->pid,
                                  executing_thread->tid, number,
                                  argument0, argument1, status,
                                  elapsed);
        pending = __atomic_fetch_and(&executing_thread->signal_pending,
                                     ~group_kill_bit, __ATOMIC_ACQ_REL);
        if ((pending & group_kill_bit) != 0U) {
            process_exit_signal(OS_SIGKILL);
        }
        if (status == K_OK && executing_thread->exec_pending) {
            process_exec_debug_mark(11U);
            frame->rip = executing_thread->exec_entry;
            frame->rsp = executing_thread->exec_stack;
            frame->rflags = 0x202ULL;
            frame->rax = 0U;
            executing_thread->exec_pending = false;
            process_exec_debug_mark(12U);
        }
        return syscall_complete_dispatch(executing_thread, status);
    }
    /* libc fork/exec use the native escape, including queued work before the
     * handler. Keep them separate from Linux syscall numbers 6 and 4. */
    bool measure_native_transition = native_escape &&
        g_syscall_delay_process != 0U &&
        executing_thread != 0 && executing_thread->process != 0 &&
        ((number == OS_SYS_PROCESS_FORK &&
          executing_thread->process->pid == g_syscall_delay_process) ||
         (number == OS_SYS_PROCESS_EXEC && executing_thread->tid ==
          executing_thread->process->linux_thread_group_leader_tid));
    bool measure_native_cost = g_syscall_cost_tid != 0U &&
        executing_thread != 0 && executing_thread->tid == g_syscall_cost_tid;
    uint64_t native_started = (measure_native_transition || measure_native_cost) ?
                             x86_read_tsc() : 0U;
    uint64_t native_argument0 = frame->rdi;
    /* Once the persistent worker owns bottom halves, a user syscall must
     * not steal arbitrary callbacks (e.g. old-address-space reclamation).
     * A four-item count is not a time bound and SyscallActive defers timer
     * preemption. Keep synchronous draining only for pre-worker boot. */
    if (!deferred_worker_started()) (void)deferred_run(4U);
    if (number >= SYSCALL_TABLE_SIZE || g_syscall_table[number] == 0) {
        return syscall_complete_dispatch(executing_thread, K_ENOSYS);
    }
    uint64_t argument5 = frame->r9;
    /* PROCESS_FORK has no user argument; its private sixth argument carries
     * the kernel-owned return frame so the child can resume after syscall. */
    if (number == OS_SYS_PROCESS_FORK || number == OS_SYS_SIGNAL_RETURN) {
        argument5 = (uint64_t)(uintptr_t)frame;
    }
    int64_t status = g_syscall_table[number](frame->rdi, frame->rsi, frame->rdx,
                                             frame->r10, frame->r8, argument5);
    uint64_t native_elapsed = (measure_native_transition || measure_native_cost) ?
                             x86_read_tsc() - native_started : 0U;
    if (measure_native_cost)
        syscall_record_cost(executing_thread->tid,
                             number | OS_SYSCALL_NATIVE_FLAG, native_elapsed);
    if (measure_native_transition)
        syscall_record_delay(executing_thread->process->pid,
                             executing_thread->tid,
                             number | OS_SYSCALL_NATIVE_FLAG,
                             native_argument0, 0U, status,
                             native_elapsed);
    if (status == K_OK) {
        if (executing_thread != 0 && executing_thread->exec_pending) {
            process_exec_debug_mark(11U);
            frame->rip = executing_thread->exec_entry;
            frame->rsp = executing_thread->exec_stack;
            frame->rflags = 0x202ULL;
            frame->rax = 0;
            executing_thread->exec_pending = false;
            process_exec_debug_mark(12U);
        }
    }
    if (number != OS_SYS_SIGNAL_RETURN) {
        status = syscall_deliver_pending_signal(frame, status);
    }
    return syscall_complete_dispatch(executing_thread, status);
}


bool x86_validate_user_frame(const arch_trap_frame_t *frame) {
    if (frame == 0 || !user_address_valid(frame->rip) ||
        !user_address_valid(frame->rsp)) return false;
    if (frame->cs != USER_CODE_SELECTOR || frame->ss != USER_DATA_SELECTOR) return false;
    if ((frame->rflags & ~RFLAGS_USER_ALLOWED) != 0) return false;
    if ((frame->rflags & RFLAGS_FIXED) == 0) return false;
    if ((frame->rflags & (RFLAGS_IOPL | RFLAGS_NESTED_TASK | RFLAGS_VIRTUAL_8086)) != 0) {
        return false;
    }
    return true;
}

int x86_syscall_return_mode(arch_trap_frame_t *frame) {
    if (!x86_validate_user_frame(frame)) return -1;
    /* 调试/恢复/对齐检查等标志走 IRETQ，常规返回走 SYSRETQ。 */
    uint64_t slow_flags = RFLAGS_TRAP | RFLAGS_RESUME | RFLAGS_ALIGNMENT_CHECK;
    return (frame->rflags & slow_flags) == 0 ? 1 : 0;
}

uint32_t x86_syscall_return_progress(uint32_t cpu_index) {
    if (cpu_index >= MAX_CPUS) return 0U;
    return __atomic_load_n(&g_syscall_return_progress[cpu_index], __ATOMIC_ACQUIRE);
}

uint64_t x86_syscall_return_number(uint32_t cpu_index) {
    if (cpu_index >= MAX_CPUS) return 0U;
    return __atomic_load_n(&g_syscall_return_number[cpu_index], __ATOMIC_ACQUIRE);
}

uint64_t x86_syscall_return_rip(uint32_t cpu_index) {
    if (cpu_index >= MAX_CPUS) return 0U;
    return __atomic_load_n(&g_syscall_return_rip[cpu_index], __ATOMIC_ACQUIRE);
}

uint32_t x86_socket_return_progress(uint32_t cpu_index) {
    if (cpu_index >= MAX_CPUS) return 0U;
    return __atomic_load_n(&g_socket_return_progress[cpu_index], __ATOMIC_ACQUIRE);
}

uint32_t x86_socket_return_active(uint32_t cpu_index) {
    if (cpu_index >= MAX_CPUS) return 0U;
    return __atomic_load_n(&g_socket_return_active[cpu_index], __ATOMIC_ACQUIRE);
}

uint64_t x86_socket_return_rip(uint32_t cpu_index) {
    if (cpu_index >= MAX_CPUS) return 0U;
    return __atomic_load_n(&g_socket_return_rip[cpu_index], __ATOMIC_ACQUIRE);
}

__noreturn void x86_syscall_bad_frame(void) {
    thread_t *thread = sched_current_thread();
    /* 用户返回帧损坏时只终止当前线程，不能让一个进程拖垮整个内核。 */
    if (thread != 0 && thread->object.type == KOBJECT_TYPE_THREAD &&
        thread->process != 0) {
        thread_exit(K_EACCES);
    }
    /* 早期启动阶段没有可回收的用户线程，只能进入不可恢复停机。 */
    for (;;) __asm__ volatile ("cli; hlt" : : : "memory");
}

bool syscall_frame_self_test(void) {
    arch_trap_frame_t frame = {0};
    uint64_t seed = 0xC0DEC0DE12345678ULL;
    frame.rip = 0x0000000040000000ULL;
    frame.rsp = 0x0000000080000000ULL;
    frame.cs = USER_CODE_SELECTOR;
    frame.ss = USER_DATA_SELECTOR;
    frame.rflags = RFLAGS_FIXED | RFLAGS_INTERRUPT;
    frame.rax = OS_SYS_DEBUG_QUERY;
    if (!x86_validate_user_frame(&frame) || x86_syscall_return_mode(&frame) != 1 ||
        syscall_dispatch(&frame) != OS_SYSCALL_ABI_VERSION) return false;

    frame.rflags |= RFLAGS_TRAP;
    if (x86_syscall_return_mode(&frame) != 0) return false;
    frame.rflags &= ~RFLAGS_TRAP;
    frame.rax = SYSCALL_TABLE_SIZE;
    if (syscall_dispatch(&frame) != K_ENOSYS) return false;

    frame.rip = 0x0000800000000000ULL;
    if (x86_validate_user_frame(&frame)) return false;
    frame.rip = 0x0000000040000000ULL;
    frame.cs = 0x08U;
    if (x86_validate_user_frame(&frame)) return false;
    frame.cs = USER_CODE_SELECTOR;
    frame.rflags |= RFLAGS_IOPL;
    if (x86_validate_user_frame(&frame)) return false;
    frame.rflags = RFLAGS_INTERRUPT;
    if (x86_validate_user_frame(&frame)) return false;

    /* 确定性扰动返回帧，覆盖常见的用户态伪造输入而不执行任意 syscall。 */
    for (uint32_t i = 0; i < 256U; ++i) {
        seed = seed * 6364136223846793005ULL + 1442695040888963407ULL;
        frame.rip = 0x0000800000000000ULL | (seed & 0xFFFFU);
        frame.rsp = 0x0000000080000000ULL;
        frame.cs = USER_CODE_SELECTOR;
        frame.ss = USER_DATA_SELECTOR;
        frame.rflags = RFLAGS_FIXED | RFLAGS_INTERRUPT;
        if (x86_validate_user_frame(&frame) ||
            syscall_dispatch(&frame) != K_EACCES) return false;

        frame.rip = 0x0000000040000000ULL;
        frame.rsp = 0x0000800000000000ULL | ((seed >> 16) & 0xFFFFU);
        if (x86_validate_user_frame(&frame) ||
            syscall_dispatch(&frame) != K_EACCES) return false;

        frame.rsp = 0x0000000080000000ULL;
        frame.cs = (uint16_t)(((seed | 1U) ^ USER_CODE_SELECTOR) | 0x0040U);
        if (x86_validate_user_frame(&frame)) return false;
        frame.cs = USER_CODE_SELECTOR;
        frame.ss = (uint16_t)(((seed >> 32) ^ USER_DATA_SELECTOR) | 0x0080U);
        if (x86_validate_user_frame(&frame)) return false;
        frame.ss = USER_DATA_SELECTOR;
        frame.rflags = RFLAGS_FIXED | RFLAGS_IOPL;
        if (x86_validate_user_frame(&frame)) return false;

        /* 每个保留位都必须被拒绝，避免伪造帧进入 SYSRETQ/IRETQ。 */
        frame.rflags = RFLAGS_FIXED | RFLAGS_INTERRUPT;
        uint64_t flag_bit = 1ULL;
        for (uint32_t bit = 0; bit < 64U; ++bit, flag_bit <<= 1U) {
            if ((RFLAGS_USER_ALLOWED & flag_bit) != 0) continue;
            frame.rflags = RFLAGS_FIXED | RFLAGS_INTERRUPT | flag_bit;
            if (x86_validate_user_frame(&frame)) return false;
        }

        /* 访问范围的两个边界及高地址随机值都必须保持为无效帧。 */
        frame.rflags = RFLAGS_FIXED | RFLAGS_INTERRUPT;
        frame.rip = USER_ADDRESS_MIN - 1U;
        if (x86_validate_user_frame(&frame)) return false;
        frame.rip = USER_ADDRESS_END;
        if (x86_validate_user_frame(&frame)) return false;
        frame.rip = 0xFFFFFFFFFFFFFFFFULL;
        if (x86_validate_user_frame(&frame)) return false;
        frame.rip = 0x0000000040000000ULL;
        frame.rsp = USER_ADDRESS_MIN - 1U;
        if (x86_validate_user_frame(&frame)) return false;
        frame.rsp = USER_ADDRESS_END;
        if (x86_validate_user_frame(&frame)) return false;
    }
    return true;
}
