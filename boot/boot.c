/*
 * boot.c —— 一个最小的 x86_64 UEFI 引导程序
 *
 * 流程：
 *   1. 关掉固件看门狗
 *   2. 通过 LoadedImage → DeviceHandle → SimpleFileSystem 打开 ESP 根目录
 *   3. 读出 \kernel.elf，按它的程序头（PT_LOAD 段）装载到各自的物理地址
 *   4. 记录 GOP 帧缓冲信息
 *   5. 为内核高半区 VMA 准备最小临时页表
 *   6. 获取 UEFI 内存映射，调用 ExitBootServices 离开 Boot Services
 *   7. 挂入高半区映射并跳到 ELF e_entry，唯一参数是 BOOT_INFO*
 *
 * 编译（见 Makefile）：
 *   clang --target=x86_64-unknown-windows -ffreestanding ... -c boot.c
 *   lld-link /subsystem:efi_application /entry:efi_main ...
 *
 * ELF 明确区分物理装载地址 p_paddr 与高半区链接地址 p_vaddr。
 * loader 按 p_paddr 拷贝映像；退出 Boot Services 后在固件 PML4 上挂入
 * 一个最小的 2M huge-page 高半区分支，再跳到高地址 e_entry。
 *
 * 相比读扁平二进制（objcopy -O binary），按 ELF 装载多了两件事：
 *   - 段的地址和权限来自程序头，不用在两边硬编码同一个常量
 *   - p_memsz > p_filesz 的部分（.bss）由装载器清零，内核不必自己清
 *
 * ABI 提醒：本文件是按微软 x64 调用约定编译的（UEFI 的要求），
 * 而内核是用 gcc/clang 按 System V 约定编译的。所以调用内核入口时
 * 必须显式写成 sysv_abi（参数在 rdi），否则参数会跑到 rcx 里去。
 *
 * 注意：所有运行时输出都用 ASCII，因为 OVMF 的 GOP 控制台字体只有 ASCII 字形。
 */
#include "efi.h"
#include "elf.h"
#include "bootinfo.h"

#define KERNEL_FILE_NAME L"\\kernel.elf"
#define PAGE_SIZE        0x1000ULL
#define PAGE_2M          0x200000ULL

#define X86_PTE_PRESENT  0x001ULL
#define X86_PTE_WRITE    0x002ULL
#define X86_PTE_LARGE    0x080ULL
#define X86_PTE_ADDR     0x000FFFFFFFFFF000ULL
#define X86_CR4_LA57     (1ULL << 12)

/*
 * loader 自己的 bootstrap 页表 arena。
 * 512 页 = 2 MiB；对当前 QEMU/早期内核足够宽裕，而且所有页都在
 * 最终 UEFI memory map 中标记为 EfiLoaderData。
 */
#define BOOT_PT_ARENA_PAGES 512ULL

/* ELF 头/程序头的大小是规范定死的，写错说明结构体没对齐 */
_Static_assert(sizeof(elf64_ehdr_t) == 64, "unexpected ELF64 header size");
_Static_assert(sizeof(elf64_phdr_t) == 56, "unexpected ELF64 program header size");

/* 内存映射缓冲区初始大小；固件报告不够大时会自动扩容 */
#define MMAP_BUF_SIZE (64 * 1024)

/*
 * bootinfo.h 给内核用的描述符必须和 UEFI 的结构逐字节一致。
 * 注意：这只是“字段布局一致”，固件实际返回的 DescriptorSize 可能更大
 * （OVMF 就是 48，规范里的结构是 40），所以两边遍历都必须按
 * GetMemoryMap 给出的步长走，不能直接当结构体数组索引。
 */
_Static_assert(sizeof(EFI_MEMORY_DESCRIPTOR) == sizeof(BOOT_MEMORY_DESCRIPTOR),
               "BOOT_MEMORY_DESCRIPTOR must match EFI_MEMORY_DESCRIPTOR");
_Static_assert(offsetof(EFI_MEMORY_DESCRIPTOR, NumberOfPages) == offsetof(BOOT_MEMORY_DESCRIPTOR, number_of_pages),
               "field offsets must match");

#define DIV_ROUND_UP(a, b) (((a) + (b) - 1) / (b))
#define ALIGN_DOWN(a, b)   ((a) & ~((b) - 1))

/* 内核是 SysV ABI 编译的，不能用本文件默认的 MS ABI 去调用 */
typedef void (__attribute__((sysv_abi)) *kernel_entry_t)(BOOT_INFO *bootinfo);

static EFI_SYSTEM_TABLE  *ST;
static EFI_BOOT_SERVICES *BS;
static BOOT_INFO          g_bootinfo;

/*
 * loader 自己持有的 bootstrap 页表。
 * 不读取、不修改固件 CR3；ExitBootServices 后直接切到 g_boot_root。
 */
static EFI_PHYSICAL_ADDRESS g_boot_pt_arena;
static UINTN g_boot_pt_used;
static EFI_PHYSICAL_ADDRESS g_boot_root;

