#include "internal.h"

/* REFACTOR_FS_FAT32_FAT_TABLE_OWNER：介质 I/O、FAT 项和簇链。 */

#define FAT_TABLE_EOC_MIN       0x0FFFFFF8U
#define FAT_TABLE_BAD_CLUSTER   0x0FFFFFF7U
#define FAT_TABLE16_EOC_MIN     0x0000FFF8U
#define FAT_TABLE16_BAD_CLUSTER 0x0000FFF7U
#define FAT_TABLE_TYPE_16       16U

#define FAT_DIAG_COM1_DATA   0x3F8U
#define FAT_DIAG_COM1_STATUS 0x3FDU

static atomic_flag g_fat_diag_lock = ATOMIC_FLAG_INIT;

static void fat32_diag_putc(char value) {
    uint16_t status_port = FAT_DIAG_COM1_STATUS;
    uint16_t data_port = FAT_DIAG_COM1_DATA;
    for (uint32_t attempt = 0U; attempt < 100000U; ++attempt) {
        uint8_t ready;
        __asm__ volatile ("inb %w1, %0" : "=a"(ready) : "d"(status_port));
        if ((ready & 0x20U) != 0U) break;
        __asm__ volatile ("pause");
    }
    __asm__ volatile ("outb %0, %w1" : : "a"((uint8_t)value), "d"(data_port));
}

static void fat32_diag_text(const char *text) {
    while (*text != '\0') fat32_diag_putc(*text++);
}

static void fat32_diag_hex(uint64_t value) {
    static const char digits[] = "0123456789abcdef";
    fat32_diag_text("0x");
    for (int shift = 60; shift >= 0; shift -= 4) {
        fat32_diag_putc(digits[(value >> (uint32_t)shift) & 0xFU]);
    }
}

void fat32_diag_failure(const char *tag, UINT64 first, UINT64 second,
                        UINT64 third) {
    while (atomic_flag_test_and_set_explicit(&g_fat_diag_lock,
                                              memory_order_acquire)) {
        __asm__ volatile ("pause");
    }
    fat32_diag_text("OS_");
    fat32_diag_text(tag);
    fat32_diag_text(" a=");
    fat32_diag_hex(first);
    fat32_diag_text(" b=");
    fat32_diag_hex(second);
    fat32_diag_text(" c=");
    fat32_diag_hex(third);
    fat32_diag_text("\r\n");
    atomic_flag_clear_explicit(&g_fat_diag_lock, memory_order_release);
}

BOOLEAN fat32_cluster_valid(const OS_FAT32 *filesystem, UINT32 cluster) {
    return filesystem != 0 && cluster >= 2U &&
           cluster <= filesystem->ClusterCount + 1U;
}

BOOLEAN fat32_root_directory_cluster(const OS_FAT32 *filesystem,
                                      UINT32 cluster) {
    return filesystem != 0 && filesystem->FatType == FAT_TABLE_TYPE_16 &&
           cluster == 0U;
}

static BOOLEAN fat32_read_sector_cached(const OS_BLOCK_DEVICE *device,
    OS_BLOCK_CACHE *cache, UINT64 lba, UINT8 *buffer) {
    if (buffer == 0 || device == 0 || lba >= device->BlockCount) {
        fat32_diag_failure("FAT_SECTOR_RANGE_FAIL", lba,
                           device != 0 ? device->BlockCount : 0U,
                           buffer != 0);
        return 0;
    }
    /*
     * 即使此 FAT API 是同步接口，NVMe 后端仍采用异步执行。
     * 完成事件可能与延迟队列消费者竞争，短暂返回 I/O 错误。
     * 请求仍在本地处理时，重试读取同一扇区。
     */
    for (UINT32 attempt = 0U; attempt < 3U; ++attempt) {
        BOOLEAN success = cache != 0 && cache->Initialized ?
            block_cache_read(cache,
                                    lba, buffer) :
            block_read(device, lba, 1U, buffer);
        if (success) return 1;
        __asm__ volatile ("pause");
    }
    fat32_diag_failure("FAT_SECTOR_IO_FAIL", lba,
                       device->BlockCount,
                       cache != 0 && cache->Initialized);
    return 0;
}

BOOLEAN fat32_read_sector(const OS_FAT32 *filesystem, UINT64 lba,
                          UINT8 *buffer) {
    return fat32_read_sector_cached(filesystem != 0 ? filesystem->Device : 0,
        filesystem != 0 ? (OS_BLOCK_CACHE *)&filesystem->Cache : 0,
        lba, buffer);
}

BOOLEAN fat32_read_boot_sector(const OS_BLOCK_DEVICE *device, UINT8 *buffer) {
    return fat32_read_sector_cached(device, 0, 0U, buffer);
}

