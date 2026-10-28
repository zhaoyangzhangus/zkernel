#include <stdio.h>
#include <assert.h>
#include <string.h>
#include <kernel/block_device.h>
#include <kernel/fat32.h>

#define TEST_SECTORS 128U
#define HELLO_DATA_LBA 4U
static UINT8 disk[TEST_SECTORS * 512U];
static UINT32 fail_write_number;
static BOOLEAN partial_write_failure;
static UINT32 write_count;
static UINT32 read_counts[TEST_SECTORS];

static void put16(UINT8 *p, UINT16 value) {
    p[0] = (UINT8)value;
    p[1] = (UINT8)(value >> 8);
}

static void put32(UINT8 *p, UINT32 value) {
    p[0] = (UINT8)value;
    p[1] = (UINT8)(value >> 8);
    p[2] = (UINT8)(value >> 16);
    p[3] = (UINT8)(value >> 24);
}

static UINT8 short_checksum(const UINT8 name[11]) {
    UINT8 checksum = 0U;
    for (UINT32 i = 0; i < 11U; ++i) {
        checksum = (UINT8)(((checksum & 1U) != 0U ? 0x80U : 0U) +
                           (checksum >> 1) + name[i]);
    }
    return checksum;
}

static void put_lfn_entry(UINT8 *entry, const char *name, size_t length,
                          UINT32 start, UINT8 sequence, UINT8 checksum) {
    static const UINT8 offsets[13] = {
        1U, 3U, 5U, 7U, 9U, 14U, 16U, 18U, 20U, 22U, 24U, 28U, 30U
    };
    memset(entry, 0xFF, 32U);
    entry[0] = sequence;
    entry[11] = 0x0FU;
    entry[13] = checksum;
    for (UINT32 i = 0; i < 13U; ++i) {
        UINT32 index = start + i;
        UINT16 value = index < length ? (UINT8)name[index] :
                       (index == length ? 0U : 0xFFFFU);
        put16(entry + offsets[i], value);
    }
}

static void put_lfn(UINT8 *entry, const char *name, const UINT8 short_name[11]) {
    size_t length = strlen(name);
    UINT8 checksum = short_checksum(short_name);
    UINT32 count = (UINT32)((length + 12U) / 13U);
    for (UINT32 ordinal = count; ordinal != 0U; --ordinal) {
        UINT32 start = (ordinal - 1U) * 13U;
        put_lfn_entry(entry, name, length, start,
                      (UINT8)ordinal | (ordinal == count ? 0x40U : 0U), checksum);
        entry += 32U;
    }
}

static BOOLEAN read_disk(VOID *context, UINT64 lba, UINT32 count, VOID *buffer) {
    UINT8 *data = (UINT8 *)context;
    if (data == 0 || buffer == 0 || lba >= TEST_SECTORS || count > TEST_SECTORS - lba) return 0;
    for (UINT32 index = 0U; index < count; ++index) ++read_counts[(UINT32)lba + index];
    memcpy(buffer, data + lba * 512ULL, count * 512U);
    return 1;
}

static BOOLEAN write_disk(VOID *context, UINT64 lba, UINT32 count, const VOID *buffer) {
    UINT8 *data = (UINT8 *)context;
    if (data == 0 || buffer == 0 || lba >= TEST_SECTORS || count > TEST_SECTORS - lba) return 0;
    ++write_count;
    if (fail_write_number != 0U && write_count == fail_write_number) {
        if (partial_write_failure) memcpy(data + lba * 512ULL, buffer, count * 256U);
        return 0;
    }
    memcpy(data + lba * 512ULL, buffer, count * 512U);
    return 1;
}

