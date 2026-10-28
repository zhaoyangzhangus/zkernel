#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <uapi/file.h>
#include <uapi/mm.h>
#include <uapi/process.h>
#include <uapi/syscall.h>

#include "elf64.h"

#define OS_RT_PAGE_SIZE       4096ULL
#define OS_RT_FILE_LIMIT      (64ULL * 1024ULL * 1024ULL)
#define OS_RT_MAX_SEGMENT_SIZE (256ULL * 1024ULL * 1024ULL)
#define OS_RT_MAX_PROGRAMS    64U
#define OS_RT_MAX_IMAGES      8U
#define OS_RT_MAX_NEEDED      16U
#define OS_RT_MAX_AUX         128U
#define OS_RT_MAX_ENV         256U
#define OS_RT_MAX_PATH        128U
#define OS_RT_MAX_NAME        64U
#define OS_RT_TLS_HEADER_WORDS 6U
#define OS_RT_TLS_MAGIC        0x4C4954454F53544CULL
#define OS_RT_LIBRARY_BASE    0x0000000060000000ULL
#define OS_RT_LIBRARY_ALIGN   0x00100000ULL
#define OS_RT_LIBRARY_GAP     0x00100000ULL

__attribute__((visibility("hidden"))) void *memset(void *destination, int value,
                                                     size_t length) {
    uint8_t *output = (uint8_t *)destination;
    while (length-- != 0U) *output++ = (uint8_t)value;
    return destination;
}

enum {
    OS_RT_AT_PHDR = 3,
    OS_RT_AT_PHENT = 4,
    OS_RT_AT_PHNUM = 5,
    OS_RT_AT_ENTRY = 9,
};

typedef struct loaded_image {
    bool used;
    char name[OS_RT_MAX_NAME];
    uint64_t base;
    uint64_t entry;
    uint64_t phdr_address;
    uint16_t phdr_size;
    uint16_t phdr_count;
    uint64_t image_start;
    uint64_t image_end;
    runtime_elf64_program_header_t phdrs[OS_RT_MAX_PROGRAMS];

    uint64_t dynamic_address;
    uint64_t dynamic_size;
    uint64_t string_table;
    uint64_t string_size;
    uint64_t symbol_table;
    uint64_t symbol_count;
    uint64_t sysv_hash;
    uint64_t gnu_hash;
    uint64_t rela;
    uint64_t rela_size;
    uint64_t rela_entry_size;
    uint64_t jmprel;
    uint64_t jmprel_size;
    uint64_t plt_reloc_type;
    uint64_t init;
    uint64_t init_array;
    uint64_t init_array_size;
    uint64_t needed[OS_RT_MAX_NEEDED];
    uint32_t needed_count;
    uint64_t tls_offset;
    uint64_t tls_size;
    uint64_t tls_file_size;
    uint64_t tls_source;
    uint64_t tls_alignment;
} loaded_image_t;

typedef struct aux_value {
    uint64_t type;
    uint64_t value;
} aux_value_t;

static int64_t runtime_syscall6(uint64_t number, uint64_t argument0,
                                  uint64_t argument1, uint64_t argument2,
                                  uint64_t argument3, uint64_t argument4,
                                  uint64_t argument5) {
    register uint64_t r10 __asm__("r10") = argument3;
    register uint64_t r8 __asm__("r8") = argument4;
    register uint64_t r9 __asm__("r9") = argument5;
    uint64_t result;
    __asm__ volatile("syscall"
                     : "=a"(result)
                     : "a"(number), "D"(argument0), "S"(argument1),
                       "d"(argument2), "r"(r10), "r"(r8), "r"(r9)
                     : "rcx", "r11", "memory");
    return (int64_t)result;
}

static int64_t runtime_syscall1(uint64_t number, uint64_t argument0) {
    return runtime_syscall6(number, argument0, 0U, 0U, 0U, 0U, 0U);
}

static int64_t runtime_syscall3(uint64_t number, uint64_t argument0,
                                  uint64_t argument1, uint64_t argument2) {
    return runtime_syscall6(number, argument0, argument1, argument2,
                              0U, 0U, 0U);
}

static int64_t runtime_syscall4(uint64_t number, uint64_t argument0,
                                  uint64_t argument1, uint64_t argument2,
                                  uint64_t argument3) {
    return runtime_syscall6(number, argument0, argument1, argument2,
                              argument3, 0U, 0U);
}

/* 仅在加载失败时输出诊断。 */
static void runtime_diag(const char *message) {
    uint64_t length = 0U;
    if (message == 0) return;
    while (message[length] != '\0') ++length;
    (void)runtime_syscall3(OS_SYS_DEBUG_WRITE,
                             (uint64_t)(uintptr_t)message, length, 0U);
}

static bool add_u64(uint64_t left, uint64_t right, uint64_t *result) {
    if (result == 0 || left > UINT64_MAX - right) return false;
    *result = left + right;
    return true;
}

static bool align_up_u64(uint64_t value, uint64_t alignment, uint64_t *result) {
    uint64_t remainder;
    if (result == 0 || alignment == 0U) return false;
    remainder = value & (alignment - 1U);
    if (remainder == 0U) {
        *result = value;
        return true;
    }
    return add_u64(value, alignment - remainder, result);
}

static uint64_t align_down_u64(uint64_t value, uint64_t alignment) {
    return value & ~(alignment - 1U);
}

static void copy_bytes(void *destination, const void *source, size_t length) {
    uint8_t *out = (uint8_t *)destination;
    const uint8_t *in = (const uint8_t *)source;
    while (length-- != 0U) *out++ = *in++;
}

static void zero_bytes(void *destination, uint64_t length) {
    uint8_t *out = (uint8_t *)destination;
    while (length-- != 0U) *out++ = 0U;
}

static bool copy_string(char *destination, size_t capacity, const char *source) {
    size_t index = 0U;
    if (destination == 0 || source == 0 || capacity == 0U) return false;
    while (index + 1U < capacity && source[index] != '\0') {
        destination[index] = source[index];
        ++index;
    }
    if (source[index] != '\0') {
        destination[0] = '\0';
        return false;
    }
    destination[index] = '\0';
    return true;
}

static bool same_string(const char *left, const char *right) {
    size_t index = 0U;
    if (left == 0 || right == 0) return false;
    while (left[index] != '\0' && right[index] != '\0') {
        if (left[index] != right[index]) return false;
        ++index;
    }
    return left[index] == right[index];
}

static bool bounded_string(const char *text, uint64_t limit, uint64_t *length) {
    uint64_t index;
    if (text == 0 || length == 0) return false;
    for (index = 0U; index < limit; ++index) {
        if (text[index] == '\0') {
            *length = index;
            return true;
        }
    }
    return false;
}

static int64_t file_open(const char *path, os_handle_t *handle) {
    if (path == 0 || handle == 0) return -1;
    *handle = OS_INVALID_HANDLE;
    return runtime_syscall4(OS_SYS_FILE_OPEN, (uint64_t)(uintptr_t)path,
                              OS_FILE_OPEN_READ, 0U,
                              (uint64_t)(uintptr_t)handle);
}

static void file_close(os_handle_t handle) {
    if (handle != OS_INVALID_HANDLE) {
        (void)runtime_syscall1(OS_SYS_HANDLE_CLOSE, handle);
    }
}

