#include <arch/x86_64/apic.h>
#include <arch/x86_64/cpu.h>
#include <arch/x86_64/paging.h>
#include <arch/x86_64/smp.h>
#include <arch/x86_64/uaccess.h>
#include <kernel/elf_loader.h>
#include <kernel/console.h>
#include <kernel/debug_stage.h>
#include <kernel/deferred.h>
#include <kernel/futex.h>
#include <kernel/wait.h>
#include <kernel/kmem.h>
#include <kernel/sched.h>
#include <kernel/vfs.h>
#include <uapi/mm.h>
#include <uapi/linux_abi.h>
#include <uapi/process.h>
#include <uapi/syscall.h>

#include "exec_internal.h"
#include "internal.h"

#ifndef OS_DEBUG_SERIAL
#define OS_DEBUG_SERIAL 0
#endif

#define ELF_EXEC_ARG_MAX      32U
#define ELF_EXEC_ENV_MAX      64U
#define ELF_EXEC_ARG_LENGTH   256U
#define EXEC_PATH_CAPACITY    256U
#define ELF_EXEC_AUX_MAX      36U
#define ELF_EXEC_METADATA_SIZE (16ULL * 1024ULL * 1024ULL)
#define ELF_EXEC_LOAD_CHUNK_SIZE (1ULL * 1024ULL * 1024ULL)

/* 供调试器使用的有界最大值。此路径绝不调度或分配内存。
 * 阶段：1=元数据，2=准备映像，3=读取段，4=复制段，5=关闭 close-on-exec 描述符，
 * 6=释放旧地址空间，7=清理加载缓冲区。 */
typedef struct exec_load_delay {
    uint64_t pid, tid, stage, bytes, duration_ns;
    int64_t status;
} exec_load_delay_t;
static volatile uint64_t g_exec_load_timing_enabled;
static volatile exec_load_delay_t g_exec_load_delays[64];
static atomic_flag g_exec_load_delay_guard = ATOMIC_FLAG_INIT;

static void exec_load_record(uint64_t pid, uint64_t tid, uint64_t stage,
                              uint64_t bytes, int64_t status, uint64_t ticks) {
    if (tid == 0U || x86_boot_cpu_features.tsc_hz == 0U) return;
    uint64_t ns = x86_tsc_to_ns(ticks);
    if (ns < 2000000ULL || atomic_flag_test_and_set_explicit(
            &g_exec_load_delay_guard, memory_order_acquire)) return;
    for (size_t index = 0U; index < 64U; ++index) {
        volatile exec_load_delay_t *slot = &g_exec_load_delays[index];
        if (slot->tid == 0U ||
            (slot->pid == pid && slot->tid == tid && slot->stage == stage)) {
            if (ns > slot->duration_ns) {
                *slot = (exec_load_delay_t){pid, tid, stage, bytes, ns, status};
            }
            break;
        }
    }
    atomic_flag_clear_explicit(&g_exec_load_delay_guard, memory_order_release);
}

static uint64_t exec_load_start(void) {
    return g_exec_load_timing_enabled != 0U ? x86_read_tsc() : 0U;
}

static void exec_load_finish(uint64_t started, uint64_t stage, uint64_t bytes,
                              kstatus_t status) {
    if (started == 0U) return;
    uint64_t ticks = x86_read_tsc() - started;
    thread_t *thread = sched_current_thread();
    if (thread == 0 || thread->process == 0) return;
    exec_load_record(thread->process->pid, thread->tid, stage, bytes, status, ticks);
}

/* 仅在失败时记录 PROCESS_EXEC 进度快照。 */
static atomic_uint g_exec_debug_stage;
static atomic_uint g_exec_debug_status;
static atomic_uint g_exec_debug_thread;
#if OS_DEBUG_SERIAL
static uint32_t g_exec_debug_serial_logs;

