#include <kernel/fat32.h>
#include "internal.h"

#ifdef OS_KERNEL_BUILD
#include <kernel/kmem.h>
#else
#include <stdlib.h>
#endif

#define FAT32_EOC_MIN       0x0FFFFFF8U
#define FAT32_BAD_CLUSTER   0x0FFFFFF7U
#define FAT16_EOC_MIN       0x0000FFF8U
#define FAT16_BAD_CLUSTER   0x0000FFF7U
#define FAT_TYPE_16         16U
#define FAT_TYPE_32         32U
#define FAT32_DIRECTORY     0x10U
#define FAT32_ENTRY_SIZE    32U
#define FAT32_ATTRIBUTE_LFN 0x0FU
#define FAT32_MAX_LFN_ENTRIES 20U
#define FAT32_MAX_CREATE_SLOTS (FAT32_MAX_LFN_ENTRIES + 1U)

static BOOLEAN is_power_of_two(UINT32 value) {
    return value != 0U && (value & (value - 1U)) == 0U;
}

BOOLEAN fat32_stat_path(OS_FAT32 *filesystem, const CHAR8 *path,
                                os_file_info_t *info) {
    UINT8 entry[32];
    UINT32 path_length = 0U;
    UINT32 position = 0U;
    if (filesystem == 0 || !filesystem->Mounted || path == 0 || info == 0) return 0;
    while (path_length < OS_FAT32_PATH_LENGTH && path[path_length] != 0) ++path_length;
    if (path_length >= OS_FAT32_PATH_LENGTH) return 0;
    while (position < path_length && path[position] == '/') ++position;
    if (position == path_length) {
        *info = (os_file_info_t){0};
        info->type = OS_FILE_TYPE_DIRECTORY;
        info->mode = 0040755U;
        info->name[0] = '/';
        return 1;
    }
    if (!fat32_resolve_path(filesystem, path + position, entry, 0, 0)) return 0;
    *info = (os_file_info_t){0};
    info->type = (entry[11] & FAT32_DIRECTORY) != 0U ?
        OS_FILE_TYPE_DIRECTORY : OS_FILE_TYPE_REGULAR;
    info->mode = info->type == OS_FILE_TYPE_DIRECTORY ? 0040755U : 0100666U;
    info->size = info->type == OS_FILE_TYPE_DIRECTORY ? 0U : fat32_read_u32(entry + 28U);
    UINT32 start = path_length;
    while (start > 0U && path[start - 1U] != '/') --start;
    UINT32 name_length = path_length - start;
    if (name_length >= OS_FILE_NAME_MAX) name_length = OS_FILE_NAME_MAX - 1U;
    for (UINT32 index = 0U; index < name_length; ++index) {
        info->name[index] = path[start + index];
    }
    info->name[name_length] = 0;
    return 1;
}

