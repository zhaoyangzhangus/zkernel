#include <arch/x86_64/apic.h>
#include <arch/x86_64/cpu.h>
#include <arch/x86_64/paging.h>
#include <arch/x86_64/smp.h>
#include <arch/x86_64/syscall_internal.h>
#include <arch/x86_64/uaccess.h>
#include <kernel/console.h>
#include <kernel/debug_stage.h>
#include <kernel/deferred.h>
#include <kernel/futex.h>
#include <kernel/kmem.h>
#include <kernel/net.h>
#include <kernel/sched.h>
#include <kernel/wait.h>
#include <kernel/vfs.h>
#include <uapi/mm.h>
#include <uapi/process.h>
#include <uapi/syscall.h>

#include "exec_internal.h"

/* REFACTOR_P5_USER_RUNTIME_TEST_OWNER: locatable user runtime self-test. */

#ifndef OS_DEBUG_SERIAL
#define OS_DEBUG_SERIAL 0
#endif

#ifndef OS_REALTEST
#define OS_REALTEST 0
#endif

#define USER_RUNTIME_COW_SYNC_ADDRESS     0x0000000067000000ULL
#define USER_RUNTIME_COW_PRIVATE_ADDRESS  0x0000000068000000ULL
#define USER_RUNTIME_COW_PROGRESS_OFFSET  4U
#define USER_RUNTIME_WAIT_RACE_START_OFFSET 0x1F4U
#define USER_RUNTIME_WAIT_RACE_GO_OFFSET    0x1F5U
#define USER_RUNTIME_WAIT_RACE_ACK_OFFSET   0x1F6U
#define USER_RUNTIME_WAIT_RACE_PASS_OFFSET  0x1F3U
#define USER_RUNTIME_EXEC_PIN_ACK_OFFSET     0x1F7U
#define USER_RUNTIME_EXEC_PIN_EXEC_ACK_OFFSET 0x1F8U
#define USER_RUNTIME_REAP_TIMEOUT_NS        5000000000ULL
#define USER_RUNTIME_TEST_TIMEOUT_NS        (180ULL * 1000000000ULL)

extern const uint8_t user_runtime_blob_start[];
extern const uint8_t user_runtime_blob_end[];
extern const uint8_t user_runtime_child[];
extern const uint8_t user_runtime_timer_helper[];
extern const uint8_t user_runtime_vm_worker[];
extern const uint8_t user_runtime_uaccess_worker[];

static uint32_t g_user_runtime_stage;
static uint64_t g_user_runtime_result;
static uint32_t g_user_runtime_cow_pass;
static uint32_t g_user_runtime_vm_concurrent_pass;
static uint32_t g_user_runtime_uaccess_pass;
static uint32_t g_user_runtime_wait_race_pass;
static uint32_t g_user_runtime_wait_race_state;
static uint32_t g_user_runtime_futex_word;
static uint32_t g_user_runtime_cow_progress;
static uint32_t g_user_runtime_cow_sync;
static uint32_t g_user_runtime_cow_private;
static uint32_t g_user_runtime_child_mark;
static uint32_t g_user_runtime_thread_count;
static uint32_t g_user_runtime_child_state;
static uint32_t g_user_runtime_child_cpu;
static uint32_t g_user_runtime_child_flags;
static uint32_t g_user_runtime_cpu_current_state;
static uint32_t g_user_runtime_cpu_runnable;
static uint64_t g_user_runtime_cpu_current_tid;

/* 执行引用/进程资源拆除阶段的最终诊断。 */
static uint32_t g_user_runtime_thread_flags;
static uint32_t g_user_runtime_process_flags;
static uint32_t g_user_runtime_process_thread_lock;
static uint32_t g_user_runtime_vm_live;
static uint32_t g_user_runtime_exec_stage;
static uint32_t g_user_runtime_exec_status;
static volatile uint32_t g_user_runtime_exec_pin_stage;
static volatile uint32_t g_user_runtime_exec_pin_flags;
static volatile uint64_t g_user_runtime_exec_pin_affinity;
static volatile uint32_t g_user_runtime_exec_pin_checked;
static volatile uint32_t g_user_runtime_exec_pin_passed;
static uint32_t g_user_runtime_main_state;
static uint32_t g_user_runtime_main_cpu;
static uint32_t g_user_runtime_main_flags;
static uint32_t g_user_runtime_list_tid;
static uint32_t g_user_runtime_list_state;
static uint32_t g_user_runtime_handle_grow_lock;
static uint32_t g_user_runtime_handle_chunk_lock;
static uintptr_t g_user_runtime_handle_waiting_lock;
static uint32_t g_user_runtime_socket_create_state;
static uint32_t g_user_runtime_syscall_return_progress;
static uint64_t g_user_runtime_syscall_return_number;
static uint64_t g_user_runtime_syscall_return_rip;
static uint32_t g_user_runtime_socket_return_progress;
static uint32_t g_user_runtime_socket_return_active;
static uint64_t g_user_runtime_socket_return_rip;
static uint32_t g_user_runtime_debug_cpu;
static uint32_t g_user_runtime_kmem_progress;
static uint32_t g_user_runtime_kmem_cache_waiting;
static uint32_t g_user_runtime_kmem_cache_class;
static uint32_t g_user_runtime_kmem_cache_owner;
static uint32_t g_user_runtime_kmem_cache_state;
static uint32_t g_user_runtime_deferred_progress;
static uint32_t g_user_runtime_deferred_waiting;
static uint32_t g_user_runtime_deferred_lock_owner;
static uint32_t g_user_runtime_deferred_lock_state;
static uint32_t g_user_runtime_deferred_worker_state;
static uint32_t g_user_runtime_deferred_queue_count;
static uint32_t g_user_runtime_bsp_preempt;
static uint32_t g_user_runtime_bsp_state;
static uint32_t g_user_runtime_bsp_runnable;
static uint64_t g_user_runtime_bsp_current_tid;
static uint32_t g_user_runtime_bsp_reschedule_pending;
static uint32_t g_user_runtime_page_table_state;
static uint32_t g_user_runtime_page_table_owner;
static uint32_t g_user_runtime_page_table_waiter;
static uint64_t g_user_runtime_page_table_wait_count;
static uint32_t g_user_runtime_tlb_state;
static uint32_t g_user_runtime_tlb_owner;
static uint32_t g_user_runtime_tlb_waiting_mask;
static uint32_t g_user_runtime_tlb_ack_mask;
static uint64_t g_user_runtime_tlb_generation;