static bool file_stat_size(const char *path, uint64_t *size) {
    os_file_stat_t request = {0};
    int64_t status;
    if (path == 0 || size == 0) return false;
    request.hdr.size = sizeof(request);
    request.hdr.version = OS_SYSCALL_ABI_VERSION;
    request.path = (uint64_t)(uintptr_t)path;
    status = runtime_syscall1(OS_SYS_FILE_STAT, (uint64_t)(uintptr_t)&request);
    if (status < 0 || request.info.type != OS_FILE_TYPE_REGULAR ||
        request.info.size == 0U || request.info.size > OS_RT_FILE_LIMIT) {
        return false;
    }
    *size = request.info.size;
    return true;
}

static bool file_seek(os_handle_t handle, uint64_t offset) {
    os_file_seek_t request = {0};
    if (offset > (uint64_t)INT64_MAX) return false;
    request.hdr.size = sizeof(request);
    request.hdr.version = OS_SYSCALL_ABI_VERSION;
    request.handle = handle;
    request.offset = (int64_t)offset;
    request.whence = OS_FILE_SEEK_SET;
    return runtime_syscall1(OS_SYS_FILE_SEEK, (uint64_t)(uintptr_t)&request) == 0;
}

static bool file_read_exact(os_handle_t handle, void *destination, uint64_t length) {
    uint8_t *output = (uint8_t *)destination;
    while (length != 0U) {
        uint64_t request_size = length > 65536U ? 65536U : length;
        uint64_t bytes = 0U;
        int64_t status = runtime_syscall4(
            OS_SYS_FILE_READ, handle, (uint64_t)(uintptr_t)output,
            request_size, (uint64_t)(uintptr_t)&bytes);
        if (status < 0 || bytes == 0U || bytes > request_size) return false;
        output += bytes;
        length -= bytes;
    }
    return true;
}

static bool file_read_at(os_handle_t handle, uint64_t offset, void *destination,
                         uint64_t length) {
    return file_seek(handle, offset) && file_read_exact(handle, destination, length);
}

static bool pointer_range(const loaded_image_t *image, uint64_t address,
                          uint64_t length) {
    if (image == 0 || address < image->image_start || address > image->image_end) {
        return false;
    }
    return length <= image->image_end - address;
}

static bool link_pointer(const loaded_image_t *image, uint64_t link_address,
                         uint64_t *runtime_address) {
    if (image == 0 || runtime_address == 0 ||
        !add_u64(image->base, link_address, runtime_address)) return false;
    return pointer_range(image, *runtime_address, 1U);
}

static bool map_anonymous(uint64_t address, uint64_t length, uint32_t protection) {
    os_vm_map_args_t request = {0};
    int64_t status;
    if (address == 0U || length == 0U || (address & (OS_RT_PAGE_SIZE - 1U)) != 0U ||
        (length & (OS_RT_PAGE_SIZE - 1U)) != 0U) return false;
    request.hdr.size = sizeof(request);
    request.hdr.version = OS_SYSCALL_ABI_VERSION;
    request.address = address;
    request.length = length;
    request.object = OS_INVALID_HANDLE;
    request.prot = protection;
    request.flags = OS_VM_PRIVATE | OS_VM_FIXED;
    status = runtime_syscall1(OS_SYS_VM_MAP, (uint64_t)(uintptr_t)&request);
    return status == 0 && request.address == address;
}

static bool map_anonymous_any(uint64_t length, uint32_t protection,
                              uint64_t *address) {
    os_vm_map_args_t request = {0};
    int64_t status;
    if (address == 0 || length == 0U ||
        (length & (OS_RT_PAGE_SIZE - 1U)) != 0U) return false;
    request.hdr.size = sizeof(request);
    request.hdr.version = OS_SYSCALL_ABI_VERSION;
    request.length = length;
    request.object = OS_INVALID_HANDLE;
    request.prot = protection;
    request.flags = OS_VM_PRIVATE;
    status = runtime_syscall1(OS_SYS_VM_MAP, (uint64_t)(uintptr_t)&request);
    if (status != 0 || request.address == 0U) return false;
    *address = request.address;
    return true;
}

static bool unmap_image(const loaded_image_t *image) {
    if (image == 0 || image->image_start >= image->image_end) return false;
    return runtime_syscall3(OS_SYS_VM_UNMAP, image->image_start,
                              image->image_end - image->image_start, 0U) == 0;
}

static bool protect_range(uint64_t address, uint64_t length, uint32_t flags) {
    uint32_t protection = 0U;
    if ((flags & OS_RT_PF_R) != 0U) protection |= OS_VM_READ;
    if ((flags & OS_RT_PF_W) != 0U) protection |= OS_VM_WRITE;
    if ((flags & OS_RT_PF_X) != 0U) protection |= OS_VM_EXEC;
    return runtime_syscall3(OS_SYS_VM_PROTECT, address, length, protection) == 0;
}

static bool image_contains_writable(const loaded_image_t *image, uint64_t address,
                                    uint64_t length) {
    if (image == 0) return false;
    for (uint16_t index = 0U; index < image->phdr_count; ++index) {
        const runtime_elf64_program_header_t *program = &image->phdrs[index];
        uint64_t start;
        uint64_t end;
        if (program->type != OS_RT_PT_LOAD ||
            (program->flags & OS_RT_PF_W) == 0U ||
            !add_u64(image->base, program->virtual_address, &start) ||
            !add_u64(start, program->memory_size, &end)) continue;
        if (address >= start && address <= end && length <= end - address) return true;
    }
    return false;
}

static bool valid_header(const runtime_elf64_header_t *header, uint64_t file_size) {
    uint64_t table_size;
    if (header == 0 || header->ident[0] != 0x7FU || header->ident[1] != 'E' ||
        header->ident[2] != 'L' || header->ident[3] != 'F' ||
        header->ident[4] != OS_RT_ELF_CLASS_64 ||
        header->ident[5] != OS_RT_ELF_DATA_LSB ||
        header->ident[6] != OS_RT_ELF_VERSION ||
        header->machine != OS_RT_ELF_MACHINE_X86_64 ||
        header->version != OS_RT_ELF_VERSION ||
        header->header_size < sizeof(*header) ||
        header->program_header_size < sizeof(runtime_elf64_program_header_t) ||
        header->program_header_count == 0U ||
        header->program_header_count > OS_RT_MAX_PROGRAMS) return false;
    if (header->program_header_count > UINT64_MAX / header->program_header_size) {
        return false;
    }
    table_size = (uint64_t)header->program_header_count * header->program_header_size;
    return header->program_header_offset <= file_size &&
           table_size <= file_size - header->program_header_offset;
}

static bool valid_load_range(const runtime_elf64_program_header_t *program,
                             uint64_t file_size) {
    uint64_t file_end;
    uint64_t memory_end;
    if (program == 0 || program->type != OS_RT_PT_LOAD ||
        program->memory_size < program->file_size ||
        program->memory_size > OS_RT_MAX_SEGMENT_SIZE ||
        (program->flags & ~(OS_RT_PF_R | OS_RT_PF_W | OS_RT_PF_X)) != 0U ||
        (program->flags & (OS_RT_PF_W | OS_RT_PF_X)) ==
            (OS_RT_PF_W | OS_RT_PF_X) ||
        !add_u64(program->offset, program->file_size, &file_end) ||
        file_end > file_size ||
        !add_u64(program->virtual_address, program->memory_size, &memory_end) ||
        memory_end <= program->virtual_address) return false;
    if (program->alignment > 1U &&
        ((program->alignment & (program->alignment - 1U)) != 0U ||
         (program->virtual_address & (program->alignment - 1U)) !=
             (program->offset & (program->alignment - 1U)))) return false;
    return true;
}