static void make_disk(void) {
    UINT8 *fat1;
    memset(disk, 0, sizeof(disk));
    fail_write_number = 0U;
    partial_write_failure = 0;
    write_count = 0U;
    memset(read_counts, 0, sizeof(read_counts));
    disk[510] = 0x55U;
    disk[511] = 0xAAU;
    put16(disk + 11U, 512U);
    disk[13] = 1U;
    put16(disk + 14U, 1U);
    disk[16] = 2U;
    put16(disk + 19U, TEST_SECTORS);
    put32(disk + 36U, 1U);
    put32(disk + 44U, 2U);
    fat1 = disk + 512U;
    put32(fat1 + 0U, 0x0FFFFFF8U);
    put32(fat1 + 4U, 0xFFFFFFFFU);
    put32(fat1 + 8U, 0x0FFFFFFFU);
    put32(fat1 + 12U, 0x0FFFFFFFU);
    /* 簇 4 留作扩展 hello.txt 的空闲簇。 */
    put32(fat1 + 16U, 0U);
    put32(fat1 + 20U, 0x0FFFFFFFU);
    /* cluster 6 保存子目录，cluster 7 保存子目录中的文件。 */
    put32(fat1 + 24U, 0x0FFFFFFFU);
    put32(fat1 + 28U, 0x0FFFFFFFU);
    memcpy(fat1 + 512U, fat1, 512U);
    memcpy(disk + 3U * 512U, "HELLO   TXT", 11U);
    disk[3U * 512U + 11U] = 0x20U;
    put16(disk + 3U * 512U + 26U, 3U);
    put32(disk + 3U * 512U + 28U, 5U);
    memcpy(disk + 4U * 512U, "hello", 5U);

    static const UINT8 long_short_name[] = "LONGFI~1TXT";
    put_lfn(disk + 3U * 512U + 32U, "Long File Name.txt", long_short_name);
    memcpy(disk + 3U * 512U + 96U, long_short_name, 11U);
    disk[3U * 512U + 96U + 11U] = 0x20U;
    put16(disk + 3U * 512U + 96U + 26U, 5U);
    put32(disk + 3U * 512U + 96U + 28U, 9U);
    memcpy(disk + 6U * 512U, "long file", 9U);

    UINT8 *root_directory = disk + 3U * 512U + 128U;
    memcpy(root_directory, "DIR        ", 11U);
    root_directory[11] = 0x10U;
    put16(root_directory + 26U, 6U);
    put32(root_directory + 28U, 0U);
    UINT8 *directory = disk + 7U * 512U;
    UINT8 *nested = directory;
    memcpy(nested, "NESTED  TXT", 11U);
    nested[11] = 0x20U;
    put16(nested + 26U, 7U);
    put32(nested + 28U, 6U);
    memcpy(disk + 8U * 512U, "nested", 6U);
}

/* 通过短别名删除时，只有校验和匹配的 LFN 槽位才属于该文件。 */
static void short_alias_lfn_ownership_test(void) {
    for (UINT32 corrupt = 0U; corrupt < 2U; ++corrupt) {
        make_disk();
        UINT8 *lfn = disk + 3U * 512U + 32U;
        if (corrupt) { lfn[13] ^= 1U; lfn[32U + 13U] ^= 1U; }
        UINT8 original[64];memcpy(original, lfn, sizeof(original));
        OS_BLOCK_MANAGER blocks = {0};OS_BLOCK_DEVICE *device = 0;OS_FAT32 fat = {0};
        assert(block_manager_init(&blocks));
        assert(block_register(&blocks, "alias0", 512U, TEST_SECTORS,
                              read_disk, write_disk, 0, disk, &device));
        assert(fat32_init(&fat, device));
        assert(fat32_remove_path(&fat, "LONGFI~1.TXT"));
        assert(fat32_destroy(&fat));
        if (corrupt) assert(!memcmp(lfn, original, sizeof(original)));
        else assert(lfn[0] == 0xE5U && lfn[32U] == 0xE5U);
        assert(disk[3U * 512U + 96U] == 0xE5U);
        assert(!memcmp(disk + 3U * 512U, "HELLO   TXT", 11U));
        assert(block_unregister(&blocks, device));
        assert(block_manager_destroy(&blocks));
    }
    puts("FAT short alias removal: matching LFN deleted, checksum-mismatched LFN preserved on media");
}