/* ================================================================== */
/* 控制台输出辅助                                                      */
/* ================================================================== */
static void con_puts(const CHAR16 *str)
{
    ST->ConOut->OutputString(ST->ConOut, (CHAR16 *)str);
}

/* 以十六进制打印，digits 为位数（不含 0x 前缀） */
static void con_hex(UINT64 value, int digits)
{
    static const CHAR16 hexchar[] = L"0123456789ABCDEF";
    CHAR16 buf[17];
    int i;

    if (digits < 1)  digits = 1;
    if (digits > 16) digits = 16;

    for (i = 0; i < digits; i++)
        buf[i] = hexchar[(value >> (4 * (digits - 1 - i))) & 0xF];
    buf[digits] = L'\0';

    con_puts(buf);
}

static void con_hex64(UINT64 value)
{
    con_puts(L"0x");
    con_hex(value, 16);
}

static void con_dec(UINT64 value)
{
    CHAR16 buf[21];
    int i = 20;

    buf[i] = L'\0';
    if (value == 0) {
        con_puts(L"0");
        return;
    }
    while (value != 0 && i > 0) {
        buf[--i] = (CHAR16)(L'0' + (value % 10));
        value /= 10;
    }
    con_puts(&buf[i]);
}

static inline void debug_char(UINT8 c)
{
    __asm__ volatile("outb %0, $0xe9" :: "a"(c));
}

static void report_error(const CHAR16 *what, EFI_STATUS status)
{
    con_puts(L"\r\n[BOOT] ERROR: ");
    con_puts(what);
    con_puts(L"\r\n[BOOT] status = ");
    con_hex64(status);
    con_puts(L"\r\n");
}

/* ================================================================== */
/* 打开 ESP 上的内核文件                                               */
/* ================================================================== */
static EFI_STATUS open_kernel_file(EFI_HANDLE image, EFI_FILE_PROTOCOL **out_file, UINT64 *out_size)
{
    static UINT8 info_buf[sizeof(EFI_FILE_INFO) + 256] __attribute__((aligned(8)));
    EFI_LOADED_IMAGE_PROTOCOL       *loaded_image = NULL;
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *fs           = NULL;
    EFI_FILE_PROTOCOL               *root         = NULL;
    EFI_FILE_PROTOCOL               *file         = NULL;
    EFI_FILE_INFO                   *info;
    UINTN info_size = sizeof(info_buf);
    EFI_STATUS status;
    EFI_GUID guid_loaded_image = EFI_LOADED_IMAGE_PROTOCOL_GUID;
    EFI_GUID guid_file_system  = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;
    EFI_GUID guid_file_info    = EFI_FILE_INFO_GUID;

    /* 拿到本映像的 LoadedImage，它的 DeviceHandle 就是 ESP */
    status = BS->HandleProtocol(image, &guid_loaded_image, (VOID **)&loaded_image);
    if (EFI_ERROR(status)) {
        report_error(L"HandleProtocol(LoadedImage) failed", status);
        return status;
    }

    status = BS->HandleProtocol(loaded_image->DeviceHandle, &guid_file_system, (VOID **)&fs);
    if (EFI_ERROR(status)) {
        report_error(L"no SimpleFileSystem on this device", status);
        return status;
    }

    status = fs->OpenVolume(fs, &root);
    if (EFI_ERROR(status)) {
        report_error(L"OpenVolume failed", status);
        return status;
    }

    status = root->Open(root, &file, KERNEL_FILE_NAME, EFI_FILE_MODE_READ, 0);
    if (EFI_ERROR(status)) {
        report_error(L"cannot open \\kernel.elf", status);
        root->Close(root);
        return status;
    }

    status = file->GetInfo(file, &guid_file_info, &info_size, info_buf);
    if (EFI_ERROR(status)) {
        report_error(L"GetInfo failed", status);
        file->Close(file);
        root->Close(root);
        return status;
    }

    info      = (EFI_FILE_INFO *)info_buf;
    *out_file = file;
    *out_size = info->FileSize;

    root->Close(root);   /* 根目录不需要了，文件句柄保持打开 */
    return EFI_SUCCESS;
}

/* ================================================================== */
/* 装载 ELF 格式的内核                                                 */
/* ================================================================== */
/* 引导程序里没有 libc，这两个小工具自己写 */
static void mem_copy(void *dst, const void *src, UINTN count)
{
    UINT8 *d = (UINT8 *)dst;
    const UINT8 *s = (const UINT8 *)src;

    while (count-- != 0)
        *d++ = *s++;
}

static void mem_set(void *dst, UINT8 value, UINTN count)
{
    UINT8 *d = (UINT8 *)dst;

    while (count-- != 0)
        *d++ = value;
}