static bool read_program_headers(os_handle_t handle,
                                 const runtime_elf64_header_t *header,
                                 runtime_elf64_program_header_t *programs) {
    for (uint16_t index = 0U; index < header->program_header_count; ++index) {
        uint64_t offset = header->program_header_offset +
                          (uint64_t)index * header->program_header_size;
        if (!file_read_at(handle, offset, &programs[index], sizeof(programs[index]))) {
            return false;
        }
    }
    return true;
}

static bool parse_dynamic(loaded_image_t *image) {
    bool terminated = false;
    if (image == 0 || image->dynamic_address == 0U || image->dynamic_size <
        sizeof(runtime_elf64_dynamic_t) ||
        !pointer_range(image, image->dynamic_address, image->dynamic_size)) return false;
    for (uint64_t offset = 0U;
         offset + sizeof(runtime_elf64_dynamic_t) <= image->dynamic_size;
         offset += sizeof(runtime_elf64_dynamic_t)) {
        const runtime_elf64_dynamic_t *entry =
            (const runtime_elf64_dynamic_t *)(uintptr_t)(image->dynamic_address + offset);
        uint64_t runtime_address;
        switch (entry->tag) {
        case OS_RT_DT_NULL:
            terminated = true;
            offset = image->dynamic_size;
            continue;
        case OS_RT_DT_NEEDED:
            if (image->needed_count >= OS_RT_MAX_NEEDED) return false;
            image->needed[image->needed_count++] = entry->value;
            break;
        case OS_RT_DT_STRTAB:
            if (!link_pointer(image, entry->value, &runtime_address)) return false;
            image->string_table = runtime_address;
            break;
        case OS_RT_DT_STRSZ:
            image->string_size = entry->value;
            break;
        case OS_RT_DT_SYMTAB:
            if (!link_pointer(image, entry->value, &runtime_address)) return false;
            image->symbol_table = runtime_address;
            break;
        case OS_RT_DT_HASH:
            if (!link_pointer(image, entry->value, &runtime_address)) return false;
            image->sysv_hash = runtime_address;
            break;
        case OS_RT_DT_GNU_HASH:
            if (!link_pointer(image, entry->value, &runtime_address)) return false;
            image->gnu_hash = runtime_address;
            break;
        case OS_RT_DT_RELA:
            if (!link_pointer(image, entry->value, &runtime_address)) return false;
            image->rela = runtime_address;
            break;
        case OS_RT_DT_RELASZ:
            image->rela_size = entry->value;
            break;
        case OS_RT_DT_RELAENT:
            image->rela_entry_size = entry->value;
            break;
        case OS_RT_DT_JMPREL:
            if (!link_pointer(image, entry->value, &runtime_address)) return false;
            image->jmprel = runtime_address;
            break;
        case OS_RT_DT_PLTRELSZ:
            image->jmprel_size = entry->value;
            break;
        case OS_RT_DT_PLTREL:
            image->plt_reloc_type = entry->value;
            break;
        case OS_RT_DT_INIT:
            if (!link_pointer(image, entry->value, &runtime_address)) return false;
            image->init = runtime_address;
            break;
        case OS_RT_DT_INIT_ARRAY:
            if (!link_pointer(image, entry->value, &runtime_address)) return false;
            image->init_array = runtime_address;
            break;
        case OS_RT_DT_INIT_ARRAYSZ:
            image->init_array_size = entry->value;
            break;
        default:
            break;
        }
    }
    if (!terminated || image->string_table == 0U || image->string_size == 0U ||
        !pointer_range(image, image->string_table, image->string_size) ||
        image->symbol_table == 0U) return false;

    if (image->sysv_hash != 0U && pointer_range(image, image->sysv_hash, 8U)) {
        const uint32_t *hash = (const uint32_t *)(uintptr_t)image->sysv_hash;
        image->symbol_count = hash[1];
    }
    if (image->symbol_count == 0U && image->gnu_hash != 0U &&
        pointer_range(image, image->gnu_hash, 16U)) {
        const uint32_t *hash = (const uint32_t *)(uintptr_t)image->gnu_hash;
        uint32_t bucket_count = hash[0];
        uint32_t symbol_offset = hash[1];
        uint32_t bloom_count = hash[2];
        uint64_t bucket_address = image->gnu_hash + 16U +
                                  (uint64_t)bloom_count * sizeof(uint64_t);
        uint64_t chain_address = bucket_address +
                                 (uint64_t)bucket_count * sizeof(uint32_t);
        uint32_t highest = symbol_offset;
        if (bucket_count == 0U || !pointer_range(image, bucket_address,
                                                  (uint64_t)bucket_count * 4U) ||
            !pointer_range(image, chain_address, 4U)) return false;
        const uint32_t *buckets = (const uint32_t *)(uintptr_t)bucket_address;
        const uint32_t *chains = (const uint32_t *)(uintptr_t)chain_address;
        for (uint32_t bucket = 0U; bucket < bucket_count; ++bucket) {
            if (buckets[bucket] > highest) highest = buckets[bucket];
        }
        if (highest != symbol_offset) {
            uint64_t chain_index = highest - symbol_offset;
            while (pointer_range(image, chain_address + chain_index * 4U, 4U) &&
                   (chains[chain_index] & 1U) == 0U) ++chain_index;
            if (!pointer_range(image, chain_address + chain_index * 4U, 4U)) {
                return false;
            }
            image->symbol_count = (uint64_t)highest + 1U;
        }
    }
    if (image->symbol_count == 0U && image->symbol_table < image->image_end) {
        image->symbol_count = (image->image_end - image->symbol_table) /
                              sizeof(runtime_elf64_symbol_t);
    }
    if (image->rela_size != 0U || image->jmprel_size != 0U) {
        if (image->rela_entry_size == 0U) {
            image->rela_entry_size = sizeof(runtime_elf64_rela_t);
        }
        if (image->plt_reloc_type == 0U) image->plt_reloc_type = OS_RT_DT_RELA_TYPE;
    }
    return image->symbol_count != 0U;
}