static void rename_writeback_failure_test(void) {
    const char *sources[] = {"hello.txt", "hello.txt", "Long File Name.txt", "hello.txt",
                             "hello.txt", "hello.txt"};
    const char *destinations[] = {"DIR/renamed-long-file-name.txt", "new.txt", "short.txt",
                                "DIR/extended-long-file-name.txt", "下载 验证.bin",
                                  "DIR/abcdefghijkl😀跨项.txt"};
    UINT32 failure_points = 0U;
    /* 中文及跨目录项代理对也必须在每个回写失败点完整恢复。 */
    for (UINT32 scenario = 0U; scenario < sizeof(sources) / sizeof(sources[0]); ++scenario) {
    const char *source = sources[scenario];
    const char *destination = destinations[scenario];
    const char *expected = scenario == 2U ? "long " : "hello";
    UINT32 flush_writes = 0U;
    for (UINT32 failure = 0U; failure == 0U || failure <= flush_writes; ++failure) {
        OS_BLOCK_MANAGER blocks = {0};
        OS_BLOCK_DEVICE *device = 0;
        OS_FAT32 fat = {0};
        OS_FAT32_FILE *file = 0;
        os_file_info_t info = {0};
        CHAR8 bytes[5];
        UINT32 size = 0U;
        make_disk();
        if (scenario == 3U) {
            /* Fill all remaining slots to force allocation and linking of
             * another directory cluster before rename publication. */
            for (UINT32 slot = 1U; slot < 16U; ++slot) {
                UINT8 *entry = disk + 7U * 512U + slot * 32U;
                memcpy(entry, "FILL0000TXT", 11U);
                entry[6] = (UINT8)('0' + slot / 10U);
                entry[7] = (UINT8)('0' + slot % 10U);
                entry[11] = 0x20U;
            }
        }
        assert(block_manager_init(&blocks));
        assert(block_register(&blocks, "rename0", 512U, TEST_SECTORS,
                                     read_disk, write_disk, 0, disk, &device));
        assert(fat32_init(&fat, device));
        assert(fat32_open(&fat, source, &file));
        /* An open destination cannot yet be detached. Refuse it without
         * changing either payload or the open source coordinates. */
        if (failure == 0U) {
            assert(fat32_create_path(&fat, "protected.txt", 0));
            assert(fat32_write_path(&fat, "protected.txt", 0U, "keep!", 5U, &size));
            OS_FAT32_FILE *protected_file = 0;
            assert(fat32_open(&fat, "protected.txt", &protected_file));
            UINT64 source_lba = file->DirectoryLba;
            UINT32 source_offset = file->DirectoryOffset;
            assert(!fat32_rename_path(&fat, source, "protected.txt"));
            assert(file->DirectoryLba == source_lba && file->DirectoryOffset == source_offset);
            assert(fat32_read_path(&fat, "protected.txt", 0U, bytes, sizeof(bytes), &size));
            assert(size == 5U && memcmp(bytes, "keep!", 5U) == 0);
            assert(fat32_read_file(file, 0U, bytes, sizeof(bytes), &size));
            assert(size == 5U && memcmp(bytes, expected, 5U) == 0);
            assert(fat32_close(protected_file));
            assert(fat32_remove_path(&fat, "protected.txt"));
        }
        UINT64 old_lba = file->DirectoryLba;
        UINT32 old_offset = file->DirectoryOffset;
        write_count = 0U;
        fail_write_number = failure;
        BOOLEAN renamed = fat32_rename_path(&fat, source, destination);
        if (failure == 0U) {
            assert(renamed);
            flush_writes = write_count;
            assert(flush_writes >= 1U);
        } else {
            assert(!renamed);
            ++failure_points;
            assert(file->DirectoryLba == old_lba && file->DirectoryOffset == old_offset);
        }
        const char *retained = renamed ? destination : source;
        const char *absent = renamed ? source : destination;
        assert(fat32_stat_path(&fat, retained, &info));
        assert(!fat32_stat_path(&fat, absent, &info));
        assert(fat32_read_file(file, 0U, bytes, sizeof(bytes), &size));
        assert(size == 5U && memcmp(bytes, expected, 5U) == 0);
        fail_write_number = 0U;
        assert(fat32_close(file));
        assert(fat32_destroy(&fat));
        if (scenario == 3U && failure != 0U) {
            /* Cluster 4 was free before directory extension. Both FAT
             * copies and the original parent link must be restored. */
            assert(disk[512U + 16U] == 0 && disk[1024U + 16U] == 0);
            /* FAT accepts every value from 0x0FFFFFF8 through 0x0FFFFFFF
             * as end-of-chain; recovery may normalize the original marker. */
            assert(disk[512U + 24U] >= 0xF8 && disk[512U + 25U] == 0xFF &&
                   disk[512U + 26U] == 0xFF && (disk[512U + 27U] & 0x0F) == 0x0F);
            assert(memcmp(disk + 512U + 24U, disk + 1024U + 24U, 4U) == 0);
        }
        /* Reload through a fresh cache to verify recovery reached the media. */
        fat = (OS_FAT32){0};
        assert(fat32_init(&fat, device));
        assert(fat32_stat_path(&fat, retained, &info));
        assert(!fat32_stat_path(&fat, absent, &info));
        assert(fat32_read_path(&fat, retained, 0U, bytes, sizeof(bytes), &size));
        assert(size == 5U && memcmp(bytes, expected, 5U) == 0);
        assert(fat32_destroy(&fat));
        assert(block_unregister(&blocks, device));
        assert(block_manager_destroy(&blocks));
    }
    }
    printf("FAT rename: %u single-write failure points across cross-directory, same-directory, reused LFN slots and full-directory extension restore source/name/handle/FAT links and survive fresh-cache reload\n", failure_points);
}

