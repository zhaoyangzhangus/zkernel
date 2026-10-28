#include <kernel/fat32.h>
#include "internal.h"

/* REFACTOR_FS_FAT32_FILE_LIFECYCLE_OWNER：文件簇、I/O 和路径句柄。 */

#define FAT32_EOC_MIN       0x0FFFFFF8U
#define FAT32_ENTRY_SIZE    32U

static BOOLEAN locate_file_cluster(OS_FAT32_FILE *file, UINT32 logical_cluster,
                                   UINT32 *physical_cluster) {
    OS_FAT32 *filesystem = file->FileSystem;
    UINT32 start = file->FirstCluster;
    UINT32 logical = 0;
    if (file->FirstCluster == 0U) return 0;
    /* 当前 extent 已能解析此请求；只有移出该范围时 seek 提示才有用。
     * 先检查下界再相减，避免上界比较发生回绕。 */
    if (file->CursorValid && logical_cluster >= file->CursorLogicalStart &&
        logical_cluster - file->CursorLogicalStart < file->CursorLength) {
        *physical_cluster = file->CursorPhysicalStart +
                            logical_cluster - file->CursorLogicalStart;
        return fat32_cluster_valid(filesystem, *physical_cluster);
    }
    /* open/resize 导致游标失效时，也会使所有 seek 提示失效。 */
    if (!file->CursorValid) {
        file->SeekCacheCount = 0;
        file->SeekCacheNext = 0;
        file->SeekIndexValid = 0U;
    } else {
        UINT32 hints = file->SeekIndexValid;
        while (hints != 0U) {
            UINT32 i = (UINT32)__builtin_ctz(hints);
            hints &= hints - 1U;
            UINT32 candidate = file->SeekIndexLogical[i];
            if (candidate <= logical_cluster &&
                (file->CursorLogicalStart > logical_cluster ||
                 candidate > file->CursorLogicalStart)) {
                file->CursorLogicalStart = candidate;
                file->CursorPhysicalStart = file->SeekIndexPhysical[i];
                file->CursorLength = file->SeekIndexLength[i];
            }
        }
        for (UINT32 i = 0; i < file->SeekCacheCount; ++i) {
            UINT32 candidate = file->SeekLogical[i];
            if (candidate <= logical_cluster &&
                (file->CursorLogicalStart > logical_cluster ||
                 candidate > file->CursorLogicalStart)) {
                file->CursorLogicalStart = candidate;
                file->CursorPhysicalStart = file->SeekPhysical[i];
                file->CursorLength = file->SeekLength[i];
            }
        }
    }

    /* 顺序读取应从上次解析出的 extent 继续。旧代码只在命中当前 extent 时使用游标，
     * 每进入下一个 extent 都从 FirstCluster 重新开始，使碎片文件退化为 O(n^2)。 */
    if (file->CursorValid && logical_cluster >= file->CursorLogicalStart) {
        UINT32 extent = file->CursorLength;
        start = file->CursorPhysicalStart;
        logical = file->CursorLogicalStart;
        if (extent != 0U && logical_cluster - logical < extent) {
            *physical_cluster = start + logical_cluster - logical;
            return fat32_cluster_valid(filesystem, *physical_cluster);
        }
        if (extent == 0U || logical > UINT32_MAX - extent) {
            file->CursorValid = 0;
            start = file->FirstCluster;
            logical = 0U;
        } else {
            logical += extent;
            if (!fat32_read_next_cluster(filesystem, start + extent - 1U, &start) ||
                start == 0U) return 0;
        }
    }

    /* 仅遍历簇链时才需要检查点间隔；当前 extent 或 seek 提示直接解析请求时不需要。 */
    UINT64 cluster_bytes = (UINT64)filesystem->BytesPerSector *
                           filesystem->SectorsPerCluster;
    UINT64 clusters = cluster_bytes != 0U ?
        file->Size / cluster_bytes + (file->Size % cluster_bytes != 0U) :
        filesystem->ClusterCount;
    UINT64 stride = clusters / 32U + (clusters % 32U != 0U);
    if (stride == 0U) stride = 1U;
    for (UINT32 hops = 0; hops < filesystem->ClusterCount; ++hops) {
        /* 远距离定位按需求验证较长簇链，减少每 16 簇重复进入 FAT 缓存。
         * 单次最多 256 簇；目标附近仍保留原有短探测，不扩大普通小读。 */
        UINT32 probe = 16U;
        if (logical_cluster > logical) {
            UINT32 distance = logical_cluster - logical;
            if (distance >= 16U) probe = distance >= 255U ? 256U : distance + 1U;
        }
        UINT32 length = fat32_extent_length(filesystem, start,
            filesystem->Fat3Available ? UINT32_MAX : probe);
        /* 在每个按文件大小划分的区间内保留首次访问的 extent。
         * 即使热点缓存被淘汰，这些检查点也无需分配即可保留。 */
        UINT64 checkpoint = logical / stride;
        if (length != 0U && checkpoint < 32U) {
            UINT32 slot = (UINT32)checkpoint;
            if ((file->SeekIndexValid & (1U << slot)) == 0U) {
                file->SeekIndexLogical[slot] = logical;
                file->SeekIndexPhysical[slot] = start;
                file->SeekIndexLength[slot] = length;
                file->SeekIndexValid |= 1U << slot;
            } else if (logical >= file->SeekIndexLogical[slot]) {
                UINT32 offset = logical - file->SeekIndexLogical[slot];
                UINT64 end = (UINT64)offset + length;
                /* 仅合并已验证且物理地址一致的相邻/重叠区间，不能跨越碎片或未验证的空洞。
                 * 保留首次检查点的位置，同时让后续随机缺页复用整段已走过的连续簇链。 */
                if (offset <= file->SeekIndexLength[slot] &&
                    (UINT64)file->SeekIndexPhysical[slot] + offset == start &&
                    end > file->SeekIndexLength[slot] && end <= UINT32_MAX)
                    file->SeekIndexLength[slot] = (UINT32)end;
            }
        }
        if (logical_cluster >= logical && logical_cluster - logical < length) {
            file->CursorValid = 1;
            file->CursorLogicalStart = logical;
            file->CursorPhysicalStart = start;
            file->CursorLength = length;
            UINT32 slot = file->SeekCacheNext;
            file->SeekLogical[slot] = logical;
            file->SeekPhysical[slot] = start;
            file->SeekLength[slot] = length;
            file->SeekCacheNext = (slot + 1U) % 8U;
            if (file->SeekCacheCount < 8U) ++file->SeekCacheCount;
            *physical_cluster = start + logical_cluster - logical;
            return fat32_cluster_valid(filesystem, *physical_cluster);
        }
        UINT32 next;
        if (length == 0U || logical > UINT32_MAX - length ||
            !fat32_read_next_cluster(filesystem, start + length - 1U, &next) ||
            next == 0U) return 0;
        logical += length;
        start = next;
    }
    return 0;
}