/*
 * 读出 \kernel.elf 并按它的程序头装载：
 *   - 只处理 PT_LOAD 段（其它类型如 GNU_STACK 装载时忽略）
 *   - 每段从文件偏移 p_offset 拷 p_filesz 字节到物理地址 p_paddr
 *   - p_memsz 比 p_filesz 多出的部分（.bss）清零
 *   - 所有段覆盖的地址范围用一次 AllocatePages(AllocateAddress) 整体占下来，
 *     避免多次申请时各自成功、合起来却互相重叠
 * 返回并将内核入口（e_entry）记进 BOOT_INFO。
 */
static EFI_STATUS load_kernel(EFI_HANDLE image, BOOT_INFO *bi)
{
    EFI_FILE_PROTOCOL *file = NULL;
    VOID          *file_buf = NULL;
    elf64_ehdr_t  *ehdr;
    UINT64 file_size, span_start, span_end;
    UINT64 virt_span_start, virt_span_end, load_delta = 0;
    UINTN read_size, pages, i;
    BOOLEAN have_load_delta = EFI_FALSE;
    EFI_PHYSICAL_ADDRESS addr;
    EFI_STATUS status;

    status = open_kernel_file(image, &file, &file_size);
    if (EFI_ERROR(status))
        return status;

    con_puts(L"[BOOT] kernel file: \\kernel.elf, size = ");
    con_dec(file_size);
    con_puts(L" bytes\r\n");

    if (file_size < sizeof(elf64_ehdr_t)) {
        report_error(L"kernel.elf is too small to be an ELF", EFI_LOAD_ERROR);
        file->Close(file);
        return EFI_LOAD_ERROR;
    }

    /* ---- 1. 把整个文件读进 pool，读完就可以关掉文件了 ---- */
    status = BS->AllocatePool(EfiLoaderData, (UINTN)file_size, &file_buf);
    if (EFI_ERROR(status)) {
        report_error(L"AllocatePool(kernel.elf) failed", status);
        file->Close(file);
        return status;
    }

    read_size = (UINTN)file_size;
    status = file->Read(file, &read_size, file_buf);
    file->Close(file);

    if (EFI_ERROR(status)) {
        report_error(L"Read(kernel.elf) failed", status);
        goto fail;
    }
    if (read_size != (UINTN)file_size) {
        report_error(L"short read on kernel.elf", EFI_LOAD_ERROR);
        status = EFI_LOAD_ERROR;
        goto fail;
    }

    /* ---- 2. 校验 ELF 头：必须是 64 位小端的 x86-64 可执行文件 ---- */
    ehdr = (elf64_ehdr_t *)file_buf;
    if (ehdr->e_ident[0] != 0x7F || ehdr->e_ident[1] != 'E' ||
        ehdr->e_ident[2] != 'L'  || ehdr->e_ident[3] != 'F') {
        report_error(L"kernel.elf: bad magic, not an ELF file", EFI_LOAD_ERROR);
        status = EFI_LOAD_ERROR;
        goto fail;
    }
    if (ehdr->e_ident[EI_CLASS] != ELF_CLASS_64 || ehdr->e_ident[EI_DATA] != ELF_DATA_2LSB) {
        report_error(L"kernel.elf: need a 64-bit little-endian ELF", EFI_LOAD_ERROR);
        status = EFI_LOAD_ERROR;
        goto fail;
    }
    if (ehdr->e_type != ELF_TYPE_EXEC || ehdr->e_machine != ELF_MACHINE_X86_64) {
        report_error(L"kernel.elf: need a statically linked x86-64 executable", EFI_LOAD_ERROR);
        status = EFI_LOAD_ERROR;
        goto fail;
    }
    if (ehdr->e_phentsize != sizeof(elf64_phdr_t) ||
        ehdr->e_phoff + (UINT64)ehdr->e_phnum * ehdr->e_phentsize > file_size) {
        report_error(L"kernel.elf: broken program header table", EFI_LOAD_ERROR);
        status = EFI_LOAD_ERROR;
        goto fail;
    }

    /* ---- 3. 先扫一遍程序头，算出所有 PT_LOAD 段覆盖的物理地址范围 ---- */
    span_start = ~0ULL;
    span_end   = 0;
    for (i = 0; i < ehdr->e_phnum; i++) {
        const elf64_phdr_t *ph = (const elf64_phdr_t *)((const UINT8 *)file_buf +
                                                        ehdr->e_phoff + i * ehdr->e_phentsize);

        if (ph->p_type != PT_LOAD)
            continue;

        /* 这两条保证下面拷贝时不会越读文件、也不会算出负的补齐长度 */
        if (ph->p_filesz > ph->p_memsz || ph->p_offset + ph->p_filesz > file_size) {
            report_error(L"kernel.elf: malformed PT_LOAD segment", EFI_LOAD_ERROR);
            status = EFI_LOAD_ERROR;
            goto fail;
        }

        /*
         * 高半区内核要求所有 PT_LOAD 保持同一个 VMA-LMA delta。
         * 这样最终页表可以把连续物理映像整体映射到连续高地址。
         */
        UINT64 delta = ph->p_vaddr - ph->p_paddr;
        if (!have_load_delta) {
            load_delta = delta;
            have_load_delta = EFI_TRUE;
        } else if (delta != load_delta) {
            report_error(L"kernel.elf: inconsistent VMA/LMA delta", EFI_LOAD_ERROR);
            status = EFI_LOAD_ERROR;
            goto fail;
        }

        if (ph->p_paddr < span_start)
            span_start = ph->p_paddr;
        if (ph->p_paddr + ph->p_memsz > span_end)
            span_end = ph->p_paddr + ph->p_memsz;
    }
    if (!have_load_delta || span_end <= span_start) {
        report_error(L"kernel.elf: no loadable segment", EFI_LOAD_ERROR);
        status = EFI_LOAD_ERROR;
        goto fail;
    }

    if (span_start > UINT64_MAX - load_delta ||
        span_end > UINT64_MAX - load_delta) {
        report_error(L"kernel.elf: virtual span overflow", EFI_LOAD_ERROR);
        status = EFI_LOAD_ERROR;
        goto fail;
    }

    virt_span_start = span_start + load_delta;
    virt_span_end = span_end + load_delta;

    if (ehdr->e_entry < virt_span_start ||
        ehdr->e_entry >= virt_span_end) {
        report_error(L"kernel.elf: entry outside PT_LOAD span", EFI_LOAD_ERROR);
        status = EFI_LOAD_ERROR;
        goto fail;
    }

    /* ---- 4. 一次把整段物理内存占下来 ---- */
    addr  = (EFI_PHYSICAL_ADDRESS)ALIGN_DOWN(span_start, PAGE_SIZE);
    pages = (UINTN)DIV_ROUND_UP(span_end - addr, PAGE_SIZE);

    status = BS->AllocatePages(AllocateAddress, EfiLoaderData, pages, &addr);
    if (EFI_ERROR(status)) {
        con_puts(L"[BOOT] AllocatePages at ");
        con_hex64(ALIGN_DOWN(span_start, PAGE_SIZE));
        con_puts(L" failed -- that physical memory is not free\r\n");
        report_error(L"AllocatePages(AllocateAddress) failed", status);
        goto fail;
    }

        con_puts(L"[BOOT] ELF64 x86-64, entry = ");
    con_hex64(ehdr->e_entry);
    con_puts(L", ");
    con_dec(ehdr->e_phnum);
    con_puts(L" program headers\r\n");

    /* ---- 5. 逐段装载：文件内容拷进去，.bss 部分清零 ---- */
    for (i = 0; i < ehdr->e_phnum; i++) {
        const elf64_phdr_t *ph = (const elf64_phdr_t *)((const UINT8 *)file_buf +
                                                        ehdr->e_phoff + i * ehdr->e_phentsize);

        if (ph->p_type != PT_LOAD)
            continue;

        mem_copy((VOID *)(UINTN)ph->p_paddr,
                 (const UINT8 *)file_buf + ph->p_offset,
                 (UINTN)ph->p_filesz);
        mem_set((VOID *)(UINTN)(ph->p_paddr + ph->p_filesz), 0,
                (UINTN)(ph->p_memsz - ph->p_filesz));

    }

    /* ---- 6. 收尾：文件缓冲还给固件，把结果记进 BOOT_INFO ---- */
    bi->kernel_base      = span_start;
    bi->kernel_virt_base = virt_span_start;
    bi->kernel_size      = span_end - span_start;
    bi->kernel_entry     = ehdr->e_entry; /* 必须在 FreePool 之前读出来 */

    BS->FreePool(file_buf);
    file_buf = NULL;

    con_puts(L"[BOOT] image phys ");
    con_hex64(span_start);
    con_puts(L"..");
    con_hex64(span_end);
    con_puts(L", virt ");
    con_hex64(virt_span_start);
    con_puts(L"..");
    con_hex64(virt_span_end);
    con_puts(L" (");
    con_dec(pages);
    con_puts(L" pages reserved)\r\n");
    return EFI_SUCCESS;

fail:
    if (file_buf != NULL)
        BS->FreePool(file_buf);
    return status;
}