static void replacement_corrupt_chain_test(void) {
    for (UINT32 scenario = 0U; scenario < 4U; ++scenario) {
        OS_BLOCK_MANAGER blocks = {0}; OS_BLOCK_DEVICE *device = 0;
        OS_FAT32 fat = {0}; OS_FAT32_FILE *file = 0;
        UINT8 before[TEST_SECTORS * 512U];
        make_disk();
        /* hello starts at 3, Long File Name starts at 5. Both already
         * contain valid data. Build shared tails or malformed cycles. */
        for (UINT32 copy = 1U; copy <= 2U; ++copy) {
            UINT8 *table = disk + copy * 512U;
            if (scenario == 0U) put32(table + 3U * 4U, 5U);
            if (scenario == 1U) {
                put32(table + 3U * 4U, 8U); put32(table + 5U * 4U, 8U);
                put32(table + 8U * 4U, 9U); put32(table + 9U * 4U, 0x0FFFFFFFU);
            }
            if (scenario == 2U) {put32(table + 3U * 4U, 8U); put32(table + 8U * 4U, 3U);}
            if (scenario == 3U) put32(table + 5U * 4U, 5U);
        }
        memcpy(before, disk, sizeof(before));
        assert(block_manager_init(&blocks));
        assert(block_register(&blocks, "corrupt0", 512U, TEST_SECTORS, read_disk, write_disk, 0, disk, &device));
        assert(fat32_init(&fat, device)); assert(fat32_open(&fat, "hello.txt", &file));
        UINT64 old_lba = file->DirectoryLba; UINT32 old_offset = file->DirectoryOffset;
        write_count = 0U;
        assert(!fat32_rename_path(&fat, "hello.txt", "Long File Name.txt"));
        assert(!write_count && !memcmp(disk, before, sizeof(before)));
        assert(file->DirectoryLba == old_lba && file->DirectoryOffset == old_offset);
        assert(fat32_close(file));
        if (scenario >= 2U) {
            const char *cyclic = scenario == 2U ? "hello.txt" : "Long File Name.txt";
            assert(!fat32_remove_path(&fat, cyclic));
            os_file_info_t info = {0};
            assert(fat32_stat_path(&fat, cyclic, &info));
            assert(!memcmp(disk, before, sizeof(before)));
        }
        assert(fat32_destroy(&fat));
        assert(block_unregister(&blocks, device)); assert(block_manager_destroy(&blocks));
    }
    puts("FAT replacement corruption: shared tails and source/target cycles rejected without disk writes");
}

static void replacement_writeback_test(void) {
    UINT32 failures = 0U;
    for (UINT32 scenario = 0U; scenario < 3U; ++scenario) {
        const char *source = scenario == 0U ? "hello.txt" : "Long File Name.txt";
        const char *target = scenario == 2U ? "DIR/Existing Long Target.txt" : "Existing Long Target.txt";
        const char *expected = scenario == 0U ? "hello" : "long ";
        UINT32 writes = 0U;
        for (UINT32 failure = 0U; failure == 0U || failure <= writes; ++failure) {
            OS_BLOCK_MANAGER blocks = {0}; OS_BLOCK_DEVICE *device = 0;
            OS_FAT32 fat = {0}; OS_FAT32_FILE *file = 0;
            CHAR8 bytes[5]; UINT32 size = 0U; os_file_info_t info = {0};
            make_disk();
            assert(block_manager_init(&blocks));
            assert(block_register(&blocks, "replace0", 512U, TEST_SECTORS, read_disk, write_disk, 0, disk, &device));
            assert(fat32_init(&fat, device));
            assert(fat32_create_path(&fat, target, 0));
            assert(fat32_write_path(&fat, target, 0U, "keep!", 5U, &size));
            OS_FAT32_FILE *old_target = 0;
            assert(fat32_open(&fat, target, &old_target));
            UINT32 retired_cluster = old_target->FirstCluster;
            assert(retired_cluster >= 2U && fat.FatCount == 2U);
            assert(fat32_close(old_target));
            assert(fat32_sync(&fat));
            assert(fat32_open(&fat, source, &file));
            UINT64 old_lba = file->DirectoryLba; UINT32 old_offset = file->DirectoryOffset;
            write_count = 0U; fail_write_number = failure;
            BOOLEAN replaced = fat32_rename_path(&fat, source, target);
            fail_write_number = 0U;
            if (failure == 0U) { assert(replaced); writes = write_count; assert(writes); }
            else { assert(!replaced); ++failures; assert(file->DirectoryLba == old_lba && file->DirectoryOffset == old_offset); }
            for (UINT32 copy = 0U; copy < fat.FatCount; ++copy) {
                /* This fixture has one FAT sector per copy. */
                UINT8 *entry = disk + (fat.FatStartLba + copy) * 512U + retired_cluster * 4U;
                UINT32 value = (UINT32)entry[0] | ((UINT32)entry[1] << 8U) |
                    ((UINT32)entry[2] << 16U) | ((UINT32)(entry[3] & 15U) << 24U);
                if (!(replaced ? value == 0U : value >= 0x0FFFFFF8U))
                    fprintf(stderr, "replacement FAT mismatch scenario=%u failure=%u copy=%u cluster=%u value=%x replaced=%u\n",
                        scenario, failure, copy, retired_cluster, value, replaced);
                assert(replaced ? value == 0U : value >= 0x0FFFFFF8U);
            }
            assert(fat32_read_path(&fat, target, 0U, bytes, 5U, &size));
            assert(size == 5U && memcmp(bytes, replaced ? expected : "keep!", 5U) == 0);
            assert(fat32_stat_path(&fat, source, &info) == !replaced);
            assert(fat32_read_file(file, 0U, bytes, 5U, &size));
            assert(size == 5U && memcmp(bytes, expected, 5U) == 0);
            assert(fat32_close(file)); assert(fat32_destroy(&fat));
            fat = (OS_FAT32){0}; assert(fat32_init(&fat, device));
            assert(fat32_read_path(&fat, target, 0U, bytes, 5U, &size));
            assert(size == 5U && memcmp(bytes, replaced ? expected : "keep!", 5U) == 0);
            assert(fat32_stat_path(&fat, source, &info) == !replaced);
            assert(fat32_destroy(&fat));assert(block_unregister(&blocks, device));assert(block_manager_destroy(&blocks));
        }
    }
    printf("FAT replacement: %u write failure points, short/long source names, same/cross directory, source handle and fresh-cache recovery passed\n", failures);
}