static bool runtime_page_table_available(void) {
    uint32_t lock_state = 0U;
    x86_page_table_debug_state(&lock_state, 0, 0, 0);
    return lock_state == 0U;
}

typedef struct runtime_wait_child_snapshot {
    bool present;
    uint64_t pid;
    uint32_t process_state;
    uint32_t thread_count;
    uint64_t tid;
    uint32_t thread_state;
    uint32_t thread_cpu;
    uint32_t thread_flags;
    uint32_t sched_flags;
} runtime_wait_child_snapshot_t;

/* CHILD_* 是运行时父线程的快照，并非 waitpid 的目标。在输出失败记录前，只捕获一次实际尚未回收的子进程。 */
static void runtime_snapshot_wait_child(
    process_t *parent, runtime_wait_child_snapshot_t *snapshot) {
    if (parent == 0 || snapshot == 0) return;
    *snapshot = (runtime_wait_child_snapshot_t){0};
    sched_preempt_disable();
    spinlock_lock(&parent->children_lock);
    for (list_head_t *node = parent->children.next;
         node != &parent->children; node = node->next) {
        process_t *child = list_entry(node, process_t, child_node);
        snapshot->present = true;
        snapshot->pid = child->pid;
        snapshot->process_state =
            atomic_load_explicit(&child->state, memory_order_acquire);
        spinlock_lock(&child->thread_lock);
        snapshot->thread_count = child->thread_count;
        if (!list_empty(&child->threads)) {
            thread_t *thread = list_entry(child->threads.next,
                                           thread_t, process_node);
            snapshot->tid = thread->tid;
            snapshot->thread_state =
                atomic_load_explicit(&thread->state, memory_order_acquire);
            snapshot->thread_cpu = thread->current_cpu;
            snapshot->thread_flags =
                __atomic_load_n(&thread->flags, __ATOMIC_ACQUIRE);
            snapshot->sched_flags = thread->sched.flags;
        }
        spinlock_unlock(&child->thread_lock);
        break;
    }
    spinlock_unlock(&parent->children_lock);
    sched_preempt_enable();
}

static void runtime_log_wait_child(const runtime_wait_child_snapshot_t *snapshot) {
    if (snapshot == 0) return;
    kernel_serial_write("OS_RUNTIME_WAIT_CHILD_PRESENT=");
    kernel_serial_write_u32(snapshot->present ? 1U : 0U);
    if (snapshot->present) {
        kernel_serial_write(" PID=");
        kernel_serial_write_u32((uint32_t)snapshot->pid);
        kernel_serial_write(" PROCESS_STATE=");
        kernel_serial_write_u32(snapshot->process_state);
        kernel_serial_write(" THREADS=");
        kernel_serial_write_u32(snapshot->thread_count);
        kernel_serial_write(" TID=");
        kernel_serial_write_u32((uint32_t)snapshot->tid);
        kernel_serial_write(" THREAD_STATE=");
        kernel_serial_write_u32(snapshot->thread_state);
        kernel_serial_write(" CPU=");
        kernel_serial_write_u32(snapshot->thread_cpu);
        kernel_serial_write(" THREAD_FLAGS=");
        kernel_serial_write_u32(snapshot->thread_flags);
        kernel_serial_write(" SCHED_FLAGS=");
        kernel_serial_write_u32(snapshot->sched_flags);
    }
    kernel_serial_write("\r\n");
}

/* 用户线程死亡后，会在其栈所属 CPU 上释放 VM。自测运行于另一 CPU，因此应等待延迟拆除完成，而不是
 * 将暂时持有的全局页表锁误判为测试失败。 */
static bool runtime_wait_for_cleanup(process_t *process, thread_t *thread) {
    uint64_t deadline = x86_read_tsc();
    uint64_t budget = x86_timeout_ns_to_tsc(USER_RUNTIME_REAP_TIMEOUT_NS);
    deadline = budget > UINT64_MAX - deadline ? UINT64_MAX : deadline + budget;

    for (;;) {
        uint32_t flags = __atomic_load_n(&thread->flags, __ATOMIC_ACQUIRE);
        if ((flags & THREAD_FLAG_EXECUTION_REF) == 0U &&
            process->vm == 0 && runtime_page_table_available()) {
            return true;
        }

        uint32_t reap_cpu = thread->current_cpu;
        uint32_t current_cpu = x86_current_cpu_index();
        if (reap_cpu < MAX_CPUS && reap_cpu != current_cpu &&
            x86_smp_cpu_online(reap_cpu)) {
            (void)x86_smp_request_reschedule(reap_cpu);
        }

        __asm__ volatile ("sti" : : : "memory");
        if (runtime_page_table_available()) sched_finish_switch();
        if ((int64_t)(x86_read_tsc() - deadline) >= 0) return false;
        __asm__ volatile ("pause");
    }
}

