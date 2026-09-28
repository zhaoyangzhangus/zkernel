/*
 * efi.h —— 最小自包含的 x86_64 UEFI 定义
 *
 * 不依赖 gnu-efi / EDK2，只包含引导程序真正用到的部分。
 * 参考 UEFI Specification 2.x。
 *
 * 注意：UEFI 在 x86_64 上使用微软 x64 调用约定（rcx, rdx, r8, r9），
 * 而 Linux 下默认是 System V 约定，所以所有 EFI 回调指针都必须带 EFIAPI。
 * 用 --target=x86_64-unknown-windows 编译时 MS ABI 本来就是默认值。
 */
#ifndef __UEFI_EFI_H__
#define __UEFI_EFI_H__

#include <stdint.h>
#include <stddef.h>

/* ------------------------------------------------------------------ */
/* 基础类型                                                            */
/* ------------------------------------------------------------------ */
typedef uint8_t   BOOLEAN;
typedef uint8_t   CHAR8;
typedef uint16_t  CHAR16;
typedef void      VOID;

typedef uint64_t  UINT64;
typedef int64_t   INT64;
typedef uint32_t  UINT32;
typedef int32_t   INT32;
typedef uint16_t  UINT16;
typedef int16_t   INT16;
typedef uint8_t   UINT8;
typedef int8_t    INT8;

typedef uint64_t  UINTN;          /* 指针宽度整数 */
typedef int64_t   INTN;

typedef VOID     *EFI_HANDLE;
typedef VOID     *EFI_EVENT;
typedef UINTN     EFI_TPL;

typedef UINT64    EFI_STATUS;
typedef UINT64    EFI_PHYSICAL_ADDRESS;
typedef UINT64    EFI_VIRTUAL_ADDRESS;

#if defined(_M_X64) || defined(_WIN64) || defined(_M_AMD64)
#  define EFIAPI                  /* MS ABI 已经是默认调用约定 */
#else
#  define EFIAPI __attribute__((ms_abi))
#endif

typedef struct {
    UINT32 Data1;
    UINT16 Data2;
    UINT16 Data3;
    UINT8  Data4[8];
} EFI_GUID;

#define EFI_TRUE  1
#define EFI_FALSE 0

/* ------------------------------------------------------------------ */
/* 状态码                                                              */
/* ------------------------------------------------------------------ */
#define EFI_SUCCESS 0

#define EFI_ERR(x) (0x8000000000000000ULL | (UINT64)(x))
#define EFI_LOAD_ERROR        EFI_ERR(1)
#define EFI_INVALID_PARAMETER EFI_ERR(2)
#define EFI_UNSUPPORTED       EFI_ERR(3)
#define EFI_BAD_BUFFER_SIZE   EFI_ERR(4)
#define EFI_BUFFER_TOO_SMALL  EFI_ERR(5)
#define EFI_NOT_READY         EFI_ERR(6)
#define EFI_DEVICE_ERROR      EFI_ERR(7)
#define EFI_WRITE_PROTECTED   EFI_ERR(8)
#define EFI_OUT_OF_RESOURCES  EFI_ERR(9)
#define EFI_NOT_FOUND         EFI_ERR(14)
#define EFI_ABORTED           EFI_ERR(21)

/* UEFI 错误码的最高位为 1 */
#define EFI_ERROR(s) ((INT64)(s) < 0)

/* ------------------------------------------------------------------ */
/* 常用 GUID                                                           */
/* ------------------------------------------------------------------ */
#define EFI_LOADED_IMAGE_PROTOCOL_GUID \
    { 0x5B1B31A1, 0x9562, 0x11D2, { 0x8E, 0x3F, 0x00, 0xA0, 0xC9, 0x69, 0x72, 0x3B } }

#define EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID \
    { 0x964E5B22, 0x6459, 0x11D2, { 0x8E, 0x39, 0x00, 0xA0, 0xC9, 0x69, 0x72, 0x3B } }

#define EFI_FILE_INFO_GUID \
    { 0x09576E92, 0x6D3F, 0x11D2, { 0x8E, 0x39, 0x00, 0xA0, 0xC9, 0x69, 0x72, 0x3B } }