/* 截断回写在每个单次写失败点后都能重试，重挂载后的目录和两个 FAT 副本一致。 */
static void truncate_writeback_failure_test(void) {
    UINT32 failures = 0U;
    for (UINT32 partial = 0U; partial < 2U; ++partial)
    for (UINT32 scenario = 0U; scenario < 3U; ++scenario) {
        UINT32 writes = 0U;
        for (UINT32 failure = 0U; failure == 0U || failure <= writes; ++failure) {
            make_disk();
            const UINT32 chain[] = {3U, 4U, 8U, 9U};
            UINT8 expected[1900], actual[1900];
            for (UINT32 i = 0U; i < sizeof(expected); ++i) expected[i] = (UINT8)(i * 23U + 7U);
            for (UINT32 copy = 0U; copy < 2U; ++copy) {
                UINT8 *fat = disk + (1U + copy) * 512U;
                for (UINT32 i = 0U; i < 4U; ++i)
                    put32(fat + chain[i] * 4U, i == 3U ? 0x0FFFFFFFU : chain[i + 1U]);
            }
            put32(disk + 3U * 512U + 28U, sizeof(expected));
            for (UINT32 i = 0U; i < sizeof(expected); ++i)
                disk[(chain[i / 512U] + 1U) * 512U + i % 512U] = expected[i];
            OS_BLOCK_MANAGER blocks = {0}; OS_BLOCK_DEVICE *device = 0;
            OS_FAT32 fs = {0}; OS_FAT32_FILE *file = 0;
            assert(block_manager_init(&blocks));
            assert(block_register(&blocks, "truncate0", 512U, TEST_SECTORS,
                                  read_disk, write_disk, 0, disk, &device));
            assert(fat32_init(&fs, device));
            assert(fat32_open(&fs, "hello.txt", &file));
            UINT64 target = scenario == 1U ? 0U : scenario == 2U ? 1800U : 600U;
            write_count = 0U; fail_write_number = failure;
            partial_write_failure = partial;
            BOOLEAN ok = fat32_truncate_file(file, target);
            fail_write_number = 0U;
            if (failure == 0U) { assert(ok); writes = write_count; assert(writes); }
            else {
                assert(!ok && file->Size == sizeof(expected) && file->FirstCluster == chain[0]);
                ++failures;
            }
            UINT32 bytes = 0U;
            assert(fat32_read_file(file, 0U, actual, sizeof(actual), &bytes));
            assert(bytes == (ok ? target : sizeof(expected)));
            assert(memcmp(actual, expected, bytes) == 0);
            assert(fat32_close(file)); assert(fat32_destroy(&fs));
            for (UINT32 copy = 0U; copy < 2U; ++copy) {
                const UINT8 *fat = disk + (1U + copy) * 512U;
                for (UINT32 i = 0U; i < 4U; ++i) {
                    const UINT8 *p = fat + chain[i] * 4U;
                    UINT32 value = (UINT32)p[0] | (UINT32)p[1] << 8U |
                                   (UINT32)p[2] << 16U | (UINT32)p[3] << 24U;
                    value &= 0x0FFFFFFFU;
                    UINT32 before = i == 3U ? 0x0FFFFFFFU : chain[i + 1U];
                    if (!ok || scenario == 2U) assert(value == before);
                    else if (scenario == 1U || i >= 2U) assert(value == 0U);
                    else if (i == 1U) assert(value >= 0x0FFFFFF8U);
                    else assert(value == chain[i + 1U]);
                }
            }
            memset(&fs, 0, sizeof(fs)); file = 0;
            assert(fat32_init(&fs, device)); assert(fat32_open(&fs, "hello.txt", &file));
            assert(file->Size == (ok ? target : sizeof(expected)));
            assert(fat32_read_file(file, 0U, actual, sizeof(actual), &bytes));
            assert(bytes == (ok ? target : sizeof(expected)) && !memcmp(actual, expected, bytes));
            if (!ok) {
                assert(fat32_truncate_file(file, target));
                assert(file->Size == target);
            }
            assert(fat32_close(file)); assert(fat32_destroy(&fs));
            assert(block_unregister(&blocks, device)); assert(block_manager_destroy(&blocks));
        }
    }
    printf("FAT truncate: %u failed/partial writes preserve data, handle and remount state; retry succeeds\n", failures);
}