static BOOLEAN fat32_enumerate_directory(OS_FAT32 *filesystem, const CHAR8 *path,
                                          UINT32 index, UINT32 limit,
                                          fat32_directory_visitor_t visitor,
                                          VOID *context) {
    UINT32 directory;
    UINT32 found = 0U;
    if (filesystem == 0 || visitor == 0 || limit == 0U || !fat32_resolve_directory_cluster(filesystem, path,
                                                                       &directory)) {
        return 0;
    }
    UINT8 sector[4096];
    UINT16 lfn_characters[260];
    BOOLEAN lfn_valid = 0;
    UINT32 lfn_expected = 0U;
    UINT8 lfn_checksum = 0U;
    UINT32 found_lfn_count = 0U;
    fat32_lfn_invalidate(&lfn_valid, &lfn_expected, &lfn_checksum);
    for (UINT32 chain = 0U; chain < filesystem->ClusterCount; ++chain) {
        BOOLEAN fixed_root = fat32_root_directory_cluster(filesystem, directory);
        if ((!fixed_root && !fat32_cluster_valid(filesystem, directory)) ||
            filesystem->BytesPerSector > sizeof(sector)) return 0;
        UINT64 first_sector = fixed_root ? filesystem->RootDirectoryLba :
                           fat32_cluster_lba(filesystem, directory);
        UINT32 sector_count = fixed_root ? filesystem->RootDirectorySectors :
                            filesystem->SectorsPerCluster;
        for (UINT32 sector_index = 0U; sector_index < sector_count;
             ++sector_index) {
            if (!fat32_read_sector(filesystem, first_sector + sector_index, sector)) return 0;
            for (UINT32 offset = 0U; offset < filesystem->BytesPerSector;
                 offset += FAT32_ENTRY_SIZE) {
                const UINT8 *entry = sector + offset;
                if (entry[0] == 0U) return 0;
                if (entry[0] == 0xE5U) {
                    fat32_lfn_invalidate(&lfn_valid, &lfn_expected, &lfn_checksum);
                    found_lfn_count = 0U;
                    continue;
                }
                if (entry[11] == FAT32_ATTRIBUTE_LFN) {
                    if ((entry[0] & 0x40U) != 0U) found_lfn_count = 0U;
                    if (found_lfn_count < FAT32_MAX_LFN_ENTRIES) ++found_lfn_count;
                    fat32_lfn_store_entry(entry, lfn_characters, &lfn_valid,
                                    &lfn_expected, &lfn_checksum);
                    continue;
                }
                if ((entry[11] & 0x08U) != 0U || fat32_dot_entry(entry)) {
                    fat32_lfn_invalidate(&lfn_valid, &lfn_expected, &lfn_checksum);
                    found_lfn_count = 0U;
                    continue;
                }
                if (found >= index) {
                    os_file_info_t value;
                    os_file_info_t *info = &value;
                    *info = (os_file_info_t){0};
                    info->type = (entry[11] & FAT32_DIRECTORY) != 0U ?
                        OS_FILE_TYPE_DIRECTORY : OS_FILE_TYPE_REGULAR;
                    info->mode = info->type == OS_FILE_TYPE_DIRECTORY ?
                        0040755U : 0100666U;
                    info->size = info->type == OS_FILE_TYPE_DIRECTORY ?
                        0U : fat32_read_u32(entry + 28U);
                    if (lfn_valid && lfn_expected == 0U &&
                        fat32_short_name_checksum(entry) == lfn_checksum) {
                        fat32_lfn_name_text(lfn_characters, info->name);
                    } else {
                        fat32_short_name_text(entry, info->name);
                    }
                    if (!visitor(context, info)) return 0;
                    if (--limit == 0U) return 1;
                }
                ++found;
                fat32_lfn_invalidate(&lfn_valid, &lfn_expected, &lfn_checksum);
                found_lfn_count = 0U;
            }
        }
        if (fixed_root) return 0;
        UINT32 next;
        if (!fat32_read_next_cluster(filesystem, directory, &next) || next == 0U) return 0;
        directory = next;
    }
    return 0;
}

static BOOLEAN fat32_copy_directory_entry(VOID *context, const os_file_info_t *info) {
    *(os_file_info_t *)context = *info;
    return 1;
}

BOOLEAN fat32_enumerate_path(OS_FAT32 *filesystem, const CHAR8 *path,
                                     UINT32 index, os_file_info_t *info) {
    if (info == 0) return 0;
    return fat32_enumerate_directory(filesystem, path, index, 1U,
                                      fat32_copy_directory_entry, info);
}

VOID fat32_visit_directory(OS_FAT32 *filesystem, const CHAR8 *path,
                                  UINT32 limit, fat32_directory_visitor_t visitor,
                                  VOID *context) {
    (void)fat32_enumerate_directory(filesystem, path, 0U, limit, visitor, context);
}

static BOOLEAN make_creation_name(OS_FAT32 *filesystem, UINT32 parent,
                                  const CHAR8 *leaf, UINT32 length,
                                  fat32_creation_name_t *name) {
    if (filesystem == 0 || leaf == 0 || name == 0 ||
        length == 0U || length > 255U) return 0;
    name->units = 0U;
    name->lfn_count = 0U;
    name->checksum = 0U;
    if (fat32_make_short_name(leaf, length, name->short_name)) return 1;
    name->units = fat32_encode_long_name(leaf, length, name->characters);
    if (name->units == 0U) return 0;
    name->lfn_count = (name->units + 12U) / 13U;
    if (!fat32_allocate_long_name_alias(filesystem, parent, leaf, length,
                                        name->short_name)) return 0;
    name->checksum = fat32_short_name_checksum(name->short_name);
    return 1;
}