static bool initialize_main_image(uint64_t stack, loaded_image_t *image) {
    uint64_t argc;
    uint64_t *cursor;
    aux_value_t aux[OS_RT_MAX_AUX];
    uint32_t aux_count = 0U;
    uint64_t phdr_address = 0U;
    uint64_t phdr_size = 0U;
    uint64_t phdr_count = 0U;
    uint64_t entry = 0U;
    bool has_phdr = false;
    bool has_entry = false;
    if (stack == 0U || image == 0) return false;
    argc = *(const uint64_t *)(uintptr_t)stack;
    if (argc > 128U) return false;
    cursor = (uint64_t *)(uintptr_t)(stack + sizeof(uint64_t));
    cursor += argc + 1U;
    for (uint32_t index = 0U; index < OS_RT_MAX_ENV; ++index) {
        if (cursor[index] == 0U) break;
        if (index + 1U == OS_RT_MAX_ENV) return false;
    }
    while (cursor[0] != 0U) ++cursor;
    ++cursor;
    while (aux_count < OS_RT_MAX_AUX) {
        aux[aux_count].type = cursor[0];
        aux[aux_count].value = cursor[1];
        ++aux_count;
        cursor += 2U;
        if (aux[aux_count - 1U].type == 0U) break;
    }
    if (aux_count == OS_RT_MAX_AUX && aux[aux_count - 1U].type != 0U) return false;
    for (uint32_t index = 0U; index < aux_count; ++index) {
        if (aux[index].type == OS_RT_AT_PHDR) {
            phdr_address = aux[index].value;
            has_phdr = true;
        } else if (aux[index].type == OS_RT_AT_PHENT) {
            phdr_size = aux[index].value;
        } else if (aux[index].type == OS_RT_AT_PHNUM) {
            phdr_count = aux[index].value;
        } else if (aux[index].type == OS_RT_AT_ENTRY) {
            entry = aux[index].value;
            has_entry = true;
        }
    }
    if (!has_phdr || !has_entry || phdr_size < sizeof(image->phdrs[0]) ||
        phdr_count == 0U || phdr_count > OS_RT_MAX_PROGRAMS) return false;
    *image = (loaded_image_t){0};
    image->used = true;
    image->base = 0U;
    image->entry = entry;
    image->phdr_address = phdr_address;
    image->phdr_size = (uint16_t)phdr_size;
    image->phdr_count = (uint16_t)phdr_count;
    if (!copy_string(image->name, sizeof(image->name), "<main>")) return false;
    for (uint16_t index = 0U; index < image->phdr_count; ++index) {
        const uint8_t *source = (const uint8_t *)(uintptr_t)(phdr_address +
            (uint64_t)index * phdr_size);
        copy_bytes(&image->phdrs[index], source, sizeof(image->phdrs[index]));
    }
    for (uint16_t index = 0U; index < image->phdr_count; ++index) {
        const runtime_elf64_program_header_t *program = &image->phdrs[index];
        uint64_t start;
        uint64_t end;
        if (program->type == OS_RT_PT_PHDR) {
            if (phdr_address < program->virtual_address) return false;
            image->base = phdr_address - program->virtual_address;
        }
        if (program->type == OS_RT_PT_LOAD) {
            if (!add_u64(image->base, program->virtual_address, &start) ||
                !add_u64(start, program->memory_size, &end) || end <= start) return false;
            if (image->image_start == 0U || start < image->image_start) image->image_start = start;
            if (end > image->image_end) image->image_end = end;
        }
    }
    if (image->image_start >= image->image_end) return false;
    for (uint16_t index = 0U; index < image->phdr_count; ++index) {
        const runtime_elf64_program_header_t *program = &image->phdrs[index];
        if (program->type == OS_RT_PT_DYNAMIC) {
            if (!link_pointer(image, program->virtual_address, &image->dynamic_address)) {
                return false;
            }
            image->dynamic_size = program->memory_size;
            break;
        }
    }
    return image->dynamic_address != 0U && parse_dynamic(image);
}

/* 保留私有 TLS 模板，并在按 PT_TLS 对齐的副本后放置精简的 System TCB。
 * FS 偏移是 align_up(p_memsz, p_align)，而不是 p_memsz：fs:0 是 local-exec ABI
 * 要求的 TLS 自指针，fs:8 是精确 TLS 大小，fs:16 是模板，fs:24 是验证标记，
 * fs:32 是 System pthread_t，fs:40 是 PT_TLS 对齐值。 */
static bool setup_main_tls(const loaded_image_t *image) {
    const runtime_elf64_program_header_t *tls = 0;
    uint64_t tls_source;
    uint64_t tls_offset;
    uint64_t tls_alignment;
    uint64_t total_size;
    uint64_t mapping_size;
    uint64_t template_mapping = 0U;
    uint64_t template_tls_base;
    uint64_t main_mapping = 0U;
    uint64_t main_tls_base;
    uint64_t fs_base;
    uint64_t *tcb;

    if (image == 0) return false;
    for (uint16_t index = 0U; index < image->phdr_count; ++index) {
        if (image->phdrs[index].type == OS_RT_PT_TLS) {
            if (tls != 0) return false;
            tls = &image->phdrs[index];
        }
    }
    if (tls == 0) return true;
    tls_alignment = tls->alignment > 1U ? tls->alignment : 1U;
    if (tls->memory_size == 0U || tls->file_size > tls->memory_size ||
        tls->memory_size > (1ULL << 20U) ||
        tls_alignment > (1ULL << 20U) ||
        (tls_alignment & (tls_alignment - 1U)) != 0U ||
        !add_u64(image->base, tls->virtual_address, &tls_source) ||
        !pointer_range(image, tls_source, tls->file_size) ||
        !align_up_u64(tls->memory_size, tls_alignment, &tls_offset) ||
        !add_u64(tls_offset, tls_alignment - 1U, &total_size) ||
        !add_u64(total_size,
                 (uint64_t)OS_RT_TLS_HEADER_WORDS * sizeof(uint64_t),
                 &total_size) ||
        !align_up_u64(total_size, OS_RT_PAGE_SIZE, &mapping_size)) {
        return false;
    }
    if (!map_anonymous_any(mapping_size, OS_VM_READ | OS_VM_WRITE,
                           &template_mapping)) return false;
    zero_bytes((void *)(uintptr_t)template_mapping, mapping_size);
    if (!align_up_u64(template_mapping, tls_alignment,
                      &template_tls_base)) {
        (void)runtime_syscall3(OS_SYS_VM_UNMAP, template_mapping,
                                 mapping_size, 0U);
        return false;
    }
    if (tls->file_size != 0U) {
        copy_bytes((void *)(uintptr_t)template_tls_base,
                   (const void *)(uintptr_t)tls_source,
                   (size_t)tls->file_size);
    }
    if (!map_anonymous_any(mapping_size, OS_VM_READ | OS_VM_WRITE,
                           &main_mapping)) {
        (void)runtime_syscall3(OS_SYS_VM_UNMAP, template_mapping,
                                 mapping_size, 0U);
        return false;
    }
    if (!align_up_u64(main_mapping, tls_alignment, &main_tls_base) ||
        !add_u64(main_tls_base, tls_offset, &fs_base)) {
        (void)runtime_syscall3(OS_SYS_VM_UNMAP, main_mapping,
                                 mapping_size, 0U);
        (void)runtime_syscall3(OS_SYS_VM_UNMAP, template_mapping,
                                 mapping_size, 0U);
        return false;
    }
    copy_bytes((void *)(uintptr_t)main_tls_base,
               (const void *)(uintptr_t)template_tls_base,
               (size_t)tls->memory_size);
    tcb = (uint64_t *)(uintptr_t)fs_base;
    tcb[0] = fs_base;
    tcb[1] = tls->memory_size;
    tcb[2] = template_tls_base;
    tcb[3] = OS_RT_TLS_MAGIC;
    tcb[4] = 0U;
    tcb[5] = tls_alignment;
    if (runtime_syscall6(OS_SYS_THREAD_CONTEXT,
                           OS_THREAD_CONTEXT_SET_FS, fs_base,
                           0U, 0U, 0U, 0U) < 0) {
        (void)runtime_syscall3(OS_SYS_VM_UNMAP, main_mapping,
                                 mapping_size, 0U);
        (void)runtime_syscall3(OS_SYS_VM_UNMAP, template_mapping,
                                 mapping_size, 0U);
        return false;
    }
    return true;
}