/* 跳过预先计数后，循环、无效链尾和不足以覆盖新长度的短链仍须在任何写盘前拒绝。 */
static void truncate_corruption_test(void) {
    for (UINT32 scenario = 0U; scenario < 3U; ++scenario) {
        make_disk();
        const UINT32 chain[] = {3U, 4U, 8U, 9U};
        for (UINT32 copy = 0U; copy < 2U; ++copy) {
            UINT8 *fat = disk + (1U + copy) * 512U;
            for (UINT32 i = 0U; i < 4U; ++i)
                put32(fat + chain[i] * 4U, i == 3U ? 0x0FFFFFFFU : chain[i + 1U]);
            if (scenario == 0U) put32(fat + 9U * 4U, 3U);
            if (scenario == 1U) put32(fat + 9U * 4U, 0x0FFFFFF7U);
            if (scenario == 2U) put32(fat + 4U * 4U, 0x0FFFFFFFU);
        }
        put32(disk + 3U * 512U + 28U, 1900U);
        OS_BLOCK_MANAGER blocks = {0}; OS_BLOCK_DEVICE *device = 0;
        OS_FAT32 fs = {0}; OS_FAT32_FILE *file = 0;
        assert(block_manager_init(&blocks));
        assert(block_register(&blocks, "truncate-bad", 512U, TEST_SECTORS,
                              read_disk, write_disk, 0, disk, &device));
        assert(fat32_init(&fs, device)); assert(fat32_open(&fs, "hello.txt", &file));
        OS_FAT32_FILE before = *file;
        write_count = 0U;
        assert(!fat32_truncate_file(file, scenario == 2U ? 1500U : 600U));
        assert(!write_count && !memcmp(file, &before, sizeof(before)));
        assert(fat32_close(file)); assert(fat32_destroy(&fs));
        assert(!write_count);
        assert(block_unregister(&blocks, device)); assert(block_manager_destroy(&blocks));
    }
    puts("FAT truncate: cyclic, invalid-tail and premature-EOC chains rejected without writes or handle changes");
}