#define EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID \
    { 0x9042A9DE, 0x23DC, 0x4A38, { 0x96, 0xFB, 0x7A, 0xDE, 0xD0, 0x80, 0x51, 0x6A } }

/* ------------------------------------------------------------------ */
/* 表头 / 系统表                                                       */
/* ------------------------------------------------------------------ */
typedef struct {
    UINT64 Signature;
    UINT32 Revision;
    UINT32 HeaderSize;
    UINT32 CRC32;
    UINT32 Reserved;
} EFI_TABLE_HEADER;

typedef struct {
    EFI_GUID VendorGuid;
    VOID    *VendorTable;
} EFI_CONFIGURATION_TABLE;

/* ------------------------------------------------------------------ */
/* Simple Text Output（控制台）                                        */
/* ------------------------------------------------------------------ */
typedef struct _EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL;

typedef EFI_STATUS (EFIAPI *EFI_TEXT_RESET)(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This, BOOLEAN ExtendedVerification);
typedef EFI_STATUS (EFIAPI *EFI_TEXT_STRING)(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This, CHAR16 *String);
typedef EFI_STATUS (EFIAPI *EFI_TEXT_TEST_STRING)(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This, CHAR16 *String);
typedef EFI_STATUS (EFIAPI *EFI_TEXT_QUERY_MODE)(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This, UINTN ModeNumber, UINTN *Columns, UINTN *Rows);
typedef EFI_STATUS (EFIAPI *EFI_TEXT_SET_MODE)(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This, UINTN ModeNumber);
typedef EFI_STATUS (EFIAPI *EFI_TEXT_SET_ATTRIBUTE)(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This, UINTN Attribute);
typedef EFI_STATUS (EFIAPI *EFI_TEXT_CLEAR_SCREEN)(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This);
typedef EFI_STATUS (EFIAPI *EFI_TEXT_SET_CURSOR_POSITION)(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This, UINTN Column, UINTN Row);
typedef EFI_STATUS (EFIAPI *EFI_TEXT_ENABLE_CURSOR)(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This, BOOLEAN Visible);

typedef struct {
    INT32   MaxMode;
    INT32   Mode;
    INT32   Attribute;
    INT32   CursorColumn;
    INT32   CursorRow;
    BOOLEAN CursorVisible;
} EFI_SIMPLE_TEXT_OUTPUT_MODE;

struct _EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL {
    EFI_TEXT_RESET                Reset;
    EFI_TEXT_STRING               OutputString;
    EFI_TEXT_TEST_STRING          TestString;
    EFI_TEXT_QUERY_MODE           QueryMode;
    EFI_TEXT_SET_MODE             SetMode;
    EFI_TEXT_SET_ATTRIBUTE        SetAttribute;
    EFI_TEXT_CLEAR_SCREEN         ClearScreen;
    EFI_TEXT_SET_CURSOR_POSITION  SetCursorPosition;
    EFI_TEXT_ENABLE_CURSOR        EnableCursor;
    EFI_SIMPLE_TEXT_OUTPUT_MODE  *Mode;
};

/* ------------------------------------------------------------------ */
/* 内存管理                                                            */
/* ------------------------------------------------------------------ */
typedef enum {
    AllocateAnyPages,
    AllocateMaxAddress,
    AllocateAddress,
    MaxAllocateType
} EFI_ALLOCATE_TYPE;

typedef enum {
    EfiReservedMemoryType,
    EfiLoaderCode,
    EfiLoaderData,
    EfiBootServicesCode,
    EfiBootServicesData,
    EfiRuntimeServicesCode,
    EfiRuntimeServicesData,
    EfiConventionalMemory,
    EfiUnusableMemory,
    EfiACPIReclaimMemory,
    EfiACPIMemoryNVS,
    EfiMemoryMappedIO,
    EfiMemoryMappedIOPortSpace,
    EfiPalCode,
    EfiPersistentMemory,
    EfiMaxMemoryType
} EFI_MEMORY_TYPE;

typedef struct {
    UINT32 Type;
    UINT32 Pad;
    EFI_PHYSICAL_ADDRESS PhysicalStart;
    EFI_VIRTUAL_ADDRESS  VirtualStart;
    UINT64 NumberOfPages;
    UINT64 Attribute;
} EFI_MEMORY_DESCRIPTOR;