BOOLEAN fat32_write_sector(OS_FAT32 *filesystem, UINT64 lba,
                           const UINT8 *buffer) {
    if (filesystem == 0 || buffer == 0 || filesystem->Device == 0 ||
        lba >= filesystem->Device->BlockCount) return 0;
    if (filesystem->Cache.Initialized) {
        return block_cache_write(&filesystem->Cache, lba, buffer);
    }
    return block_write(filesystem->Device, lba, 1U, buffer);
}

UINT64 fat32_cluster_lba(const OS_FAT32 *filesystem, UINT32 cluster) {
    return filesystem->DataStartLba +
           (UINT64)(cluster - 2U) * filesystem->SectorsPerCluster;
}

BOOLEAN fat32_read_fat_entry(const OS_FAT32 *filesystem, UINT32 fat_number,
                             UINT32 cluster, UINT32 *value) {
    UINT8 sector[4096];
    UINT64 byte_offset;
    UINT64 lba;
    UINT32 offset;
    UINT32 entry_size;
    UINT64 fat_start;
    if (filesystem == 0 || value == 0 ||
        !fat32_cluster_valid(filesystem, cluster) || fat_number == 0U ||
        fat_number > filesystem->FatCount) return 0;
    entry_size = filesystem->FatType == FAT_TABLE_TYPE_16 ?
                 sizeof(UINT16) : sizeof(UINT32);
    byte_offset = (UINT64)cluster * entry_size;
    lba = byte_offset / filesystem->BytesPerSector;
    offset = (UINT32)(byte_offset % filesystem->BytesPerSector);
    if (filesystem->BytesPerSector > sizeof(sector) ||
        offset + entry_size > filesystem->BytesPerSector) return 0;
    fat_start = filesystem->FatStartLba +
                (UINT64)(fat_number - 1U) * filesystem->FatSectors;
    if (!fat32_read_sector(filesystem, fat_start + lba, sector)) return 0;
    *value = filesystem->FatType == FAT_TABLE_TYPE_16 ?
             fat32_read_u16(sector + offset) :
             fat32_read_u32(sector + offset) & 0x0FFFFFFFU;
    return 1;
}

BOOLEAN fat32_write_fat_entry(OS_FAT32 *filesystem, UINT32 fat_number,
                              UINT32 cluster, UINT32 value) {
    UINT8 sector[4096];
    UINT64 byte_offset;
    UINT64 lba;
    UINT32 offset;
    UINT32 entry_size;
    UINT64 fat_start;
    if (filesystem == 0 ||
        !fat32_cluster_valid(filesystem, cluster) || fat_number == 0U ||
        fat_number > filesystem->FatCount ||
        filesystem->BytesPerSector > sizeof(sector)) return 0;
    entry_size = filesystem->FatType == FAT_TABLE_TYPE_16 ?
                 sizeof(UINT16) : sizeof(UINT32);
    byte_offset = (UINT64)cluster * entry_size;
    lba = byte_offset / filesystem->BytesPerSector;
    offset = (UINT32)(byte_offset % filesystem->BytesPerSector);
    if (offset + entry_size > filesystem->BytesPerSector) return 0;
    fat_start = filesystem->FatStartLba +
                (UINT64)(fat_number - 1U) * filesystem->FatSectors;
    if (!fat32_read_sector(filesystem, fat_start + lba, sector)) return 0;
    UINT32 old_value = filesystem->FatType == FAT_TABLE_TYPE_16 ?
        fat32_read_u16(sector + offset) : fat32_read_u32(sector + offset) & 0x0FFFFFFFU;
    value &= filesystem->FatType == FAT_TABLE_TYPE_16 ? 0xFFFFU : 0x0FFFFFFFU;
    if (filesystem->FatType == FAT_TABLE_TYPE_16) {
        fat32_write_u16(sector + offset, (UINT16)value);
    } else {
        fat32_write_u32(sector + offset, value);
    }
    if (!fat32_write_sector(filesystem, fat_start + lba, sector)) {
        if (fat_number == 1U) {
            filesystem->FreeClusterCountValid = 0;
            filesystem->FreeClusterSearchStart = 2U;
        }
        return 0;
    }
    if (fat_number == 1U) {
        /* 搜索下界不是未经验证的“下一个空闲簇”提示。释放和事务回滚必须重新暴露更早的空洞；
         * 分配当前下界后只前进一簇，剩余占用区间仍由下一次搜索实际验证。 */
        UINT32 floor = filesystem->FreeClusterSearchStart;
        if (floor < 2U || (UINT64)floor > (UINT64)filesystem->ClusterCount + 2U)
            floor = 2U;
        if (value == 0U && cluster < floor) floor = cluster;
        else if (value != 0U && cluster == floor) ++floor;
        filesystem->FreeClusterSearchStart = floor;
    }
    if (fat_number == 1U && filesystem->FreeClusterCountValid) {
        if (old_value == 0U && value != 0U) {
            if (filesystem->FreeClusterCount == 0U) filesystem->FreeClusterCountValid = 0;
            else --filesystem->FreeClusterCount;
        } else if (old_value != 0U && value == 0U) {
            if (filesystem->FreeClusterCount >= filesystem->ClusterCount)
                filesystem->FreeClusterCountValid = 0;
            else ++filesystem->FreeClusterCount;
        }
    }
    return 1;
}

