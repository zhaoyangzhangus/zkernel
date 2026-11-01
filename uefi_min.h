#ifndef ZKERNEL_UEFI_MIN_H
#define ZKERNEL_UEFI_MIN_H
#include <stdint.h>
#include <stddef.h>
/* EFI x86-64 uses Microsoft x64 ABI. Compiled with clang -target x86_64-pc-windows-msvc. */
typedef uint64_t EFI_STATUS;
typedef uint64_t EFI_PHYSICAL_ADDRESS;
typedef void *EFI_HANDLE;
typedef uint16_t CHAR16;
#define EFI_SUCCESS UINT64_C(0)
#define EFI_ERROR_BIT (UINT64_C(1) << 63)
#define EFI_ERROR(s) ((s) & EFI_ERROR_BIT)
#define EFI_NOT_FOUND (EFI_ERROR_BIT | UINT64_C(14))
#define EFI_BUFFER_TOO_SMALL (EFI_ERROR_BIT | UINT64_C(5))
#define EFI_OUT_OF_RESOURCES (EFI_ERROR_BIT | UINT64_C(9))
#define EFI_VARIABLE_NV_BS_RT (UINT32_C(1)|UINT32_C(2)|UINT32_C(4))
#define EFI_ALLOCATE_ANY_PAGES 0
#define EFI_ALLOCATE_MAX_ADDRESS 1
#define EFI_LOADER_DATA 2
#define EFI_PAGE_BYTES 4096u

typedef struct {
    uint64_t signature;
    uint32_t revision;
    uint32_t header_size;
    uint32_t crc32;
    uint32_t reserved;
} EFI_TABLE_HEADER;
typedef struct {
    uint32_t a;
    uint16_t b, c;
    uint8_t d[8];
} EFI_GUID;
typedef struct {
    void *reset;
    EFI_STATUS (*output_string)(void *self, const CHAR16 *str);
    void *rest[8];
} EFI_SIMPLE_TEXT_OUTPUT;
typedef struct {
    EFI_TABLE_HEADER header;
    void *firmware_vendor;
    uint32_t firmware_revision;
    uint32_t padding;
    EFI_HANDLE console_in_handle;
    void *con_in;
    EFI_HANDLE console_out_handle;
    EFI_SIMPLE_TEXT_OUTPUT *con_out;
    EFI_HANDLE stderr_handle;
    EFI_SIMPLE_TEXT_OUTPUT *stderr;
    void *runtime_services;
    void *boot_services;
    uint64_t nr_config_tables;
    void *config_tables;
} EFI_SYSTEM_TABLE;
/* Service tables: after standard 24-byte table header follow 64-bit function pointers. */
typedef struct {
    EFI_TABLE_HEADER header;
    void *slot[64];
} EFI_SERVICES;
enum {
    BS_ALLOCATE_PAGES = 2,
    BS_FREE_PAGES = 3,
    BS_GET_MEMORY_MAP = 4,
    BS_ALLOCATE_POOL = 5,
    BS_FREE_POOL = 6,
    BS_STALL = 28,
    BS_SET_WATCHDOG_TIMER = 29,
    RT_GET_VARIABLE = 6,
    RT_SET_VARIABLE = 8,
};
typedef EFI_STATUS (*EFI_ALLOC_PAGES)(int, int, size_t, EFI_PHYSICAL_ADDRESS *);
typedef EFI_STATUS (*EFI_FREE_PAGES)(EFI_PHYSICAL_ADDRESS, size_t);
typedef EFI_STATUS (*EFI_STALL)(size_t microseconds);
typedef EFI_STATUS (*EFI_SET_WATCHDOG)(size_t, uint64_t, size_t, const CHAR16 *);
typedef EFI_STATUS (*EFI_GET_VARIABLE)(const CHAR16 *, const EFI_GUID *, uint32_t *, size_t *, void *);
typedef EFI_STATUS (*EFI_SET_VARIABLE)(const CHAR16 *, const EFI_GUID *, uint32_t, size_t, const void *);
#endif