/* ------------------------------------------------------------------ */
/* Boot Services                                                       */
/* ------------------------------------------------------------------ */
typedef EFI_STATUS (EFIAPI *EFI_ALLOCATE_PAGES)(EFI_ALLOCATE_TYPE Type, EFI_MEMORY_TYPE MemoryType,
                                                UINTN Pages, EFI_PHYSICAL_ADDRESS *Memory);
typedef EFI_STATUS (EFIAPI *EFI_FREE_PAGES)(EFI_PHYSICAL_ADDRESS Memory, UINTN Pages);
typedef EFI_STATUS (EFIAPI *EFI_GET_MEMORY_MAP)(UINTN *MemoryMapSize, EFI_MEMORY_DESCRIPTOR *MemoryMap,
                                                UINTN *MapKey, UINTN *DescriptorSize, UINT32 *DescriptorVersion);
typedef EFI_STATUS (EFIAPI *EFI_ALLOCATE_POOL)(EFI_MEMORY_TYPE PoolType, UINTN Size, VOID **Buffer);
typedef EFI_STATUS (EFIAPI *EFI_FREE_POOL)(VOID *Buffer);
typedef EFI_STATUS (EFIAPI *EFI_HANDLE_PROTOCOL)(EFI_HANDLE Handle, EFI_GUID *Protocol, VOID **Interface);
typedef EFI_STATUS (EFIAPI *EFI_LOCATE_PROTOCOL)(EFI_GUID *Protocol, VOID *Registration, VOID **Interface);
typedef EFI_STATUS (EFIAPI *EFI_EXIT_BOOT_SERVICES)(EFI_HANDLE ImageHandle, UINTN MapKey);
typedef EFI_STATUS (EFIAPI *EFI_SET_WATCHDOG_TIMER)(UINTN Timeout, UINT64 WatchdogCode, UINTN DataSize, CHAR16 *WatchdogData);
typedef EFI_STATUS (EFIAPI *EFI_STALL)(UINTN Microseconds);

typedef struct {
    EFI_TABLE_HEADER Hdr;

    VOID *RaiseTPL;                       /*  0x18 */
    VOID *RestoreTPL;                     /*  0x20 */
    EFI_ALLOCATE_PAGES AllocatePages;     /*  0x28 */
    EFI_FREE_PAGES     FreePages;         /*  0x30 */
    EFI_GET_MEMORY_MAP GetMemoryMap;      /*  0x38 */
    EFI_ALLOCATE_POOL  AllocatePool;      /*  0x40 */
    EFI_FREE_POOL      FreePool;          /*  0x48 */
    VOID *CreateEvent;                    /*  0x50 */
    VOID *SetTimer;                       /*  0x58 */
    VOID *WaitForEvent;                   /*  0x60 */
    VOID *SignalEvent;                    /*  0x68 */
    VOID *CloseEvent;                     /*  0x70 */
    VOID *CheckEvent;                     /*  0x78 */
    VOID *InstallProtocolInterface;       /*  0x80 */
    VOID *ReinstallProtocolInterface;     /*  0x88 */
    VOID *UninstallProtocolInterface;     /*  0x90 */
    EFI_HANDLE_PROTOCOL HandleProtocol;   /*  0x98 */
    VOID *Reserved;                       /*  0xA0 */
    VOID *RegisterProtocolNotify;         /*  0xA8 */
    VOID *LocateHandle;                   /*  0xB0 */
    VOID *LocateDevicePath;               /*  0xB8 */
    VOID *InstallConfigurationTable;      /*  0xC0 */
    VOID *LoadImage;                      /*  0xC8 */
    VOID *StartImage;                     /*  0xD0 */
    VOID *Exit;                           /*  0xD8 */
    VOID *UnloadImage;                    /*  0xE0 */
    EFI_EXIT_BOOT_SERVICES ExitBootServices; /* 0xE8 */
    VOID *GetNextMonotonicCount;          /*  0xF0 */
    EFI_STALL Stall;                      /*  0xF8 */
    EFI_SET_WATCHDOG_TIMER SetWatchdogTimer; /* 0x100 */
    VOID *ConnectController;              /* 0x108 */
    VOID *DisconnectController;           /* 0x110 */
    VOID *OpenProtocol;                   /* 0x118 */
    VOID *CloseProtocol;                  /* 0x120 */
    VOID *OpenProtocolInformation;        /* 0x128 */
    VOID *ProtocolsPerHandle;             /* 0x130 */
    VOID *LocateHandleBuffer;             /* 0x138 */
    EFI_LOCATE_PROTOCOL LocateProtocol;   /* 0x140 */
    VOID *InstallMultipleProtocolInterfaces;   /* 0x148 */
    VOID *UninstallMultipleProtocolInterfaces; /* 0x150 */
    VOID *CalculateCrc32;                 /* 0x158 */
    VOID *CopyMem;                        /* 0x160 */
    VOID *SetMem;                         /* 0x168 */
    VOID *CreateEventEx;                  /* 0x170 */
} EFI_BOOT_SERVICES;

