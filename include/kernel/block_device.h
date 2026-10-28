#pragma once

#include <uefi.h>

/*
 * 块设备注册表属于内核存储接口，不是 USB 兼容性细节。将其 ABI 保存在规范内核头文件中，
 * 使 NVMe、USB MSC、页缓存和文件系统代码共用同一声明。
 */
#define OS_BLOCK_DEVICE_COUNT 32U
#define OS_BLOCK_NAME_LENGTH  32U

typedef BOOLEAN (*OS_BLOCK_READ)(VOID *context, UINT64 lba,
                                     UINT32 count, VOID *buffer);
typedef BOOLEAN (*OS_BLOCK_WRITE)(VOID *context, UINT64 lba,
                                      UINT32 count, const VOID *buffer);
typedef BOOLEAN (*OS_BLOCK_FLUSH)(VOID *context);

typedef struct {
    CHAR8 Name[OS_BLOCK_NAME_LENGTH];
    UINT32 BlockSize;
    UINT64 BlockCount;
    OS_BLOCK_READ Read;
    OS_BLOCK_WRITE Write;
    OS_BLOCK_FLUSH Flush;
    VOID *Context;
    BOOLEAN Registered;
} OS_BLOCK_DEVICE;

typedef struct {
    OS_BLOCK_DEVICE Devices[OS_BLOCK_DEVICE_COUNT];
    UINT32 DeviceCount;
    BOOLEAN Initialized;
} OS_BLOCK_MANAGER;

BOOLEAN block_manager_init(OS_BLOCK_MANAGER *manager);
BOOLEAN block_manager_destroy(OS_BLOCK_MANAGER *manager);
BOOLEAN block_register(OS_BLOCK_MANAGER *manager,
                              const CHAR8 *name, UINT32 block_size,
                              UINT64 block_count, OS_BLOCK_READ read,
                              OS_BLOCK_WRITE write,
                              OS_BLOCK_FLUSH flush, VOID *context,
                              OS_BLOCK_DEVICE **device);
BOOLEAN block_unregister(OS_BLOCK_MANAGER *manager,
                                 OS_BLOCK_DEVICE *device);
BOOLEAN block_read(const OS_BLOCK_DEVICE *device, UINT64 lba,
                          UINT32 count, VOID *buffer);
BOOLEAN block_write(const OS_BLOCK_DEVICE *device, UINT64 lba,
                           UINT32 count, const VOID *buffer);
BOOLEAN block_flush(const OS_BLOCK_DEVICE *device);
