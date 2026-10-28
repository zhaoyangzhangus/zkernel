#pragma once

#include <kernel/fat32.h>

void fat32_diag_failure(const char *tag, UINT64 first, UINT64 second,
                        UINT64 third);

BOOLEAN fat32_cluster_valid(const OS_FAT32 *filesystem, UINT32 cluster);
BOOLEAN fat32_root_directory_cluster(const OS_FAT32 *filesystem,
                                      UINT32 cluster);
BOOLEAN fat32_read_sector(const OS_FAT32 *filesystem, UINT64 lba,
                          UINT8 *buffer);
BOOLEAN fat32_read_boot_sector(const OS_BLOCK_DEVICE *device, UINT8 *buffer);
BOOLEAN fat32_write_sector(OS_FAT32 *filesystem, UINT64 lba,
                           const UINT8 *buffer);
UINT64 fat32_cluster_lba(const OS_FAT32 *filesystem, UINT32 cluster);
BOOLEAN fat32_read_fat_entry(const OS_FAT32 *filesystem, UINT32 fat_number,
                             UINT32 cluster, UINT32 *value);
BOOLEAN fat32_write_fat_entry(OS_FAT32 *filesystem, UINT32 fat_number,
                              UINT32 cluster, UINT32 value);
BOOLEAN fat32_write_file_fat_value(OS_FAT32 *filesystem, UINT32 cluster,
                                   UINT32 chain_value, UINT32 extent_value);
BOOLEAN fat32_find_free_cluster(OS_FAT32 *filesystem, UINT32 *cluster);
/* 调用方持有变更锁；扫描到 first 之前不回绕。 */
BOOLEAN fat32_find_free_cluster_from(OS_FAT32 *filesystem, UINT32 first,
                                      UINT32 *cluster);
BOOLEAN fat32_read_next_cluster(const OS_FAT32 *filesystem, UINT32 cluster,
                                UINT32 *next);
/* 返回不超过 maximum 的连续前缀；探测失败保留已验证部分，零上限不执行 I/O。 */
UINT32 fat32_extent_length(const OS_FAT32 *filesystem, UINT32 cluster,
                           UINT32 maximum);
BOOLEAN fat32_free_cluster_chain(OS_FAT32 *filesystem, UINT32 first);
/* 调用方持有变更锁；new_last 为零表示释放整条链。 */
BOOLEAN fat32_truncate_clusters(OS_FAT32_FILE *file, UINT32 new_last,
                                UINT64 size);
BOOLEAN fat32_replace_file_entry(OS_FAT32 *filesystem,
    UINT64 source_lba, UINT32 source_offset,
    const UINT64 lfn_lba[20], const UINT32 lfn_offset[20], UINT32 lfn_count,
    UINT64 target_lba, UINT32 target_offset);
void fat32_rollback_directory_extensions(OS_FAT32 *filesystem,
                                          UINT32 anchor,
                                          const UINT32 clusters[21],
                                          UINT32 count);

void fat32_open_files_lock(OS_FAT32 *filesystem);
void fat32_open_files_unlock(OS_FAT32 *filesystem);
void fat32_rebind_open_file(OS_FAT32 *filesystem, UINT64 old_lba,
                            UINT32 old_offset, UINT64 new_lba,
                            UINT32 new_offset);
void fat32_mutation_lock(OS_FAT32 *filesystem);
void fat32_mutation_unlock(OS_FAT32 *filesystem);
BOOLEAN fat32_writeback(OS_FAT32 *filesystem);

BOOLEAN fat32_allocate_long_name_alias(OS_FAT32 *filesystem, UINT32 directory,
                                        const CHAR8 *name, UINT32 length,
                                        UINT8 result[11]);
UINT32 fat32_entry_cluster(const UINT8 entry[32]);
UINT8 fat32_short_name_checksum(const UINT8 short_name[11]);
BOOLEAN fat32_find_directory_entry_ex(OS_FAT32 *filesystem,
                                      UINT32 directory_cluster,
                                      const CHAR8 *name, UINT32 name_length,
                                      UINT8 result[32], UINT64 *result_lba,
                                      UINT32 *result_offset,
                                      UINT64 lfn_lba[20], UINT32 lfn_offset[20],
                                      UINT32 *lfn_count);