/* ================================================================== */
/* loader 自己的 bootstrap 页表                                        */
/* ================================================================== */
static UINT64 read_cr3(void)
{
    UINT64 value;
    __asm__ volatile("mov %%cr3, %0" : "=r"(value));
    return value;
}

static UINT64 read_cr4(void)
{
    UINT64 value;
    __asm__ volatile("mov %%cr4, %0" : "=r"(value));
    return value;
}

static void write_cr3(UINT64 value)
{
    __asm__ volatile("mov %0, %%cr3" :: "r"(value) : "memory");
}

static BOOLEAN bootstrap_usable_type(UINT32 type)
{
    return type == EfiLoaderCode ||
           type == EfiLoaderData ||
           type == EfiBootServicesCode ||
           type == EfiBootServicesData ||
           type == EfiConventionalMemory;
}

static UINT64 *boot_pt_alloc(EFI_PHYSICAL_ADDRESS *out_phys)
{
    if (g_boot_pt_arena == 0 ||
        g_boot_pt_used >= BOOT_PT_ARENA_PAGES)
        return NULL;

    EFI_PHYSICAL_ADDRESS phys =
        g_boot_pt_arena + (EFI_PHYSICAL_ADDRESS)g_boot_pt_used * PAGE_SIZE;
    ++g_boot_pt_used;

    mem_set((VOID *)(UINTN)phys, 0, PAGE_SIZE);

    if (out_phys != NULL)
        *out_phys = phys;
    return (UINT64 *)(UINTN)phys;
}