static BOOLEAN count_file_clusters(const OS_FAT32 *filesystem, UINT32 first,
                                   UINT64 *count, UINT32 *last) {
    UINT32 cluster = first;
    UINT64 total = 0U;
    if (filesystem == 0 || count == 0 || first == 0U) {
        if (count != 0) *count = 0U;
        return first == 0U;
    }
    /* 统计需要走完整条链，连续区间无需按短随机读取的探测上限切段。
     * 按已统计簇数限制总工作量，损坏链循环时也不能反复扫描整卷。 */
    while (total < filesystem->ClusterCount) {
        UINT32 length;
        UINT32 next;
        UINT32 remaining = (UINT32)(filesystem->ClusterCount - total);
        if (!fat32_cluster_valid(filesystem, cluster) ||
            (length = fat32_extent_length(filesystem, cluster, remaining)) == 0U ||
            length > remaining) return 0;
        total += length;
        if (last != 0) *last = cluster + length - 1U;
        if (!fat32_read_next_cluster(filesystem, cluster + length - 1U, &next)) return 0;
        if (next == 0U) {
            *count = total;
            return 1;
        }
        cluster = next;
    }
    return 0;
}

static BOOLEAN update_file_directory_entry(OS_FAT32_FILE *file,
                                            UINT32 first_cluster, UINT64 size) {
    OS_FAT32 *filesystem = file == 0 ? 0 : file->FileSystem;
    UINT8 sector[4096];
    UINT8 *entry;
    if (filesystem == 0 || filesystem->BytesPerSector > sizeof(sector) ||
        file->DirectoryOffset > filesystem->BytesPerSector - FAT32_ENTRY_SIZE ||
        size > UINT32_MAX || !fat32_read_sector(filesystem, file->DirectoryLba, sector)) {
        return 0;
    }
    entry = sector + file->DirectoryOffset;
    fat32_write_u16(entry + 20U, (UINT16)(first_cluster >> 16));
    fat32_write_u16(entry + 26U, (UINT16)first_cluster);
    fat32_write_u32(entry + 28U, (UINT32)size);
    return fat32_write_sector(filesystem, file->DirectoryLba, sector);
}