BOOLEAN fat32_write_file_fat_value(OS_FAT32 *filesystem, UINT32 cluster,
                                   UINT32 chain_value, UINT32 extent_value) {
    for (UINT32 fat = 1U; fat <= filesystem->FatCount; ++fat) {
        UINT32 value = filesystem->Fat3Available && fat == 3U ?
                       extent_value : chain_value;
        if (!fat32_write_fat_entry(filesystem, fat, cluster, value)) return 0;
    }
    return 1;
}

BOOLEAN fat32_find_free_cluster_from(OS_FAT32 *filesystem, UINT32 first,
                                      UINT32 *cluster) {
    UINT8 sector[4096];
    UINT64 loaded_lba = 0U;
    BOOLEAN loaded = 0;
    UINT32 entry_size;
    if (filesystem == 0 || cluster == 0 || first < 2U ||
        filesystem->FatCount == 0U ||
        filesystem->ClusterCount > UINT32_MAX - 2U ||
        filesystem->BytesPerSector == 0U ||
        filesystem->BytesPerSector > sizeof(sector)) return 0;
    UINT32 floor = filesystem->FreeClusterSearchStart;
    if (floor < 2U || floor > filesystem->ClusterCount + 2U) floor = 2U;
    if (first < floor) first = floor;
    entry_size = filesystem->FatType == FAT_TABLE_TYPE_16 ?
                 sizeof(UINT16) : sizeof(UINT32);
    for (UINT32 candidate = first;
         candidate <= filesystem->ClusterCount + 1U; ++candidate) {
        UINT64 byte_offset = (UINT64)candidate * entry_size;
        UINT64 lba = byte_offset / filesystem->BytesPerSector;
        UINT32 offset = (UINT32)(byte_offset % filesystem->BytesPerSector);
        if (offset + entry_size > filesystem->BytesPerSector) return 0;
        /* 分配由调用方的变更锁串行化。只保留本次扫描的扇区快照；
         * 下一次分配会重新读取数据。 */
        if (!loaded || loaded_lba != lba) {
            if (!fat32_read_sector(filesystem,
                filesystem->FatStartLba + lba, sector)) return 0;
            loaded_lba = lba;
            loaded = 1;
        }
        UINT32 value = filesystem->FatType == FAT_TABLE_TYPE_16 ?
                       fat32_read_u16(sector + offset) :
                       fat32_read_u32(sector + offset) & 0x0FFFFFFFU;
        if (value == 0U) {
            /* 只有从已知下界开始的完整扫描才能扩展下界。显式跳到较后位置不能跳过未检查区间。 */
            if (first == floor) filesystem->FreeClusterSearchStart = candidate;
            *cluster = candidate;
            return 1;
        }
    }
    if (first == floor) filesystem->FreeClusterSearchStart = filesystem->ClusterCount + 2U;
    return 0;
}

BOOLEAN fat32_find_free_cluster(OS_FAT32 *filesystem, UINT32 *cluster) {
    return fat32_find_free_cluster_from(filesystem, 2U, cluster);
}

BOOLEAN fat32_read_next_cluster(const OS_FAT32 *filesystem, UINT32 cluster,
                                UINT32 *next) {
    UINT32 value;
    UINT32 eoc_min = filesystem != 0 && filesystem->FatType == FAT_TABLE_TYPE_16 ?
                     FAT_TABLE16_EOC_MIN : FAT_TABLE_EOC_MIN;
    UINT32 bad_cluster = filesystem != 0 &&
                         filesystem->FatType == FAT_TABLE_TYPE_16 ?
                         FAT_TABLE16_BAD_CLUSTER : FAT_TABLE_BAD_CLUSTER;
    if (!fat32_read_fat_entry(filesystem, 1U, cluster, &value) ||
        value == bad_cluster) return 0;
    if (value >= eoc_min) {
        *next = 0U;
        return 1;
    }
    if (!fat32_cluster_valid(filesystem, value)) return 0;
    *next = value;
    return 1;
}