bool user_elf_runtime_self_test(void) {
    uint8_t image[0x3200U];
    for (size_t i = 0; i < sizeof(image); ++i) image[i] = 0;
    elf64_header_t *header = (elf64_header_t *)image;
    header->ident[0] = 0x7FU;
    header->ident[1] = 'E';
    header->ident[2] = 'L';
    header->ident[3] = 'F';
    header->ident[4] = ELF_CLASS_64;
    header->ident[5] = ELF_DATA_LSB;
    header->ident[6] = ELF_VERSION_CURRENT;
    header->type = ELF_TYPE_EXEC;
    header->machine = ELF_MACHINE_X86_64;
    header->version = ELF_VERSION_CURRENT;
    header->entry = 0x400000ULL;
    header->program_header_offset = sizeof(*header);
    header->header_size = sizeof(*header);
    header->program_header_size = sizeof(elf64_program_header_t);
    header->program_header_count = 2U;
    elf64_program_header_t *programs =
        (elf64_program_header_t *)(image + sizeof(*header));
    elf64_program_header_t *code_program = &programs[0];
    code_program->type = ELF_PT_LOAD;
    code_program->flags = ELF_PF_READ | ELF_PF_EXEC;
    code_program->offset = 0x1000U;
    code_program->virtual_address = 0x400000ULL;
    code_program->memory_size = 2U * PAGE_SIZE;
    code_program->alignment = PAGE_SIZE;
    size_t code_size = (size_t)(user_runtime_blob_end -
                                user_runtime_blob_start);
    if (code_size == 0 || code_size > 2U * PAGE_SIZE) return false;
    code_program->file_size = code_size;
    for (size_t i = 0; i < code_size; ++i) {
        image[0x1000U + i] = user_runtime_blob_start[i];
    }

    elf64_program_header_t *data_program = &programs[1];
    data_program->type = ELF_PT_LOAD;
    data_program->flags = ELF_PF_READ | ELF_PF_WRITE;
    data_program->offset = 0x3000U;
    data_program->virtual_address = 0x402000ULL;
    data_program->file_size = 0x1E0U;
    data_program->memory_size = PAGE_SIZE;
    data_program->alignment = PAGE_SIZE;
    os_vm_map_args_t *arguments = (os_vm_map_args_t *)(image + 0x3000U);
    arguments->hdr.size = sizeof(*arguments);
    arguments->hdr.version = OS_SYSCALL_ABI_VERSION;
    arguments->length = PAGE_SIZE;
    arguments->prot = OS_VM_READ | OS_VM_WRITE;
    arguments->flags = OS_VM_PRIVATE;

    os_vm_map_args_t *stack_arguments =
        (os_vm_map_args_t *)(image + 0x3000U + 0x30U);
    stack_arguments->hdr.size = sizeof(*stack_arguments);
    stack_arguments->hdr.version = OS_SYSCALL_ABI_VERSION;
    stack_arguments->length = PAGE_SIZE;
    stack_arguments->prot = OS_VM_READ | OS_VM_WRITE;
    stack_arguments->flags = OS_VM_PRIVATE | OS_VM_STACK;

    os_thread_create_t *thread_arguments =
        (os_thread_create_t *)(image + 0x3000U + 0x60U);
    thread_arguments->hdr.size = sizeof(*thread_arguments);
    thread_arguments->hdr.version = OS_SYSCALL_ABI_VERSION;
    thread_arguments->entry = 0x400000ULL +
        (uint64_t)(user_runtime_child - user_runtime_blob_start);
    thread_arguments->argument = 0x1234U;

    os_thread_create_t *timer_arguments =
        (os_thread_create_t *)(image + 0x3000U + 0xC0U);
    timer_arguments->hdr.size = sizeof(*timer_arguments);
    timer_arguments->hdr.version = OS_SYSCALL_ABI_VERSION;
    timer_arguments->entry = 0x400000ULL +
        (uint64_t)(user_runtime_timer_helper - user_runtime_blob_start);
    timer_arguments->argument = 0x5678U;
    *(uint64_t *)(void *)(image + 0x3000U + 0xF0U) = 0x400000ULL +
        (uint64_t)(user_runtime_vm_worker - user_runtime_blob_start);
    static const char boot_path[] = "/usr/apps/gshell/gshell";
    for (size_t i = 0; i < sizeof(boot_path); ++i) {
        image[0x3000U + 0x100U + i] = (uint8_t)boot_path[i];
    }
    static const char fat32_path[] = "/usr/apps/notepad/notepad";
    for (size_t i = 0; i < sizeof(fat32_path); ++i) {
        image[0x3000U + 0x180U + i] = (uint8_t)fat32_path[i];
    }
    static const char exec_path[] = "/usr/apps/libc-test/libc-test";
    for (size_t i = 0; i < sizeof(exec_path); ++i) {
        image[0x3000U + 0x1A0U + i] = (uint8_t)exec_path[i];
    }
    uint64_t *exec_argv = (uint64_t *)(void *)(image + 0x3000U + 0x1D0U);
    exec_argv[0] = 0x004021A0ULL;
    exec_argv[1] = 0;

    process_t *process = 0;
    thread_t *thread = 0;
    vm_space_t *runtime_space = 0;
    user_elf_image_info_t info;
    vaddr_t stack_pointer = 0;
    paddr_t kernel_root = x86_current_root_table();
    uint64_t timer_ticks_before = lapic_tick_count();
    paddr_t marker_physical;
    bool success = false;
    bool exec_pin_test_armed = false;
    bool exec_pin_exec_requested = false;
    bool exec_pin_test_checked = false;
    debug_trace_set(false);
    g_user_runtime_stage = 0;
    g_user_runtime_result = 0;
    g_user_runtime_cow_pass = 0;
    g_user_runtime_vm_concurrent_pass = 0;
    g_user_runtime_uaccess_pass = 0;
    g_user_runtime_wait_race_pass = 0;
    g_user_runtime_wait_race_state = 0;
    g_user_runtime_futex_word = 0;
    g_user_runtime_cow_progress = 0;
    g_user_runtime_cow_sync = 0;
    g_user_runtime_cow_private = 0;
    g_user_runtime_child_mark = 0;
    g_user_runtime_thread_count = 0;
    g_user_runtime_child_state = UINT32_MAX;
    g_user_runtime_child_cpu = UINT32_MAX;
    g_user_runtime_child_flags = 0;
    g_user_runtime_cpu_current_state = UINT32_MAX;
    g_user_runtime_cpu_runnable = 0;
    g_user_runtime_cpu_current_tid = 0;
    g_user_runtime_exec_stage = 0;
    g_user_runtime_exec_status = 0;
    g_user_runtime_exec_pin_stage = 0;
    g_user_runtime_exec_pin_flags = UINT32_MAX;
    g_user_runtime_exec_pin_affinity = 0;
    g_user_runtime_exec_pin_checked = 0;
    g_user_runtime_exec_pin_passed = 0;
    g_user_runtime_main_state = UINT32_MAX;
    g_user_runtime_main_cpu = UINT32_MAX;
    g_user_runtime_main_flags = 0;
    g_user_runtime_list_tid = 0;
    g_user_runtime_list_state = UINT32_MAX;
    g_user_runtime_handle_grow_lock = 0;
    g_user_runtime_handle_chunk_lock = 0;
    g_user_runtime_handle_waiting_lock = 0;
    g_user_runtime_socket_create_state = 0;
    g_user_runtime_syscall_return_progress = 0;
    g_user_runtime_syscall_return_number = 0;
    g_user_runtime_syscall_return_rip = 0;
    g_user_runtime_socket_return_progress = 0;
    g_user_runtime_socket_return_active = 0;
    g_user_runtime_socket_return_rip = 0;
    g_user_runtime_debug_cpu = x86_current_cpu_index();
    g_user_runtime_kmem_progress = 0;
    g_user_runtime_kmem_cache_waiting = 0;
    g_user_runtime_kmem_cache_class = UINT32_MAX;
    g_user_runtime_kmem_cache_owner = UINT32_MAX;
    g_user_runtime_kmem_cache_state = 0;
    g_user_runtime_deferred_progress = 0;
    g_user_runtime_deferred_waiting = 0;
    g_user_runtime_deferred_lock_owner = UINT32_MAX;
    g_user_runtime_deferred_lock_state = 0;
    g_user_runtime_deferred_worker_state = 0;
    g_user_runtime_deferred_queue_count = 0;
    g_user_runtime_bsp_preempt = 0;
    g_user_runtime_bsp_state = 0;
    g_user_runtime_bsp_runnable = 0;
    g_user_runtime_bsp_current_tid = 0;
    g_user_runtime_bsp_reschedule_pending = 0;
    g_user_runtime_page_table_state = 0;
    g_user_runtime_page_table_owner = UINT32_MAX;
    g_user_runtime_page_table_waiter = UINT32_MAX;
    g_user_runtime_page_table_wait_count = 0;
    g_user_runtime_tlb_state = 0;
    g_user_runtime_tlb_owner = UINT32_MAX;
    g_user_runtime_tlb_waiting_mask = 0;
    g_user_runtime_tlb_ack_mask = 0;
    g_user_runtime_tlb_generation = 0;
    if (process_create(0, &process) != K_OK ||
        process_load_elf_image(process, image, sizeof(image), &info, &stack_pointer) != K_OK ||
        thread_create_user_suspended(process, info.entry, stack_pointer, 0, 0,
                                     &thread) != K_OK) {
        goto cleanup;
    }
    if (x86_smp_discovered_count() > 1U) {
        cpumask_t remote_mask = {0};
        uint32_t current_cpu = x86_current_cpu_index();
        uint32_t remote_cpu = UINT32_MAX;
        for (uint32_t cpu = 0; cpu < x86_smp_discovered_count(); ++cpu) {
            if (cpu != current_cpu && x86_smp_cpu_online(cpu)) {
                remote_cpu = cpu;
                break;
            }
        }
        if (remote_cpu == UINT32_MAX) goto cleanup;
        remote_mask.bits[remote_cpu >> 6] = 1ULL << (remote_cpu & 63U);
        if (sched_set_affinity(thread, &remote_mask) != K_OK) goto cleanup;
    }
    debug_trace_set(true);
    if (thread_start(thread) != K_OK) goto cleanup;
    runtime_space = process->vm;
    vm_space_get(runtime_space);

    /* 单次 schedule() 只保证执行一次调度决策，不保证用户进程已经退出。当所有用户线程短暂阻塞时，
     * idle 会恢复执行；此时若立即 cleanup，可能释放仍会被定时器唤醒的进程。应持续调度，直到主线程
     * 和进程都发布 DEAD，或达到自测硬截止时间。 */
    uint64_t runtime_deadline = x86_read_tsc();
    /*
    * COM1 阶段跟踪有意精确到字节，以便定位每次状态转换。在非 PTY 的 Windows/WSL 启动方式下，
    * 高频 xHCI 诊断会使文件后端变得比客户机本身慢得多。realtest 还会执行两次 4.4 MB 的 NASM 映像，
    * 因此即使禁用 COM1 镜像，也使用相同的有界预算。
     */
#if OS_DEBUG_SERIAL || OS_REALTEST
    /* 首条 Blend2D 流水线会在有界测试期间进行 JIT 编译。 */
    uint64_t runtime_budget =
        x86_timeout_ns_to_tsc(USER_RUNTIME_TEST_TIMEOUT_NS);
#else
    uint64_t runtime_budget = x86_timeout_ns_to_tsc(15000000000ULL);
#endif
    runtime_deadline = runtime_budget > UINT64_MAX - runtime_deadline ?
                       UINT64_MAX : runtime_deadline + runtime_budget;
#if OS_DEBUG_SERIAL
    uint8_t runtime_progress = 0xFFU;
#endif
    for (;;) {
        /* 寮曞 idle 灏氭棤鐙珛涓婁笅鏂囷紝鏄惧紡鎭㈠ IF 浠ユ帴鏀跺畾鏃跺櫒鍜岄噸璋冨害 IPI銆?*/
        __asm__ volatile ("sti" : : : "memory");
        schedule();
        sched_finish_switch();
        if (!exec_pin_test_armed && runtime_page_table_available() &&
            x86_translate_page(runtime_space->root_table, 0x00402000ULL,
                               &marker_physical, 0) == K_OK) {
            uint8_t *progress_page =
                (uint8_t *)phys_to_direct(marker_physical);
            if (progress_page != 0 && progress_page[0xA8U] == 17U) {
                __atomic_fetch_or(&process->flags,
                                  PROCESS_FLAG_INIT_CPU_PINNED,
                                  __ATOMIC_ACQ_REL);
                __atomic_store_n(
                    &progress_page[USER_RUNTIME_EXEC_PIN_ACK_OFFSET], 1U,
                    __ATOMIC_RELEASE);
                exec_pin_test_armed = true;
            }
        }
        if (exec_pin_test_armed && !exec_pin_exec_requested &&
            runtime_page_table_available() &&
            x86_translate_page(runtime_space->root_table, 0x00402000ULL,
                               &marker_physical, 0) == K_OK) {
            uint8_t *progress_page =
                (uint8_t *)phys_to_direct(marker_physical);
            if (progress_page != 0 && progress_page[0xA8U] == 18U) {
                process_exec_debug_reset();
                __atomic_store_n(
                    &progress_page[USER_RUNTIME_EXEC_PIN_EXEC_ACK_OFFSET],
                    1U, __ATOMIC_RELEASE);
                exec_pin_exec_requested = true;
            }
        }
        if (exec_pin_test_armed && !exec_pin_test_checked) {
            uint32_t exec_stage = process_exec_debug_stage();
            if (exec_pin_exec_requested && exec_stage == 12U &&
                process_exec_debug_status() == K_OK) {
                uint32_t flags =
                    __atomic_load_n(&process->flags, __ATOMIC_ACQUIRE);
                uint64_t affinity = thread->affinity.bits[0];
                bool all_online_cpus_allowed = true;
                for (uint32_t cpu = 0; cpu < x86_smp_discovered_count(); ++cpu) {
                    if (x86_smp_cpu_online(cpu) &&
                        (thread->affinity.bits[cpu >> 6U] &
                         (1ULL << (cpu & 63U))) == 0U) {
                        all_online_cpus_allowed = false;
                    }
                }
                g_user_runtime_exec_pin_stage = exec_stage;
                g_user_runtime_exec_pin_flags = flags;
                g_user_runtime_exec_pin_affinity = affinity;
                g_user_runtime_exec_pin_passed =
                    (flags & PROCESS_FLAG_INIT_CPU_PINNED) == 0U &&
                    all_online_cpus_allowed ? 1U : 0U;
                g_user_runtime_exec_pin_checked = 1U;
                exec_pin_test_checked = true;
            }
        }
#if OS_DEBUG_SERIAL
        if (runtime_page_table_available() &&
            x86_translate_page(runtime_space->root_table, 0x00402000ULL,
                               &marker_physical, 0) == K_OK) {
            uint8_t *progress_page = (uint8_t *)phys_to_direct(marker_physical);
            if (progress_page != 0 && progress_page[0xA8U] != runtime_progress) {
                runtime_progress = progress_page[0xA8U];
                debug_trace_stage(OS_DEBUG_PHASE_USER_RUNTIME,
                                         OS_DEBUG_STEP_USER_MARK,
                                         runtime_progress);
            }
        }
#endif
        if (atomic_load_explicit(&thread->state, memory_order_acquire) == THREAD_DEAD &&
            atomic_load_explicit(&process->state, memory_order_acquire) == PROCESS_DEAD) {
            break;
        }
        if ((int64_t)(x86_read_tsc() - runtime_deadline) >= 0) break;
        __asm__ volatile ("pause");
    }
    /* 执行 CPU 完成 VM 拆除前，PROCESS_DEAD 就会先发布。 */
    bool cleanup_complete = runtime_wait_for_cleanup(process, thread);

    if (atomic_load_explicit(&thread->state, memory_order_acquire) != THREAD_DEAD ||
        atomic_load_explicit(&process->state, memory_order_acquire) != PROCESS_DEAD) {
        runtime_wait_child_snapshot_t wait_child = {0};
        runtime_snapshot_wait_child(process, &wait_child);
        runtime_log_wait_child(&wait_child);
    }

    thread_t *current = sched_current_thread();
    g_user_runtime_thread_count = process->thread_count;
    g_user_runtime_main_state =
        atomic_load_explicit(&thread->state, memory_order_acquire);
    g_user_runtime_main_cpu = thread->current_cpu;
    g_user_runtime_main_flags = __atomic_load_n(&thread->flags, __ATOMIC_ACQUIRE);
    for (list_head_t *node = process->threads.next;
         node != &process->threads; node = node->next) {
        thread_t *candidate = (thread_t *)((uint8_t *)node -
            __builtin_offsetof(thread_t, process_node));
        if (candidate != thread) {
            g_user_runtime_list_tid = (uint32_t)candidate->tid;
            g_user_runtime_list_state =
                atomic_load_explicit(&candidate->state, memory_order_acquire);
            g_user_runtime_child_state = atomic_load_explicit(&candidate->state,
                                                               memory_order_acquire);
            g_user_runtime_child_cpu = candidate->current_cpu;
            g_user_runtime_child_flags = candidate->sched.flags;
            break;
        }
    }
    /*
    * 运行时等待诊断：
    * 如果辅助线程已经退出，则复用 CHILD_* 输出 main thread 本身。
    *
    * CHILD_STATE：
    *   0 NEW（新建）
    *   1 READY（就绪）
    *   2 RUNNING（运行中）
    *   3 BLOCKED（阻塞）
    *   4 STOPPED（已停止）
    *   5 DEAD（已死亡）
     *
    * CHILD_FLAGS：
    *   bit0      = 已加入 SCHED_ENTITY_ENQUEUED 队列
    *   bit16     = blocked_waiter != NULL
    *   bits24-31 = waiter_state
    *               0 WAITING（等待中）
    *               1 WOKEN（已唤醒）
    *               2 TIMED_OUT（超时）
    *               3 CANCELLED（已取消）
     */
    if (g_user_runtime_child_state == UINT32_MAX && thread != 0) {
        g_user_runtime_child_state =
            atomic_load_explicit(&thread->state, memory_order_acquire);
        g_user_runtime_child_cpu = thread->current_cpu;

        uint32_t diag_flags = thread->sched.flags;
        waiter_t *diag_waiter = atomic_load_explicit(&thread->blocked_waiter,
                                                      memory_order_acquire);
        if (diag_waiter != 0) {
            diag_flags |= (1U << 16);
            diag_flags |=
                (atomic_load_explicit(&diag_waiter->state,
                                      memory_order_acquire) & 0xFFU) << 24;
        }
        g_user_runtime_child_flags = diag_flags;
    }

    if (g_user_runtime_child_cpu != UINT32_MAX) {
        (void)sched_debug_cpu(g_user_runtime_child_cpu,
                              &g_user_runtime_cpu_current_state,
                              &g_user_runtime_cpu_current_tid,
                              &g_user_runtime_cpu_runnable);

        /*
         * 运行时 CPU 调度停滞诊断。
         *
         * CHILD_FLAGS：
         *   bit0      = 主线程已入队
         *   bits8-15  = 目标 CPU 的 PreemptDisable 值
         *   bit16     = 主线程 blocked_waiter != NULL
         *   bit17     = 目标 CPU 的 g_tlb_waiting 状态
         *   bit18     = 目标 CPU 存在待处理的重新调度请求
         *   bits24-31 = waiter 状态
         */
        uint32_t preempt_count =
            x86_cpu_preempt_disable_count(g_user_runtime_child_cpu);

        g_user_runtime_child_flags |=
            (preempt_count & 0xFFU) << 8;

        if (x86_tlb_cpu_waiting(g_user_runtime_child_cpu)) {
            g_user_runtime_child_flags |= (1U << 17);
        }

        if (x86_smp_reschedule_pending(g_user_runtime_child_cpu)) {
            g_user_runtime_child_flags |= (1U << 18);
        }

    }
    x86_page_table_debug_state(&g_user_runtime_page_table_state,
                               &g_user_runtime_page_table_owner,
                               &g_user_runtime_page_table_waiter,
                               &g_user_runtime_page_table_wait_count);
    x86_tlb_debug_state(&g_user_runtime_tlb_state,
                        &g_user_runtime_tlb_owner,
                        &g_user_runtime_tlb_waiting_mask,
                        &g_user_runtime_tlb_ack_mask,
                        &g_user_runtime_tlb_generation);

    uint8_t marker = 0;
    if (runtime_page_table_available() &&
        x86_translate_page(runtime_space->root_table, 0x402000ULL,
                           &marker_physical, 0) == K_OK) {
        uint8_t *page = (uint8_t *)phys_to_direct(marker_physical);
        if (page != 0) {
            g_user_runtime_futex_word = *(const uint32_t *)(const void *)(page + 0xA0U);
            g_user_runtime_child_mark = *(const uint32_t *)(const void *)(page + 0xA4U);
            marker = page[0xA4U];
            g_user_runtime_stage = page[0xA8U];
            g_user_runtime_result = *(const uint64_t *)(const void *)(page + 0xB0U);
            g_user_runtime_cow_pass = page[0xBCU];
            /* VM 并发测试标记位于用户数据页的 0x1F0，避开定时器参数区域。 */
            g_user_runtime_vm_concurrent_pass = page[0x1F0U];
            g_user_runtime_uaccess_pass = page[0x1F2U];
            g_user_runtime_wait_race_pass =
                page[USER_RUNTIME_WAIT_RACE_PASS_OFFSET];
            g_user_runtime_wait_race_state =
                (uint32_t)page[USER_RUNTIME_WAIT_RACE_START_OFFSET] |
                ((uint32_t)page[USER_RUNTIME_WAIT_RACE_GO_OFFSET] << 8U) |
                ((uint32_t)page[USER_RUNTIME_WAIT_RACE_ACK_OFFSET] << 16U) |
                (g_user_runtime_wait_race_pass << 24U);
        }
    }
    /* 释放保留的运行时地址空间前，记录共享 COW 握手状态和父进程的私有字节。这些值可区分子进程缺页
     * 故障与调度超时。 */
    paddr_t cow_physical;
    if (runtime_page_table_available() && runtime_space != 0 &&
        x86_translate_page(runtime_space->root_table,
                           USER_RUNTIME_COW_SYNC_ADDRESS,
                           &cow_physical, 0) == K_OK) {
        uint8_t *cow_page = (uint8_t *)phys_to_direct(cow_physical);
        if (cow_page != 0) {
            g_user_runtime_cow_sync = cow_page[0];
            g_user_runtime_cow_progress =
                cow_page[USER_RUNTIME_COW_PROGRESS_OFFSET];
        }
    }
    if (runtime_page_table_available() && runtime_space != 0 &&
        x86_translate_page(runtime_space->root_table,
                           USER_RUNTIME_COW_PRIVATE_ADDRESS,
                           &cow_physical, 0) == K_OK) {
        uint8_t *cow_page = (uint8_t *)phys_to_direct(cow_physical);
        if (cow_page != 0) g_user_runtime_cow_private = cow_page[0];
    }
    /*
    * 获取实际的生命周期/资源标志快照。
     *
    * 注意：CHILD_FLAGS 主要来自 thread->sched.flags，因此无法判断 EXECUTION_REF 是否仍由线程持有。
     */
    g_user_runtime_thread_flags =
        __atomic_load_n(&thread->flags, __ATOMIC_ACQUIRE);

    g_user_runtime_process_flags =
        __atomic_load_n(&process->flags, __ATOMIC_ACQUIRE);

    g_user_runtime_process_thread_lock =
        atomic_load_explicit(&process->thread_lock.state,
                             memory_order_acquire);

    g_user_runtime_handle_grow_lock =
        atomic_load_explicit(&process->handles.grow_lock.state,
                             memory_order_acquire);
    if (process->handles.chunks != 0 && process->handles.chunk_count != 0U &&
        process->handles.chunks[0] != 0) {
        g_user_runtime_handle_chunk_lock =
            atomic_load_explicit(&process->handles.chunks[0]->lock.state,
                                 memory_order_acquire);
    }
    g_user_runtime_handle_waiting_lock = handle_debug_waiting_lock();
    g_user_runtime_socket_create_state =
        syscall_socket_create_debug_progress(thread->current_cpu);
    g_user_runtime_syscall_return_progress =
        x86_syscall_return_progress(thread->current_cpu);
    g_user_runtime_syscall_return_number =
        x86_syscall_return_number(thread->current_cpu);
    g_user_runtime_syscall_return_rip =
        x86_syscall_return_rip(thread->current_cpu);
    g_user_runtime_socket_return_progress =
        x86_socket_return_progress(thread->current_cpu);
    g_user_runtime_socket_return_active =
        x86_socket_return_active(thread->current_cpu);
    g_user_runtime_socket_return_rip =
        x86_socket_return_rip(thread->current_cpu);
    g_user_runtime_debug_cpu = thread->current_cpu;
    g_user_runtime_kmem_cache_waiting =
        kmem_debug_cache_waiting(thread->current_cpu);
    g_user_runtime_kmem_progress =
        kmem_debug_progress(thread->current_cpu);
    g_user_runtime_kmem_cache_class =
        kmem_debug_cache_class(thread->current_cpu);
    g_user_runtime_kmem_cache_owner =
        kmem_debug_cache_owner(g_user_runtime_kmem_cache_class);
    g_user_runtime_kmem_cache_state =
        kmem_debug_cache_state(g_user_runtime_kmem_cache_class);
    g_user_runtime_deferred_progress =
        deferred_debug_progress(thread->current_cpu);
    g_user_runtime_deferred_waiting =
        deferred_debug_waiting(thread->current_cpu);
    g_user_runtime_deferred_lock_owner = deferred_debug_lock_owner();
    g_user_runtime_deferred_lock_state = deferred_debug_lock_state();
    g_user_runtime_deferred_worker_state = deferred_debug_worker_state();
    g_user_runtime_deferred_queue_count = deferred_debug_queue_count();
    g_user_runtime_bsp_preempt = x86_cpu_preempt_disable_count(0U);
    (void)sched_debug_cpu(0U, &g_user_runtime_bsp_state,
                          &g_user_runtime_bsp_current_tid,
                          &g_user_runtime_bsp_runnable);
    g_user_runtime_bsp_reschedule_pending =
        x86_smp_reschedule_pending(0U) ? 1U : 0U;

    g_user_runtime_vm_live =
        process->vm != 0 ? 1U : 0U;

    g_user_runtime_exec_stage = process_exec_debug_stage();
    g_user_runtime_exec_status = process_exec_debug_status();

    bool thread_dead = atomic_load_explicit(&thread->state, memory_order_acquire) == THREAD_DEAD;
    bool process_dead = atomic_load_explicit(&process->state, memory_order_acquire) == PROCESS_DEAD;
    bool tick_seen = lapic_tick_count() > timer_ticks_before;
    bool kernel_thread_current = current != 0 && current->process == 0;
    bool kernel_root_current = x86_current_root_table().value == kernel_root.value;
    /*
    * 最终运行时阶段会有意执行并退出第二个 ELF。此转换会释放旧运行时地址空间，因此检查代码不再能读取
    * 原来的 0xA5 标记。替换映像正常退出时会发布 EXEC_STAGE=14，该值即为此预期路径的成功标记。
     */
    bool exec_exit_completed =
        g_user_runtime_exec_stage == 14U &&
        g_user_runtime_exec_status == K_OK;
    bool exec_pin_test_passed = exec_pin_test_armed &&
        exec_pin_exec_requested && exec_pin_test_checked &&
        g_user_runtime_exec_pin_passed == 1U;
    if (exec_pin_test_armed && !exec_pin_test_passed) {
        g_user_runtime_stage = 19U;
        g_user_runtime_result |= 0x400U;
    }
    success = cleanup_complete && thread_dead && thread->exit_code == 0 &&
              process->thread_count == 0 &&
              process_dead && process->exit_code == 0 && process->vm == 0 &&
              (marker == 0xA5U || exec_exit_completed) &&
              exec_pin_test_passed && tick_seen &&
              kernel_thread_current &&
              kernel_root_current;
    if (!success && g_user_runtime_result == 0U) {
        uint64_t failure = 0U;
        if (!thread_dead) failure |= 1U;
        if (thread->exit_code != 0) failure |= 2U;
        if (process->thread_count != 0) failure |= 4U;
        if (!process_dead) failure |= 8U;
        if (process->exit_code != 0) failure |= 16U;
        if (process->vm != 0) failure |= 32U;
        if (marker != 0xA5U) failure |= 64U;
        if (!tick_seen) failure |= 128U;
        if (!kernel_thread_current) failure |= 256U;
        if (!kernel_root_current) failure |= 512U;
        g_user_runtime_result = failure;
    }

cleanup:
    debug_trace_set(false);
    if (runtime_space != 0) vm_space_put(runtime_space);
    if (thread != 0) {
        (void)thread_terminate(thread, K_ECANCELED);
        object_put(thread);
    }
    if (process != 0) {
        if (thread == 0) (void)process_abort(process);
        object_put(process);
    }
    return success;
}