/* 截断只修改目录项时，也须在发布内存长度前完成写回；失败时尽力恢复原目录项。 */
static BOOLEAN commit_truncate_size(OS_FAT32_FILE *file, UINT64 size) {
    OS_FAT32 *filesystem = file == 0 ? 0 : file->FileSystem;
    UINT8 original[FAT32_ENTRY_SIZE], replacement[FAT32_ENTRY_SIZE];
    if (filesystem == 0 || size > UINT32_MAX ||
        !fat32_read_directory_slot(filesystem, file->DirectoryLba,
                                   file->DirectoryOffset, original)) return 0;
    for (UINT32 i = 0U; i < FAT32_ENTRY_SIZE; ++i) replacement[i] = original[i];
    fat32_write_u32(replacement + 28U, (UINT32)size);
    if (fat32_write_directory_slot(filesystem, file->DirectoryLba,
                                   file->DirectoryOffset, replacement) &&
        fat32_writeback(filesystem)) return 1;
    (void)fat32_write_directory_slot(filesystem, file->DirectoryLba,
                                      file->DirectoryOffset, original);
    (void)fat32_writeback(filesystem);
    return 0;
}

/* 扩展前清空新暴露的区间。完整扇区无需读旧数据，首尾残片保留已有内容。 */
static BOOLEAN zero_file_range(OS_FAT32_FILE *file, UINT32 first,
                               UINT64 begin, UINT64 end) {
    OS_FAT32 *filesystem = file->FileSystem;
    OS_FAT32_FILE cursor = {0};
    UINT8 sector[4096];
    UINT64 cluster_bytes = (UINT64)filesystem->BytesPerSector *
                           filesystem->SectorsPerCluster;
    if (begin >= end) return 1;
    if (first == 0U || cluster_bytes == 0U || filesystem->BytesPerSector == 0U ||
        filesystem->BytesPerSector > sizeof(sector)) return 0;
    cursor.FileSystem = filesystem;
    cursor.FirstCluster = first;
    cursor.Size = end;
    while (begin < end) {
        UINT32 cluster, offset = (UINT32)(begin % cluster_bytes);
        UINT32 in_sector = offset % filesystem->BytesPerSector;
        UINT32 count = filesystem->BytesPerSector - in_sector;
        if ((UINT64)count > end - begin) count = (UINT32)(end - begin);
        if (!locate_file_cluster(&cursor, (UINT32)(begin / cluster_bytes), &cluster)) return 0;
        UINT64 lba = fat32_cluster_lba(filesystem, cluster) +
                     offset / filesystem->BytesPerSector;
        if ((in_sector != 0U || count != filesystem->BytesPerSector) &&
            !fat32_read_sector(filesystem, lba, sector)) return 0;
        __builtin_memset(sector + in_sector, 0, count);
        if (!fat32_write_sector(filesystem, lba, sector)) return 0;
        begin += count;
    }
    return 1;
}