static BOOLEAN fat32_create_path_locked(OS_FAT32 *filesystem,
                                        const CHAR8 *path,
                                        BOOLEAN directory) {
    CHAR8 leaf[OS_FAT32_PATH_LENGTH];
    UINT8 entry[FAT32_ENTRY_SIZE];
    fat32_creation_name_t name;
    UINT8 lfn_entry[FAT32_ENTRY_SIZE];
    UINT64 slot_lba[FAT32_MAX_CREATE_SLOTS];
    UINT32 slot_offset[FAT32_MAX_CREATE_SLOTS];
    UINT32 new_clusters[FAT32_MAX_CREATE_SLOTS];
    UINT32 parent;
    UINT32 leaf_length = 0U;
    UINT32 slot_count;
    UINT32 new_cluster_count = 0U;
    UINT32 extension_anchor = 0U;
    UINT32 written_slots = 0U;
    UINT32 first_cluster = 0U;
    if (!fat32_resolve_parent_directory(filesystem, path, &parent, leaf) ||
        (!fat32_cluster_valid(filesystem, parent) &&
         !fat32_root_directory_cluster(filesystem, parent))) return 0;
    while (leaf_length < OS_FAT32_PATH_LENGTH && leaf[leaf_length] != 0) ++leaf_length;
    if (leaf_length == 0U || leaf_length > 255U ||
        fat32_find_directory_entry(filesystem, parent, leaf, leaf_length, entry, 0, 0) ||
        !make_creation_name(filesystem, parent, leaf, leaf_length, &name)) return 0;
    slot_count = name.lfn_count + 1U;
    if (!fat32_find_directory_slots(filesystem, parent, slot_count, slot_lba, slot_offset,
                              new_clusters, &new_cluster_count, &extension_anchor)) return 0;

    for (UINT32 i = 0U; i < FAT32_ENTRY_SIZE; ++i) entry[i] = 0U;
    for (UINT32 i = 0U; i < 11U; ++i) entry[i] = name.short_name[i];
    entry[11] = directory ? FAT32_DIRECTORY : 0x20U;
    if (directory) {
        if (!fat32_allocate_zero_cluster(filesystem, &first_cluster) ||
            !fat32_initialize_directory_cluster(filesystem, first_cluster, parent)) {
            if (first_cluster != 0U) (void)fat32_free_cluster_chain(filesystem, first_cluster);
            fat32_rollback_directory_extensions(filesystem, extension_anchor, new_clusters,
                                          new_cluster_count);
            return 0;
        }
    }
    fat32_write_u16(entry + 20U, (UINT16)(first_cluster >> 16));
    fat32_write_u16(entry + 26U, (UINT16)first_cluster);
    fat32_write_u32(entry + 28U, 0U);
    for (UINT32 ordinal = name.lfn_count; ordinal != 0U; --ordinal) {
        fat32_make_lfn_entry(name.characters, name.units, ordinal,
                             name.checksum, lfn_entry);
        UINT32 slot = name.lfn_count - ordinal;
        if (!fat32_write_directory_slot(filesystem, slot_lba[slot], slot_offset[slot],
                                  lfn_entry)) break;
        ++written_slots;
    }
    if (written_slots == name.lfn_count &&
        fat32_write_directory_slot(filesystem, slot_lba[name.lfn_count], slot_offset[name.lfn_count],
                             entry)) {
        return 1;
    }
    for (UINT32 i = 0U; i < written_slots; ++i) {
        (void)fat32_delete_directory_slot(filesystem, slot_lba[i], slot_offset[i]);
    }
    if (written_slots == name.lfn_count) {
        (void)fat32_delete_directory_slot(filesystem, slot_lba[name.lfn_count],
                                     slot_offset[name.lfn_count]);
    }
    if (first_cluster != 0U) (void)fat32_free_cluster_chain(filesystem, first_cluster);
    fat32_rollback_directory_extensions(filesystem, extension_anchor, new_clusters,
                                  new_cluster_count);
    return 0;
}

BOOLEAN fat32_create_path(OS_FAT32 *filesystem, const CHAR8 *path,
                                  BOOLEAN directory) {
    BOOLEAN success;
    if (filesystem == 0) return 0;
    fat32_mutation_lock(filesystem);
    success = fat32_create_path_locked(filesystem, path, directory);
    if (success) success = fat32_writeback(filesystem);
    fat32_mutation_unlock(filesystem);
    return success;
}