uint32_t user_elf_runtime_failure_stage(void) {
    return g_user_runtime_stage;
}

uint64_t user_elf_runtime_failure_result(void) {
    return g_user_runtime_result;
}

bool user_elf_runtime_cow_passed(void) {
    return g_user_runtime_cow_pass == 0xC0U;
}

bool user_elf_runtime_vm_concurrent_passed(void) {
    return g_user_runtime_vm_concurrent_pass == 0xD2U;
}

bool user_elf_runtime_uaccess_passed(void) {
    return g_user_runtime_uaccess_pass == 0xA7U;
}

bool user_elf_runtime_wait_race_passed(void) {
    return g_user_runtime_wait_race_pass == 0xB7U;
}

uint32_t user_elf_runtime_wait_race_state(void) {
    return g_user_runtime_wait_race_state;
}

uint32_t user_elf_runtime_futex_word(void) {
    return g_user_runtime_futex_word;
}

uint32_t user_elf_runtime_cow_progress(void) {
    return g_user_runtime_cow_progress;
}

uint32_t user_elf_runtime_cow_sync(void) {
    return g_user_runtime_cow_sync;
}

uint32_t user_elf_runtime_cow_private(void) {
    return g_user_runtime_cow_private;
}

uint32_t user_elf_runtime_child_mark(void) {
    return g_user_runtime_child_mark;
}