/* 扩展或越过文件末尾写入时，不得暴露旧扇区或已释放簇内的内容。 */
static void file_growth_zero_test(void) {
    for (UINT32 scenario=0U;scenario<4U;++scenario) {
        make_disk();
        memset(disk + 4U*512U + 5U, 0xa5, 512U-5U);
        memset(disk + 5U*512U, 0xb6, 512U);
        OS_BLOCK_MANAGER blocks={0};OS_BLOCK_DEVICE *device=0;
        OS_FAT32 fs={0};OS_FAT32_FILE *file=0;
        assert(block_manager_init(&blocks));
        assert(block_register(&blocks,"growth0",512U,TEST_SECTORS,read_disk,write_disk,0,disk,&device));
        assert(fat32_init(&fs,device));assert(fat32_open(&fs,"hello.txt",&file));
        UINT32 bytes=999U;
        assert(fat32_write_file(file,2000U,0,0U,&bytes) && bytes==0U && file->Size==5U);
        if(scenario==3U) assert(fat32_truncate_file(file,2U));
        UINT32 prefix=scenario==3U?2U:5U;
        UINT64 size=(scenario==0U||scenario==3U)?100U:700U;
        if(scenario==2U) {
            UINT8 marker=0x42;
            assert(fat32_write_file(file,699U,&marker,1U,&bytes)&&bytes==1U);
        } else assert(fat32_truncate_file(file,size));
        UINT8 out[700];memset(out,0xcc,sizeof(out));
        assert(fat32_read_file(file,0U,out,sizeof(out),&bytes) && bytes==size);
        assert(!memcmp(out,"hello",prefix));
        for(UINT32 i=prefix;i<size;++i) assert(out[i]==(scenario==2U&&i==699U?0x42U:0U));
        assert(fat32_close(file));assert(fat32_destroy(&fs));
        memset(&fs,0,sizeof(fs));file=0;
        assert(fat32_init(&fs,device));assert(fat32_open(&fs,"hello.txt",&file));
        assert(fat32_read_file(file,0U,out,sizeof(out),&bytes) && bytes==size);
        assert(!memcmp(out,"hello",prefix));
        for(UINT32 i=prefix;i<size;++i) assert(out[i]==(scenario==2U&&i==699U?0x42U:0U));
        assert(fat32_close(file));assert(fat32_destroy(&fs));
        assert(block_unregister(&blocks,device));assert(block_manager_destroy(&blocks));
    }
    puts("FAT growth: same/new clusters, seek writes, regrowth and empty writes survive remount");
}