static BOOLEAN extend_file_to(OS_FAT32_FILE *file, UINT64 required_size,
                              UINT64 zero_until) {
    OS_FAT32 *filesystem = file == 0 ? 0 : file->FileSystem;
    UINT64 cluster_bytes;
    UINT64 required_clusters;
    UINT64 existing_clusters = 0U;
    UINT32 old_last = 0U;
    UINT32 new_first = 0U;
    UINT32 new_last = 0U;
    UINT32 orphan = 0U;
    UINT32 scan_start = 2U;
    UINT32 old_first;
    BOOLEAN attached = 0;
    if (filesystem == 0 || required_size > UINT32_MAX) return 0;
    if (required_size <= file->Size) return 1;
    if (zero_until > required_size) return 0;
    cluster_bytes = (UINT64)filesystem->BytesPerSector *
                    filesystem->SectorsPerCluster;
    if (cluster_bytes == 0U || required_size > UINT64_MAX - (cluster_bytes - 1U)) {
        return 0;
    }
    required_clusters = (required_size + cluster_bytes - 1U) / cluster_bytes;
    old_first = file->FirstCluster;
    if (!count_file_clusters(filesystem, old_first, &existing_clusters, &old_last)) {
        return 0;
    }
    if (required_clusters <= existing_clusters) {
        /* 目录项提交成功后再发布内存长度；失败时保留旧状态，使重试仍会更新介质。 */
        if (!zero_file_range(file, old_first, file->Size, zero_until)) return 0;
        if (!update_file_directory_entry(file, old_first, required_size)) return 0;
        file->Size = required_size;
        file->CursorValid = 0;
        return 1;
    }
    if (required_clusters > filesystem->ClusterCount + 1ULL) return 0;

    for (UINT64 index = existing_clusters; index < required_clusters; ++index) {
        UINT32 cluster;
        if (!fat32_find_free_cluster_from(filesystem, scan_start, &cluster)) goto fail;
        orphan = cluster;
        if (!fat32_write_file_fat_value(filesystem, cluster, FAT32_EOC_MIN, 1U)) goto fail;
        orphan = 0U;
        if (new_first == 0U) {
            new_first = cluster;
        } else if (!fat32_write_file_fat_value(filesystem, new_last, cluster, 1U)) {
            orphan = cluster;
            goto fail;
        }
        new_last = cluster;
        /* 在此持变更锁事务中，较早的条目均已占用。不要持久化此提示：回滚/删除之后可能释放这些条目。 */
        scan_start = cluster + 1U;
    }
    if (old_last != 0U) {
        if (!fat32_write_file_fat_value(filesystem, old_last, new_first, 1U)) goto fail;
        attached = 1;
    } else {
        old_first = new_first;
    }
    if (!zero_file_range(file, old_first, file->Size, zero_until)) goto fail;
    if (!update_file_directory_entry(file, old_first, required_size)) goto fail;
    file->FirstCluster = old_first;
    file->Size = required_size;
    file->CursorValid = 0;
    return 1;

fail:
    if (attached) (void)fat32_write_file_fat_value(filesystem, old_last,
                                               FAT32_EOC_MIN, 1U);
    if (new_first != 0U) (void)fat32_free_cluster_chain(filesystem, new_first);
    if (orphan != 0U) (void)fat32_write_file_fat_value(filesystem, orphan, 0U, 0U);
    return 0;
}

static BOOLEAN truncate_file_to(OS_FAT32_FILE *file, UINT64 required_size) {
    OS_FAT32 *filesystem = file == 0 ? 0 : file->FileSystem;
    UINT64 cluster_bytes;
    UINT64 required_clusters;
    if (filesystem == 0 || required_size > UINT32_MAX) return 0;
    if (required_size > file->Size) return extend_file_to(file, required_size, required_size);
    /* 文件长度未变时无需遍历簇链、重写目录项或丢弃已验证的查找缓存。 */
    if (required_size == file->Size) return 1;
    cluster_bytes = (UINT64)filesystem->BytesPerSector * filesystem->SectorsPerCluster;
    if (cluster_bytes == 0U || required_size > UINT64_MAX - (cluster_bytes - 1U)) return 0;
    required_clusters = required_size == 0U ? 0U :
        (required_size + cluster_bytes - 1U) / cluster_bytes;
    if (required_clusters == 0U) {
        if (file->FirstCluster != 0U) {
            if (!fat32_truncate_clusters(file, 0U, 0U)) return 0;
        } else if (!commit_truncate_size(file, 0U)) return 0;
        file->FirstCluster = 0U;
    } else {
        UINT32 new_last, next;
        /* 仅定位新链尾，不再预先计数整条链。事务会验证待释放的后缀并保存回滚快照。
         * 使用局部游标，提交失败时原句柄的查找索引也保持不变。 */
        OS_FAT32_FILE cursor = *file;
        if (!locate_file_cluster(&cursor, (UINT32)(required_clusters - 1U), &new_last) ||
            !fat32_read_next_cluster(filesystem, new_last, &next)) return 0;
        if (next == 0U) {
            if (!commit_truncate_size(file, required_size)) return 0;
        } else if (!fat32_truncate_clusters(file, new_last, required_size)) return 0;
    }
    file->Size = required_size;
    file->CursorValid = 0;
    return 1;
}