BOOLEAN fat32_find_directory_entry(OS_FAT32 *filesystem,
                                   UINT32 directory_cluster,
                                   const CHAR8 *name, UINT32 name_length,
                                   UINT8 result[32], UINT64 *result_lba,
                                   UINT32 *result_offset);
BOOLEAN fat32_resolve_path(OS_FAT32 *filesystem, const CHAR8 *path,
                           UINT8 result[32], UINT64 *result_lba,
                           UINT32 *result_offset);
BOOLEAN fat32_resolve_directory_cluster(OS_FAT32 *filesystem,
                                         const CHAR8 *path,
                                         UINT32 *cluster);
BOOLEAN fat32_resolve_parent_directory(OS_FAT32 *filesystem,
                                        const CHAR8 *path,
                                        UINT32 *directory,
                                        CHAR8 leaf[OS_FAT32_PATH_LENGTH]);
BOOLEAN fat32_find_directory_slots(OS_FAT32 *filesystem, UINT32 directory,
                                   UINT32 slot_count, UINT64 lba[21],
                                   UINT32 offset[21], UINT32 new_clusters[21],
                                   UINT32 *new_cluster_count,
                                   UINT32 *extension_anchor);
BOOLEAN fat32_write_directory_slot(OS_FAT32 *filesystem, UINT64 lba,
                                    UINT32 offset,
                                    const UINT8 entry[32]);
BOOLEAN fat32_delete_directory_slot(OS_FAT32 *filesystem, UINT64 lba,
                                     UINT32 offset);
BOOLEAN fat32_read_directory_slot(OS_FAT32 *filesystem, UINT64 lba,
                                   UINT32 offset, UINT8 entry[32]);
BOOLEAN fat32_allocate_zero_cluster(OS_FAT32 *filesystem, UINT32 *cluster);
BOOLEAN fat32_initialize_directory_cluster(OS_FAT32 *filesystem,
                                            UINT32 cluster, UINT32 parent);
BOOLEAN fat32_directory_is_empty(OS_FAT32 *filesystem, UINT32 directory);
BOOLEAN fat32_file_is_open_at(OS_FAT32 *filesystem, UINT64 lba,
                              UINT32 offset);

UINT16 fat32_read_u16(const UINT8 *data);
UINT32 fat32_read_u32(const UINT8 *data);
void fat32_write_u16(UINT8 *data, UINT16 value);
void fat32_write_u32(UINT8 *data, UINT32 value);

BOOLEAN fat32_make_short_name(const CHAR8 *text, UINT32 length,
                              UINT8 result[11]);
BOOLEAN fat32_short_name_equal(const UINT8 left[11], const UINT8 right[11]);
BOOLEAN fat32_make_long_name_alias(const CHAR8 *name, UINT32 length,
                                   UINT32 suffix, UINT8 result[11]);

void fat32_lfn_reset(UINT16 characters[260], BOOLEAN *valid,
                     UINT32 *expected, UINT8 *checksum);
/* 无效字符不会被消费；新的 LFN 序列头会在发布有效状态前执行完整重置。 */
void fat32_lfn_invalidate(BOOLEAN *valid, UINT32 *expected, UINT8 *checksum);
void fat32_lfn_store_entry(const UINT8 entry[32], UINT16 characters[260],
                           BOOLEAN *valid, UINT32 *expected,
                           UINT8 *checksum);
/* 创建和重命名共用准备结果；目录项写入阶段不再重复解析 UTF-8。 */
typedef struct {
    UINT8 short_name[11];
    UINT8 checksum;
    UINT32 lfn_count;
    UINT32 units;
    UINT16 characters[260];
} fat32_creation_name_t;
UINT32 fat32_encode_long_name(const CHAR8 *name, UINT32 length,
                               UINT16 output[260]);
BOOLEAN fat32_lfn_units_equal(const UINT16 characters[260],
                              const UINT16 encoded[260], UINT32 count);
UINT32 fat32_short_name_text(const UINT8 entry[32],
                             CHAR8 output[OS_FILE_NAME_MAX]);
UINT32 fat32_lfn_name_text(const UINT16 characters[260],
                           CHAR8 output[OS_FILE_NAME_MAX]);
BOOLEAN fat32_dot_entry(const UINT8 entry[32]);
void fat32_make_lfn_entry(const UINT16 name[260], UINT32 units,
                          UINT32 ordinal, UINT8 checksum,
                          UINT8 entry[32]);