uint32_t user_elf_runtime_thread_count(void) {
    return g_user_runtime_thread_count;
}

uint32_t user_elf_runtime_child_state(void) {
    return g_user_runtime_child_state;
}

uint32_t user_elf_runtime_child_cpu(void) {
    return g_user_runtime_child_cpu;
}

uint32_t user_elf_runtime_child_flags(void) {
    return g_user_runtime_child_flags;
}

uint32_t user_elf_runtime_cpu_current_state(void) {
    return g_user_runtime_cpu_current_state;
}

uint32_t user_elf_runtime_cpu_runnable(void) {
    return g_user_runtime_cpu_runnable;
}

uint64_t user_elf_runtime_cpu_current_tid(void) {
    return g_user_runtime_cpu_current_tid;
}

uint32_t user_elf_runtime_thread_flags(void) {
    return g_user_runtime_thread_flags;
}

uint32_t user_elf_runtime_process_flags(void) {
    return g_user_runtime_process_flags;
}

uint32_t user_elf_runtime_process_thread_lock(void) {
    return g_user_runtime_process_thread_lock;
}

uint32_t user_elf_runtime_handle_grow_lock(void) {
    return g_user_runtime_handle_grow_lock;
}

uint32_t user_elf_runtime_handle_chunk_lock(void) {
    return g_user_runtime_handle_chunk_lock;
}