/* 根据已验证的连续簇范围计算可直接传给块设备的字节数。 */
static UINT64 fat32_contiguous_extent_bytes(OS_FAT32_FILE *file, UINT32 logical_cluster,
    UINT32 in_cluster, UINT32 cluster_bytes, UINT64 wanted) {
    OS_FAT32 *filesystem = file->FileSystem;
    UINT64 extent_bytes = cluster_bytes - in_cluster;
    if (file->CursorValid && logical_cluster >= file->CursorLogicalStart &&
        logical_cluster - file->CursorLogicalStart < file->CursorLength) {
        extent_bytes = (UINT64)(file->CursorLength -
            (logical_cluster - file->CursorLogicalStart)) * cluster_bytes - in_cluster;
        /* 从已验证范围的尾部继续，避免重新扫描连续 FAT 项。 */
        if (!filesystem->Fat3Available && extent_bytes < wanted) {
            UINT64 missing = wanted - extent_bytes;
            UINT32 more = (UINT32)(missing / cluster_bytes +
                                   (missing % cluster_bytes != 0U));
            UINT32 tail = fat32_extent_length(filesystem,
                file->CursorPhysicalStart + file->CursorLength - 1U, more + 1U);
            if (tail > 1U) {
                file->CursorLength += tail - 1U;
                extent_bytes += (UINT64)(tail - 1U) * cluster_bytes;
            }
        }
    }
    return extent_bytes > wanted ? wanted : extent_bytes;
}

static BOOLEAN fat32_read(OS_FAT32_FILE *file, UINT64 offset, VOID *buffer,
                          UINT32 capacity, UINT32 *read_size) {
    OS_FAT32 *filesystem = file == 0 ? 0 : file->FileSystem;
    UINT8 sector[4096];
    UINT64 remaining;
    UINT8 *destination = (UINT8 *)buffer;
    if (file == 0 || filesystem == 0 || !file->Used || buffer == 0 || read_size == 0 ||
        capacity == 0U || offset > file->Size || filesystem->BytesPerSector > sizeof(sector)) {
        fat32_diag_failure("FAT_READ_ARGS_FAIL", offset, capacity,
                           file != 0 ? file->Size : 0U);
        return 0;
    }
    remaining = file->Size - offset;
    if (remaining > capacity) remaining = capacity;
    *read_size = 0;
    while ((UINT64)*read_size < remaining) {
        UINT64 current = offset + *read_size;
        UINT32 cluster_bytes = filesystem->BytesPerSector * filesystem->SectorsPerCluster;
        UINT32 logical_cluster = (UINT32)(current / cluster_bytes);
        UINT32 physical_cluster;
        UINT32 in_cluster = (UINT32)(current % cluster_bytes);
        UINT32 sector_offset = in_cluster % filesystem->BytesPerSector;
        UINT32 copy_size;
        if (!locate_file_cluster(file, logical_cluster, &physical_cluster)) {
            fat32_diag_failure("FAT_LOCATE_FAIL", current, logical_cluster,
                               file->FirstCluster);
            return 0;
        }
        UINT64 sector_lba = fat32_cluster_lba(filesystem, physical_cluster) +
                            in_cluster / filesystem->BytesPerSector;
        /* 小型读取仍正常进入缓存；仅当请求足以填满整个扇区缓存时才采用流式读取。 */
        if (sector_offset == 0U && remaining - *read_size >=
                (UINT64)OS_BLOCK_CACHE_ENTRY_COUNT * filesystem->BytesPerSector) {
            UINT64 wanted = remaining - *read_size;
            if (wanted > OS_BLOCK_CACHE_MAX_READ_BYTES) wanted = OS_BLOCK_CACHE_MAX_READ_BYTES;
            UINT64 extent_bytes = fat32_contiguous_extent_bytes(file, logical_cluster,
                in_cluster, cluster_bytes, wanted);
            UINT64 sectors = extent_bytes / filesystem->BytesPerSector;
            UINT32 maximum = OS_BLOCK_CACHE_MAX_READ_BYTES / filesystem->BytesPerSector;
            if (sectors > maximum) sectors = maximum;
            if (sectors >= OS_BLOCK_CACHE_ENTRY_COUNT) {
                BOOLEAN success = 0;
                for (UINT32 attempt = 0U; attempt < 3U && !success; ++attempt) {
                    success = filesystem->Cache.Initialized ?
                        block_cache_read_range(&filesystem->Cache,
                            sector_lba, (UINT32)sectors, destination + *read_size) :
                        block_read(filesystem->Device, sector_lba,
                            (UINT32)sectors, destination + *read_size);
                }
                if (!success) {
                    fat32_diag_failure("FAT_DATA_RANGE_FAIL", sector_lba,
                                       sectors, physical_cluster);
                    return 0;
                }
                *read_size += (UINT32)sectors * filesystem->BytesPerSector;
                continue;
            }
        }
        copy_size = filesystem->BytesPerSector - sector_offset;
        if (copy_size > remaining - *read_size) copy_size = (UINT32)(remaining - *read_size);
        /* 完整扇区直接填充调用方缓冲区；仅头尾残片需要栈上中转。 */
        BOOLEAN complete_sector = sector_offset == 0U &&
                                  copy_size == filesystem->BytesPerSector;
        UINT8 *target = complete_sector ? destination + *read_size : sector;
        if (!fat32_read_sector(filesystem, sector_lba, target)) {
            fat32_diag_failure("FAT_DATA_SECTOR_FAIL", sector_lba,
                               logical_cluster, physical_cluster);
            return 0;
        }
        if (!complete_sector)
            __builtin_memcpy(destination + *read_size, sector + sector_offset, copy_size);
        *read_size += copy_size;
    }
    return 1;
}