/* x86-64 variant-II 静态 TLS：可执行文件最靠近 FS，启动时加载的 DSO 放在其下方。
 * 遵循 TLS 段首字节的对齐相位。现有六字 TCB 描述整个不可变模板，因此 pthread
 * 创建线程时可以复制所有启动模块，而无需更改已部署 ABI。 */
static bool plan_static_tls(loaded_image_t *images, uint32_t count,
                            uint64_t *total, uint64_t *alignment) {
    const uint64_t limit = 1ULL << 20U;
    if (images == 0 || count == 0U || count > OS_RT_MAX_IMAGES ||
        total == 0 || alignment == 0) return false;
    *total = 0U;
    *alignment = sizeof(uint64_t);
    for (uint32_t index = 0U; index < count; ++index) {
        loaded_image_t *image = &images[index];
        const runtime_elf64_program_header_t *tls = 0;
        uint64_t size, phase, end;
        image->tls_offset = image->tls_size = image->tls_file_size = 0U;
        image->tls_source = image->tls_alignment = 0U;
        if (image->phdr_count > OS_RT_MAX_PROGRAMS) return false;
        for (uint16_t program = 0U; program < image->phdr_count; ++program) {
            if (image->phdrs[program].type != OS_RT_PT_TLS) continue;
            if (tls != 0) return false;
            tls = &image->phdrs[program];
        }
        if (tls == 0) continue;
        size = tls->alignment > 1U ? tls->alignment : 1U;
        if (tls->memory_size > limit || tls->file_size > tls->memory_size ||
            size > limit || (size & (size - 1U)) != 0U) return false;
        if (tls->memory_size == 0U) continue;
        phase = tls->virtual_address & (size - 1U);
        if (!add_u64(*total, tls->memory_size, &end) ||
            !add_u64(end, phase, &end) || !align_up_u64(end, size, &end) ||
            end < phase || end - phase > limit ||
            !add_u64(image->base, tls->virtual_address, &image->tls_source) ||
            !pointer_range(image, image->tls_source, tls->file_size)) return false;
        image->tls_offset = end - phase;
        image->tls_size = tls->memory_size;
        image->tls_file_size = tls->file_size;
        image->tls_alignment = size;
        *total = image->tls_offset;
        if (size > *alignment) *alignment = size;
    }
    return align_up_u64(*total, *alignment, total) && *total <= limit;
}

static bool setup_static_tls(const loaded_image_t *images, uint32_t count,
                             uint64_t total, uint64_t alignment) {
    uint64_t template_mapping = 0U, main_mapping = 0U;
    uint64_t template_base, main_base, fs_base, length, mapping_size;
    uint32_t modules = 0U;
    for (uint32_t index = 0U; index < count; ++index) {
        if (images[index].tls_size != 0U) ++modules;
    }
    if (modules == 0U) return true;
    if (modules == 1U && images[0].tls_size != 0U &&
        images[0].tls_alignment >= sizeof(uint64_t) &&
        (images[0].tls_source & (images[0].tls_alignment - 1U)) == 0U) {
        return setup_main_tls(&images[0]);
    }
    if (total == 0U || alignment == 0U ||
        !add_u64(total, alignment - 1U, &length) ||
        !add_u64(length, OS_RT_TLS_HEADER_WORDS * sizeof(uint64_t), &length) ||
        !align_up_u64(length, OS_RT_PAGE_SIZE, &mapping_size) ||
        !map_anonymous_any(mapping_size, OS_VM_READ | OS_VM_WRITE,
                           &template_mapping)) return false;
    zero_bytes((void *)(uintptr_t)template_mapping, mapping_size);
    if (!align_up_u64(template_mapping, alignment, &template_base)) goto failure;
    for (uint32_t index = 0U; index < count; ++index) {
        if (images[index].tls_file_size == 0U) continue;
        copy_bytes((void *)(uintptr_t)(template_base + total - images[index].tls_offset),
                   (const void *)(uintptr_t)images[index].tls_source,
                   (size_t)images[index].tls_file_size);
    }
    if (!map_anonymous_any(mapping_size, OS_VM_READ | OS_VM_WRITE,
                           &main_mapping) ||
        !align_up_u64(main_mapping, alignment, &main_base) ||
        !add_u64(main_base, total, &fs_base)) goto failure;
    zero_bytes((void *)(uintptr_t)main_mapping, mapping_size);
    copy_bytes((void *)(uintptr_t)main_base, (const void *)(uintptr_t)template_base,
               (size_t)total);
    uint64_t *tcb = (uint64_t *)(uintptr_t)fs_base;
    tcb[0] = fs_base;
    tcb[1] = total;
    tcb[2] = template_base;
    tcb[3] = OS_RT_TLS_MAGIC;
    tcb[4] = 0U;
    tcb[5] = alignment;
    if (runtime_syscall6(OS_SYS_THREAD_CONTEXT, OS_THREAD_CONTEXT_SET_FS,
                           fs_base, 0U, 0U, 0U, 0U) >= 0) return true;
failure:
    if (main_mapping != 0U) {
        (void)runtime_syscall3(OS_SYS_VM_UNMAP, main_mapping, mapping_size, 0U);
    }
    (void)runtime_syscall3(OS_SYS_VM_UNMAP, template_mapping, mapping_size, 0U);
    return false;
}