static BOOLEAN fat32_remove_path_locked(OS_FAT32 *filesystem,
                                        const CHAR8 *path, int directory) {
    CHAR8 leaf[OS_FAT32_PATH_LENGTH];
    UINT8 entry[FAT32_ENTRY_SIZE];
    UINT64 lfn_lba[FAT32_MAX_LFN_ENTRIES];
    UINT32 lfn_offset[FAT32_MAX_LFN_ENTRIES];
    UINT32 parent;
    UINT32 leaf_length = 0U;
    UINT64 entry_lba = 0U;
    UINT32 entry_offset = 0U;
    UINT32 lfn_count = 0U;
    UINT32 first_cluster;
    UINT8 lfn_entries[FAT32_MAX_LFN_ENTRIES][FAT32_ENTRY_SIZE];
    UINT8 short_entry[FAT32_ENTRY_SIZE];
    UINT32 deleted_lfn = 0U;
    if (!fat32_resolve_parent_directory(filesystem, path, &parent, leaf) ||
        filesystem->BytesPerSector > OS_BLOCK_CACHE_MAX_SIZE) return 0;
    while (leaf_length < OS_FAT32_PATH_LENGTH && leaf[leaf_length] != 0) ++leaf_length;
    if (leaf_length == 0U || (leaf[0] == '.' && (leaf_length == 1U || leaf[1] == '.'))) {
        return 0;
    }
    if (!fat32_find_directory_entry_ex(filesystem, parent, leaf, leaf_length, entry,
                                 &entry_lba, &entry_offset, lfn_lba, lfn_offset,
                                 &lfn_count) ||
        fat32_file_is_open_at(filesystem, entry_lba, entry_offset)) return 0;
    if (directory >= 0 &&
        (((entry[11] & FAT32_DIRECTORY) != 0U) != (directory != 0))) return 0;
    first_cluster = fat32_entry_cluster(entry);
    if ((entry[11] & FAT32_DIRECTORY) != 0U) {
        if (first_cluster == 0U || !fat32_directory_is_empty(filesystem, first_cluster)) return 0;
    }
    if (!fat32_read_directory_slot(filesystem, entry_lba, entry_offset, short_entry)) return 0;
    for (UINT32 i = 0U; i < lfn_count; ++i) {
        if (!fat32_read_directory_slot(filesystem, lfn_lba[i], lfn_offset[i], lfn_entries[i])) {
            return 0;
        }
    }
    /* 先隐藏短名，避免中途失败时查找器看到没有对应短名的 LFN。 */
    if (!fat32_delete_directory_slot(filesystem, entry_lba, entry_offset)) return 0;
    for (UINT32 i = 0U; i < lfn_count; ++i) {
        if (!fat32_delete_directory_slot(filesystem, lfn_lba[i], lfn_offset[i])) {
            for (UINT32 restore = 0U; restore < deleted_lfn; ++restore) {
                (void)fat32_write_directory_slot(filesystem, lfn_lba[restore],
                                            lfn_offset[restore], lfn_entries[restore]);
            }
            (void)fat32_write_directory_slot(filesystem, entry_lba, entry_offset, short_entry);
            return 0;
        }
        ++deleted_lfn;
    }
    if (first_cluster == 0U || fat32_free_cluster_chain(filesystem, first_cluster)) return 1;

    /* FAT 释放失败时，目录项也必须恢复，否则文件会变成不可达对象。 */
    for (UINT32 restore = 0U; restore < lfn_count; ++restore) {
        (void)fat32_write_directory_slot(filesystem, lfn_lba[restore], lfn_offset[restore],
                                    lfn_entries[restore]);
    }
    (void)fat32_write_directory_slot(filesystem, entry_lba, entry_offset, short_entry);
    return 0;
}

BOOLEAN fat32_remove_path_typed(OS_FAT32 *filesystem, const CHAR8 *path,
                                int directory) {
    BOOLEAN success;
    if (filesystem == 0) return 0;
    fat32_mutation_lock(filesystem);
    success = fat32_writeback(filesystem);
    if (success) {
        success = fat32_remove_path_locked(filesystem, path, directory);
        if (success) success = fat32_writeback(filesystem);
    }
    fat32_mutation_unlock(filesystem);
    return success;
}

BOOLEAN fat32_remove_path(OS_FAT32 *filesystem, const CHAR8 *path) {
    return fat32_remove_path_typed(filesystem, path, -1);
}

static BOOLEAN fat32_restore_directory_slots(
    OS_FAT32 *filesystem, const UINT64 *lba,
    const UINT32 *offset, UINT32 count,
    const UINT8 entries[][FAT32_ENTRY_SIZE]) {
    BOOLEAN success = 1;
    for (UINT32 index = 0U; index < count; ++index) {
        if (!fat32_write_directory_slot(filesystem, lba[index], offset[index],
                                        entries[index])) success = 0;
    }
    return success;
}