static BOOLEAN fat32_write_locked(OS_FAT32_FILE *file, UINT64 offset,
                                  const VOID *buffer, UINT32 size,
                                  UINT32 *written_size) {
    OS_FAT32 *filesystem = file == 0 ? 0 : file->FileSystem;
    UINT8 sector[4096];
    const UINT8 *source = (const UINT8 *)buffer;
    if (file == 0 || filesystem == 0 || !file->Used ||
        (buffer == 0 && size != 0U) || written_size == 0 ||
        offset > UINT64_MAX - size || filesystem->BytesPerSector > sizeof(sector)) return 0;
    *written_size = 0;
    if (size == 0U) return 1;
    if (!extend_file_to(file, offset + size,
                        offset > file->Size ? offset : file->Size)) return 0;
    while (*written_size < size) {
        UINT64 current = offset + *written_size;
        UINT32 cluster_bytes = filesystem->BytesPerSector * filesystem->SectorsPerCluster;
        UINT32 logical_cluster = (UINT32)(current / cluster_bytes);
        UINT32 physical_cluster;
        UINT32 in_cluster = (UINT32)(current % cluster_bytes);
        UINT32 sector_offset = in_cluster % filesystem->BytesPerSector;
        UINT32 copy_size = filesystem->BytesPerSector - sector_offset;
        UINT64 lba;
        if (copy_size > size - *written_size) copy_size = size - *written_size;
        if (!locate_file_cluster(file, logical_cluster, &physical_cluster)) return 0;
        lba = fat32_cluster_lba(filesystem, physical_cluster) +
              in_cluster / filesystem->BytesPerSector;
        /* 大块完整扇区沿连续簇直接写入；小块与头尾残片继续使用缓存。 */
        if (sector_offset == 0U && size - *written_size >=
                (UINT64)OS_BLOCK_CACHE_ENTRY_COUNT * filesystem->BytesPerSector) {
            UINT64 wanted = size - *written_size;
            if (wanted > OS_BLOCK_CACHE_MAX_WRITE_BYTES) wanted = OS_BLOCK_CACHE_MAX_WRITE_BYTES;
            UINT64 extent_bytes = fat32_contiguous_extent_bytes(file, logical_cluster,
                in_cluster, cluster_bytes, wanted);
            UINT32 sectors = (UINT32)(extent_bytes / filesystem->BytesPerSector);
            if (sectors >= OS_BLOCK_CACHE_ENTRY_COUNT) {
                if (!block_cache_write_range(&filesystem->Cache, lba, sectors,
                                             source + *written_size)) return 0;
                *written_size += sectors * filesystem->BytesPerSector;
                continue;
            }
        }
        /* 完整覆盖时既不读旧扇区，也不复制到栈缓冲区；缓存仍复制并持有自己的数据。 */
        const UINT8 *data = source + *written_size;
        if (sector_offset != 0U || copy_size != filesystem->BytesPerSector) {
            if (!fat32_read_sector(filesystem, lba, sector)) return 0;
            __builtin_memcpy(sector + sector_offset, data, copy_size);
            data = sector;
        }
        if (!block_cache_write(&filesystem->Cache, lba, data)) return 0;
        *written_size += copy_size;
    }
    return 1;
}