typedef struct {
    EFI_TABLE_HEADER Hdr;
    CHAR16 *FirmwareVendor;
    UINT32  FirmwareRevision;
    EFI_HANDLE ConsoleInHandle;
    VOID *ConIn;
    EFI_HANDLE ConsoleOutHandle;
    EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *ConOut;
    EFI_HANDLE StandardErrorHandle;
    EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *StdErr;
    VOID *RuntimeServices;
    EFI_BOOT_SERVICES *BootServices;
    UINTN NumberOfTableEntries;
    EFI_CONFIGURATION_TABLE *ConfigurationTable;
} EFI_SYSTEM_TABLE;

/* ------------------------------------------------------------------ */
/* Loaded Image Protocol                                               */
/* ------------------------------------------------------------------ */
#define EFI_LOADED_IMAGE_PROTOCOL_REVISION 0x1000

typedef struct {
    UINT32 Revision;
    EFI_HANDLE ParentHandle;
    EFI_SYSTEM_TABLE *SystemTable;
    EFI_HANDLE DeviceHandle;      /* 本映像所在设备（用来找 ESP） */
    VOID *FilePath;
    VOID *Reserved;
    UINT32 LoadOptionsSize;
    VOID *LoadOptions;
    VOID *ImageBase;
    UINT64 ImageSize;
    UINT32 ImageDataType;
    EFI_PHYSICAL_ADDRESS Unload;
} EFI_LOADED_IMAGE_PROTOCOL;

/* ------------------------------------------------------------------ */
/* Simple File System Protocol                                         */
/* ------------------------------------------------------------------ */
typedef struct _EFI_FILE_PROTOCOL EFI_FILE_PROTOCOL;

typedef EFI_STATUS (EFIAPI *EFI_FILE_OPEN)(EFI_FILE_PROTOCOL *This, EFI_FILE_PROTOCOL **NewHandle,
                                           CHAR16 *FileName, UINT64 OpenMode, UINT64 Attributes);
typedef EFI_STATUS (EFIAPI *EFI_FILE_CLOSE)(EFI_FILE_PROTOCOL *This);
typedef EFI_STATUS (EFIAPI *EFI_FILE_DELETE)(EFI_FILE_PROTOCOL *This);
typedef EFI_STATUS (EFIAPI *EFI_FILE_READ)(EFI_FILE_PROTOCOL *This, UINTN *BufferSize, VOID *Buffer);
typedef EFI_STATUS (EFIAPI *EFI_FILE_WRITE)(EFI_FILE_PROTOCOL *This, UINTN *BufferSize, VOID *Buffer);
typedef EFI_STATUS (EFIAPI *EFI_FILE_GET_POSITION)(EFI_FILE_PROTOCOL *This, UINT64 *Position);
typedef EFI_STATUS (EFIAPI *EFI_FILE_SET_POSITION)(EFI_FILE_PROTOCOL *This, UINT64 Position);
typedef EFI_STATUS (EFIAPI *EFI_FILE_GET_INFO)(EFI_FILE_PROTOCOL *This, EFI_GUID *InformationType,
                                               UINTN *BufferSize, VOID *Buffer);
typedef EFI_STATUS (EFIAPI *EFI_FILE_SET_INFO)(EFI_FILE_PROTOCOL *This, EFI_GUID *InformationType,
                                               UINTN BufferSize, VOID *Buffer);
typedef EFI_STATUS (EFIAPI *EFI_FILE_FLUSH)(EFI_FILE_PROTOCOL *This);