uintptr_t user_elf_runtime_handle_waiting_lock(void) {
    return g_user_runtime_handle_waiting_lock;
}

uint32_t user_elf_runtime_socket_create_state(void) {
    return g_user_runtime_socket_create_state;
}

uint32_t user_elf_runtime_syscall_return_progress(void) {
    return g_user_runtime_syscall_return_progress;
}

uint64_t user_elf_runtime_syscall_return_number(void) {
    return g_user_runtime_syscall_return_number;
}

uint64_t user_elf_runtime_syscall_return_rip(void) {
    return g_user_runtime_syscall_return_rip;
}

uint32_t user_elf_runtime_socket_return_progress(void) {
    return g_user_runtime_socket_return_progress;
}

uint32_t user_elf_runtime_socket_return_active(void) {
    return g_user_runtime_socket_return_active;
}

uint64_t user_elf_runtime_socket_return_rip(void) {
    return g_user_runtime_socket_return_rip;
}

uint32_t user_elf_runtime_debug_cpu(void) {
    return g_user_runtime_debug_cpu;
}

uint32_t user_elf_runtime_kmem_progress(void) {
    return g_user_runtime_kmem_progress;
}

uint32_t user_elf_runtime_kmem_cache_waiting(void) {
    return g_user_runtime_kmem_cache_waiting;
}

