#pragma once

#include <kernel/elf_loader.h>

/* REFACTOR_P5_EXEC_INTERNAL: private ELF image model shared by loader tests. */

#define ELF_CLASS_64          2U
#define ELF_DATA_LSB          1U
#define ELF_VERSION_CURRENT   1U
#define ELF_TYPE_EXEC         2U
#define ELF_TYPE_DYN          3U
#define ELF_MACHINE_X86_64    62U
#define ELF_PT_LOAD           1U
#define ELF_PT_DYNAMIC        2U
#define ELF_PT_INTERP         3U
#define ELF_PT_PHDR           6U
#define ELF_PF_EXEC           1U
#define ELF_PF_WRITE          2U
#define ELF_PF_READ           4U
#define ELF_MAX_PROGRAMS      128U
/* Chrome 的文本段大于原有 256 MiB 加载器限制。VFS exec 路径现在以有界分块方式读取文件，因此该限制
 * 作用于单个已映射 PT_LOAD，而不再要求分配同等大小的连续内核内存。 */
#define ELF_MAX_SEGMENT_SIZE  (1ULL * 1024ULL * 1024ULL * 1024ULL)
#define ELF_PIE_BIAS          0x0000000040000000ULL
#define ELF_INTERP_BIAS       0x0000000020000000ULL
#define ELF_INTERP_MAX        4096U

typedef struct __attribute__((packed)) {
    uint8_t ident[16];
    uint16_t type;
    uint16_t machine;
    uint32_t version;
    uint64_t entry;
    uint64_t program_header_offset;
    uint64_t section_header_offset;
    uint32_t flags;
    uint16_t header_size;
    uint16_t program_header_size;
    uint16_t program_header_count;
    uint16_t section_header_size;
    uint16_t section_header_count;
    uint16_t section_name_index;
} elf64_header_t;

typedef struct __attribute__((packed)) {
    uint32_t type;
    uint32_t flags;
    uint64_t offset;
    uint64_t virtual_address;
    uint64_t physical_address;
    uint64_t file_size;
    uint64_t memory_size;
    uint64_t alignment;
} elf64_program_header_t;

typedef struct elf_validation {
    const elf64_header_t *header;
    uint64_t bias;
    uint64_t entry;
    uint64_t phdr_address;
    uint64_t interp_offset;
    uint64_t interp_size;
    uint64_t load_end;
    bool has_interpreter;
} elf_validation_t;

/* 运行时测试使用的 PROCESS_EXEC 失败进度快照。 */
uint32_t process_exec_debug_stage(void);
uint32_t process_exec_debug_status(void);
void process_exec_debug_reset(void);