struct _EFI_FILE_PROTOCOL {
    UINT64 Revision;
    EFI_FILE_OPEN         Open;
    EFI_FILE_CLOSE        Close;
    EFI_FILE_DELETE       Delete;
    EFI_FILE_READ         Read;
    EFI_FILE_WRITE        Write;
    EFI_FILE_GET_POSITION GetPosition;
    EFI_FILE_SET_POSITION SetPosition;
    EFI_FILE_GET_INFO     GetInfo;
    EFI_FILE_SET_INFO     SetInfo;
    EFI_FILE_FLUSH        Flush;
};

typedef struct {
    UINT64 Revision;
    EFI_STATUS (EFIAPI *OpenVolume)(VOID *This, EFI_FILE_PROTOCOL **Root);
} EFI_SIMPLE_FILE_SYSTEM_PROTOCOL;

#define EFI_FILE_MODE_READ   0x0000000000000001ULL
#define EFI_FILE_MODE_WRITE  0x0000000000000002ULL
#define EFI_FILE_MODE_CREATE 0x8000000000000000ULL

typedef struct {
    UINT16 Year;
    UINT8  Month;
    UINT8  Day;
    UINT8  Hour;
    UINT8  Minute;
    UINT8  Second;
    UINT8  Pad1;
    UINT32 Nanosecond;
    INT16  TimeZone;
    UINT8  Daylight;
    UINT8  Pad2;
} EFI_TIME;

/* 变长结构体，FileName 实际是变长数组 */
typedef struct {
    UINT64   Size;
    UINT64   FileSize;        /* 文件字节数 */
    UINT64   PhysicalSize;
    EFI_TIME CreateTime;
    EFI_TIME LastAccessTime;
    EFI_TIME ModificationTime;
    UINT64   Attribute;
    CHAR16   FileName[1];
} EFI_FILE_INFO;

/* ------------------------------------------------------------------ */
/* Graphics Output Protocol（帧缓冲）                                  */
/* ------------------------------------------------------------------ */
typedef enum {
    PixelRedGreenBlueReserved8BitPerColor = 0,
    PixelBlueGreenRedReserved8BitPerColor = 1,
    PixelBitMask                         = 2,
    PixelBltOnly                         = 3,
    PixelFormatMax                       = 4
} EFI_GRAPHICS_PIXEL_FORMAT;

typedef struct {
    UINT32 RedMask;
    UINT32 GreenMask;
    UINT32 BlueMask;
    UINT32 ReservedMask;
} EFI_PIXEL_BITMASK;

typedef struct {
    UINT32 Version;
    UINT32 HorizontalResolution;
    UINT32 VerticalResolution;
    UINT32 PixelFormat;
    EFI_PIXEL_BITMASK PixelInformation;
    UINT32 PixelsPerScanLine;
} EFI_GRAPHICS_OUTPUT_MODE_INFORMATION;

typedef struct {
    UINT32 MaxMode;
    UINT32 Mode;
    EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *Info;
    UINTN  SizeOfInfo;
    EFI_PHYSICAL_ADDRESS FrameBufferBase;
    UINTN  FrameBufferSize;
} EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE;

typedef struct {
    EFI_STATUS (EFIAPI *QueryMode)(VOID *This, UINT32 ModeNumber, UINTN *SizeOfInfo,
                                   EFI_GRAPHICS_OUTPUT_MODE_INFORMATION **Info);
    EFI_STATUS (EFIAPI *SetMode)(VOID *This, UINT32 ModeNumber);
    EFI_STATUS (EFIAPI *Blt)(VOID *This, VOID *BltBuffer, UINT32 BltOperation,
                             UINTN SourceX, UINTN SourceY, UINTN DestinationX, UINTN DestinationY,
                             UINTN Width, UINTN Height, UINTN Delta);
    EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE *Mode;
} EFI_GRAPHICS_OUTPUT_PROTOCOL;

/* ------------------------------------------------------------------ */
/* 一些杂项                                                            */
/* ------------------------------------------------------------------ */
#define EFI_SIZE_TO_PAGES(size) (((size) + 0xFFFULL) / 0x1000ULL)

#endif /* __UEFI_EFI_H__ */