static BOOLEAN fat32_delete_directory_slots(OS_FAT32 *filesystem,
                                             const UINT64 *lba,
                                             const UINT32 *offset,
                                             UINT32 count) {
    BOOLEAN success = 1;
    for (UINT32 index = 0U; index < count; ++index) {
        if (!fat32_delete_directory_slot(filesystem, lba[index], offset[index])) {
            success = 0;
        }
    }
    return success;
}

static BOOLEAN fat32_rename_path_locked(OS_FAT32 *filesystem,
                                         const CHAR8 *old_path,
                                         const CHAR8 *new_path) {
    CHAR8 old_leaf[OS_FAT32_PATH_LENGTH];
    CHAR8 new_leaf[OS_FAT32_PATH_LENGTH];
    UINT8 source_entry[FAT32_ENTRY_SIZE];
    UINT8 destination_entry[FAT32_ENTRY_SIZE];
    UINT8 destination_lfn[FAT32_ENTRY_SIZE];
    fat32_creation_name_t name;
    UINT64 source_lba[FAT32_MAX_LFN_ENTRIES];
    UINT32 source_offset[FAT32_MAX_LFN_ENTRIES];
    UINT64 destination_lba[FAT32_MAX_CREATE_SLOTS];
    UINT32 destination_offset[FAT32_MAX_CREATE_SLOTS];
    UINT32 old_parent;
    UINT32 new_parent;
    UINT32 old_leaf_length = 0U;
    UINT32 new_leaf_length = 0U;
    UINT32 source_lfn_count = 0U;
    UINT32 destination_slot_count;
    UINT32 destination_new_clusters[FAT32_MAX_CREATE_SLOTS];
    UINT32 destination_new_cluster_count = 0U;
    UINT32 destination_extension_anchor = 0U;
    UINT64 source_short_lba = 0U;
    UINT32 source_short_offset = 0U;
    /* 直接读取到回滚快照，避免保存两份相同目录项；目标项逐个生成并写入。 */
    UINT8 saved_slots[FAT32_MAX_CREATE_SLOTS][FAT32_ENTRY_SIZE];
    UINT64 saved_lba[FAT32_MAX_CREATE_SLOTS];
    UINT32 saved_offset[FAT32_MAX_CREATE_SLOTS];
    UINT32 saved_count;
    UINT32 written_count = 0U;
    BOOLEAN same_parent;
    BOOLEAN reuse_source_slots = 0;

    if (filesystem == 0 || old_path == 0 || new_path == 0 ||
        !fat32_resolve_parent_directory(filesystem, old_path, &old_parent,
                                         old_leaf) ||
        !fat32_resolve_parent_directory(filesystem, new_path, &new_parent,
                                         new_leaf)) return 0;
    while (old_leaf_length < OS_FAT32_PATH_LENGTH &&
           old_leaf[old_leaf_length] != 0) ++old_leaf_length;
    while (new_leaf_length < OS_FAT32_PATH_LENGTH &&
           new_leaf[new_leaf_length] != 0) ++new_leaf_length;
    if (old_leaf_length == 0U || new_leaf_length == 0U ||
        old_leaf_length > 255U || new_leaf_length > 255U ||
        (new_leaf[0] == '.' && (new_leaf_length == 1U ||
                                (new_leaf_length == 2U && new_leaf[1] == '.')))) {
        return 0;
    }
    if (!fat32_find_directory_entry_ex(filesystem, old_parent, old_leaf,
                                       old_leaf_length, source_entry,
                                       &source_short_lba, &source_short_offset,
                                       source_lba, source_offset,
                                       &source_lfn_count)) {
        return 0;
    }
    UINT64 target_lba = 0U;
    UINT32 target_offset = 0U;
    if (fat32_find_directory_entry(filesystem, new_parent, new_leaf,
            new_leaf_length, destination_entry, &target_lba, &target_offset)) {
        if (target_lba == source_short_lba && target_offset == source_short_offset) return 1;
        return fat32_replace_file_entry(filesystem, source_short_lba, source_short_offset,
            source_lba, source_offset, source_lfn_count, target_lba, target_offset);
    }
    if ((source_entry[11] & FAT32_DIRECTORY) != 0U && old_parent != new_parent) {
        /* 移动目录还必须修改其 '..' 项。在原生 FAT 目录父项更新实现前，
         * 暂不将该操作纳入此小型事务。 */
        return 0;
    }
    if (!fat32_read_directory_slot(filesystem, source_short_lba,
                                   source_short_offset, saved_slots[source_lfn_count])) return 0;
    for (UINT32 index = 0U; index < source_lfn_count; ++index) {
        if (!fat32_read_directory_slot(filesystem, source_lba[index],
                                       source_offset[index], saved_slots[index])) {
            return 0;
        }
    }
    if (!make_creation_name(filesystem, new_parent, new_leaf, new_leaf_length, &name)) {
        return 0;
    }
    destination_slot_count = name.lfn_count + 1U;
    same_parent = old_parent == new_parent;
    if (same_parent && destination_slot_count <= source_lfn_count + 1U) {
        reuse_source_slots = 1;
        for (UINT32 index = 0U; index < name.lfn_count; ++index) {
            if (index < source_lfn_count) {
                destination_lba[index] = source_lba[index];
                destination_offset[index] = source_offset[index];
            }
        }
        if (name.lfn_count < source_lfn_count) {
            destination_lba[name.lfn_count] =
                source_lba[name.lfn_count];
            destination_offset[name.lfn_count] =
                source_offset[name.lfn_count];
        } else {
            destination_lba[name.lfn_count] = source_short_lba;
            destination_offset[name.lfn_count] = source_short_offset;
        }
    } else {
        if (!fat32_find_directory_slots(filesystem, new_parent,
                                         destination_slot_count, destination_lba,
                                         destination_offset,
                                         destination_new_clusters,
                                         &destination_new_cluster_count,
                                         &destination_extension_anchor)) return 0;
    }

    for (UINT32 index = 0U; index < source_lfn_count; ++index) {
        saved_lba[index] = source_lba[index];
        saved_offset[index] = source_offset[index];
    }
    saved_lba[source_lfn_count] = source_short_lba;
    saved_offset[source_lfn_count] = source_short_offset;
    saved_count = source_lfn_count + 1U;

    for (UINT32 ordinal = name.lfn_count; ordinal != 0U; --ordinal) {
        fat32_make_lfn_entry(name.characters, name.units, ordinal,
                             name.checksum, destination_lfn);
        if (!fat32_write_directory_slot(filesystem, destination_lba[written_count],
                                        destination_offset[written_count],
                                        destination_lfn)) goto rollback;
        ++written_count;
    }
    for (UINT32 byte = 0U; byte < FAT32_ENTRY_SIZE; ++byte) {
        destination_entry[byte] = saved_slots[source_lfn_count][byte];
    }
    for (UINT32 byte = 0U; byte < 11U; ++byte) {
        destination_entry[byte] = name.short_name[byte];
    }
    if (!fat32_write_directory_slot(filesystem,
                                    destination_lba[name.lfn_count],
                                    destination_offset[name.lfn_count],
                                    destination_entry)) goto rollback;
    written_count = destination_slot_count;

    if (reuse_source_slots) {
        UINT32 used_source_lfn = name.lfn_count < source_lfn_count ?
                                  name.lfn_count + 1U : source_lfn_count;
        for (UINT32 index = used_source_lfn; index < source_lfn_count; ++index) {
            if (!fat32_delete_directory_slot(filesystem, source_lba[index],
                                             source_offset[index])) goto rollback;
        }
        if (name.lfn_count < source_lfn_count &&
            !fat32_delete_directory_slot(filesystem, source_short_lba,
                                         source_short_offset)) goto rollback;
    } else if (!fat32_delete_directory_slots(filesystem, source_lba,
                                              source_offset, source_lfn_count) ||
               !fat32_delete_directory_slot(filesystem, source_short_lba,
                                            source_short_offset)) goto rollback;
    /* 回写成功前保留已保存的目录槽位。提交失败时不得向打开的句柄发布新坐标。 */
    if (!fat32_writeback(filesystem)) goto rollback;
    fat32_rebind_open_file(filesystem, source_short_lba, source_short_offset,
                           destination_lba[name.lfn_count],
                           destination_offset[name.lfn_count]);
    return 1;

rollback:
    if (!reuse_source_slots && written_count != 0U) {
        (void)fat32_delete_directory_slots(filesystem, destination_lba,
                                           destination_offset, written_count);
    }
    (void)fat32_restore_directory_slots(filesystem, saved_lba, saved_offset,
                                        saved_count, saved_slots);
    if (!reuse_source_slots) {
        fat32_rollback_directory_extensions(filesystem,
                                            destination_extension_anchor,
                                            destination_new_clusters,
                                            destination_new_cluster_count);
    }
    /* 尽力恢复暂时性写入失败。介质持续故障时无法保证持久性；保留脏缓存，
     * 供之后的 sync 重试。 */
    (void)fat32_writeback(filesystem);
    return 0;
}

