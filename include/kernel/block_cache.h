#pragma once

#include <kernel/block_device.h>
#include <kernel/mutex.h>

#define OS_BLOCK_CACHE_ENTRY_COUNT 16U
#define OS_BLOCK_CACHE_MAX_SIZE    4096U
#define OS_BLOCK_CACHE_MAX_READ_BYTES (1024U * 1024U)
#define OS_BLOCK_CACHE_MAX_WRITE_BYTES (1024U * 1024U)

typedef struct {
    BOOLEAN Valid;
    BOOLEAN Dirty;
    UINT64 Lba;
    UINT64 LastUse;
    UINT8 Data[OS_BLOCK_CACHE_MAX_SIZE];
} OS_BLOCK_CACHE_ENTRY;

typedef struct {
    const OS_BLOCK_DEVICE *Device;
    OS_BLOCK_CACHE_ENTRY Entries[OS_BLOCK_CACHE_ENTRY_COUNT];
    UINT64 Clock;
    UINT64 LastReadLba;
    BOOLEAN LastReadValid;
    /* 已调度的调用方可在后端处理磁盘 I/O 时休眠。 */
    kmutex_t WaitLock;
    /* 用于单线程启动早期或引导空闲阶段的回退锁。 */
    UINT32 Lock;
    BOOLEAN Initialized;
} OS_BLOCK_CACHE;

BOOLEAN block_cache_init(OS_BLOCK_CACHE *cache,
                                const OS_BLOCK_DEVICE *device);
BOOLEAN block_cache_read(OS_BLOCK_CACHE *cache, UINT64 lba,
                                VOID *buffer);
/* 流式读取有界范围，同时处理干净和已修改的缓存扇区。 */
BOOLEAN block_cache_read_range(OS_BLOCK_CACHE *cache, UINT64 lba,
                                      UINT32 count, VOID *buffer);
BOOLEAN block_cache_write(OS_BLOCK_CACHE *cache, UINT64 lba,
                                 const VOID *buffer);
/* 连续完整扇区直接写入，同时维护已有缓存的一致性。 */
BOOLEAN block_cache_write_range(OS_BLOCK_CACHE *cache, UINT64 lba,
                                UINT32 count, const VOID *buffer);
/* 将已修改扇区写入设备，但不下发设备缓存刷新命令。 */
BOOLEAN block_cache_writeback(OS_BLOCK_CACHE *cache);
BOOLEAN block_cache_flush(OS_BLOCK_CACHE *cache);
BOOLEAN block_cache_destroy(OS_BLOCK_CACHE *cache);
BOOLEAN block_cache_invalidate(OS_BLOCK_CACHE *cache, UINT64 lba);