static BOOLEAN boot_child(UINT64 *parent, UINT32 index, UINT64 **out)
{
    UINT64 entry = parent[index];

    if ((entry & X86_PTE_PRESENT) != 0) {
        if ((entry & X86_PTE_LARGE) != 0)
            return EFI_FALSE;
        *out = (UINT64 *)(UINTN)(entry & X86_PTE_ADDR);
        return EFI_TRUE;
    }

    EFI_PHYSICAL_ADDRESS phys;
    UINT64 *child = boot_pt_alloc(&phys);
    if (child == NULL)
        return EFI_FALSE;

    parent[index] =
        ((UINT64)phys & X86_PTE_ADDR) |
        X86_PTE_PRESENT | X86_PTE_WRITE;

    *out = child;
    return EFI_TRUE;
}

static BOOLEAN boot_map_4k(UINT64 *pml4, UINT64 va, UINT64 pa)
{
    UINT32 i4 = (UINT32)((va >> 39) & 0x1FFU);
    UINT32 i3 = (UINT32)((va >> 30) & 0x1FFU);
    UINT32 i2 = (UINT32)((va >> 21) & 0x1FFU);
    UINT32 i1 = (UINT32)((va >> 12) & 0x1FFU);

    UINT64 *pdpt;
    UINT64 *pd;
    UINT64 *pt;

    if (!boot_child(pml4, i4, &pdpt) ||
        !boot_child(pdpt, i3, &pd) ||
        !boot_child(pd, i2, &pt))
        return EFI_FALSE;

    if ((pt[i1] & X86_PTE_PRESENT) != 0)
        return EFI_FALSE;

    pt[i1] =
        (pa & X86_PTE_ADDR) |
        X86_PTE_PRESENT | X86_PTE_WRITE;
    return EFI_TRUE;
}

static BOOLEAN boot_map_2m(UINT64 *pml4, UINT64 va, UINT64 pa)
{
    UINT32 i4 = (UINT32)((va >> 39) & 0x1FFU);
    UINT32 i3 = (UINT32)((va >> 30) & 0x1FFU);
    UINT32 i2 = (UINT32)((va >> 21) & 0x1FFU);

    UINT64 *pdpt;
    UINT64 *pd;

    if (!boot_child(pml4, i4, &pdpt) ||
        !boot_child(pdpt, i3, &pd))
        return EFI_FALSE;

    if ((pd[i2] & X86_PTE_PRESENT) != 0)
        return EFI_FALSE;

    pd[i2] =
        (pa & ~(PAGE_2M - 1)) |
        X86_PTE_PRESENT | X86_PTE_WRITE | X86_PTE_LARGE;
    return EFI_TRUE;
}

static BOOLEAN boot_map_range(UINT64 *pml4,
                              UINT64 va, UINT64 pa, UINT64 bytes)
{
    if ((va & (PAGE_SIZE - 1)) != 0 ||
        (pa & (PAGE_SIZE - 1)) != 0 ||
        (bytes & (PAGE_SIZE - 1)) != 0)
        return EFI_FALSE;

    while (bytes != 0) {
        if ((va & (PAGE_2M - 1)) == 0 &&
            (pa & (PAGE_2M - 1)) == 0 &&
            bytes >= PAGE_2M) {
            if (!boot_map_2m(pml4, va, pa))
                return EFI_FALSE;
            va += PAGE_2M;
            pa += PAGE_2M;
            bytes -= PAGE_2M;
            continue;
        }

        if (!boot_map_4k(pml4, va, pa))
            return EFI_FALSE;

        va += PAGE_SIZE;
        pa += PAGE_SIZE;
        bytes -= PAGE_SIZE;
    }

    return EFI_TRUE;
}

static EFI_STATUS prepare_bootstrap_page_tables(void)
{
    if ((read_cr4() & X86_CR4_LA57) != 0) {
        report_error(L"5-level paging is not supported", EFI_UNSUPPORTED);
        return EFI_UNSUPPORTED;
    }

    EFI_PHYSICAL_ADDRESS arena = 0;
    EFI_STATUS status =
        BS->AllocatePages(AllocateAnyPages, EfiLoaderData,
                          BOOT_PT_ARENA_PAGES, &arena);
    if (EFI_ERROR(status)) {
        report_error(L"AllocatePages(bootstrap page tables) failed", status);
        return status;
    }

    g_boot_pt_arena = arena;
    g_boot_pt_used = 0;
    g_boot_root = 0;

    con_puts(L"[BOOT] bootstrap page-table arena = ");
    con_hex64((UINT64)arena);
    con_puts(L"\r\n");
    return EFI_SUCCESS;
}