BOOLEAN fat32_rename_path(OS_FAT32 *filesystem,
                                  const CHAR8 *old_path,
                                  const CHAR8 *new_path) {
    BOOLEAN success;
    if (filesystem == 0) return 0;
    fat32_mutation_lock(filesystem);
    success = fat32_writeback(filesystem);
    if (success) {
        success = fat32_rename_path_locked(filesystem, old_path, new_path);
    }
    fat32_mutation_unlock(filesystem);
    return success;
}

BOOLEAN fat32_writeback(OS_FAT32 *filesystem) {
    if (filesystem == 0 || filesystem->Device == 0) return 0;
    /* 测试和早期直接 I/O 恢复路径可能会暂时绕过缓存。 */
    return !filesystem->Cache.Initialized ||
           block_cache_writeback(&filesystem->Cache);
}

BOOLEAN fat32_sync(OS_FAT32 *filesystem) {
    BOOLEAN success;
    if (filesystem == 0 || !filesystem->Mounted) return 0;
    fat32_mutation_lock(filesystem);
    success = block_cache_flush(&filesystem->Cache);
    fat32_mutation_unlock(filesystem);
    return success;
}

BOOLEAN fat32_init(OS_FAT32 *filesystem,
                          OS_BLOCK_DEVICE *device) {
    UINT8 boot_sector[4096];
    UINT32 total_sectors16;
    UINT32 total_sectors32;
    UINT64 total_sectors;
    UINT32 root_entries;
    UINT32 fat_sectors16;
    UINT32 root_directory_sectors;
    UINT32 fat_type;
    UINT64 data_sectors;
    UINT32 fat1_zero;
    UINT32 fat2_zero;
    UINT32 fat1_one;
    UINT32 fat2_one;
    if (filesystem == 0 || device == 0 || filesystem->Mounted ||
        device->BlockSize < 512U || device->BlockSize > sizeof(boot_sector) ||
        !is_power_of_two(device->BlockSize)) return 0;
    if (!fat32_read_boot_sector(device, boot_sector)) return 0;
    if (boot_sector[510] != 0x55U || boot_sector[511] != 0xAAU) return 0;
    UINT32 bytes_per_sector = fat32_read_u16(boot_sector + 11U);
    UINT32 sectors_per_cluster = boot_sector[13];
    UINT32 reserved = fat32_read_u16(boot_sector + 14U);
    UINT32 fat_count = boot_sector[16];
    root_entries = fat32_read_u16(boot_sector + 17U);
    fat_sectors16 = fat32_read_u16(boot_sector + 22U);
    UINT32 fat_sectors = fat32_read_u32(boot_sector + 36U);
    UINT32 root_cluster = fat32_read_u32(boot_sector + 44U);
    total_sectors16 = fat32_read_u16(boot_sector + 19U);
    total_sectors32 = fat32_read_u32(boot_sector + 32U);
    total_sectors = total_sectors16 != 0U ? total_sectors16 : total_sectors32;
    if (bytes_per_sector == 0U) return 0;
    fat_type = fat_sectors16 != 0U && root_entries != 0U ? FAT_TYPE_16 : FAT_TYPE_32;
    if (fat_type == FAT_TYPE_16) {
        fat_sectors = fat_sectors16;
        root_cluster = 0U;
        root_directory_sectors =
            ((root_entries * FAT32_ENTRY_SIZE) + bytes_per_sector - 1U) /
            bytes_per_sector;
    } else {
        root_directory_sectors = 0U;
    }
    if (bytes_per_sector != device->BlockSize || !is_power_of_two(bytes_per_sector) ||
        sectors_per_cluster == 0U || !is_power_of_two(sectors_per_cluster) || reserved == 0U ||
        fat_count < 2U || fat_sectors == 0U || total_sectors == 0 ||
        ((fat_type == FAT_TYPE_32 && root_cluster < 2U) ||
         (fat_type == FAT_TYPE_16 && root_entries == 0U)) ||
        (UINT64)reserved + (UINT64)fat_count * fat_sectors + root_directory_sectors >=
            total_sectors) return 0;
    data_sectors = total_sectors - reserved - (UINT64)fat_count * fat_sectors -
                   root_directory_sectors;
    if (data_sectors / sectors_per_cluster == 0 || data_sectors / sectors_per_cluster > 0x0FFFFFF0ULL ||
        total_sectors > device->BlockCount) return 0;
    filesystem->Device = device;
    filesystem->BytesPerSector = bytes_per_sector;
    filesystem->SectorsPerCluster = sectors_per_cluster;
    filesystem->ReservedSectorCount = reserved;
    filesystem->FatCount = fat_count;
    filesystem->FatSectors = fat_sectors;
    filesystem->RootCluster = root_cluster;
    filesystem->RootDirectorySectors = root_directory_sectors;
    filesystem->ClusterCount = (UINT32)(data_sectors / sectors_per_cluster);
    filesystem->FreeClusterCount = 0U;
    filesystem->FreeClusterSearchStart = 2U;
    filesystem->FreeClusterCountValid = 0;
    filesystem->FatStartLba = reserved;
    filesystem->RootDirectoryLba = reserved + (UINT64)fat_count * fat_sectors;
    filesystem->DataStartLba = filesystem->RootDirectoryLba + root_directory_sectors;
    filesystem->Fat3StartLba = reserved + (UINT64)2U * fat_sectors;
    filesystem->Fat3Available = fat_type == FAT_TYPE_32 && fat_count >= 3U;
    filesystem->FatType = fat_type;
    if (!block_cache_init(&filesystem->Cache, device)) return 0;
    if ((!fat32_root_directory_cluster(filesystem, root_cluster) &&
         !fat32_cluster_valid(filesystem, root_cluster)) ||
        !fat32_read_fat_entry(filesystem, 1U, 0U + 2U, &fat1_zero) ||
        !fat32_read_fat_entry(filesystem, 2U, 0U + 2U, &fat2_zero) ||
        !fat32_read_fat_entry(filesystem, 1U, 3U, &fat1_one) ||
        !fat32_read_fat_entry(filesystem, 2U, 3U, &fat2_one) ||
        fat1_zero != fat2_zero || fat1_one != fat2_one) {
        block_cache_destroy(&filesystem->Cache);
        return 0;
    }
    filesystem->OpenFileLock = 0U;
    filesystem->MutationLock = 0U;
#ifdef OS_KERNEL_BUILD
    io_mutex_init(&filesystem->MutationMutex, true);
#endif
    for (UINT32 i = 0; i < OS_FAT32_MAX_OPEN_FILES; ++i) {
        filesystem->OpenFiles[i].Used = 0;
    }
    filesystem->Mounted = 1;
    return 1;
}

