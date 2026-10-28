#pragma once

#include "base.h"
#include "block_device.h"
#include "block_cache.h"
#include "io_mutex.h"
#include <uapi/file.h>

/* FAT32 owns its path and open-file boundary; VFS consumes these APIs. */
#define OS_FAT32_PATH_LENGTH 256U
#define OS_FAT32_MAX_OPEN_FILES 512U

typedef struct OS_FAT32 OS_FAT32;

typedef struct {
    BOOLEAN Used;
    OS_FAT32 *FileSystem;
    UINT32 FirstCluster;
    UINT64 Size;
    UINT32 Attributes;
    UINT64 DirectoryLba;
    UINT32 DirectoryOffset;
    BOOLEAN CursorValid;
    UINT32 CursorLogicalStart;
    UINT32 CursorPhysicalStart;
    UINT32 CursorLength;
    UINT32 SeekCacheCount;
    UINT32 SeekCacheNext;
    UINT32 SeekLogical[8];
    UINT32 SeekPhysical[8];
    UINT32 SeekLength[8];
    UINT32 SeekIndexValid;
    UINT32 SeekIndexLogical[32];
    UINT32 SeekIndexPhysical[32];
    UINT32 SeekIndexLength[32];
} OS_FAT32_FILE;

struct OS_FAT32 {
    OS_BLOCK_DEVICE *Device;
    UINT32 BytesPerSector;
    UINT32 SectorsPerCluster;
    UINT32 ReservedSectorCount;
    UINT32 FatCount;
    UINT32 FatSectors;
    UINT32 RootCluster;
    UINT32 RootDirectorySectors;
    UINT32 ClusterCount;
    UINT64 FatStartLba;
    UINT64 RootDirectoryLba;
    UINT64 DataStartLba;
    UINT64 Fat3StartLba;
    UINT32 FatType;
    BOOLEAN Fat3Available;
    OS_BLOCK_CACHE Cache;
    UINT32 OpenFileLock;
    UINT32 MutationLock;
    io_mutex_t MutationMutex;
    OS_FAT32_FILE OpenFiles[OS_FAT32_MAX_OPEN_FILES];
    /* 由变更锁保护。首次查询扫描主 FAT，后续分配/释放同步维护。 */
    UINT64 FreeClusterCount;
    /* 变更锁保护：此位置之前的有效簇均已确认占用，释放簇时向前修正。 */
    UINT32 FreeClusterSearchStart;
    BOOLEAN FreeClusterCountValid;
    BOOLEAN Mounted;
};

BOOLEAN fat32_init(OS_FAT32 *filesystem,
                          OS_BLOCK_DEVICE *device);
BOOLEAN fat32_destroy(OS_FAT32 *filesystem);
BOOLEAN fat32_open(OS_FAT32 *filesystem, const CHAR8 *path,
                          OS_FAT32_FILE **out);
BOOLEAN fat32_close(OS_FAT32_FILE *file);
BOOLEAN fat32_stat_path(OS_FAT32 *filesystem, const CHAR8 *path,
                                os_file_info_t *info);
BOOLEAN fat32_enumerate_path(OS_FAT32 *filesystem, const CHAR8 *path,
                                     UINT32 index, os_file_info_t *info);
/* Visit at most limit entries in one traversal. A false visitor result stops
 * immediately; end-of-directory/read failure retains already visited entries. */
typedef BOOLEAN (*fat32_directory_visitor_t)(VOID *context,
                                                   const os_file_info_t *info);
VOID fat32_visit_directory(OS_FAT32 *filesystem, const CHAR8 *path,
                                  UINT32 limit, fat32_directory_visitor_t visitor,
                                  VOID *context);
BOOLEAN fat32_read_path(OS_FAT32 *filesystem, const CHAR8 *path,
                                UINT64 offset, VOID *buffer, UINT32 capacity,
                                UINT32 *read_size);
/* Read through an already-open FAT file so sequential callers retain the
 * cluster cursor across backend/page-cache requests. */
BOOLEAN fat32_read_file(OS_FAT32_FILE *file, UINT64 offset,
                                VOID *buffer, UINT32 capacity,
                                UINT32 *read_size);
BOOLEAN fat32_write_path(OS_FAT32 *filesystem, const CHAR8 *path,
                                 UINT64 offset, const VOID *buffer, UINT32 size,
                                 UINT32 *written_size);
BOOLEAN fat32_write_file(OS_FAT32_FILE *file, UINT64 offset,
                                const VOID *buffer, UINT32 size,
                                UINT32 *written_size);
BOOLEAN fat32_truncate_path(OS_FAT32 *filesystem, const CHAR8 *path,
                                    UINT64 size);
BOOLEAN fat32_truncate_file(OS_FAT32_FILE *file, UINT64 size);
BOOLEAN fat32_create_path(OS_FAT32 *filesystem, const CHAR8 *path,
                                  BOOLEAN directory);
BOOLEAN fat32_remove_path(OS_FAT32 *filesystem, const CHAR8 *path);
BOOLEAN fat32_remove_path_typed(OS_FAT32 *filesystem, const CHAR8 *path,
                                int directory);
BOOLEAN fat32_rename_path(OS_FAT32 *filesystem, const CHAR8 *old_path,
                                  const CHAR8 *new_path);
BOOLEAN fat32_sync(OS_FAT32 *filesystem);

/* 返回数据区的簇数、空闲簇数和簇大小；失败时不发布部分结果。 */
BOOLEAN fat32_space(OS_FAT32 *filesystem, UINT64 *blocks,
                    UINT64 *free_blocks, UINT64 *block_size);