/*
 * 必须在最后一次 GetMemoryMap() 之后调用：
 * 这里只消耗已经预留的 g_boot_pt_arena，不再调用任何 Boot Services，
 * 因而不会改变 map_key。
 */
static EFI_STATUS build_bootstrap_page_tables(const BOOT_INFO *bi)
{
    if (bi == NULL ||
        bi->mmap_addr == 0 ||
        bi->mmap_desc_size < sizeof(EFI_MEMORY_DESCRIPTOR) ||
        bi->mmap_desc_count == 0 ||
        bi->kernel_base == 0 ||
        bi->kernel_virt_base == 0 ||
        bi->kernel_size == 0)
        return EFI_INVALID_PARAMETER;

    g_boot_pt_used = 0;

    EFI_PHYSICAL_ADDRESS root_phys;
    UINT64 *pml4 = boot_pt_alloc(&root_phys);
    if (pml4 == NULL)
        return EFI_OUT_OF_RESOURCES;

    /*
     * 先建立所有 loader/boot-services/普通 RAM 的 identity mapping。
     * 切 CR3 的瞬间 loader RIP、RSP、BOOT_INFO、memory map buffer 都仍然
     * 使用原来的低地址，因此必须在新 CR3 下继续可访问。
     */
    const UINT8 *p = (const UINT8 *)(UINTN)bi->mmap_addr;

    for (UINT32 i = 0; i < bi->mmap_desc_count;
         ++i, p += bi->mmap_desc_size) {
        const EFI_MEMORY_DESCRIPTOR *d =
            (const EFI_MEMORY_DESCRIPTOR *)(const VOID *)p;

        if (!bootstrap_usable_type(d->Type) ||
            d->NumberOfPages == 0)
            continue;

        if (d->NumberOfPages > UINT64_MAX / PAGE_SIZE)
            return EFI_LOAD_ERROR;

        UINT64 bytes = d->NumberOfPages * PAGE_SIZE;
        if (d->PhysicalStart > UINT64_MAX - bytes)
            return EFI_LOAD_ERROR;

        if (!boot_map_range(pml4,
                            d->PhysicalStart,
                            d->PhysicalStart,
                            bytes))
            return EFI_OUT_OF_RESOURCES;
    }

    /*
     * 再给同一物理内核映像建立最终高半区别名。
     * identity alias 暂时保留，等 kernel 自己接管 CR3 后再自然消失。
     */
    if (bi->kernel_size > UINT64_MAX - (PAGE_SIZE - 1))
        return EFI_LOAD_ERROR;

    UINT64 kernel_bytes =
        (bi->kernel_size + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);

    if (!boot_map_range(pml4,
                        bi->kernel_virt_base,
                        bi->kernel_base,
                        kernel_bytes))
        return EFI_OUT_OF_RESOURCES;

    g_boot_root = root_phys;
    return EFI_SUCCESS;
}

static EFI_STATUS activate_bootstrap_page_tables(const BOOT_INFO *bi)
{
    if (g_boot_root == 0)
        return EFI_INVALID_PARAMETER;

    /* R: 即将离开固件页表，切换到 loader 自己的 bootstrap CR3。 */
    debug_char('R');

    write_cr3((UINT64)g_boot_root);

    /* C: 新 CR3 下 loader 的低地址 RIP/RSP identity mapping 正常。 */
    debug_char('C');

    volatile const UINT8 *high =
        (volatile const UINT8 *)(UINTN)bi->kernel_virt_base;
    volatile const UINT8 *phys =
        (volatile const UINT8 *)(UINTN)bi->kernel_base;

    if (*high != *phys)
        return EFI_DEVICE_ERROR;

    /* H: kernel 高地址 alias 与物理映像一致。 */
    debug_char('H');
    return EFI_SUCCESS;
}