static bool load_library_file(const char *path, const char *name,
                              uint64_t *next_base, loaded_image_t *image) {
    runtime_elf64_header_t header = {0};
    runtime_elf64_program_header_t programs[OS_RT_MAX_PROGRAMS] = {0};
    uint64_t file_size;
    uint64_t minimum = UINT64_MAX;
    uint64_t maximum = 0U;
    uint64_t mapping_start;
    uint64_t load_end;
    uint64_t runtime_end;
    uint64_t mapping_length;
    uint64_t image_span;
    uint64_t base;
    os_handle_t handle = OS_INVALID_HANDLE;
    bool mapped = false;
    bool has_load = false;
    bool has_dynamic = false;
    if (path == 0 || name == 0 || next_base == 0 || image == 0 ||
        !file_stat_size(path, &file_size) || file_open(path, &handle) < 0) return false;
    if (!file_read_at(handle, 0U, &header, sizeof(header)) ||
        !valid_header(&header, file_size) || header.type != OS_RT_ET_DYN ||
        !read_program_headers(handle, &header, programs)) goto failure;
    for (uint16_t index = 0U; index < header.program_header_count; ++index) {
        const runtime_elf64_program_header_t *program = &programs[index];
        uint64_t end;
        if (program->type == OS_RT_PT_LOAD) {
            if (!valid_load_range(program, file_size)) goto failure;
            if (program->virtual_address < minimum) minimum = program->virtual_address;
            if (!add_u64(program->virtual_address, program->memory_size, &end) ||
                end > maximum) maximum = end;
            has_load = true;
        } else if (program->type == OS_RT_PT_DYNAMIC) {
            if (has_dynamic || program->memory_size < sizeof(runtime_elf64_dynamic_t)) {
                goto failure;
            }
            has_dynamic = true;
        }
    }
    if (!has_load || !has_dynamic || minimum == UINT64_MAX ||
        !align_up_u64(maximum, OS_RT_PAGE_SIZE, &load_end)) goto failure;
    minimum = align_down_u64(minimum, OS_RT_PAGE_SIZE);
    if (!align_up_u64(*next_base, OS_RT_LIBRARY_ALIGN, &mapping_start) ||
        mapping_start == 0U || mapping_start < minimum || load_end <= minimum) goto failure;
    image_span = load_end - minimum;
    if (!add_u64(mapping_start, image_span, &runtime_end) ||
        runtime_end <= mapping_start || runtime_end >= 0x0000800000000000ULL) goto failure;
    base = mapping_start - minimum;
    mapping_length = runtime_end - mapping_start;
    if (!map_anonymous(mapping_start, mapping_length, OS_VM_READ | OS_VM_WRITE)) goto failure;
    mapped = true;
    *image = (loaded_image_t){0};
    image->used = true;
    image->base = base;
    image->entry = base + header.entry;
    image->image_start = mapping_start;
    image->image_end = runtime_end;
    image->phdr_size = header.program_header_size;
    image->phdr_count = header.program_header_count;
    for (uint16_t index = 0U; index < image->phdr_count; ++index) {
        image->phdrs[index] = programs[index];
        if (programs[index].type == OS_RT_PT_PHDR) {
            image->phdr_address = base + programs[index].virtual_address;
        }
        if (programs[index].type == OS_RT_PT_DYNAMIC) {
            image->dynamic_address = base + programs[index].virtual_address;
            image->dynamic_size = programs[index].memory_size;
        }
    }
    if (!copy_string(image->name, sizeof(image->name), name)) goto failure;
    for (uint16_t index = 0U; index < image->phdr_count; ++index) {
        const runtime_elf64_program_header_t *program = &programs[index];
        uint64_t target;
        if (program->type != OS_RT_PT_LOAD || program->memory_size == 0U) continue;
        if (!add_u64(base, program->virtual_address, &target) ||
            !file_read_at(handle, program->offset, (void *)(uintptr_t)target,
                          program->file_size)) goto failure;
        if (program->memory_size > program->file_size) {
            zero_bytes((void *)(uintptr_t)(target + program->file_size),
                       program->memory_size - program->file_size);
        }
    }
    file_close(handle);
    if (!parse_dynamic(image)) {
        (void)unmap_image(image);
        return false;
    }
    if (!add_u64(runtime_end, OS_RT_LIBRARY_GAP, next_base)) {
        (void)unmap_image(image);
        return false;
    }
    return true;

failure:
    file_close(handle);
    if (mapped) (void)runtime_syscall3(OS_SYS_VM_UNMAP, mapping_start,
                                         mapping_length, 0U);
    return false;
}

static bool make_library_path(const char *name, char path[OS_RT_MAX_PATH]) {
    static const char prefix[] = "/lib/";
    size_t index = 0U;
    size_t name_index = 0U;
    if (name == 0 || path == 0 || name[0] == '\0') return false;
    if (name[0] == '/') return copy_string(path, OS_RT_MAX_PATH, name);
    while (prefix[index] != '\0') {
        if (index + 1U >= OS_RT_MAX_PATH) return false;
        path[index] = prefix[index];
        ++index;
    }
    while (index + 1U < OS_RT_MAX_PATH && name[name_index] != '\0') {
        path[index++] = name[name_index++];
    }
    if (name[name_index] != '\0') return false;
    path[index] = '\0';
    return true;
}

/* 绝对路径和 SONAME 依赖必须共用同一份 libc 状态。 */
static const char *canonical_library_name(const char *name) {
    if (same_string(name, "/lib/libsystemc.so.1")) return "libsystemc.so.1";
    return name;
}

static int32_t find_image(const loaded_image_t *images, uint32_t count,
                          const char *name) {
    for (uint32_t index = 1U; index < count; ++index) {
        if (images[index].used && same_string(images[index].name, name)) {
            return (int32_t)index;
        }
    }
    return -1;
}

static bool load_needed(loaded_image_t *images, uint32_t *count,
                        uint64_t *next_base, uint32_t image_index) {
    loaded_image_t *owner = &images[image_index];
    for (uint32_t needed_index = 0U; needed_index < owner->needed_count;
         ++needed_index) {
        uint64_t name_offset = owner->needed[needed_index];
        const char *name;
        char path[OS_RT_MAX_PATH];
        uint64_t name_length;
        if (name_offset >= owner->string_size ||
            owner->string_table > UINT64_MAX - name_offset) return false;
        name = (const char *)(uintptr_t)(owner->string_table + name_offset);
        if (!bounded_string(name, owner->string_size - name_offset, &name_length) ||
            name_length == 0U || name_length >= OS_RT_MAX_NAME) return false;
        name = canonical_library_name(name);
        if (find_image(images, *count, name) >= 0) continue;
        if (*count >= OS_RT_MAX_IMAGES || !make_library_path(name, path) ||
            !load_library_file(path, name, next_base, &images[*count])) return false;
        ++*count;
        if (!load_needed(images, count, next_base, *count - 1U)) return false;
    }
    return true;
}

static bool image_symbol(const loaded_image_t *image, uint32_t index,
                         const runtime_elf64_symbol_t **symbol) {
    uint64_t offset;
    uint64_t address;
    if (image == 0 || symbol == 0 || (uint64_t)index >= image->symbol_count) return false;
    offset = (uint64_t)index * sizeof(runtime_elf64_symbol_t);
    if (!add_u64(image->symbol_table, offset, &address) ||
        !pointer_range(image, address, sizeof(runtime_elf64_symbol_t))) return false;
    *symbol = (const runtime_elf64_symbol_t *)(uintptr_t)address;
    return true;
}

static bool symbol_name(const loaded_image_t *image,
                        const runtime_elf64_symbol_t *symbol,
                        const char **name) {
    uint64_t address;
    uint64_t unused_length;
    if (image == 0 || symbol == 0 || name == 0 || symbol->name >= image->string_size ||
        !add_u64(image->string_table, symbol->name, &address) ||
        !pointer_range(image, address, 1U) ||
        !bounded_string((const char *)(uintptr_t)address,
                        image->string_size - symbol->name, &unused_length)) return false;
    *name = (const char *)(uintptr_t)address;
    return true;
}

static bool symbol_value(const loaded_image_t *image, uint32_t index,
                         const char *wanted, uint64_t *value) {
    const runtime_elf64_symbol_t *symbol;
    const char *name;
    uint64_t address;
    if (!image_symbol(image, index, &symbol) || symbol->section == OS_RT_SHN_UNDEF ||
        !symbol_name(image, symbol, &name) || !same_string(name, wanted) ||
        !add_u64(image->base, symbol->value, &address)) return false;
    *value = address;
    return true;
}

static bool symbol_exported(const runtime_elf64_symbol_t *symbol) {
    uint32_t binding;
    uint32_t visibility;
    if (symbol == 0) return false;
    binding = runtime_elf64_symbol_bind(symbol->info);
    visibility = symbol->other & 3U;
    return (binding == OS_RT_STB_GLOBAL || binding == OS_RT_STB_WEAK ||
            binding == OS_RT_STB_GNU_UNIQUE) &&
           (visibility == 0U || visibility == 3U);
}