uint32_t user_elf_runtime_kmem_cache_class(void) {
    return g_user_runtime_kmem_cache_class;
}

uint32_t user_elf_runtime_kmem_cache_owner(void) {
    return g_user_runtime_kmem_cache_owner;
}

uint32_t user_elf_runtime_kmem_cache_state(void) {
    return g_user_runtime_kmem_cache_state;
}

uint32_t user_elf_runtime_deferred_progress(void) {
    return g_user_runtime_deferred_progress;
}

uint32_t user_elf_runtime_deferred_waiting(void) {
    return g_user_runtime_deferred_waiting;
}

uint32_t user_elf_runtime_deferred_lock_owner(void) {
    return g_user_runtime_deferred_lock_owner;
}

uint32_t user_elf_runtime_deferred_lock_state(void) {
    return g_user_runtime_deferred_lock_state;
}

uint32_t user_elf_runtime_deferred_worker_state(void) {
    return g_user_runtime_deferred_worker_state;
}

uint32_t user_elf_runtime_deferred_queue_count(void) {
    return g_user_runtime_deferred_queue_count;
}

uint32_t user_elf_runtime_bsp_preempt(void) {
    return g_user_runtime_bsp_preempt;
}

uint32_t user_elf_runtime_bsp_state(void) {
    return g_user_runtime_bsp_state;
}