/* ================================================================== */
/* 记录 GOP 帧缓冲信息                                                 */
/* ================================================================== */
static void save_framebuffer(BOOT_INFO *bi)
{
    EFI_GRAPHICS_OUTPUT_PROTOCOL      *gop = NULL;
    EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE *mode;
    EFI_GUID guid_gop = EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID;
    EFI_STATUS status;

    bi->fb_phys_base = 0;
    bi->fb_size = 0;
    bi->fb_base = 0;

    status = BS->LocateProtocol(&guid_gop, NULL, (VOID **)&gop);
    if (EFI_ERROR(status) || gop == NULL || gop->Mode == NULL || gop->Mode->Info == NULL) {
        con_puts(L"[BOOT] no GraphicsOutput protocol, framebuffer unavailable\r\n");
        return;
    }

    mode = gop->Mode;
    bi->fb_phys_base           = mode->FrameBufferBase;
    bi->fb_size                = mode->FrameBufferSize;
    bi->fb_width               = mode->Info->HorizontalResolution;
    bi->fb_height              = mode->Info->VerticalResolution;
    bi->fb_pixels_per_scanline = mode->Info->PixelsPerScanLine;
    bi->fb_pixel_format        = mode->Info->PixelFormat;
    bi->fb_red_mask            = mode->Info->PixelInformation.RedMask;
    bi->fb_green_mask          = mode->Info->PixelInformation.GreenMask;
    bi->fb_blue_mask           = mode->Info->PixelInformation.BlueMask;
    bi->fb_reserved_mask       = mode->Info->PixelInformation.ReservedMask;

    con_puts(L"[BOOT] framebuffer: ");
    con_dec(bi->fb_width);
    con_puts(L"x");
    con_dec(bi->fb_height);
    con_puts(L", format=");
    con_dec(bi->fb_pixel_format);
    con_puts(L", base=");
    con_hex64(bi->fb_phys_base);
    con_puts(L", size=");
    con_dec(bi->fb_size);
    con_puts(L"\r\n");
}

static void dump_firmware_cr3(const BOOT_INFO *bi)
{
    UINT64 cr3 = read_cr3();
    UINT64 root = cr3 & X86_PTE_ADDR;

    con_puts(L"[BOOT] firmware CR3 raw = ");
    con_hex64(cr3);
    con_puts(L", root = ");
    con_hex64(root);
    con_puts(L"\r\n");

    const UINT8 *p = (const UINT8 *)(UINTN)bi->mmap_addr;
    BOOLEAN found = EFI_FALSE;

    for (UINT32 i = 0; i < bi->mmap_desc_count;
         ++i, p += bi->mmap_desc_size) {
        const EFI_MEMORY_DESCRIPTOR *d =
            (const EFI_MEMORY_DESCRIPTOR *)(const VOID *)p;

        if (d->NumberOfPages > UINT64_MAX / PAGE_SIZE)
            continue;

        UINT64 bytes = d->NumberOfPages * PAGE_SIZE;
        if (d->PhysicalStart > UINT64_MAX - bytes)
            continue;

        UINT64 end = d->PhysicalStart + bytes;

        if (root >= d->PhysicalStart && root < end) {
            con_puts(L"[BOOT] firmware CR3 descriptor: index=");
            con_dec(i);
            con_puts(L" type=");
            con_dec(d->Type);
            con_puts(L" phys=");
            con_hex64(d->PhysicalStart);
            con_puts(L"..");
            con_hex64(end);
            con_puts(L" pages=");
            con_dec(d->NumberOfPages);
            con_puts(L"\r\n");
            found = EFI_TRUE;
            break;
        }
    }

    if (!found)
        con_puts(L"[BOOT] firmware CR3 descriptor: NOT FOUND\r\n");

    /*
     * 关键测试：在 Boot Services 仍有效时，把 CR3 的物理根页直接按
     * 同值虚拟地址解引用。如果 UEFI x64 identity mapping 覆盖该页，
     * 这里应能安全读出 PML4[0]/[256]/[510]/[511]。
     */
    volatile const UINT64 *pml4 =
        (volatile const UINT64 *)(UINTN)root;

    con_puts(L"[BOOT] firmware PML4[0]   = ");
    con_hex64(pml4[0]);
    con_puts(L"\r\n");

    con_puts(L"[BOOT] firmware PML4[256] = ");
    con_hex64(pml4[256]);
    con_puts(L"\r\n");

    con_puts(L"[BOOT] firmware PML4[510] = ");
    con_hex64(pml4[510]);
    con_puts(L"\r\n");

    con_puts(L"[BOOT] firmware PML4[511] = ");
    con_hex64(pml4[511]);
    con_puts(L"\r\n");
}