static bool exec_debug_serial_allow(void) {
    uint32_t observed = __atomic_load_n(&g_exec_debug_serial_logs,
                                       __ATOMIC_RELAXED);
    while (observed < 128U) {
        if (__atomic_compare_exchange_n(&g_exec_debug_serial_logs, &observed,
                observed + 1U, true, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {
            return true;
        }
    }
    return false;
}
#endif

void process_exec_debug_mark(uint32_t stage) {
    thread_t *current = sched_current_thread();
    uint32_t target = atomic_load_explicit(&g_exec_debug_thread,
                                           memory_order_acquire);
    if (current == 0 || target == 0 || (uint32_t)current->tid != target) return;
    atomic_store_explicit(&g_exec_debug_stage, stage, memory_order_release);
#if OS_DEBUG_SERIAL
    if (exec_debug_serial_allow()) {
        kernel_serial_printf_serial_only(
            "OS_EXEC_PROGRESS tid=%llu stage=%x\r\n",
            (unsigned long long)current->tid, stage);
    }
#endif
}

static void exec_debug_progress(uint32_t stage) {
    process_exec_debug_mark(stage);
}

static void exec_debug_result(kstatus_t status) {
    atomic_store_explicit(&g_exec_debug_status, (uint32_t)status,
                          memory_order_release);
}

uint32_t process_exec_debug_stage(void) {
    return atomic_load_explicit(&g_exec_debug_stage, memory_order_acquire);
}

uint32_t process_exec_debug_status(void) {
    return atomic_load_explicit(&g_exec_debug_status, memory_order_acquire);
}

void process_exec_debug_reset(void) {
    atomic_store_explicit(&g_exec_debug_thread, 0U, memory_order_release);
    atomic_store_explicit(&g_exec_debug_stage, 0U, memory_order_release);
    atomic_store_explicit(&g_exec_debug_status, K_EBUSY, memory_order_release);
#if OS_DEBUG_SERIAL
    __atomic_store_n(&g_exec_debug_serial_logs, 0U, __ATOMIC_RELAXED);
#endif
}

typedef struct elf_exec_arguments {
    uint32_t count;
    char values[ELF_EXEC_ARG_MAX][ELF_EXEC_ARG_LENGTH];
    uint32_t environment_count;
    char environment[ELF_EXEC_ENV_MAX][ELF_EXEC_ARG_LENGTH];
} elf_exec_arguments_t;

/*
 * exec 诊断有意仅用于失败路径，以免改变成功运行时的计时结果。
 */
static void exec_diag_status(const char *tag, kstatus_t status) {
    kernel_serial_write("OS_DIAG_EXEC_");
    kernel_serial_write(tag);
    kernel_serial_write(" STATUS=");
    kernel_serial_write_u32((uint32_t)status);
    kernel_serial_write("\r\n");
}

static void exec_diag_prepare(kstatus_t status, uint32_t stage) {
    kernel_serial_write("OS_DIAG_EXEC_PREPARE_FAIL STATUS=");
    kernel_serial_write_u32((uint32_t)status);
    kernel_serial_write(" SUBSTAGE=");
    kernel_serial_write_u32(stage);
    kernel_serial_write("\r\n");
}

static kstatus_t exec_copy_user_path(const char __user *user_path,
                                     char output[EXEC_PATH_CAPACITY]) {
    if (user_path == 0 || output == 0) return K_EINVAL;
    for (size_t index = 0U; index < EXEC_PATH_CAPACITY; ++index) {
        kstatus_t status = copy_from_user(&output[index], user_path + index,
                                          sizeof(output[index]));
        if (status != K_OK) return status;
        if (output[index] == '\0') {
            return index == 0U ? K_ENOENT : K_OK;
        }
    }
    output[EXEC_PATH_CAPACITY - 1U] = '\0';
    return K_ENAMETOOLONG;
}

static bool exec_path_is_self_exe(const char *path) {
    static const char self_exe[] = "/proc/self/exe";
    size_t index = 0U;
    if (path == 0) return false;
    while (path[index] != '\0' && self_exe[index] != '\0' &&
           path[index] == self_exe[index]) {
        ++index;
    }
    return path[index] == '\0' && self_exe[index] == '\0';
}

static kstatus_t exec_resolve_path(process_t *process,
                                   const char __user *user_path,
                                   char output[EXEC_PATH_CAPACITY]) {
    kstatus_t status = exec_copy_user_path(user_path, output);
    if (status != K_OK) return status;
    if (!exec_path_is_self_exe(output))
        return vfs_resolve_user_path(user_path, true, false, output);

    process_thread_lock(process);
    __builtin_memcpy(output, process->linux_exe, EXEC_PATH_CAPACITY);
    process_thread_unlock(process);
    return output[0] != '\0' ? K_OK : K_ENOENT;
}

static void exec_store_linux_exe(process_t *process, const char *path) {
    size_t index = 0U;
    if (process == 0 || path == 0) return;
    for (; index + 1U < sizeof(process->linux_exe) && path[index] != '\0';
         ++index) {
        process->linux_exe[index] = path[index];
    }
    process->linux_exe[index] = '\0';
    for (++index; index < sizeof(process->linux_exe); ++index) {
        process->linux_exe[index] = '\0';
    }
}

static void exec_update_process_name(process_t *process, const char *path) {
    if (process != 0 && path != 0) process_set_name(process, path);
}

/*
 * 释放旧地址空间可能取消映射所有 VMA，并为每个页面执行 TLB shootdown。CR3 已切换到新空间后，
 * exec 系统调用不能继续占用旧进程的执行栈。后台工作线程使用内核根页表和普通可抢占上下文，
 * 因此拆除过程不会使新加载的用户线程陷入停滞。
 */
static void exec_release_old_space_work(void *argument) {
    vm_space_put((vm_space_t *)argument);
}

static void exec_release_old_space(vm_space_t *space) {
    if (space == 0) return;
    /* 绝不在 xHCI/输入下半部中销毁 VM。后台队列已满属于异常情况；应使用安全的同步回退路径
     * 保留资源所有权，而不是泄漏地址空间。 */
    if (!deferred_background_schedule(exec_release_old_space_work, space)) {
        vm_space_put(space);
    }
}

enum {
    AUX_NULL = 0,
    AUX_PHDR = 3,
    AUX_PHENT = 4,
    AUX_PHNUM = 5,
    AUX_PAGESZ = 6,
    AUX_BASE = 7,
    AUX_FLAGS = 8,
    AUX_ENTRY = 9,
    AUX_UID = 11,
    AUX_EUID = 12,
    AUX_GID = 13,
    AUX_EGID = 14,
    AUX_PLATFORM = 15,
    AUX_CLKTCK = 17,
    AUX_SECURE = 23,
    AUX_RANDOM = 25,
    AUX_EXECFN = 31,
};

static bool range_inside(uint64_t offset, uint64_t length, uint64_t total) {
    return offset <= total && length <= total - offset;
}

static bool power_of_two(uint64_t value) {
    return value != 0 && (value & (value - 1ULL)) == 0;
}

static bool align_up_page(uint64_t value, uint64_t *result) {
    if (value > UINT64_MAX - (PAGE_SIZE - 1ULL)) return false;
    *result = (value + PAGE_SIZE - 1ULL) & ~(PAGE_SIZE - 1ULL);
    return true;
}

static const elf64_program_header_t *program_header(const uint8_t *image,
                                                     const elf64_header_t *header,
                                                     uint16_t index) {
    return (const elf64_program_header_t *)(image + header->program_header_offset +
           (uint64_t)index * header->program_header_size);
}

static uint32_t vm_protection(uint32_t elf_flags) {
    uint32_t protection = VM_PROT_USER;
    if ((elf_flags & ELF_PF_READ) != 0) protection |= VM_PROT_READ;
    if ((elf_flags & ELF_PF_WRITE) != 0) protection |= VM_PROT_WRITE;
    if ((elf_flags & ELF_PF_EXEC) != 0) protection |= VM_PROT_EXEC;
    return protection;
}

static uint32_t fault_access(uint32_t protection) {
    if ((protection & VM_PROT_WRITE) != 0) return VM_PROT_WRITE;
    if ((protection & VM_PROT_READ) != 0) return VM_PROT_READ;
    return VM_PROT_EXEC;
}

static kstatus_t write_address_space(vm_space_t *space, uint64_t address,
                                     const void *source, size_t size) {
    const uint8_t *input = (const uint8_t *)source;
    while (size != 0) {
        uint64_t page_address = address & ~(PAGE_SIZE - 1ULL);
        uint64_t page_offset = address & (PAGE_SIZE - 1ULL);
        size_t chunk = PAGE_SIZE - (size_t)page_offset;
        if (chunk > size) chunk = size;
        paddr_t physical;
        kstatus_t status = x86_translate_page(space->root_table, (vaddr_t)page_address,
                                              &physical, 0);
        if (status != K_OK) {
            status = vm_handle_fault(space, &(vm_fault_info_t){
                (vaddr_t)page_address, VM_PROT_WRITE, 0});
            if (status == K_OK) {
                status = x86_translate_page(space->root_table,
                                            (vaddr_t)page_address,
                                            &physical, 0);
            }
        }
        if (status != K_OK) return status;
        uint8_t *destination = (uint8_t *)phys_to_direct(physical);
        if (destination == 0) return K_EIO;
        /* 已转换的页面范围限定了此次复制；使用内核整数批量操作，不触碰 FPU 状态，也不在此处
         * 调度。 */
        __builtin_memcpy(destination + page_offset, input, chunk);
        address += chunk;
        input += chunk;
        size -= chunk;
    }
    return K_OK;
}

static kstatus_t read_vfs_range(file_t *file, uint64_t offset,
                                void *buffer, size_t size);

static kstatus_t validate_elf(const uint8_t *image, size_t image_size,
                              uint64_t file_size, uint64_t requested_bias,
                              elf_validation_t *output) {
    if (output == 0) return K_EINVAL;
    if (image == 0 || image_size < sizeof(elf64_header_t) ||
        file_size < image_size) return K_EINVAL;
    *output = (elf_validation_t){0};
    const elf64_header_t *header = (const elf64_header_t *)image;
    if (header->ident[0] != 0x7FU || header->ident[1] != 'E' ||
        header->ident[2] != 'L' || header->ident[3] != 'F' ||
        header->ident[4] != ELF_CLASS_64 || header->ident[5] != ELF_DATA_LSB ||
        header->ident[6] != ELF_VERSION_CURRENT ||
        (header->type != ELF_TYPE_EXEC && header->type != ELF_TYPE_DYN) ||
        header->machine != ELF_MACHINE_X86_64 || header->version != ELF_VERSION_CURRENT ||
        header->header_size < sizeof(*header) ||
        header->program_header_size < sizeof(elf64_program_header_t) ||
        header->program_header_count == 0 ||
        header->program_header_count > ELF_MAX_PROGRAMS) return K_EINVAL;
    uint64_t table_size = (uint64_t)header->program_header_size *
                          header->program_header_count;
    if (!range_inside(header->program_header_offset, table_size, image_size)) return K_EINVAL;
    uint64_t bias = header->type == ELF_TYPE_DYN ? requested_bias : 0U;
    bool executable_entry = false;
    bool load_found = false;
    uint64_t phdr_address = 0;
    if (header->entry > UINT64_MAX - bias) return K_EINVAL;
    uint64_t entry = bias + header->entry;

    for (uint16_t i = 0; i < header->program_header_count; ++i) {
        const elf64_program_header_t *program = program_header(image, header, i);
        if (program->type == ELF_PT_INTERP) {
            if (output->has_interpreter || program->file_size == 0U ||
                program->file_size > ELF_INTERP_MAX ||
                !range_inside(program->offset, program->file_size, image_size)) {
                return K_EINVAL;
            }
            const uint8_t *path = image + program->offset;
            bool terminated = false;
            for (uint64_t byte = 0; byte < program->file_size; ++byte) {
                if (path[byte] == 0U) {
                    terminated = true;
                    output->interp_size = byte + 1U;
                    break;
                }
            }
            if (!terminated || path[0] != '/') return K_EINVAL;
            output->has_interpreter = true;
            output->interp_offset = program->offset;
            /* PT_INTERP 鐨勫瓧绗︿覆鏈韩涓嶄骇鐢熸槧灏勩€?*/
        }
        if (program->type == ELF_PT_DYNAMIC &&
            (program->memory_size < program->file_size ||
             !range_inside(program->offset, program->file_size, file_size) ||
             program->virtual_address > UINT64_MAX - bias ||
             bias + program->virtual_address > UINT64_MAX - program->memory_size)) {
            return K_EINVAL;
        }
        if (program->type == ELF_PT_DYNAMIC) {
            /* 鍔ㄦ€佹鍙敱鐢ㄦ埛鎬佽В閲婂櫒瑙ｆ瀽锛屽唴鏍镐笉鎵ц绗﹀彿閲嶅畾浣嶃€?*/
        }
        if (program->type == ELF_PT_INTERP) {
            /* 鍔ㄦ€侀摼鎺ュ櫒鐢辩敤鎴锋€佽杞斤紱鍐呮牳褰撳墠鍙帴鍙楁棤瑙ｉ噴鍣ㄧ殑闈欐€?ELF銆?*/
            /* 宸茶褰曡В閲婂櫒璺緞锛涙槧灏勫拰鍏ュ彛鍒囨崲鍦ㄨ杞介樁娈靛畬鎴愩€?*/
        }
        if (program->type == ELF_PT_PHDR) {
            if (program->virtual_address > UINT64_MAX - bias) return K_EINVAL;
            phdr_address = bias + program->virtual_address;
        }
        if (program->type != ELF_PT_LOAD) continue;
        if (program->memory_size < program->file_size ||
            program->memory_size > ELF_MAX_SEGMENT_SIZE ||
            !range_inside(program->offset, program->file_size, file_size) ||
            program->virtual_address > UINT64_MAX - bias ||
            bias + program->virtual_address > UINT64_MAX - program->memory_size ||
            (program->flags & ~(ELF_PF_READ | ELF_PF_WRITE | ELF_PF_EXEC)) != 0 ||
            (program->flags & (ELF_PF_WRITE | ELF_PF_EXEC)) ==
                (ELF_PF_WRITE | ELF_PF_EXEC)) return K_EINVAL;
        if (program->alignment > 1U &&
            (!power_of_two(program->alignment) ||
             (program->virtual_address & (program->alignment - 1ULL)) !=
             (program->offset & (program->alignment - 1ULL)))) return K_EINVAL;
        if (program->memory_size == 0) continue;
        uint64_t start = bias + program->virtual_address;
        uint64_t end = start + program->memory_size;
        if (start < 0x10000ULL || end > X86_64_USER_TOP + 1ULL || end <= start) return K_EINVAL;
        load_found = true;
        if (end > output->load_end) output->load_end = end;
        if ((program->flags & ELF_PF_EXEC) != 0 && entry >= start && entry < end) {
            executable_entry = true;
        }
        uint64_t phdr_delta = header->program_header_offset >= program->offset ?
                              header->program_header_offset - program->offset : UINT64_MAX;
        if (phdr_address == 0 && phdr_delta <= program->file_size &&
            table_size <= program->file_size - phdr_delta) {
            phdr_address = start + phdr_delta;
        }
    }
    if (!load_found || !executable_entry) return K_EINVAL;
    output->header = header;
    output->bias = bias;
    output->entry = entry;
    output->phdr_address = phdr_address;
    return K_OK;
}

static kstatus_t map_exec_range(vm_space_t *space, vnode_t *vnode,
                                uint64_t file_offset, vaddr_t address,
                                size_t size, uint32_t protection,
                                bool file_backed) {
    if (space == 0 || size == 0U ||
        (address & (PAGE_SIZE - 1ULL)) != 0U ||
        (size & (PAGE_SIZE - 1ULL)) != 0U ||
        (file_backed && (vnode == 0 ||
                         (file_offset & (PAGE_SIZE - 1ULL)) != 0U))) {
        return K_EINVAL;
    }
    uint64_t end = (uint64_t)address + (uint64_t)size;
    if (end <= (uint64_t)address) return K_EINVAL;

    /* 常见情况是映射一个全新的范围。 */
    bool range_mapped = vm_range_is_mapped(space, address, size);
#if OS_DEBUG_SERIAL
    if (exec_debug_serial_allow()) {
        kernel_serial_printf_serial_only(
            "OS_EXEC_MAP_RANGE address=%llx size=%llx mapped=%u\r\n",
            (unsigned long long)address, (unsigned long long)size,
            range_mapped ? 1U : 0U);
    }
#endif
    if (!range_mapped) {
        vm_object_t *object = 0;
#if OS_DEBUG_SERIAL
        if (file_backed && exec_debug_serial_allow()) {
            kernel_serial_printf_serial_only(
                "OS_EXEC_MAP_OBJECT_BEGIN offset=%llx size=%llx\r\n",
                (unsigned long long)file_offset,
                (unsigned long long)size);
        }
#endif
        kstatus_t status = file_backed ?
            vm_object_create_file(vnode, vfs_vm_file_ops(), vnode->size,
                                  file_offset, size, &object) : K_OK;
#if OS_DEBUG_SERIAL
        if (file_backed && exec_debug_serial_allow()) {
            kernel_serial_printf_serial_only(
                "OS_EXEC_MAP_OBJECT_DONE status=%d object=%p\r\n",
                (int)status, (void *)object);
        }
#endif
        if (status != K_OK) return status;
        vaddr_t mapped_address = address;
#if OS_DEBUG_SERIAL
        if (exec_debug_serial_allow()) {
            kernel_serial_printf_serial_only(
                "OS_EXEC_MAP_VMA_BEGIN address=%llx size=%llx\r\n",
                (unsigned long long)address, (unsigned long long)size);
        }
#endif
        status = vm_map_object(space, object, &mapped_address, 0U, size,
                               protection, VM_MAP_PRIVATE | VM_MAP_FIXED);
#if OS_DEBUG_SERIAL
        if (exec_debug_serial_allow()) {
            kernel_serial_printf_serial_only(
                "OS_EXEC_MAP_VMA_DONE status=%d address=%llx\r\n",
                (int)status, (unsigned long long)mapped_address);
        }
#endif
        vm_object_put(object);
        if (status == K_OK) return K_OK;
        if (status != K_EBUSY) return status;
    }

    /* 多个 PT_LOAD 可能共用最后一页。应在已映射区域处分割，并更新现有 VMA，而不是插入重叠项。 */
    uint64_t cursor = (uint64_t)address;
    while (cursor < end) {
        bool mapped = vm_range_is_mapped(space, (vaddr_t)cursor, PAGE_SIZE);
        uint64_t run_end = cursor + PAGE_SIZE;
        while (run_end < end &&
               vm_range_is_mapped(space, (vaddr_t)run_end, PAGE_SIZE) == mapped) {
            run_end += PAGE_SIZE;
        }
        size_t run_size = (size_t)(run_end - cursor);
        kstatus_t status;
        if (mapped) {
            status = vm_protect(space, (vaddr_t)cursor, run_size, protection);
        } else {
            vm_object_t *object = 0;
            uint64_t run_file_offset = file_offset + (cursor - address);
            if (file_backed) {
                if (run_file_offset < file_offset ||
                    run_file_offset > UINT64_MAX - run_size) return K_EOVERFLOW;
                status = vm_object_create_file(vnode, vfs_vm_file_ops(),
                                               vnode->size, run_file_offset,
                                               run_size, &object);
            } else {
                status = K_OK;
            }
            if (status == K_OK) {
                vaddr_t mapped_address = (vaddr_t)cursor;
                status = vm_map_object(space, object, &mapped_address, 0U,
                                       run_size, protection,
                                       VM_MAP_PRIVATE | VM_MAP_FIXED);
            }
            vm_object_put(object);
        }
        if (status != K_OK) return status;
        cursor = run_end;
    }
    return K_OK;
}

static kstatus_t map_load_segment_from_vfs(
    vm_space_t *space, const elf64_program_header_t *program, uint64_t bias,
    file_t *source_file) {
    if (space == 0 || program == 0 || source_file == 0 ||
        source_file->vnode == 0 || program->type != ELF_PT_LOAD ||
        program->memory_size == 0U) return K_EINVAL;
    uint64_t virtual_start = bias + program->virtual_address;
    uint64_t memory_end;
    if (virtual_start < bias ||
        program->memory_size > UINT64_MAX - virtual_start ||
        !align_up_page(virtual_start + program->memory_size, &memory_end)) {
        return K_EINVAL;
    }
    uint64_t mapping_start = virtual_start & ~(PAGE_SIZE - 1ULL);
    uint64_t file_offset = program->offset & ~(PAGE_SIZE - 1ULL);
    uint32_t protection = vm_protection(program->flags);
    uint64_t data_end = virtual_start + program->file_size;
    uint64_t file_mapping_end = data_end & ~(PAGE_SIZE - 1ULL);
    if (file_mapping_end < mapping_start) file_mapping_end = mapping_start;
    if (file_mapping_end > memory_end) file_mapping_end = memory_end;

    kstatus_t status = K_OK;

    /* Linux 可执行文件的可写部分在 exec 后为私有映射。将其保留为匿名区域并以大块请求流式
     * 写入：若映射为 FAT 后备文件，动态重定位器的每次写入都会触发单独的页缓存/COW 操作。
     * PT_LOAD 边界的共享页仍由 map_exec_range 处理，并在复制段数据前保留其中已有字节。 */
    if ((program->flags & ELF_PF_WRITE) != 0U) {
        status = map_exec_range(space, 0, 0U, (vaddr_t)mapping_start,
                                (size_t)(memory_end - mapping_start),
                                protection, false);
        if (status != K_OK) return status;
        if (program->file_size != 0U) {
            if (program->offset > (uint64_t)INT64_MAX) return K_EINVAL;
            /* 小段按实际长度分配；大块中转分配失败时缩小批次，不改变最终段内容。
             * 最小重试批次为一页，但本来不足一页的段无需扩大。 */
            size_t capacity = program->file_size > ELF_EXEC_LOAD_CHUNK_SIZE ?
                              (size_t)ELF_EXEC_LOAD_CHUNK_SIZE : (size_t)program->file_size;
            uint8_t *chunk;
            while ((chunk = (uint8_t *)kmalloc(capacity, 0U)) == 0) {
                if (capacity <= PAGE_SIZE) return K_ENOMEM;
                capacity /= 2U;
                if (capacity < PAGE_SIZE) capacity = PAGE_SIZE;
            }
            uint64_t position = 0U;
            status = vfs_seek(source_file, (int64_t)program->offset,
                              OS_FILE_SEEK_SET, &position);
            if (status == K_OK) {
                uint64_t copied = 0U;
                while (copied < program->file_size) {
                    uint64_t remaining = program->file_size - copied;
                    size_t request = remaining > capacity ?
                                     capacity :
                                     (size_t)remaining;
                    uint64_t bytes = 0U;
#if OS_DEBUG_SERIAL
                    if (exec_debug_serial_allow()) {
                        kernel_serial_printf_serial_only(
                            "OS_EXEC_COPY_BEGIN offset=%llx copied=%llx size=%llx\r\n",
                            (unsigned long long)program->offset,
                            (unsigned long long)copied,
                            (unsigned long long)request);
                    }
#endif
                    uint64_t read_started = exec_load_start();
                    status = vfs_read_cached_kernel(source_file, chunk, request,
                                             &bytes);
                    exec_load_finish(read_started, 3U, bytes, status);
                    if (status != K_OK || bytes == 0U || bytes > request) {
                        if (status == K_OK) status = K_EIO;
#if OS_DEBUG_SERIAL
                        if (exec_debug_serial_allow()) {
                            kernel_serial_printf_serial_only(
                                "OS_EXEC_COPY_READ_FAIL copied=%llx status=%d bytes=%llx\r\n",
                                (unsigned long long)copied, (int)status,
                                (unsigned long long)bytes);
                        }
#endif
                        break;
                    }
                    uint64_t copy_started = exec_load_start();
                    status = write_address_space(
                        space, virtual_start + copied, chunk, (size_t)bytes);
                    exec_load_finish(copy_started, 4U, bytes, status);
#if OS_DEBUG_SERIAL
                    if (exec_debug_serial_allow()) {
                        kernel_serial_printf_serial_only(
                            "OS_EXEC_COPY_DONE copied=%llx bytes=%llx status=%d\r\n",
                            (unsigned long long)copied,
                            (unsigned long long)bytes, (int)status);
                    }
#endif
                    if (status != K_OK) break;
                    copied += bytes;
                    if (bytes < request && copied < program->file_size) {
                        status = K_EIO;
                        break;
                    }
                }
            }
            kfree(chunk);
            if (status != K_OK) return status;
        }

        /* 如果 p_filesz 在页面中间结束，则清零该共享边界页的剩余部分。后续 BSS 页面由
         * anon_page 清零。 */
        if ((data_end & (PAGE_SIZE - 1ULL)) != 0U && data_end < memory_end) {
            uint64_t zero_end = (data_end + PAGE_SIZE - 1ULL) &
                                ~(PAGE_SIZE - 1ULL);
            if (zero_end > memory_end) zero_end = memory_end;
            uint8_t zeros[PAGE_SIZE] = {0};
            status = write_address_space(space, data_end, zeros,
                                         (size_t)(zero_end - data_end));
            if (status != K_OK) return status;
        }
        return K_OK;
    }

    if (file_mapping_end > mapping_start) {
        status = map_exec_range(space, source_file->vnode, file_offset,
                                (vaddr_t)mapping_start,
                                (size_t)(file_mapping_end - mapping_start),
                                protection, true);
    }
    if (status == K_OK && memory_end > file_mapping_end) {
        status = map_exec_range(space, 0, 0U, (vaddr_t)file_mapping_end,
                                (size_t)(memory_end - file_mapping_end),
                                protection, false);
    }
    if (status != K_OK) return status;

    if (data_end > file_mapping_end) {
        /* 最后一个不完整文件页使用匿名映射，以确保 p_filesz 之后的字节为零。它最多只有一页，
         * 因为完整页面均由文件后备。 */
        uint64_t copy_start = virtual_start > file_mapping_end ?
                              virtual_start : file_mapping_end;
        uint64_t copy_size = data_end - copy_start;
        if (copy_size > PAGE_SIZE ||
            program->offset > UINT64_MAX - (copy_start - virtual_start)) {
            return K_EOVERFLOW;
        }
        uint8_t tail[PAGE_SIZE] = {0};
        status = read_vfs_range(
            source_file, program->offset + (copy_start - virtual_start),
            tail, (size_t)copy_size);
        if (status != K_OK) return status;
        paddr_t tail_physical;
        uint64_t tail_page = copy_start & ~(PAGE_SIZE - 1ULL);
        if (x86_translate_page(space->root_table, (vaddr_t)tail_page,
                               &tail_physical, 0) != K_OK) {
            status = vm_handle_fault(space, &(vm_fault_info_t){
                (vaddr_t)tail_page, fault_access(protection), 0});
            if (status != K_OK) return status;
        }
        status = write_address_space(space, copy_start, tail,
                                     (size_t)copy_size);
        if (status != K_OK) return status;
    }

    return K_OK;
}

static kstatus_t map_load_segment(vm_space_t *space, const uint8_t *image,
                                  const elf64_program_header_t *program,
                                  uint64_t bias, file_t *source_file) {
    if (program->type != ELF_PT_LOAD || program->memory_size == 0) return K_OK;
    if (source_file != 0) {
        return map_load_segment_from_vfs(space, program, bias, source_file);
    }
    exec_debug_progress(0x120U);
    uint64_t virtual_start = bias + program->virtual_address;
    uint64_t mapping_start = virtual_start & ~(PAGE_SIZE - 1ULL);
    uint64_t mapping_end;
    if (!align_up_page(virtual_start + program->memory_size, &mapping_end)) return K_EINVAL;
    size_t mapping_size = (size_t)(mapping_end - mapping_start);
    vm_object_t *object = 0;
    kstatus_t status = vm_object_create_anon(mapping_size, &object);
    if (status != K_OK) {
        exec_debug_result(status);
        return status;
    }
    vaddr_t address = (vaddr_t)mapping_start;
    uint32_t protection = vm_protection(program->flags);
    exec_debug_progress(0x121U);
    status = vm_map_object(space, object, &address, 0, mapping_size, protection,
                           VM_MAP_PRIVATE | VM_MAP_FIXED);
    vm_object_put(object);
    if (status != K_OK) {
        exec_debug_result(status);
        return status;
    }
    uint32_t access = fault_access(protection);
    for (uint64_t page = mapping_start; page < mapping_end; page += PAGE_SIZE) {
        exec_debug_progress(0x122U |
                            (uint32_t)((page - mapping_start) >> PAGE_SHIFT));
        status = vm_handle_fault(space, &(vm_fault_info_t){(vaddr_t)page, access, 0});
        if (status != K_OK) {
            exec_debug_result(status);
            return status;
        }
    }
    exec_debug_progress(0x123U);
    if (program->file_size == 0U) return K_OK;
    return write_address_space(space, virtual_start, image + program->offset,
                               (size_t)program->file_size);
}

static kstatus_t map_elf_segments(vm_space_t *space, const uint8_t *image,
                                  const elf_validation_t *validation,
                                  file_t *source_file) {
    if (space == 0 || image == 0 || validation == 0 || validation->header == 0) {
        return K_EINVAL;
    }
    for (uint16_t i = 0; i < validation->header->program_header_count; ++i) {
        const elf64_program_header_t *program =
            program_header(image, validation->header, i);
        if (program->type != ELF_PT_LOAD || program->memory_size == 0U) {
            continue;
        }
#if OS_DEBUG_SERIAL
        if (exec_debug_serial_allow()) {
            kernel_serial_printf_serial_only(
                "OS_EXEC_SEGMENT_BEGIN index=%u flags=%u file=%llx memory=%llx\r\n",
                (unsigned int)i, (unsigned int)program->flags,
                (unsigned long long)program->file_size,
                (unsigned long long)program->memory_size);
        }
#endif
        kstatus_t status = map_load_segment(space, image, program,
                                            validation->bias, source_file);
#if OS_DEBUG_SERIAL
        if (exec_debug_serial_allow()) {
            if (status == K_OK) {
                kernel_serial_printf_serial_only(
                    "OS_EXEC_SEGMENT_DONE index=%u\r\n", (unsigned int)i);
            } else {
                kernel_serial_printf_serial_only(
                    "OS_EXEC_SEGMENT_FAIL index=%u status=%d\r\n",
                    (unsigned int)i, (int)status);
            }
        }
#endif
        if (status != K_OK) return status;
    }
    return K_OK;
}

static kstatus_t copy_exec_vector(
    const char __user *const __user *vector, uint32_t maximum,
    char values[][ELF_EXEC_ARG_LENGTH], uint32_t *count) {
    if (count == 0) return K_EINVAL;
    *count = 0U;
    if (vector == 0) return K_OK;
    for (uint32_t index = 0U; index < maximum; ++index) {
        const char __user *value_pointer = 0;
        kstatus_t status = copy_from_user(&value_pointer, vector + index,
                                          sizeof(value_pointer));
        if (status != K_OK) return status;
        if (value_pointer == 0) {
            *count = index;
            return K_OK;
        }
        bool terminated = false;
        for (uint32_t character = 0U; character < ELF_EXEC_ARG_LENGTH; ++character) {
            char value = 0;
            status = copy_from_user(&value, value_pointer + character,
                                    sizeof(value));
            if (status != K_OK) return status;
            values[index][character] = value;
            if (value == '\0') {
                terminated = true;
                break;
            }
            if (character + 1U == ELF_EXEC_ARG_LENGTH) return K_EINVAL;
        }
        if (!terminated) return K_EINVAL;
    }
    return K_EINVAL;
}

static kstatus_t copy_exec_arguments(
    const char __user *const __user *argv,
    const char __user *const __user *envp,
    elf_exec_arguments_t *arguments) {
    kstatus_t status;
    if (arguments == 0) return K_EINVAL;
    status = copy_exec_vector(argv, ELF_EXEC_ARG_MAX, arguments->values,
                              &arguments->count);
    if (status != K_OK) return status;
    return copy_exec_vector(envp, ELF_EXEC_ENV_MAX, arguments->environment,
                            &arguments->environment_count);
}

static kstatus_t build_initial_stack(vm_space_t *space,
                                     const user_elf_image_info_t *info,
                                     const elf_exec_arguments_t *arguments,
                                     const os_exec_fd_map_t *descriptor_map,
                                     vaddr_t *stack_pointer) {
    vaddr_t guard_address = OS_LINUX_USER_STACK_BASE - PAGE_SIZE;
    vaddr_t stack_address = OS_LINUX_USER_STACK_BASE;
    kstatus_t status = vm_map_object(space, 0, &guard_address, 0, PAGE_SIZE, 0,
                                     VM_MAP_PRIVATE | VM_MAP_FIXED | VM_MAP_GUARD);
    if (status != K_OK) return status;
    status = vm_map_object(space, 0, &stack_address, 0,
                           OS_LINUX_USER_STACK_SIZE,
                           VM_PROT_READ | VM_PROT_WRITE | VM_PROT_USER,
                           VM_MAP_PRIVATE | VM_MAP_FIXED | VM_MAP_STACK);
    if (status != K_OK) return status;
    status = vm_handle_fault(space, &(vm_fault_info_t){
                                                       OS_LINUX_USER_STACK_TOP - 1U,
                                                       VM_PROT_WRITE, 0});
    if (status != K_OK) return status;

    uint8_t random_bytes[16];
    uint32_t seed_low;
    uint32_t seed_high;
    __asm__ volatile ("rdtsc" : "=a"(seed_low), "=d"(seed_high));
    uint64_t seed = ((uint64_t)seed_high << 32) | seed_low;
    for (uint32_t i = 0; i < sizeof(random_bytes); ++i) {
        seed ^= seed << 13;
        seed ^= seed >> 7;
        seed ^= seed << 17;
        random_bytes[i] = (uint8_t)seed;
    }
    if (arguments == 0 || arguments->count > ELF_EXEC_ARG_MAX ||
        arguments->environment_count > ELF_EXEC_ENV_MAX) return K_EINVAL;
    vaddr_t random_address = OS_LINUX_USER_STACK_TOP -
                             sizeof(random_bytes);
    status = write_address_space(space, random_address, random_bytes, sizeof(random_bytes));
    if (status != K_OK) return status;

    uint64_t argv_values[ELF_EXEC_ARG_MAX + 1U];
    uint64_t environment_values[ELF_EXEC_ENV_MAX + 1U];
    for (uint32_t index = 0U; index < ELF_EXEC_ARG_MAX + 1U; ++index) {
        argv_values[index] = 0U;
    }
    for (uint32_t index = 0U; index < ELF_EXEC_ENV_MAX + 1U; ++index) {
        environment_values[index] = 0U;
    }
    vaddr_t string_pointer = random_address;
    for (uint32_t index = arguments->count; index != 0U; --index) {
        const char *argument = arguments->values[index - 1U];
        size_t length = 0U;
        while (length < ELF_EXEC_ARG_LENGTH && argument[length] != '\0') ++length;
        if (length >= ELF_EXEC_ARG_LENGTH) return K_EINVAL;
        string_pointer -= length + 1U;
        status = write_address_space(space, string_pointer, argument, length + 1U);
        if (status != K_OK) return status;
        argv_values[index - 1U] = string_pointer;
    }
    for (uint32_t index = arguments->environment_count; index != 0U; --index) {
        const char *environment = arguments->environment[index - 1U];
        size_t length = 0U;
        while (length < ELF_EXEC_ARG_LENGTH && environment[length] != '\0') {
            ++length;
        }
        if (length >= ELF_EXEC_ARG_LENGTH) return K_EINVAL;
        string_pointer -= length + 1U;
        status = write_address_space(space, string_pointer, environment,
                                     length + 1U);
        if (status != K_OK) return status;
        environment_values[index - 1U] = string_pointer;
    }

    vaddr_t execfn_address = 0U;
    vaddr_t platform_address = 0U;
    if (info->linux_abi) {
        static const char platform[] = LINUX_ARCH_ELF_PLATFORM;
        const char *execfn = arguments->count != 0U ? arguments->values[0] : "";
        size_t platform_length = sizeof(platform);
        size_t execfn_length = 0U;
        while (execfn_length < ELF_EXEC_ARG_LENGTH &&
               execfn[execfn_length] != '\0') ++execfn_length;
        if (execfn_length >= ELF_EXEC_ARG_LENGTH) return K_EINVAL;
        string_pointer -= platform_length;
        platform_address = string_pointer;
        status = write_address_space(space, platform_address, platform,
                                     platform_length);
        if (status != K_OK) return status;
        string_pointer -= execfn_length + 1U;
        execfn_address = string_pointer;
        status = write_address_space(space, execfn_address, execfn,
                                     execfn_length + 1U);
        if (status != K_OK) return status;
    }

    vaddr_t descriptor_map_address = 0U;
    if (descriptor_map != 0 && descriptor_map->count != 0U) {
        size_t map_size = sizeof(*descriptor_map);
        string_pointer -= map_size;
        descriptor_map_address = string_pointer;
        status = write_address_space(space, descriptor_map_address,
                                     descriptor_map, map_size);
        if (status != K_OK) return status;
    }

    /* Linux 在此处写入 17 对 auxv，包括 AUX_NULL。预留两个额外槽位，避免终止项覆盖 argv_values。 */
    uint64_t auxiliary[ELF_EXEC_AUX_MAX];
    size_t auxiliary_count = 0U;
#define LINUX_AUX_PUSH(tag, value) do { \
        auxiliary[auxiliary_count++] = (tag); \
        auxiliary[auxiliary_count++] = (value); \
    } while (0)
    LINUX_AUX_PUSH(AUX_PHDR, info->program_headers);
    LINUX_AUX_PUSH(AUX_PHENT, info->program_header_size);
    LINUX_AUX_PUSH(AUX_PHNUM, info->program_header_count);
    LINUX_AUX_PUSH(AUX_PAGESZ, PAGE_SIZE);
    LINUX_AUX_PUSH(AUX_BASE, info->interpreter_base);
    LINUX_AUX_PUSH(AUX_ENTRY, info->main_entry);
    LINUX_AUX_PUSH(AUX_RANDOM, random_address);
    if (info->linux_abi) {
        LINUX_AUX_PUSH(AUX_FLAGS, 0U);
        LINUX_AUX_PUSH(AUX_UID, 0U);
        LINUX_AUX_PUSH(AUX_EUID, 0U);
        LINUX_AUX_PUSH(AUX_GID, 0U);
        LINUX_AUX_PUSH(AUX_EGID, 0U);
        LINUX_AUX_PUSH(AUX_PLATFORM, platform_address);
        LINUX_AUX_PUSH(AUX_CLKTCK, 100U);
        LINUX_AUX_PUSH(AUX_SECURE, 0U);
        LINUX_AUX_PUSH(AUX_EXECFN, execfn_address);
    }
    /* Linux 子进程也需要 System 描述符映射：Chromium 的直接子进程启动器会在 exec 前重新映射
     * 共享文件，libc 则在启动时从此私有 auxv 项导入这些原生句柄。 */
    if (descriptor_map_address != 0U) {
        LINUX_AUX_PUSH(OS_AUX_FD_MAP, descriptor_map_address);
    }
    LINUX_AUX_PUSH(AUX_NULL, 0U);
#undef LINUX_AUX_PUSH
    size_t vector_size = sizeof(uint64_t) *
                         (arguments->environment_count + arguments->count + 3U);
    size_t stack_data_size = auxiliary_count * sizeof(uint64_t) + vector_size;
    vaddr_t stack_top = string_pointer & ~15ULL;
    size_t padding = (size_t)((stack_top - stack_data_size) & 15ULL);
    vaddr_t sp = stack_top - padding;
    /* argc、argv、envp 和 auxv 的总长度随参数数量变化；参数数量为偶数时补齐 8 字节，
     * 使新程序入口仍满足 x86-64 初始栈的 16 字节对齐要求。 */
    sp -= auxiliary_count * sizeof(uint64_t);
    status = write_address_space(space, sp, auxiliary,
                                 auxiliary_count * sizeof(uint64_t));
    if (status != K_OK) return status;
    sp -= sizeof(uint64_t) * (arguments->environment_count + 1U);
    if (write_address_space(space, sp, environment_values,
                            sizeof(uint64_t) *
                            (arguments->environment_count + 1U)) != K_OK) {
        return K_EIO;
    }
    sp -= sizeof(uint64_t) * (arguments->count + 1U);
#if OS_DEBUG_SERIAL
    kernel_serial_printf_serial_only(
        "OS_EXEC_STACK argc=%u sp=%llx argv0=%llx argv1=%llx\r\n",
        arguments->count, (unsigned long long)sp,
        (unsigned long long)argv_values[0],
        (unsigned long long)(arguments->count > 1U ? argv_values[1] : 0U));
#endif
    if (write_address_space(space, sp, argv_values,
                            sizeof(uint64_t) * (arguments->count + 1U)) != K_OK) {
        return K_EIO;
    }
    uint64_t argc = arguments->count;
    sp -= sizeof(uint64_t); /* 参数个数 */
    if (write_address_space(space, sp, &argc, sizeof(argc)) != K_OK) return K_EIO;
    if ((sp & 15U) != 0) return K_EIO;
    *stack_pointer = sp;
    return K_OK;
}

static kstatus_t read_vfs_range(file_t *file, uint64_t offset,
                                void *buffer, size_t size) {
    if (file == 0 || file->vnode == 0 || buffer == 0 || size == 0U ||
        offset > (uint64_t)INT64_MAX || offset > file->vnode->size ||
        (uint64_t)size > file->vnode->size - offset) {
        return K_EINVAL;
    }
    uint64_t position = 0U;
    kstatus_t status = vfs_seek(file, (int64_t)offset, OS_FILE_SEEK_SET,
                                &position);
    if (status != K_OK) return status;
    uint64_t bytes = 0U;
    status = vfs_read_kernel(file, buffer, size, &bytes);
    if (status != K_OK) return status;
    return bytes == size ? K_OK : K_EIO;
}

/* 仅读取 ELF 头和程序头前缀。大型 VFS 可执行文件会在下方逐个 PT_LOAD 加载；若在此处保留
 * 完整文件，就需要一次性分配与 Chrome 本身一样大的连续内核内存。 */
static kstatus_t read_vfs_metadata(file_t *file, uint8_t **image_out,
                                   size_t *size_out) {
    elf64_header_t header_copy;
    uint64_t file_size;
    uint64_t table_size;
    uint64_t metadata_size;
    uint8_t *image;
    kstatus_t status;
    if (file == 0 || file->vnode == 0 || image_out == 0 || size_out == 0 ||
        file->vnode->size < sizeof(elf64_header_t) ||
        file->vnode->size > (uint64_t)SIZE_MAX) return K_EINVAL;
    *image_out = 0;
    *size_out = 0;
    file_size = file->vnode->size;
    status = read_vfs_range(file, 0U, &header_copy, sizeof(header_copy));
    if (status != K_OK) return status;
    if (header_copy.program_header_size < sizeof(elf64_program_header_t) ||
        header_copy.program_header_count == 0U ||
        header_copy.program_header_count > ELF_MAX_PROGRAMS ||
        header_copy.program_header_offset > file_size) {
        return K_EINVAL;
    }
    table_size = (uint64_t)header_copy.program_header_size *
                 header_copy.program_header_count;
    if (table_size > file_size - header_copy.program_header_offset) {
        return K_EINVAL;
    }
    metadata_size = header_copy.program_header_offset + table_size;
    if (metadata_size < sizeof(header_copy)) metadata_size = sizeof(header_copy);
    if (metadata_size > ELF_EXEC_METADATA_SIZE ||
        metadata_size > (uint64_t)SIZE_MAX) {
        return K_EINVAL;
    }
    image = (uint8_t *)kmalloc((size_t)metadata_size, 0U);
    if (image == 0) return K_ENOMEM;
    /* 保留已验证的头快照，只读取尚未获取的前缀，避免重复 I/O 及重读头导致的尺寸失配。 */
    __builtin_memcpy(image, &header_copy, sizeof(header_copy));
    status = metadata_size > sizeof(header_copy) ?
        read_vfs_range(file, sizeof(header_copy), image + sizeof(header_copy),
                       (size_t)metadata_size - sizeof(header_copy)) : K_OK;
    if (status != K_OK) {
        kfree(image);
        return status;
    }

    const elf64_header_t *header = (const elf64_header_t *)image;
    uint64_t required_size = metadata_size;
    for (uint16_t index = 0U; index < header->program_header_count; ++index) {
        const elf64_program_header_t *program =
            program_header(image, header, index);
        if (program->type != ELF_PT_INTERP) continue;
        if (program->file_size == 0U ||
            program->file_size > ELF_INTERP_MAX ||
            program->offset > file_size ||
            program->file_size > file_size - program->offset) {
            kfree(image);
            return K_EINVAL;
        }
        uint64_t interpreter_end = program->offset + program->file_size;
        if (interpreter_end > ELF_EXEC_METADATA_SIZE ||
            interpreter_end > (uint64_t)SIZE_MAX) {
            kfree(image);
            return K_EINVAL;
        }
        if (interpreter_end > required_size) required_size = interpreter_end;
    }
    /* 所有解释器描述完成校验后最多扩容一次；已读取前缀直接复制，设备只读取新增后缀。 */
    if (required_size > metadata_size) {
        uint8_t *extended_image = (uint8_t *)kmalloc(
            (size_t)required_size, 0U);
        if (extended_image == 0) {
            kfree(image);
            return K_ENOMEM;
        }
        __builtin_memcpy(extended_image, image, (size_t)metadata_size);
        status = read_vfs_range(file, metadata_size, extended_image + metadata_size,
                                (size_t)(required_size - metadata_size));
        if (status != K_OK) {
            kfree(extended_image);
            kfree(image);
            return status;
        }
        kfree(image);
        image = extended_image;
        metadata_size = required_size;
    }
    *image_out = image;
    *size_out = (size_t)metadata_size;
    return K_OK;
}

/* VFS exec 会立即读取解释器映像，因为它体积较小，且准备初始地址空间时就需要使用。 */
static kstatus_t read_vfs_image(const char *path, uint8_t **image_out,
                                size_t *size_out) {
    if (path == 0 || image_out == 0 || size_out == 0) return K_EINVAL;
    *image_out = 0;
    *size_out = 0;
    file_t *file = 0;
    kstatus_t status = vfs_open_kernel(path, VFS_OPEN_READ, 0U, &file);
    if (status != K_OK) return status;
    if (file->vnode == 0 || file->vnode->size == 0U ||
        file->vnode->size > (uint64_t)ELF_MAX_SEGMENT_SIZE * 4ULL ||
        file->vnode->size > (uint64_t)SIZE_MAX) {
        vfs_close(file);
        return K_EINVAL;
    }
    size_t size = (size_t)file->vnode->size;
    uint8_t *image = (uint8_t *)kmalloc(size, 0);
    if (image == 0) {
        vfs_close(file);
        return K_ENOMEM;
    }
    uint64_t bytes = 0;
    status = vfs_read_kernel(file, image, size, &bytes);
    vfs_close(file);
    if (status != K_OK || bytes != size) {
        kfree(image);
        return status == K_OK ? K_EIO : status;
    }
    *image_out = image;
    *size_out = size;
    return K_OK;
}

static kstatus_t prepare_elf_space(const void *raw_image, size_t image_size,
                                   file_t *main_file, uint64_t main_file_size,
                                   vm_space_t **space_out, user_elf_image_info_t *info,
                                   const elf_exec_arguments_t *arguments,
                                   const os_exec_fd_map_t *descriptor_map,
                                   vaddr_t *stack_pointer,
                                   uint32_t *diag_stage) {
    const uint8_t *image = (const uint8_t *)raw_image;
    elf_validation_t main_validation;
    elf_validation_t interpreter_validation;
    uint8_t *interpreter_image = 0;
    size_t interpreter_size = 0;
    bool linux_abi = false;
    uint64_t brk_base = 0U;
    if (info == 0) return K_EINVAL;
    *info = (user_elf_image_info_t){0};
    exec_debug_progress(0x101U);
    if (diag_stage != 0) *diag_stage = 1U; /* 验证主 ELF */
    kstatus_t status = validate_elf(image, image_size, main_file_size,
                                    ELF_PIE_BIAS,
                                    &main_validation);
    if (status != K_OK) return status;

    if (main_validation.has_interpreter) {
        char interpreter_path[ELF_INTERP_MAX];
        if (main_validation.interp_size == 0U ||
            main_validation.interp_size > sizeof(interpreter_path)) return K_EINVAL;
        for (uint64_t i = 0; i < main_validation.interp_size; ++i) {
            interpreter_path[i] = (char)image[main_validation.interp_offset + i];
        }
        linux_abi = linux_elf_interpreter_path(interpreter_path);
        if (diag_stage != 0) *diag_stage = 2U; /* 读取 PT_INTERP */
        exec_debug_progress(0x102U);
        status = read_vfs_image(interpreter_path, &interpreter_image,
                                &interpreter_size);
        if (status != K_OK) return status;
        if (diag_stage != 0) *diag_stage = 3U; /* 验证 PT_INTERP */
        exec_debug_progress(0x103U);
        status = validate_elf(interpreter_image, interpreter_size,
                              interpreter_size, ELF_INTERP_BIAS,
                              &interpreter_validation);
        if (status != K_OK || interpreter_validation.has_interpreter) {
            kfree(interpreter_image);
            return status == K_OK ? K_EINVAL : status;
        }
    }

    vm_space_t *space = 0;
    if (diag_stage != 0) *diag_stage = 4U; /* 创建新的 vm_space */
    exec_debug_progress(0x104U);
    status = vm_space_create(&space);
    if (status != K_OK) {
        kfree(interpreter_image);
        return status;
    }
    if (diag_stage != 0) *diag_stage = 5U; /* 映射主映像 PT_LOAD 段 */
    exec_debug_progress(0x105U);
    status = map_elf_segments(space, image, &main_validation, main_file);
    if (status == K_OK && interpreter_image != 0) {
        if (diag_stage != 0) *diag_stage = 6U; /* 映射解释器 PT_LOAD 段 */
        exec_debug_progress(0x106U);
        status = map_elf_segments(space, interpreter_image, &interpreter_validation,
                                  0);
    }
    if (status != K_OK) {
        vm_space_put(space);
        kfree(interpreter_image);
        return status;
    }
    info->main_entry = (vaddr_t)main_validation.entry;
    info->entry = interpreter_image != 0 ?
                  (vaddr_t)interpreter_validation.entry : info->main_entry;
    info->interpreter_base = interpreter_image != 0 ?
                             (vaddr_t)interpreter_validation.bias : 0U;
    info->program_headers = (vaddr_t)main_validation.phdr_address;
    info->program_header_size = main_validation.header->program_header_size;
    info->program_header_count = main_validation.header->program_header_count;
    info->load_bias = (vaddr_t)main_validation.bias;
    if (!align_up_page(main_validation.load_end, &brk_base)) {
        vm_space_put(space);
        kfree(interpreter_image);
        return K_EOVERFLOW;
    }
    info->brk_base = (vaddr_t)brk_base;
    info->linux_abi = linux_abi;
    if (diag_stage != 0) *diag_stage = 7U; /* 构建初始用户栈 */
    exec_debug_progress(0x107U);
    status = build_initial_stack(space, info, arguments, descriptor_map,
                                 stack_pointer);
    if (status != K_OK) {
        vm_space_put(space);
        kfree(interpreter_image);
        return status;
    }
    kfree(interpreter_image);
    *space_out = space;
    if (diag_stage != 0) *diag_stage = 8U;
    exec_debug_progress(0x108U);
    return K_OK;
}

kstatus_t process_load_elf_image(process_t *process, const void *image, size_t image_size,
                                 user_elf_image_info_t *info, vaddr_t *stack_pointer) {
    if (process == 0 || info == 0 || stack_pointer == 0) return K_EINVAL;
    elf_exec_arguments_t *arguments =
        (elf_exec_arguments_t *)kzalloc(sizeof(*arguments), 0);
    if (arguments == 0) return K_ENOMEM;
    (void)copy_exec_arguments(0, 0, arguments);
    vm_space_t *new_space = 0;
    kstatus_t status = prepare_elf_space(image, image_size, 0, image_size,
                                          &new_space, info, arguments, 0,
                                          stack_pointer, 0);
    kfree(arguments);
    if (status != K_OK) return status;
    process_thread_lock(process);
    /* 准备阶段不持有所有者锁。此期间进程可能退出；一次性资源拆除开始后，绝不能重新安装 VM。 */
    if (process->thread_count != 0 ||
        atomic_load_explicit(&process->state, memory_order_acquire) != PROCESS_RUNNING ||
        (__atomic_load_n(&process->flags, __ATOMIC_ACQUIRE) &
         PROCESS_FLAG_RESOURCES_RELEASED) != 0U) {
        process_thread_unlock(process);
        vm_space_put(new_space);
        return K_EBUSY;
    }
    vm_space_t *old_space = process->vm;
    process->vm = new_space;
    process->linux_loader_pending = info->linux_abi &&
                                    info->interpreter_base != 0U;
    process->personality = info->linux_abi && !process->linux_loader_pending ?
        OS_PROCESS_PERSONALITY_LINUX_X86_64 :
        OS_PROCESS_PERSONALITY_NATIVE;
    process->linux_brk_base = info->brk_base;
    process->linux_brk = info->brk_base;
    linux_fd_entry_t previous_stdio[3];
    linux_fd_table_reset_stdio(&process->linux_fds, previous_stdio);
    process_thread_unlock(process);
    linux_fd_table_release_stdio(previous_stdio);
    vm_space_put(old_space);
    return K_OK;
}

kstatus_t process_exec_from_vfs(
    process_t *process, const char __user *path,
    const char __user *const __user *argv,
    const char __user *const __user *envp,
    const os_exec_fd_map_t *descriptor_map) {
    exec_debug_result(K_OK);
    if (process == 0 || path == 0) {
        exec_debug_result(K_EINVAL);
        return K_EINVAL;
    }
    thread_t *thread = sched_current_thread();
    if (thread == 0 || thread->process != process) {
        exec_debug_result(K_EPERM);
        return K_EPERM;
    }
    char executable_path[EXEC_PATH_CAPACITY] = {0};
    kstatus_t status = exec_resolve_path(process, path, executable_path);
    if (status != K_OK) {
        exec_debug_result(status);
        exec_diag_status("PATH_FAIL", status);
        return status;
    }
    atomic_store_explicit(&g_exec_debug_thread, (uint32_t)thread->tid,
                          memory_order_release);
#if OS_DEBUG_SERIAL
    __atomic_store_n(&g_exec_debug_serial_logs, 0U, __ATOMIC_RELAXED);
#endif
    exec_debug_progress(1U);

    elf_exec_arguments_t *arguments =
        (elf_exec_arguments_t *)kzalloc(sizeof(*arguments), 0);
    if (arguments == 0) {
        exec_debug_result(K_ENOMEM);
        return K_ENOMEM;
    }
    exec_debug_progress(2U);
    status = copy_exec_arguments(argv, envp, arguments);
    if (status != K_OK) {
        kfree(arguments);
        exec_debug_result(status);
        exec_diag_status("ARGS_FAIL", status);
        return status;
    }
    file_t *file = 0;
    exec_debug_progress(3U);
    /* 实际打开的路径与记录的执行文件路径必须是同一次解析结果。 */
    status = vfs_open_kernel(executable_path, VFS_OPEN_READ, 0U, &file);
    if (status != K_OK) {
        kfree(arguments);
        exec_debug_result(status);
        exec_diag_status("OPEN_FAIL", status);
        return status;
    }
    if (file->vnode == 0 || file->vnode->size == 0U ||
        file->vnode->size > (uint64_t)SIZE_MAX) {
        vfs_close(file);
        kfree(arguments);
        exec_debug_result(K_EINVAL);
        exec_diag_status("SIZE_FAIL", K_EINVAL);
        return K_EINVAL;
    }

    uint64_t file_size = file->vnode->size;
    size_t image_size = 0U;
    uint8_t *image = 0;
    exec_debug_progress(4U);
    uint64_t metadata_started = exec_load_start();
    status = read_vfs_metadata(file, &image, &image_size);
    exec_load_finish(metadata_started, 1U, image_size, status);
    if (status != K_OK) {
    vfs_close(file);
        kfree(arguments);
        exec_debug_result(status);
        exec_diag_status("METADATA_FAIL", status);
        return status;
    }

    vm_space_t *new_space = 0;
    user_elf_image_info_t info;
    vaddr_t stack_pointer = 0;
    uint32_t prepare_stage = 0U;
    exec_debug_progress(6U);
    uint64_t prepare_started = exec_load_start();
    status = prepare_elf_space(image, image_size, file, file_size,
                               &new_space, &info, arguments, descriptor_map,
                               &stack_pointer, &prepare_stage);
    exec_load_finish(prepare_started, 2U, file_size, status);
    uint64_t cleanup_started = exec_load_start();
    kfree(image);
    vfs_close(file);
    kfree(arguments);
    exec_load_finish(cleanup_started, 7U, image_size, status);
    if (status != K_OK) {
        exec_debug_result(status);
        exec_diag_prepare(status, prepare_stage);
        return status;
    }

#if OS_DEBUG_SERIAL
    kernel_serial_write("OS_DIAG_EXEC_PREPARE_OK\r\n");
#endif

    exec_debug_progress(7U);
    process_thread_lock(process);
    if (atomic_load_explicit(&process->state, memory_order_acquire) != PROCESS_RUNNING ||
        process->thread_count != 1U || thread->exec_pending) {
        uint32_t thread_count = process->thread_count;
        uint32_t process_state =
            atomic_load_explicit(&process->state, memory_order_relaxed);
        process_thread_unlock(process);
        vm_space_put(new_space);
        kernel_serial_write("OS_DIAG_EXEC_COMMIT_BUSY THREADS=");
        kernel_serial_write_u32(thread_count);
        kernel_serial_write(" STATE=");
        kernel_serial_write_u32(process_state);
        kernel_serial_write("\r\n");
        exec_debug_result(K_EBUSY);
        return K_EBUSY;
    }

    uint32_t process_flags = __atomic_load_n(&process->flags, __ATOMIC_ACQUIRE);
    if ((process_flags & PROCESS_FLAG_INIT_CPU_PINNED) != 0U) {
        cpumask_t online_affinity = {0};
        uint32_t cpu_count = x86_smp_discovered_count();
        for (uint32_t cpu = 0; cpu < cpu_count; ++cpu) {
            if (x86_smp_cpu_online(cpu)) {
                online_affinity.bits[cpu >> 6U] |=
                    1ULL << (cpu & 63U);
            }
        }
        /* 当前 CPU 已上线且仍在允许集合中，因此此操作会扩大引导线程的亲和性掩码，而不会在
         * exec 提交期间迁移该线程。 */
        status = sched_set_affinity(thread, &online_affinity);
        if (status != K_OK) {
            process_thread_unlock(process);
            vm_space_put(new_space);
            exec_debug_result(status);
            return status;
        }
    }

    vm_space_t *old_space = process->vm;
    process->vm = new_space;
    /* 仅用于启动阶段的 CPU 亲和性不适用于此新映像及其创建的线程：上方的 sched_set_affinity
     * 已扩大 exec 线程的亲和性，清除此标志后，后续 pthread/clone 线程即可使用所有在线 CPU。 */
    __atomic_fetch_and(&process->flags, ~PROCESS_FLAG_INIT_CPU_PINNED,
                       __ATOMIC_ACQ_REL);
    process->linux_loader_pending = info.linux_abi &&
                                    info.interpreter_base != 0U;
    process->personality = info.linux_abi && !process->linux_loader_pending ?
        OS_PROCESS_PERSONALITY_LINUX_X86_64 :
        OS_PROCESS_PERSONALITY_NATIVE;
    process->linux_brk_base = info.brk_base;
    process->linux_brk = info.brk_base;
    exec_store_linux_exe(process, executable_path);
    thread->exec_entry = info.entry;
    thread->exec_stack = stack_pointer;
    thread->exec_pending = true;
    thread->arch.fs_base = 0U;
    thread->signal_frame = 0U;
    thread->signal_depth = 0U;
    spinlock_lock(&process->signal_lock);
    for (uint32_t signal = 1U; signal <= OS_SIGNAL_COUNT; ++signal) {
        if (process->signal_actions[signal].handler != OS_SIG_IGN) {
            process->signal_actions[signal] = (os_signal_action_t){0};
        }
    }
    spinlock_unlock(&process->signal_lock);
    process_thread_unlock(process);
    /* POSIX 只关闭标记为 close-on-exec 的描述符，且仅在新映像准备成功后关闭。句柄回调在表锁
     * 之外运行，以免 socket/pipe 拆除过程与 exec 发生死锁。 */
    uint64_t close_started = exec_load_start();
    status = handle_table_close_on_exec(&process->handles);
    if (status != K_OK) {
        /* 映像提交已经有效；应保留 exec 成功状态，仅为异常路径记录失败诊断。 */
        exec_diag_status("CLOEXEC_FAIL", status);
    }
    linux_fd_table_close_on_exec(process);
    exec_load_finish(close_started, 5U, 0U, status);
    /* 先切到新 CR3，再把旧页表交给可抢占的 deferred worker 回收。 */
    /* 切换 CR3 前先复制旧映像路径；在新提交的地址空间中，旧用户指针已无效。 */
    exec_update_process_name(process, executable_path);
    exec_debug_progress(8U);
    x86_activate_root_table_pcid(new_space->root_table, new_space->pcid);
    x86_set_user_fs_base(0U);
    x86_fp_state_reset_current(&thread->arch);
    exec_debug_progress(9U);
    uint64_t release_started = exec_load_start();
    exec_release_old_space(old_space);
    exec_load_finish(release_started, 6U, 0U, K_OK);
    exec_debug_progress(10U);
    exec_debug_result(K_OK);
#if OS_DEBUG_SERIAL
    kernel_serial_write("OS_DIAG_EXEC_COMMIT_OK\r\n");
#endif
    return K_OK;
}

kstatus_t process_exec(process_t *process, const char __user *path,
                       const char __user *const __user *argv,
                       const char __user *const __user *envp,
                       const os_exec_fd_map_t *descriptor_map) {
    return process_exec_from_vfs(process, path, argv, envp, descriptor_map);
}

static void user_elf_loader_self_test_diag(uint32_t stage,
                                           kstatus_t status) {
    kernel_serial_write("OS_DIAG_USER_ELF_SELFTEST_FAIL STAGE=");
    kernel_serial_write_u32(stage);
    kernel_serial_write(" STATUS=");
    kernel_serial_write_u32((uint32_t)status);
    kernel_serial_write("\r\n");
}

bool user_elf_loader_self_test(void) {
    uint8_t image[0x1008U];
    static uint8_t dynamic_image[0x3000U];
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
    header->program_header_count = 1U;
    elf64_program_header_t *program = (elf64_program_header_t *)(image + sizeof(*header));
    program->type = ELF_PT_LOAD;
    program->flags = ELF_PF_READ | ELF_PF_EXEC;
    program->offset = 0x1000U;
    program->virtual_address = 0x400000ULL;
    program->file_size = 7U;
    program->memory_size = PAGE_SIZE;
    program->alignment = PAGE_SIZE;
    const uint8_t code[7] = {0x31, 0xFF, 0x31, 0xC0, 0x0F, 0x05, 0xF4};
    for (uint32_t i = 0; i < sizeof(code); ++i) image[0x1000U + i] = code[i];

    process_t *process = 0;
    thread_t *thread = 0;
    process_t *dynamic_process = 0;
    user_elf_image_info_t info;
    vaddr_t stack_pointer = 0;
    paddr_t physical;
    kstatus_t status = K_OK;
    uint32_t failure_stage = 0U;
    kstatus_t failure_status = K_EIO;
    bool success = false;
    status = process_create(0, &process);
    if (status != K_OK) {
        failure_stage = 1U;
        failure_status = status;
        goto cleanup;
    }
    status = process_load_elf_image(process, image, sizeof(image), &info,
                                    &stack_pointer);
    if (status != K_OK) {
        failure_stage = 2U;
        failure_status = status;
        goto cleanup;
    }
    if (info.entry != 0x400000ULL || (stack_pointer & 15U) != 0) {
        failure_stage = 3U;
        goto cleanup;
    }
    status = x86_translate_page(process->vm->root_table, info.entry,
                                &physical, 0);
    if (status != K_OK) {
        failure_stage = 4U;
        failure_status = status;
        goto cleanup;
    }
    uint8_t *loaded = (uint8_t *)phys_to_direct(physical);
    if (loaded == 0) {
        failure_stage = 5U;
        goto cleanup;
    }
    for (uint32_t i = 0; i < sizeof(code); ++i) {
        if (loaded[i] != code[i]) {
            failure_stage = 6U;
            goto cleanup;
        }
    }
    status = thread_create_user(process, info.entry, stack_pointer, 0, &thread);
    if (status != K_OK) {
        failure_stage = 7U;
        failure_status = status;
        goto cleanup;
    }
    if (thread->arch.switch_ctx.r12 == 0 || thread->kernel_stack_base == 0) {
        failure_stage = 8U;
        goto cleanup;
    }

    /* 鍐嶉獙璇佷竴娆″甫 PT_INTERP 鐨?ELF锛氳В閲婂櫒鐢?VFS 鍙栧嚭骞跺厛浜庝富绋嬪簭鍏ュ彛鍚姩銆?*/
    for (size_t i = 0; i < sizeof(dynamic_image); ++i) dynamic_image[i] = 0;
    elf64_header_t *dynamic_header = (elf64_header_t *)dynamic_image;
    *dynamic_header = *header;
    dynamic_header->program_header_count = 2U;
    elf64_program_header_t *dynamic_programs =
        (elf64_program_header_t *)(dynamic_image + sizeof(*dynamic_header));
    dynamic_programs[0] = *program;
    dynamic_programs[1].type = ELF_PT_INTERP;
    dynamic_programs[1].offset = 0x2000U;
    dynamic_programs[1].file_size = sizeof("/lib/ld-linux-system.so.1");
    const char interpreter_path[] = "/lib/ld-linux-system.so.1";
    for (size_t i = 0; i < sizeof(interpreter_path); ++i) {
        dynamic_image[0x2000U + i] = (uint8_t)interpreter_path[i];
    }
    user_elf_image_info_t dynamic_info;
    vaddr_t dynamic_stack = 0;
    paddr_t dynamic_physical;
    status = process_create(0, &dynamic_process);
    if (status != K_OK) {
        failure_stage = 9U;
        failure_status = status;
        goto cleanup;
    }
    status = process_load_elf_image(dynamic_process, dynamic_image,
                                    sizeof(dynamic_image), &dynamic_info,
                                    &dynamic_stack);
    if (status != K_OK) {
        failure_stage = 10U;
        failure_status = status;
        goto cleanup;
    }
    /* 解释器的链接时入口属于 Linux 兼容别名 ld-linux-system.so.1，本内核自测有意不固定该地址。
     * 应验证加载偏移和已映射入口范围，而不是将测试绑定到某个链接脚本的入口地址。 */
    if (dynamic_info.main_entry != 0x400000ULL ||
        dynamic_info.interpreter_base != ELF_INTERP_BIAS ||
        dynamic_info.entry < ELF_INTERP_BIAS ||
        dynamic_info.entry >= ELF_INTERP_BIAS + ELF_MAX_SEGMENT_SIZE) {
        failure_stage = 11U;
        goto cleanup;
    }
    status = x86_translate_page(dynamic_process->vm->root_table,
                                dynamic_info.entry, &dynamic_physical, 0);
    if (status != K_OK) {
        failure_stage = 12U;
        failure_status = status;
        goto cleanup;
    }
    success = true;

cleanup:
    if (thread != 0) {
        (void)thread_terminate(thread, K_ECANCELED);
        object_put(thread);
    }
    if (dynamic_process != 0) {
        (void)process_abort(dynamic_process);
        object_put(dynamic_process);
    }
    if (process != 0) {
        if (thread == 0) (void)process_abort(process);
        object_put(process);
    }
    if (!success && failure_stage != 0U) {
        user_elf_loader_self_test_diag(failure_stage, failure_status);
    }
    return success;
}