int main(void) {
    OS_BLOCK_MANAGER blocks = {0};
    OS_BLOCK_DEVICE *device = 0;
    OS_FAT32 fat = {0};
    OS_FAT32_FILE *rollback_file = 0;
    CHAR8 buffer[6] = {0};
    CHAR8 nested_buffer[7] = {0};
    UINT8 extension[600];
    UINT8 full_sector[512];
    UINT32 hello_data_reads;
    UINT32 size = 0;
    for (UINT32 i = 0; i < sizeof(extension); ++i) extension[i] = (UINT8)(i ^ 0x5AU);
    file_growth_zero_test();
    truncate_writeback_failure_test();
    truncate_corruption_test();
    short_alias_lfn_ownership_test();
    rename_writeback_failure_test();
    replacement_writeback_test();
    replacement_corrupt_chain_test();
    make_disk();
    if (!block_manager_init(&blocks) ||
        !block_register(&blocks, "mem0", 512U, TEST_SECTORS,
                               read_disk, write_disk, 0, disk, &device)) return 1;
    if (!fat32_init(&fat, device)) return 2;
    if (!fat32_read_path(&fat, "hello.txt", 0U, buffer, 5U, &size) ||
        size != 5U || strcmp(buffer, "hello") != 0) return 3;
    if (!fat32_read_path(&fat, "Long File Name.txt", 0U,
                                buffer, 5U, &size) || size != 5U ||
        memcmp(buffer, "long ", 5U) != 0 ||
        !fat32_read_path(&fat, "DIR/NESTED.TXT", 0U,
                                nested_buffer, 6U, &size) || size != 6U ||
        memcmp(nested_buffer, "nested", 6U) != 0 ||
        !fat32_write_path(&fat, "hello.txt", 0U, "world", 5U, &size) ||
        size != 5U) return 4;
    memset(full_sector, 0xA5, sizeof(full_sector));
    memcpy(full_sector, "world", 5U);
    if (!block_cache_invalidate(&fat.Cache, HELLO_DATA_LBA)) return 5;
    hello_data_reads = read_counts[HELLO_DATA_LBA];
    if (!fat32_write_path(&fat, "hello.txt", 0U, full_sector,
                                 sizeof(full_sector), &size) ||
        size != sizeof(full_sector) || read_counts[HELLO_DATA_LBA] != hello_data_reads) return 5;
    if (!block_cache_invalidate(&fat.Cache, HELLO_DATA_LBA)) return 5;
    hello_data_reads = read_counts[HELLO_DATA_LBA];
    if (!fat32_write_path(&fat, "hello.txt", 510U, "xy", 2U, &size) ||
        size != 2U ||
        read_counts[HELLO_DATA_LBA] != hello_data_reads + 1U ||
        !block_cache_read(&fat.Cache, HELLO_DATA_LBA, full_sector) ||
        memcmp(full_sector, "world", 5U) != 0 || full_sector[509U] != 0xA5U ||
        full_sector[510U] != 'x' || full_sector[511U] != 'y') return 5;
    if (!fat32_write_path(&fat, "hello.txt", 5U, extension,
                                 sizeof(extension), &size) ||
        size != sizeof(extension) ||
        !fat32_sync(&fat) ||
        memcmp(disk + 5U * 512U, extension + 507U, 93U) != 0 ||
        disk[3U * 512U + 28U] != (UINT8)(5U + sizeof(extension)) ||
        disk[512U + 16U] != 0xF8U || disk[1024U + 16U] != 0xF8U ||
        !fat32_create_path(&fat, "new.bin", 0) ||
        fat32_create_path(&fat, "new.bin", 0) ||
        !fat32_create_path(&fat, "Long Created File.txt", 0) ||
        !fat32_write_path(&fat, "Long Created File.txt", 0U,
                                 "lfn", 3U, &size) || size != 3U ||
        !fat32_sync(&fat)) return 5;
    /* 注入第二次目录写失败，验证短名已删除时 LFN 失败会恢复原目录项。 */
    fat.Cache.Initialized = 0U;
    if (!fat32_open(&fat, "Long Created File.txt", &rollback_file) ||
        !fat32_close(rollback_file)) return 5;
    fail_write_number = 2U;
    write_count = 0U;
    BOOLEAN removed_after_failure = fat32_remove_path(&fat, "Long Created File.txt");
    BOOLEAN found_after_failure = fat32_open(&fat, "Long Created File.txt", &rollback_file);
    if (removed_after_failure || !found_after_failure) return 5;
    if (!fat32_close(rollback_file)) return 5;
    fail_write_number = 0U;
    fat.Cache.Initialized = 1U;
    if (!fat32_remove_path(&fat, "Long Created File.txt") ||
        !fat32_create_path(&fat, "ninechars.txt", 0) ||
        !fat32_write_path(&fat, "ninechars.txt", 0U,
                                 "chain", 5U, &size) || size != 5U ||
        !fat32_sync(&fat)) return 5;
    /* 目录短名和 LFN 各写一次，随后在 FAT 副本写入阶段注入失败。 */
    fat.Cache.Initialized = 0U;
    fail_write_number = 4U;
    write_count = 0U;
    BOOLEAN removed_during_fat_failure =
        fat32_remove_path(&fat, "ninechars.txt");
    BOOLEAN found_during_fat_failure =
        fat32_open(&fat, "ninechars.txt", &rollback_file);
    if (removed_during_fat_failure || !found_during_fat_failure) return 5;
    if (!fat32_close(rollback_file)) return 5;
    fail_write_number = 0U;
    fat.Cache.Initialized = 1U;
    if (!fat32_remove_path(&fat, "ninechars.txt") ||
        !fat32_create_path(&fat, "Long Folder Name", 1) ||
        !fat32_create_path(&fat, "Long Folder Name/Long Child Name.txt", 0) ||
        !fat32_remove_path(&fat, "Long Folder Name/Long Child Name.txt") ||
        !fat32_remove_path(&fat, "Long Folder Name") ||
        !fat32_create_path(&fat, "folder", 1) ||
        !fat32_create_path(&fat, "folder/child.bin", 0) ||
        fat32_remove_path(&fat, "folder") ||
        !fat32_remove_path(&fat, "folder/child.bin") ||
        !fat32_remove_path(&fat, "folder") ||
        !fat32_remove_path(&fat, "new.bin")) return 5;
    /* Cached-handle read/write/truncate coherence, as used by VFS. */
    OS_FAT32_FILE *coherent = 0;
    char current[32] = {0};
    const char manifest[] = "MANIFEST-000001\n";
    if (!fat32_create_path(&fat, "current", 0) ||
        !fat32_open(&fat, "current", &coherent) ||
        !fat32_write_file(coherent, 0, manifest,
                                  sizeof(manifest) - 1U, &size) ||
        coherent->Size != sizeof(manifest) - 1U ||
        !fat32_read_file(coherent, 0, current, sizeof(current), &size) ||
        size != sizeof(manifest) - 1U || memcmp(current, manifest, size) ||
        !fat32_truncate_file(coherent, 0) || coherent->Size != 0 ||
        !fat32_read_file(coherent, 0, current, sizeof(current), &size) ||
        size != 0 ||
        !fat32_write_file(coherent, 0, "new\n", 4, &size) ||
        !fat32_read_file(coherent, 0, current, sizeof(current), &size) ||
        size != 4 || memcmp(current, "new\n", 4) ||
        !fat32_close(coherent)) return 7;
    if (!fat32_destroy(&fat) ||
        memcmp(disk + 4U * 512U, "world", 5U) != 0 ||
        !block_unregister(&blocks, device) || !block_manager_destroy(&blocks)) return 6;
    puts("fat32: ok");
    return 0;
}
