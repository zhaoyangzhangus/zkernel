#pragma once

#include <kernel/process.h>

typedef struct user_elf_image_info {
    vaddr_t entry;
    /* 动态 ELF 的解释器入口；静态 ELF 时与 entry 相同。 */
    vaddr_t main_entry;
    /* 动态 ELF 解释器的装载基址，供 AT_BASE 使用。 */
    vaddr_t interpreter_base;
    vaddr_t program_headers;
    uint16_t program_header_size;
    uint16_t program_header_count;
    vaddr_t load_bias;
    vaddr_t brk_base;
    bool linux_abi;
} user_elf_image_info_t;

kstatus_t process_load_elf_image(process_t *process, const void *image, size_t image_size,
                                 user_elf_image_info_t *info, vaddr_t *stack_pointer);
/* 从文件分块加载新进程映像；maximum_size 保留调用者的文件大小策略。 */
kstatus_t process_load_elf_from_vfs(process_t *process, const char *path,
    uint64_t maximum_size, user_elf_image_info_t *info, vaddr_t *stack_pointer);
kstatus_t process_exec_from_vfs(
    process_t *process, const char __user *path,
    const char __user *const __user *argv,
    const char __user *const __user *envp,
    const os_exec_fd_map_t *descriptor_map);
bool user_elf_loader_self_test(void);
bool user_elf_runtime_self_test(void);
uint32_t user_elf_runtime_failure_stage(void);
uint64_t user_elf_runtime_failure_result(void);
bool user_elf_runtime_cow_passed(void);
bool user_elf_runtime_vm_concurrent_passed(void);
bool user_elf_runtime_uaccess_passed(void);
bool user_elf_runtime_wait_race_passed(void);
uint32_t user_elf_runtime_wait_race_state(void);
uint32_t user_elf_runtime_futex_word(void);
uint32_t user_elf_runtime_cow_progress(void);
uint32_t user_elf_runtime_cow_sync(void);
uint32_t user_elf_runtime_cow_private(void);
uint32_t user_elf_runtime_child_mark(void);
uint32_t user_elf_runtime_thread_count(void);
uint32_t user_elf_runtime_child_state(void);
uint32_t user_elf_runtime_child_cpu(void);
uint32_t user_elf_runtime_child_flags(void);
uint32_t user_elf_runtime_cpu_current_state(void);
uint32_t user_elf_runtime_cpu_runnable(void);
uint64_t user_elf_runtime_cpu_current_tid(void);
uint32_t user_elf_runtime_thread_flags(void);
uint32_t user_elf_runtime_process_flags(void);
uint32_t user_elf_runtime_process_thread_lock(void);
uint32_t user_elf_runtime_handle_grow_lock(void);
uint32_t user_elf_runtime_handle_chunk_lock(void);
uintptr_t user_elf_runtime_handle_waiting_lock(void);
uint32_t user_elf_runtime_socket_create_state(void);
uint32_t user_elf_runtime_syscall_return_progress(void);
uint64_t user_elf_runtime_syscall_return_number(void);
uint64_t user_elf_runtime_syscall_return_rip(void);
uint32_t user_elf_runtime_socket_return_progress(void);
uint32_t user_elf_runtime_socket_return_active(void);
uint64_t user_elf_runtime_socket_return_rip(void);
uint32_t user_elf_runtime_debug_cpu(void);
uint32_t user_elf_runtime_kmem_progress(void);
uint32_t user_elf_runtime_kmem_cache_waiting(void);
uint32_t user_elf_runtime_kmem_cache_class(void);
uint32_t user_elf_runtime_kmem_cache_owner(void);
uint32_t user_elf_runtime_kmem_cache_state(void);
uint32_t user_elf_runtime_deferred_progress(void);
uint32_t user_elf_runtime_deferred_waiting(void);
uint32_t user_elf_runtime_deferred_lock_owner(void);
uint32_t user_elf_runtime_deferred_lock_state(void);
uint32_t user_elf_runtime_deferred_worker_state(void);
uint32_t user_elf_runtime_deferred_queue_count(void);
uint32_t user_elf_runtime_bsp_preempt(void);
uint32_t user_elf_runtime_bsp_state(void);
uint32_t user_elf_runtime_bsp_runnable(void);
uint64_t user_elf_runtime_bsp_current_tid(void);
uint32_t user_elf_runtime_bsp_reschedule_pending(void);
uint32_t user_elf_runtime_vm_live(void);
uint32_t user_elf_runtime_exec_stage(void);
uint32_t user_elf_runtime_exec_status(void);
uint32_t user_elf_runtime_main_state(void);
uint32_t user_elf_runtime_main_cpu(void);
uint32_t user_elf_runtime_main_flags(void);
uint32_t user_elf_runtime_list_tid(void);
uint32_t user_elf_runtime_list_state(void);
uint32_t user_elf_runtime_page_table_state(void);
uint32_t user_elf_runtime_page_table_owner(void);
uint32_t user_elf_runtime_page_table_waiter(void);
uint64_t user_elf_runtime_page_table_wait_count(void);
uint32_t user_elf_runtime_tlb_state(void);
uint32_t user_elf_runtime_tlb_owner(void);
uint32_t user_elf_runtime_tlb_waiting_mask(void);
uint32_t user_elf_runtime_tlb_ack_mask(void);
uint64_t user_elf_runtime_tlb_generation(void);
