#pragma once

#include <stdint.h>

/* Chromium 异步信号安全符号化器使用的最小 x86_64 ELF ABI。 */
typedef uint16_t Elf64_Half;
typedef int16_t Elf64_SHalf;
typedef uint32_t Elf64_Word;
typedef int32_t Elf64_Sword;
typedef uint64_t Elf64_Xword;
typedef int64_t Elf64_Sxword;
typedef uint64_t Elf64_Addr;
typedef uint64_t Elf64_Off;
typedef uint16_t Elf64_Section;
typedef uint16_t Elf64_Versym;

typedef struct {
    unsigned char e_ident[16];
    Elf64_Half e_type;
    Elf64_Half e_machine;
    Elf64_Word e_version;
    Elf64_Addr e_entry;
    Elf64_Off e_phoff;
    Elf64_Off e_shoff;
    Elf64_Word e_flags;
    Elf64_Half e_ehsize;
    Elf64_Half e_phentsize;
    Elf64_Half e_phnum;
    Elf64_Half e_shentsize;
    Elf64_Half e_shnum;
    Elf64_Half e_shstrndx;
} Elf64_Ehdr;

typedef struct {
    Elf64_Word p_type;
    Elf64_Word p_flags;
    Elf64_Off p_offset;
    Elf64_Addr p_vaddr;
    Elf64_Addr p_paddr;
    Elf64_Xword p_filesz;
    Elf64_Xword p_memsz;
    Elf64_Xword p_align;
} Elf64_Phdr;

typedef struct {
    Elf64_Word sh_name;
    Elf64_Word sh_type;
    Elf64_Xword sh_flags;
    Elf64_Addr sh_addr;
    Elf64_Off sh_offset;
    Elf64_Xword sh_size;
    Elf64_Word sh_link;
    Elf64_Word sh_info;
    Elf64_Xword sh_addralign;
    Elf64_Xword sh_entsize;
} Elf64_Shdr;

typedef struct {
    Elf64_Word st_name;
    unsigned char st_info;
    unsigned char st_other;
    Elf64_Half st_shndx;
    Elf64_Addr st_value;
    Elf64_Xword st_size;
} Elf64_Sym;

typedef struct {
    Elf64_Sxword d_tag;
    union {
        Elf64_Xword d_val;
        Elf64_Addr d_ptr;
    } d_un;
} Elf64_Dyn;

/* build-id 读取器使用的 ELF note 头部。 */
typedef struct {
    Elf64_Word n_namesz;
    Elf64_Word n_descsz;
    Elf64_Word n_type;
} Elf64_Nhdr;

typedef struct {
    Elf64_Half vd_version;
    Elf64_Half vd_flags;
    Elf64_Half vd_ndx;
    Elf64_Half vd_cnt;
    Elf64_Word vd_hash;
    Elf64_Word vd_aux;
    Elf64_Word vd_next;
} Elf64_Verdef;

typedef struct {
    Elf64_Word vda_name;
    Elf64_Word vda_next;
} Elf64_Verdaux;

typedef struct {
    uint64_t a_type;
    union {
        uint64_t a_val;
        void *a_ptr;
    } a_un;
} Elf64_auxv_t;

#ifndef ElfW
#define ElfW(type) Elf64_##type
#endif

#ifndef ELFMAG
#define ELFMAG "\177ELF"
#endif
#ifndef SELFMAG
#define SELFMAG 4
#endif

#ifndef ELFMAG0
#define ELFMAG0 0x7f
#define ELFMAG1 'E'
#define ELFMAG2 'L'
#define ELFMAG3 'F'
#endif

#ifndef EI_MAG0
#define EI_MAG0 0
#define EI_MAG1 1
#define EI_MAG2 2
#define EI_MAG3 3
#define EI_CLASS 4
#define EI_DATA 5
#define EI_VERSION 6
#define EI_OSABI 7
#define EI_ABIVERSION 8
#define EI_PAD 9
#endif

#ifndef ELFCLASS64
#define ELFCLASSNONE 0
#define ELFCLASS32 1
#define ELFCLASS64 2
#endif

#ifndef ELFDATA2LSB
#define ELFDATANONE 0
#define ELFDATA2LSB 1
#define ELFDATA2MSB 2
#endif

#ifndef EV_CURRENT
#define EV_NONE 0
#define EV_CURRENT 1
#endif

#ifndef ET_NONE
#define ET_NONE 0
#endif
#ifndef ET_REL
#define ET_REL 1
#endif
#ifndef ET_EXEC
#define ET_EXEC 2
#endif
#ifndef ET_DYN
#define ET_DYN 3
#endif
#ifndef ET_CORE
#define ET_CORE 4
#endif

#ifndef PT_LOAD
#define PT_LOAD 1
#endif
#ifndef PT_NOTE
#define PT_NOTE 4
#endif
#ifndef PF_X
#define PF_X (1U << 0)
#endif
#ifndef PT_DYNAMIC
#define PT_DYNAMIC 2
#endif

#ifndef SHT_NULL
#define SHT_NULL 0
#endif
#ifndef SHT_SYMTAB
#define SHT_SYMTAB 2
#endif
#ifndef SHT_STRTAB
#define SHT_STRTAB 3
#endif
#ifndef SHT_DYNSYM
#define SHT_DYNSYM 11
#endif

#ifndef SHN_UNDEF
#define SHN_UNDEF 0
#define SHN_LORESERVE 0xff00
#endif

#ifndef STB_LOCAL
#define STB_LOCAL 0
#define STB_GLOBAL 1
#define STB_WEAK 2
#endif
#ifndef STT_NOTYPE
#define STT_NOTYPE 0
#define STT_OBJECT 1
#define STT_FUNC 2
#define STT_SECTION 3
#define STT_FILE 4
#define STT_COMMON 5
#define STT_TLS 6
#endif

#ifndef ELF64_ST_BIND
#define ELF64_ST_BIND(info) ((info) >> 4)
#define ELF64_ST_TYPE(info) ((info) & 0xf)
#endif
#ifndef ELF_ST_TYPE
#define ELF_ST_TYPE(info) ELF64_ST_TYPE(info)
#endif

#ifndef DT_NULL
#define DT_NULL 0
#define DT_HASH 4
#define DT_STRTAB 5
#define DT_SYMTAB 6
#define DT_STRSZ 10
#define DT_SONAME 14
#define DT_GNU_HASH 0x6ffffef5
#define DT_VERSYM 0x6ffffff0
#define DT_VERDEF 0x6ffffffc
#define DT_VERDEFNUM 0x6ffffffd
#endif

#ifndef NT_GNU_BUILD_ID
#define NT_GNU_BUILD_ID 3
#endif
