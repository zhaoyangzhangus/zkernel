/*
 * bootinfo.h —— 引导程序传给内核的启动信息
 *
 * 这个头文件被两边同时使用：
 *   - boot/boot.c   用 --target=x86_64-unknown-windows 编译（MS ABI）
 *   - init/main.c   用 gcc/clang 编译（System V ABI）
 * 所以这里只允许使用固定宽度类型（uint32_t/uint64_t），
 * 绝对不能出现 long / unsigned long / size_t 这类宽度随 ABI 变化的类型。
 */
#ifndef __KERNEL_BOOTINFO_H__
#define __KERNEL_BOOTINFO_H__

#include <stdint.h>

/* 'B''O''O''T''I''N''F''O' 按小端解释 */
#define BOOTINFO_MAGIC 0x4F464E49544F4F42ULL
#define BOOTINFO_VERSION 4

/*
 * 内存映射里的一项，与 UEFI 的 EFI_MEMORY_DESCRIPTOR 逐字段对应
 * （boot.c 里有 _Static_assert 保证两者大小一致）。
 * 注意 UEFI 要求数组元素按 mmap_desc_size 步进，不能直接当作结构体数组用。
 */
typedef struct {
    uint32_t type;             /* 取值见下面的 MEM_* */
    uint32_t reserved;
    uint64_t physical_start;   /* 物理起始地址 */
    uint64_t virtual_start;    /* 退出 Boot Services 后无意义 */
    uint64_t number_of_pages;  /* 页数，一页 4 KiB */
    uint64_t attribute;
} BOOT_MEMORY_DESCRIPTOR;

/* 内存类型，与 EFI_MEMORY_TYPE 一一对应 */
#define MEM_RESERVED              0
#define MEM_LOADER_CODE           1
#define MEM_LOADER_DATA           2
#define MEM_BOOT_SERVICES_CODE    3
#define MEM_BOOT_SERVICES_DATA    4
#define MEM_RUNTIME_SERVICES_CODE 5
#define MEM_RUNTIME_SERVICES_DATA 6
#define MEM_CONVENTIONAL          7   /* 普通可用内存 */
#define MEM_UNUSABLE              8
#define MEM_ACPI_RECLAIM          9
#define MEM_ACPI_NVS              10
#define MEM_MMIO                  11
#define MEM_MMIO_PORT_SPACE       12
#define MEM_PAL_CODE              13
#define MEM_PERSISTENT            14
#define MEM_MAX_TYPE              15

typedef struct {
    uint64_t magic;          /* 必须等于 BOOTINFO_MAGIC */
    uint32_t version;        /* BOOTINFO_VERSION */
    uint32_t reserved0;

    /* ---- 帧缓冲（EFI_GRAPHICS_OUTPUT_PROTOCOL 当前模式）---- */
    uint64_t fb_phys_base;   /* GOP framebuffer 物理地址 */
    uint64_t fb_size;        /* GOP 报告的 framebuffer 字节数 */
    uint64_t fb_base;        /* 内核映射后的虚拟地址；映射前为 0 */
    uint32_t fb_width;
    uint32_t fb_height;
    uint32_t fb_pixels_per_scanline;
    uint32_t fb_pixel_format; /* EFI_GRAPHICS_PIXEL_FORMAT 的值 */
    uint32_t fb_red_mask;
    uint32_t fb_green_mask;
    uint32_t fb_blue_mask;
    uint32_t fb_reserved_mask;

    /* ---- UEFI 内存映射（ExitBootServices 之后依然有效）----
     * 描述符数组，按 EFI_MEMORY_DESCRIPTOR 解释：
     *   Type(4) Pad(4) PhysicalStart(8) VirtualStart(8) NumberOfPages(8) Attribute(8)
     * 类型常量见 efi.h 的 EFI_MEMORY_TYPE。退出 Boot Services 后，
     * EfiBootServicesCode/Data 也归操作系统所有。
     */
    uint64_t mmap_addr;       /* loader: identity VA；kernel: direct-map VA */
    uint64_t mmap_size;       /* 字节数 */
    uint64_t mmap_desc_size;  /* 每个描述符字节数 */
    uint32_t mmap_desc_version;
    uint32_t mmap_desc_count;

    /* ---- Runtime Services，之后实现 reboot/shutdown 会用到 ---- */
    uint64_t runtime_services;

    /* ---- 内核映像（ELF）被装载到的位置 ---- */
    uint64_t kernel_base;      /* 所有 PT_LOAD 段覆盖到的最低物理地址 */
    uint64_t kernel_virt_base; /* 同一映像对应的最低高半区虚拟地址 */
    uint64_t kernel_size;      /* 段覆盖的总跨度（字节） */
    uint64_t kernel_entry;     /* ELF e_entry：高半区入口地址 */
} BOOT_INFO;

#endif /* __KERNEL_BOOTINFO_H__ */