static bool resolve_symbol(const loaded_image_t *images, uint32_t count,
                           const loaded_image_t *owner, uint32_t symbol_index,
                           uint64_t *value) {
    const runtime_elf64_symbol_t *symbol;
    const char *name;
    uint32_t binding;
    uint64_t weak_value = 0U;
    bool weak_found = false;
    if (!image_symbol(owner, symbol_index, &symbol) ||
        !symbol_name(owner, symbol, &name)) return false;
    binding = runtime_elf64_symbol_bind(symbol->info);
    if ((symbol->other & 3U) != 0U || binding == OS_RT_STB_LOCAL) {
        if (symbol->section == OS_RT_SHN_UNDEF) return binding == OS_RT_STB_WEAK;
        return symbol_value(owner, symbol_index, name, value);
    }
    for (uint32_t index = 0U; index < count; ++index) {
        for (uint32_t candidate = 0U; candidate < images[index].symbol_count; ++candidate) {
            const runtime_elf64_symbol_t *candidate_symbol;
            if (!image_symbol(&images[index], candidate, &candidate_symbol) ||
                !symbol_exported(candidate_symbol)) continue;
            if (symbol_value(&images[index], candidate, name, value)) {
                if (runtime_elf64_symbol_bind(candidate_symbol->info) ==
                    OS_RT_STB_GLOBAL ||
                    runtime_elf64_symbol_bind(candidate_symbol->info) ==
                    OS_RT_STB_GNU_UNIQUE) return true;
                weak_value = *value;
                weak_found = true;
            }
        }
    }
    if (weak_found) {
        *value = weak_value;
        return true;
    }
    if (binding == OS_RT_STB_WEAK) {
        *value = 0U;
        return true;
    }
    return false;
}

static bool resolve_static_tls(const loaded_image_t *images, uint32_t count,
                               const loaded_image_t *owner, uint32_t symbol_index,
                               uint64_t *value, uint64_t *offset) {
    const runtime_elf64_symbol_t *reference, *selected = 0;
    const loaded_image_t *definition = 0;
    const char *name;
    if (!image_symbol(owner, symbol_index, &reference) ||
        !symbol_name(owner, reference, &name)) return false;
    uint32_t binding = runtime_elf64_symbol_bind(reference->info);
    if ((reference->other & 3U) != 0U || binding == OS_RT_STB_LOCAL) {
        if (reference->section != OS_RT_SHN_UNDEF) {
            selected = reference;
            definition = owner;
        }
    } else {
        for (uint32_t index = 0U; index < count; ++index) {
            for (uint32_t candidate = 0U; candidate < images[index].symbol_count;
                 ++candidate) {
                const runtime_elf64_symbol_t *symbol;
                const char *candidate_name;
                if (!image_symbol(&images[index], candidate, &symbol) ||
                    symbol->section == OS_RT_SHN_UNDEF || !symbol_exported(symbol) ||
                    !symbol_name(&images[index], symbol, &candidate_name) ||
                    !same_string(candidate_name, name)) continue;
                selected = symbol;
                definition = &images[index];
                if (runtime_elf64_symbol_bind(symbol->info) != OS_RT_STB_WEAK) {
                    goto found;
                }
            }
        }
    }
found:
    if (selected == 0) {
        if (binding != OS_RT_STB_WEAK) return false;
        *value = *offset = 0U;
        return true;
    }
    if ((selected->info & 15U) != OS_RT_STT_TLS || definition->tls_offset == 0U ||
        selected->value >= definition->tls_size ||
        selected->size > definition->tls_size - selected->value) return false;
    *value = selected->value;
    *offset = definition->tls_offset;
    return true;
}

static bool relocation_target(const loaded_image_t *image, uint64_t offset,
                              uint64_t length, uint64_t *target) {
    if (image == 0 || target == 0 || !add_u64(image->base, offset, target) ||
        !pointer_range(image, *target, length) ||
        !image_contains_writable(image, *target, length)) return false;
    return true;
}

static bool apply_relocation_table(const loaded_image_t *images, uint32_t count,
                                   const loaded_image_t *image, uint64_t address,
                                   uint64_t size) {
    uint64_t entry_size = image->rela_entry_size == 0U ?
                          sizeof(runtime_elf64_rela_t) : image->rela_entry_size;
    if (size == 0U) return true;
    if (address == 0U || entry_size != sizeof(runtime_elf64_rela_t) ||
        size % entry_size != 0U || !pointer_range(image, address, size)) return false;
    for (uint64_t offset = 0U; offset < size; offset += entry_size) {
        const runtime_elf64_rela_t *rela =
            (const runtime_elf64_rela_t *)(uintptr_t)(address + offset);
        uint32_t type = runtime_elf64_relocation_type(rela->info);
        uint32_t symbol_index = runtime_elf64_relocation_symbol(rela->info);
        uint64_t target;
        uint64_t symbol = 0U;
        uint64_t value;
        if (type == OS_RT_R_X86_64_NONE) continue;
        /* 解析器可能调用其他 DSO 并读取 TLS。将其延后到所有常规重定位、TLS 和
         * 可执行映射准备就绪后再执行。 */
        if (type == OS_RT_R_X86_64_IRELATIVE) {
            if (symbol_index != 0U ||
                !relocation_target(image, rela->offset, sizeof(uint64_t), &target)) {
                return false;
            }
            continue;
        }
        if (type == OS_RT_R_X86_64_TPOFF64) {
            uint64_t tls_offset;
            if (!relocation_target(image, rela->offset, sizeof(uint64_t), &target)) {
                return false;
            }
            if (symbol_index == 0U) {
                if (image->tls_offset == 0U) return false;
                symbol = 0U;
                tls_offset = image->tls_offset;
            } else if (!resolve_static_tls(images, count, image, symbol_index,
                                            &symbol, &tls_offset)) return false;
            *(uint64_t *)(uintptr_t)target = symbol + (uint64_t)rela->addend - tls_offset;
            continue;
        }
        if (type == OS_RT_R_X86_64_32 || type == OS_RT_R_X86_64_32S) {
            if (!relocation_target(image, rela->offset, sizeof(uint32_t), &target)) return false;
        } else if (!relocation_target(image, rela->offset, sizeof(uint64_t), &target)) {
            return false;
        }
        if (type != OS_RT_R_X86_64_RELATIVE &&
            !resolve_symbol(images, count, image, symbol_index, &symbol)) return false;
        switch (type) {
        case OS_RT_R_X86_64_RELATIVE:
            value = image->base + (uint64_t)rela->addend;
            *(uint64_t *)(uintptr_t)target = value;
            break;
        case OS_RT_R_X86_64_64:
        case OS_RT_R_X86_64_GLOB_DAT:
        case OS_RT_R_X86_64_JUMP_SLOT:
            *(uint64_t *)(uintptr_t)target = symbol + (uint64_t)rela->addend;
            break;
        case OS_RT_R_X86_64_PC32:
        case OS_RT_R_X86_64_PLT32: {
            int64_t relative = (int64_t)(symbol + (uint64_t)rela->addend - target);
            if (relative < INT32_MIN || relative > INT32_MAX) return false;
            *(uint32_t *)(uintptr_t)target = (uint32_t)(int32_t)relative;
            break;
        }
        case OS_RT_R_X86_64_32:
            value = symbol + (uint64_t)rela->addend;
            if (value > UINT32_MAX) return false;
            *(uint32_t *)(uintptr_t)target = (uint32_t)value;
            break;
        case OS_RT_R_X86_64_32S:
            value = symbol + (uint64_t)rela->addend;
            if ((int64_t)value < INT32_MIN || (int64_t)value > INT32_MAX) return false;
            *(uint32_t *)(uintptr_t)target = (uint32_t)value;
            break;
        default:
            return false;
        }
    }
    return true;
}