/* ================================================================== */
/* 取内存映射并退出 Boot Services                                      */
/* ================================================================== */
static EFI_STATUS leave_boot_services(EFI_HANDLE image, BOOT_INFO *bi)
{
    EFI_MEMORY_DESCRIPTOR *mmap = NULL;
    UINTN  mmap_buf_size = MMAP_BUF_SIZE;
    UINTN  map_size = 0, map_key = 0, desc_size = 0;
    UINT32 desc_version = 0;
    EFI_STATUS status;

    status = BS->AllocatePool(EfiLoaderData, mmap_buf_size, (VOID **)&mmap);
    if (EFI_ERROR(status)) {
        report_error(L"AllocatePool(memory map) failed", status);
        return status;
    }

    for (;;) {
        map_size = mmap_buf_size;
        status = BS->GetMemoryMap(&map_size, mmap, &map_key, &desc_size, &desc_version);

        if (status == EFI_BUFFER_TOO_SMALL) {
            /* 缓冲区不够，扩容后重来（此时还没退出 Boot Services，可以继续分配） */
            VOID *bigger = NULL;

            BS->FreePool(mmap);
            mmap_buf_size = map_size + PAGE_SIZE;
            status = BS->AllocatePool(EfiLoaderData, mmap_buf_size, &bigger);
            if (EFI_ERROR(status)) {
                report_error(L"AllocatePool(bigger memory map) failed", status);
                return status;
            }
            mmap = (EFI_MEMORY_DESCRIPTOR *)bigger;
            continue;
        }
        if (EFI_ERROR(status)) {
            report_error(L"GetMemoryMap failed", status);
            return status;
        }

        bi->mmap_addr         = (UINT64)(UINTN)mmap;
        bi->mmap_size         = (UINT64)map_size;
        bi->mmap_desc_size    = (UINT64)desc_size;
        bi->mmap_desc_version = desc_version;
        bi->mmap_desc_count   =
            (desc_size != 0) ? (UINT32)(map_size / desc_size) : 0;

        /*
         * Boot Services 尚未退出：直接验证 firmware CR3 根页是否真的
         * 处于 identity mapping 中，并打印 PML4 关键槽位。
         */
        dump_firmware_cr3(bi);

        /*
         * 不再修改固件页表。使用最终 memory map，在预留 arena 内构建
         * loader 自己的 bootstrap CR3。该过程不调用 Boot Services，
         * 所以 map_key 仍然有效。
         */
        status = build_bootstrap_page_tables(bi);
        if (EFI_ERROR(status)) {
            report_error(L"build bootstrap page tables failed", status);
            return status;
        }

        /* map_key 必须来自最近一次 GetMemoryMap，且两次调用之间不能有任何内存分配 */
        status = BS->ExitBootServices(image, map_key);
        if (!EFI_ERROR(status))
            break;

        if (status != EFI_INVALID_PARAMETER) {
            report_error(L"ExitBootServices failed", status);
            return status;
        }
        /* 内存映射变了，重新取一次再试 */
    }

    return EFI_SUCCESS;
}

/* ================================================================== */
/* 入口                                                                */
/* ================================================================== */
EFI_STATUS EFIAPI efi_main(EFI_HANDLE image_handle, EFI_SYSTEM_TABLE *system_table)
{
    kernel_entry_t entry;
    EFI_STATUS status;

    ST = system_table;
    BS = system_table->BootServices;

    /* 固件默认有 5 分钟看门狗，不关掉内核跑到一半会被重启 */
    BS->SetWatchdogTimer(0, 0, 0, NULL);

    con_puts(L"\r\n[BOOT] minimal UEFI bootloader (x86_64)\r\n");
    con_puts(L"[BOOT] UEFI spec ");
    con_dec(ST->Hdr.Revision >> 16);
    con_puts(L".");
    con_dec((ST->Hdr.Revision >> 8) & 0xFF);
    con_puts(L", firmware rev ");
    con_hex64(ST->FirmwareRevision);
    con_puts(L"\r\n");

    g_bootinfo.magic = BOOTINFO_MAGIC;
    g_bootinfo.version = BOOTINFO_VERSION;
    g_bootinfo.runtime_services = (UINT64)(UINTN)ST->RuntimeServices;

    status = load_kernel(image_handle, &g_bootinfo);
    if (EFI_ERROR(status))
        return status;

    /* 入口地址来自 ELF 的 e_entry，不再写死常量 */
    entry = (kernel_entry_t)(UINTN)g_bootinfo.kernel_entry;

    save_framebuffer(&g_bootinfo);

    /*
     * EBS 前只预留 bootstrap 页表 arena；真正页表内容在最后一次
     * GetMemoryMap 之后构建，因此既能覆盖最终内存布局，也不会改变 map_key。
     */
    status = prepare_bootstrap_page_tables();
    if (EFI_ERROR(status))
        return status;

    status = leave_boot_services(image_handle, &g_bootinfo);
    if (EFI_ERROR(status))
        return status;      /* ExitBootServices 失败时控制台还能用，直接返回固件 */

    /* E: ExitBootServices 已成功返回。 */
    debug_char('E');

    /*
     * 从此不能再调用任何 Boot Services（ConOut / AllocatePool 都失效）。
     * 内存映射缓冲区已经记录在 BOOT_INFO 里，内存布局也不会再变化。
     */

    /* 关中断，挂入临时高半区映射，然后直接进入高地址 e_entry。 */
    __asm__ volatile("cli");

    status = activate_bootstrap_page_tables(&g_bootinfo);
    if (EFI_ERROR(status)) {
        /*
         * 已经 ExitBootServices，不能再调用固件控制台。
         * debugcon: 'M' = bootstrap CR3 切换/高地址校验失败。
         */
        debug_char('M');
        for (;;)
            __asm__ volatile("hlt");
    }

    /* debugcon: 'J' = 即将 jump/call 到高半区 kernel_entry。 */
    debug_char('J');

    entry(&g_bootinfo);

    /* 正常情况不会返回 */
    for (;;)
        __asm__ volatile("hlt");

    return EFI_SUCCESS;
}