BOOLEAN fat32_destroy(OS_FAT32 *filesystem) {
    if (filesystem == 0 || !filesystem->Mounted) return 0;
    for (UINT32 i = 0; i < OS_FAT32_MAX_OPEN_FILES; ++i) {
        if (filesystem->OpenFiles[i].Used) return 0;
    }
    if (!block_cache_destroy(&filesystem->Cache)) return 0;
    filesystem->Mounted = 0;
    filesystem->Device = 0;
    return 1;
}

BOOLEAN fat32_open(OS_FAT32 *filesystem, const CHAR8 *path,
                          OS_FAT32_FILE **out) {
    UINT8 entry[32];
    UINT64 entry_lba = 0U;
    UINT32 entry_offset = 0U;

    if (out != 0) *out = 0;
    if (filesystem == 0 || !filesystem->Mounted || path == 0 || out == 0 ||
        !fat32_resolve_path(filesystem, path, entry, &entry_lba, &entry_offset) ||
        (entry[11] & FAT32_DIRECTORY) != 0U) return 0;

    fat32_open_files_lock(filesystem);
    for (UINT32 i = 0; i < OS_FAT32_MAX_OPEN_FILES; ++i) {
        OS_FAT32_FILE *file = &filesystem->OpenFiles[i];
        if (file->Used) continue;

        file->FileSystem = filesystem;
        file->FirstCluster = fat32_entry_cluster(entry);
        file->Size = fat32_read_u32(entry + 28U);
        file->Attributes = entry[11];
        file->DirectoryLba = entry_lba;
        file->DirectoryOffset = entry_offset;
        file->CursorValid = 0;
        file->CursorLogicalStart = 0;
        file->CursorPhysicalStart = 0;
        file->CursorLength = 0;
        file->Used = 1;

        *out = file;
        fat32_open_files_unlock(filesystem);
        return 1;
    }

    fat32_open_files_unlock(filesystem);
    return 0;
}