UINT32 fat32_extent_length(const OS_FAT32 *filesystem, UINT32 cluster,
                           UINT32 maximum) {
    UINT32 length;
    if (maximum == 0U) return 0U;
    if (!filesystem->Fat3Available) {
        /* 标准 FAT 映像没有第三张表中的 extent 提示。应验证实际簇链的有界前缀，
         * 而不是把每个簇都视为碎片。游标缓存会复用这段已验证区间。 */
        UINT8 sector[4096];
        UINT64 loaded_lba = 0U;
        BOOLEAN loaded = 0;
        UINT32 entry_size = filesystem->FatType == FAT_TABLE_TYPE_16 ?
                            sizeof(UINT16) : sizeof(UINT32);
        if (filesystem->FatCount == 0U ||
            filesystem->BytesPerSector == 0U ||
            filesystem->BytesPerSector > sizeof(sector) ||
            !fat32_cluster_valid(filesystem, cluster)) return 1U;
        length = 1U;
        while (length < maximum &&
               fat32_cluster_valid(filesystem, cluster + length)) {
            UINT64 byte_offset = (UINT64)(cluster + length - 1U) * entry_size;
            UINT64 lba = byte_offset / filesystem->BytesPerSector;
            UINT32 offset = (UINT32)(byte_offset % filesystem->BytesPerSector);
            if (offset + entry_size > filesystem->BytesPerSector) break;
            /* 同次探测中，相邻 FAT 项复用一个扇区快照，避免反复加锁和复制。
             * 不跨调用保留快照；簇链变更仍由调用方的文件系统锁串行化。 */
            if (!loaded || loaded_lba != lba) {
                if (!fat32_read_sector(filesystem,
                    filesystem->FatStartLba + lba, sector)) break;
                loaded_lba = lba;
                loaded = 1;
            }
            UINT32 next = filesystem->FatType == FAT_TABLE_TYPE_16 ?
                          fat32_read_u16(sector + offset) :
                          fat32_read_u32(sector + offset) & 0x0FFFFFFFU;
            if (next != cluster + length) break;
            ++length;
        }
        return length;
    }
    if (
        !fat32_read_fat_entry(filesystem, 3U, cluster, &length) ||
        length == 0U || length > filesystem->ClusterCount + 2U - cluster) {
        return 1U;
    }
    return length < maximum ? length : maximum;
}

/* 只扫描有效数据簇；镜像 FAT 和扩展表不参与计数。变更锁可睡眠，扫描时允许调度。 */
BOOLEAN fat32_space(OS_FAT32 *filesystem, UINT64 *blocks,
                    UINT64 *free_blocks, UINT64 *block_size) {
    UINT8 sector[4096];
    if (filesystem == 0 || blocks == 0 || free_blocks == 0 || block_size == 0 ||
        !filesystem->Mounted || filesystem->BytesPerSector < 512U ||
        filesystem->BytesPerSector > sizeof(sector) ||
        (filesystem->BytesPerSector & (filesystem->BytesPerSector - 1U)) != 0U ||
        filesystem->SectorsPerCluster == 0U || filesystem->FatCount == 0U ||
        filesystem->ClusterCount == 0U || filesystem->ClusterCount > 0x0FFFFFF0U ||
        (filesystem->FatType != 16U && filesystem->FatType != 32U)) return 0;
    UINT32 entry_size = filesystem->FatType == 16U ? 2U : 4U;
    if (((UINT64)filesystem->ClusterCount + 2U) * entry_size >
        (UINT64)filesystem->FatSectors * filesystem->BytesPerSector) return 0;
    fat32_mutation_lock(filesystem);
    if (!filesystem->FreeClusterCountValid) {
        UINT64 free_count = 0U, loaded = UINT64_MAX;
        UINT32 first_free = filesystem->ClusterCount + 2U;
        for (UINT32 cluster = 2U; cluster < filesystem->ClusterCount + 2U; ++cluster) {
            UINT64 byte = (UINT64)cluster * entry_size;
            UINT64 lba = byte / filesystem->BytesPerSector;
            UINT32 offset = (UINT32)(byte % filesystem->BytesPerSector);
            if (loaded != lba) {
                if (!fat32_read_sector(filesystem, filesystem->FatStartLba + lba, sector)) {
                    fat32_mutation_unlock(filesystem);
                    return 0;
                }
                loaded = lba;
            }
            UINT32 value = entry_size == 2U ? fat32_read_u16(sector + offset) :
                           fat32_read_u32(sector + offset) & 0x0FFFFFFFU;
            if (value == 0U) {
                ++free_count;
                if (first_free == filesystem->ClusterCount + 2U) first_free = cluster;
            }
        }
        filesystem->FreeClusterCount = free_count;
        /* 空间查询已经完整验证主 FAT，分配器无需再扫描相同的已占用前缀。 */
        filesystem->FreeClusterSearchStart = first_free;
        filesystem->FreeClusterCountValid = 1;
    }
    *blocks = filesystem->ClusterCount;
    *free_blocks = filesystem->FreeClusterCount;
    *block_size = (UINT64)filesystem->BytesPerSector * filesystem->SectorsPerCluster;
    fat32_mutation_unlock(filesystem);
    return 1;
}