uint32_t user_elf_runtime_bsp_runnable(void) {
    return g_user_runtime_bsp_runnable;
}

uint64_t user_elf_runtime_bsp_current_tid(void) {
    return g_user_runtime_bsp_current_tid;
}

uint32_t user_elf_runtime_bsp_reschedule_pending(void) {
    return g_user_runtime_bsp_reschedule_pending;
}


uint32_t user_elf_runtime_vm_live(void) {
    return g_user_runtime_vm_live;
}

uint32_t user_elf_runtime_exec_stage(void) {
    return g_user_runtime_exec_stage;
}

uint32_t user_elf_runtime_exec_status(void) {
    return g_user_runtime_exec_status;
}

uint32_t user_elf_runtime_main_state(void) {
    return g_user_runtime_main_state;
}

uint32_t user_elf_runtime_main_cpu(void) {
    return g_user_runtime_main_cpu;
}

uint32_t user_elf_runtime_main_flags(void) {
    return g_user_runtime_main_flags;
}

uint32_t user_elf_runtime_list_tid(void) {
    return g_user_runtime_list_tid;
}

uint32_t user_elf_runtime_list_state(void) {
    return g_user_runtime_list_state;
}

uint32_t user_elf_runtime_page_table_state(void) {
    return g_user_runtime_page_table_state;
}

uint32_t user_elf_runtime_page_table_owner(void) {
    return g_user_runtime_page_table_owner;
}

uint32_t user_elf_runtime_page_table_waiter(void) {
    return g_user_runtime_page_table_waiter;
}

uint64_t user_elf_runtime_page_table_wait_count(void) {
    return g_user_runtime_page_table_wait_count;
}

uint32_t user_elf_runtime_tlb_state(void) {
    return g_user_runtime_tlb_state;
}

uint32_t user_elf_runtime_tlb_owner(void) {
    return g_user_runtime_tlb_owner;
}

uint32_t user_elf_runtime_tlb_waiting_mask(void) {
    return g_user_runtime_tlb_waiting_mask;
}

uint32_t user_elf_runtime_tlb_ack_mask(void) {
    return g_user_runtime_tlb_ack_mask;
}

uint64_t user_elf_runtime_tlb_generation(void) {
    return g_user_runtime_tlb_generation;
}