static bool apply_relocations(const loaded_image_t *images, uint32_t count,
                              const loaded_image_t *image) {
    if (!apply_relocation_table(images, count, image, image->rela, image->rela_size)) {
        return false;
    }
    if (image->jmprel_size != 0U && image->plt_reloc_type != OS_RT_DT_RELA_TYPE) {
        return false;
    }
    return apply_relocation_table(images, count, image, image->jmprel,
                                  image->jmprel_size);
}

static bool protect_image(const loaded_image_t *image) {
    for (uint16_t index = 0U; index < image->phdr_count; ++index) {
        const runtime_elf64_program_header_t *program = &image->phdrs[index];
        uint64_t start;
        uint64_t end;
        if (program->type != OS_RT_PT_LOAD) continue;
        if (program->memory_size == 0U ||
            !add_u64(image->base, program->virtual_address, &start) ||
            !add_u64(start, program->memory_size, &end)) {
            runtime_diag("OS_DIAG_RT_PROTECT_CALC\r\n");
            return false;
        }
        start = align_down_u64(start, OS_RT_PAGE_SIZE);
        if (!align_up_u64(end, OS_RT_PAGE_SIZE, &end) || end <= start) {
            runtime_diag("OS_DIAG_RT_PROTECT_ALIGN\r\n");
            return false;
        }
        if (!protect_range(start, end - start, program->flags)) {
            runtime_diag("OS_DIAG_RT_PROTECT_SYSCALL\r\n");
            return false;
        }
    }
    return true;
}

typedef void (*init_function_t)(void);

static bool apply_indirect_relocation_table(const loaded_image_t *image,
                                            uint64_t address, uint64_t size) {
    uint64_t entry_size = image->rela_entry_size == 0U ?
                          sizeof(runtime_elf64_rela_t) : image->rela_entry_size;
    if (size == 0U) return true;
    if (address == 0U || entry_size != sizeof(runtime_elf64_rela_t) ||
        size % entry_size != 0U || !pointer_range(image, address, size)) return false;
    for (uint64_t offset = 0U; offset < size; offset += entry_size) {
        const runtime_elf64_rela_t *rela =
            (const runtime_elf64_rela_t *)(uintptr_t)(address + offset);
        uint64_t target, resolver;
        bool executable = false;
        if (runtime_elf64_relocation_type(rela->info) != OS_RT_R_X86_64_IRELATIVE) {
            continue;
        }
        if (runtime_elf64_relocation_symbol(rela->info) != 0U || rela->addend < 0 ||
            !relocation_target(image, rela->offset, sizeof(uint64_t), &target) ||
            !add_u64(image->base, (uint64_t)rela->addend, &resolver)) return false;
        for (uint16_t index = 0U; index < image->phdr_count; ++index) {
            const runtime_elf64_program_header_t *program = &image->phdrs[index];
            uint64_t start;
            if (program->type == OS_RT_PT_LOAD && (program->flags & OS_RT_PF_X) &&
                add_u64(image->base, program->virtual_address, &start) &&
                resolver >= start && resolver - start < program->memory_size) {
                executable = true;
                break;
            }
        }
        if (!executable) return false;
        *(uint64_t *)(uintptr_t)target =
            ((uint64_t (*)(void))(uintptr_t)resolver)();
    }
    return true;
}

static bool apply_indirect_relocations(const loaded_image_t *image) {
    return apply_indirect_relocation_table(image, image->rela, image->rela_size) &&
           apply_indirect_relocation_table(image, image->jmprel, image->jmprel_size);
}

static bool call_initializers(const loaded_image_t *image) {
    if (image->init != 0U) ((init_function_t)(uintptr_t)image->init)();
    if (image->init_array_size % sizeof(uint64_t) != 0U ||
        (image->init_array_size != 0U &&
         !pointer_range(image, image->init_array, image->init_array_size))) return false;
    for (uint64_t offset = 0U; offset < image->init_array_size; offset += sizeof(uint64_t)) {
        uint64_t function = *(const uint64_t *)(uintptr_t)(image->init_array + offset);
        if (function != 0U) ((init_function_t)(uintptr_t)function)();
    }
    return true;
}

uint64_t runtime_linker_main(uint64_t initial_stack) {
    loaded_image_t images[OS_RT_MAX_IMAGES] = {0};
    uint32_t image_count = 1U;
    uint64_t next_base = OS_RT_LIBRARY_BASE;
    uint64_t library_floor;
    uint64_t tls_total, tls_alignment;
    if (!initialize_main_image(initial_stack, &images[0])) {
        runtime_diag("OS_DIAG_RT_FAIL main-image\r\n");
        return 0U;
    }
    /* Chrome 这样的大型 PIE 可能超出历史固定库基址。将 DSO 放在主映像之后，
     * 避免其固定映射与可执行文件的 PT_LOAD 范围冲突。 */
    if (images[0].image_end >= next_base) {
        if (!add_u64(images[0].image_end, OS_RT_LIBRARY_GAP,
                     &library_floor) ||
            !align_up_u64(library_floor, OS_RT_LIBRARY_ALIGN,
                          &next_base)) {
            runtime_diag("OS_DIAG_RT_FAIL library-base\r\n");
            return 0U;
        }
    }
    if (!load_needed(images, &image_count, &next_base, 0U)) {
        runtime_diag("OS_DIAG_RT_FAIL needed\r\n");
        return 0U;
    }
    if (!plan_static_tls(images, image_count, &tls_total, &tls_alignment)) {
        runtime_diag("OS_DIAG_RT_FAIL tls-layout\r\n");
        return 0U;
    }
    for (uint32_t index = 0U; index < image_count; ++index) {
        if (!apply_relocations(images, image_count, &images[index])) {
            runtime_diag("OS_DIAG_RT_FAIL reloc\r\n");
            return 0U;
        }
    }
    if (!setup_static_tls(images, image_count, tls_total, tls_alignment)) {
        runtime_diag("OS_DIAG_RT_FAIL tls\r\n");
        return 0U;
    }
    for (uint32_t index = 1U; index < image_count; ++index) {
        if (!protect_image(&images[index])) {
            runtime_diag("OS_DIAG_RT_FAIL protect\r\n");
            return 0U;
        }
    }
    for (uint32_t index = 0U; index < image_count; ++index) {
        if (!apply_indirect_relocations(&images[index])) {
            runtime_diag("OS_DIAG_RT_FAIL indirect-reloc\r\n");
            return 0U;
        }
    }
    for (uint32_t index = image_count; index > 1U; --index) {
        if (!call_initializers(&images[index - 1U])) {
            runtime_diag("OS_DIAG_RT_FAIL init\r\n");
            return 0U;
        }
    }
    return images[0].entry;
}
