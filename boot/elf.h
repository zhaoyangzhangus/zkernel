/*
 * elf.h —— 引导程序解析 ELF64 所需的最小定义
 *
 * 只包含装载一个可执行文件真正用到的部分：文件头、程序头，以及 PT_LOAD。
 * （节区头/symbol/重定位表都不需要——内核是静态链接好的可执行文件。）
 */
#ifndef __BOOT_ELF_H__
#define __BOOT_ELF_H__

#include <stdint.h>

/* e_ident 里各字段的下标 */
#define EI_CLASS  4
#define EI_DATA   5

#define ELF_CLASS_64       2
#define ELF_DATA_2LSB      1
#define ELF_TYPE_EXEC      2
#define ELF_MACHINE_X86_64 62

/* 程序头里 p_type 的取值，我们只关心 PT_LOAD */
#define PT_LOAD 1

/* 程序头里 p_flags 的位 */
#define PF_X 1
#define PF_W 2
#define PF_R 4

/* ELF64 文件头，固定 64 字节，各字段位置见 ELF 规范 */
typedef struct {
    uint8_t  e_ident[16];    /* 魔数、位数、字节序…… */
    uint16_t e_type;         /* ET_EXEC / ET_DYN / ET_REL */
    uint16_t e_machine;      /* EM_X86_64 */
    uint32_t e_version;
    uint64_t e_entry;        /* 入口虚拟地址，装载完就跳到这里 */
    uint64_t e_phoff;        /* 程序头表在文件里的偏移 */
    uint64_t e_shoff;        /* 节区头表偏移（用不到） */
    uint32_t e_flags;
    uint16_t e_ehsize;       /* 本结构体大小，应为 64 */
    uint16_t e_phentsize;    /* 每个程序头的大小，应为 56 */
    uint16_t e_phnum;        /* 程序头个数 */
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
} elf64_ehdr_t;

/* ELF64 程序头，固定 56 字节 */
typedef struct {
    uint32_t p_type;         /* PT_LOAD 表示这一段要装进内存 */
    uint32_t p_flags;
    uint64_t p_offset;       /* 数据在文件里的偏移 */
    uint64_t p_vaddr;        /* 链接时的虚拟地址 */
    uint64_t p_paddr;        /* 物理地址；本内核没有分页偏移，等于 p_vaddr */
    uint64_t p_filesz;       /* 文件里有多少字节（可能为 0） */
    uint64_t p_memsz;        /* 内存里占多少字节；多出 p_filesz 的部分要清零（.bss） */
    uint64_t p_align;
} elf64_phdr_t;

#endif /* __BOOT_ELF_H__ */
