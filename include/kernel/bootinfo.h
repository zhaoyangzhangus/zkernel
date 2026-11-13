#pragma once

#include <uefi.h>

#define OS_BOOTINFO_MAGIC 0x4C4954454F534249ULL
#define OS_BOOTINFO_VERSION 6U
#define OS_BOOTSTRAP_STACK_SIZE (2ULL * 1024ULL * 1024ULL)

enum {
    OS_BOOTINFO_HAS_FRAMEBUFFER = 1U << 0,
    OS_BOOTINFO_HAS_ACPI        = 1U << 1,
    OS_BOOTINFO_HAS_SMBIOS      = 1U << 2,
    OS_BOOTINFO_HAS_RNG         = 1U << 3,
    OS_BOOTINFO_HAS_BOOT_DEVICE = 1U << 11,
};

enum {
    OS_BOOT_DEVICE_TRANSPORT_UNKNOWN = 0,
    OS_BOOT_DEVICE_TRANSPORT_USB = 1,
    OS_BOOT_DEVICE_TRANSPORT_NVME = 2,
};

enum {
    OS_PIXEL_RGBR = 0,
    OS_PIXEL_BGRR = 1,
    OS_PIXEL_BITMASK = 2,
    OS_PIXEL_BLT_ONLY = 3,
};

/* 此结构属于加载器/内核 ABI。保持字段顺序和宽度稳定。 */
typedef struct os_boot_info {
    UINT64 Magic;
    UINT32 Version;
    UINT32 Size;
    UINT64 Flags;

    UINT64 KernelPhysicalBase;
    UINT64 KernelVirtualBase;
    UINT64 KernelSize;
    UINT64 KernelEntry;

    UINT64 MemoryMap;
    UINT64 MemoryMapSize;
    UINT64 MemoryDescriptorSize;
    UINT32 MemoryDescriptorVersion;
    UINT32 Reserved0;

    UINT64 FrameBufferBase;
    UINT64 FrameBufferSize;
    UINT32 FrameBufferWidth;
    UINT32 FrameBufferHeight;
    UINT32 FrameBufferPixelsPerScanLine;
    UINT32 FrameBufferFormat;
    UINT32 FrameBufferMask[4];
    UINT32 Reserved1;

    UINT64 AcpiRsdp;
    UINT64 Smbios;
    UINT64 Smbios3;
    UINT64 RuntimeServices;
    UINT64 SystemTable;

    UINT8 RandomSeed[32];
    UINT64 CommandLine;
    UINT64 LoaderName;
    UINT64 LoaderNameSize;

    UINT64 MemoryMapBufferSize;
    UINT64 CommandLineSize;
    UINT64 BootInfoPhysicalBase;
    UINT64 BootstrapStackBase;
    UINT64 BootstrapStackSize;
    UINT64 BootstrapStackTop;
    UINT64 LoaderImageBase;
    UINT64 LoaderImageSize;
    UINT64 PageDatabasePhysicalBase;
    UINT64 PageDatabaseSize;

    UINT64 ApTrampolineBase;
    UINT64 ApTrampolineSize;

    UINT64 BootDeviceHandle;
    UINT32 BootDevicePathSize;
    UINT32 BootDevicePartitionNumber;
    UINT64 BootDevicePartitionStartLba;
    UINT64 BootDevicePartitionSizeLba;
    UINT8 BootDevicePartitionSignature[16];
    UINT8 BootDevicePathHash[32];
    UINT8 BootDevicePartitionMbrType;
    UINT8 BootDevicePartitionSignatureType;
    UINT8 BootDeviceTransport;
    UINT8 Reserved3;
    UINT16 Reserved4;
} OS_BOOT_INFO;