static BOOLEAN fat32_write(OS_FAT32_FILE *file, UINT64 offset,
                           const VOID *buffer, UINT32 size,
                           UINT32 *written_size) {
    OS_FAT32 *filesystem = file == 0 ? 0 : file->FileSystem;
    BOOLEAN success;
    if (filesystem == 0) return 0;

    fat32_mutation_lock(filesystem);
    success = fat32_write_locked(file, offset, buffer, size, written_size);
    fat32_mutation_unlock(filesystem);
    return success;
}

BOOLEAN fat32_close(OS_FAT32_FILE *file) {
    OS_FAT32 *filesystem = file == 0 ? 0 : file->FileSystem;
    if (file == 0 || filesystem == 0) return 0;

    fat32_open_files_lock(filesystem);
    if (!file->Used) {
        fat32_open_files_unlock(filesystem);
        return 0;
    }
    file->Used = 0;
    file->CursorValid = 0;
    fat32_open_files_unlock(filesystem);
    return 1;
}

BOOLEAN fat32_read_path(OS_FAT32 *filesystem, const CHAR8 *path,
                                UINT64 offset, VOID *buffer, UINT32 capacity,
                                UINT32 *read_size) {
    OS_FAT32_FILE *file = 0;
    BOOLEAN success;
    if (filesystem == 0 || path == 0 || read_size == 0) return 0;
    if (capacity == 0U) {
        *read_size = 0;
        return 1;
    }
    if (!fat32_open(filesystem, path, &file)) return 0;
    success = fat32_read(file, offset, buffer, capacity, read_size);
    if (!fat32_close(file)) success = 0;
    return success;
}

BOOLEAN fat32_read_file(OS_FAT32_FILE *file, UINT64 offset,
                               VOID *buffer, UINT32 capacity,
                               UINT32 *read_size) {
    OS_FAT32 *filesystem = file == 0 ? 0 : file->FileSystem;
    if (filesystem == 0 || read_size == 0) return 0;
    return fat32_read(file, offset, buffer, capacity, read_size);
}

BOOLEAN fat32_write_path(OS_FAT32 *filesystem, const CHAR8 *path,
                                 UINT64 offset, const VOID *buffer, UINT32 size,
                                 UINT32 *written_size) {
    OS_FAT32_FILE *file = 0;
    BOOLEAN success;
    if (filesystem == 0 || path == 0 || written_size == 0) return 0;
    if (size == 0U) {
        *written_size = 0;
        return 1;
    }
    if (!fat32_open(filesystem, path, &file)) return 0;
    success = fat32_write(file, offset, buffer, size, written_size);
    if (!fat32_close(file)) success = 0;
    return success;
}

BOOLEAN fat32_write_file(OS_FAT32_FILE *file, UINT64 offset,
                                const VOID *buffer, UINT32 size,
                                UINT32 *written_size) {
    OS_FAT32 *filesystem = file == 0 ? 0 : file->FileSystem;
    if (filesystem == 0 || written_size == 0) return 0;
    return fat32_write(file, offset, buffer, size, written_size);
}

static BOOLEAN fat32_truncate_path_locked(OS_FAT32 *filesystem,
                                           const CHAR8 *path, UINT64 size) {
    OS_FAT32_FILE *file = 0;
    BOOLEAN success;
    if (filesystem == 0 || path == 0 || size > UINT32_MAX ||
        !fat32_open(filesystem, path, &file)) return 0;
    success = truncate_file_to(file, size);
    if (!fat32_close(file)) success = 0;
    return success;
}

BOOLEAN fat32_truncate_file(OS_FAT32_FILE *file, UINT64 size) {
    OS_FAT32 *filesystem = file == 0 ? 0 : file->FileSystem;
    if (filesystem == 0 || !file->Used || size > UINT32_MAX) return 0;
    fat32_mutation_lock(filesystem);
    BOOLEAN success = fat32_writeback(filesystem);
    if (success) success = truncate_file_to(file, size);
    if (success) success = fat32_writeback(filesystem);
    fat32_mutation_unlock(filesystem);
    return success;
}

BOOLEAN fat32_truncate_path(OS_FAT32 *filesystem, const CHAR8 *path,
                                    UINT64 size) {
    BOOLEAN success;
    if (filesystem == 0) return 0;
    fat32_mutation_lock(filesystem);
    success = fat32_writeback(filesystem);
    if (success) {
        success = fat32_truncate_path_locked(filesystem, path, size);
        if (success) success = fat32_writeback(filesystem);
    }
    fat32_mutation_unlock(filesystem);
    return success;
}
