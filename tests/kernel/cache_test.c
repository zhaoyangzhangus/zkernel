#include <stdio.h>
#include <string.h>
#include <kernel/block_device.h>
#include <kernel/block_cache.h>

#define TEST_BLOCKS 32U
static UINT8 disk[TEST_BLOCKS * 512U];
static UINT32 read_count;

static BOOLEAN read_disk(VOID *context, UINT64 lba, UINT32 count, VOID *buffer) {
    if (context == 0 || buffer == 0 || lba >= TEST_BLOCKS || count > TEST_BLOCKS - lba) return 0;
    ++read_count;
    memcpy(buffer, (UINT8 *)context + lba * 512ULL, count * 512U);
    return 1;
}

static BOOLEAN write_disk(VOID *context, UINT64 lba, UINT32 count, const VOID *buffer) {
    if (context == 0 || buffer == 0 || lba >= TEST_BLOCKS || count > TEST_BLOCKS - lba) return 0;
    memcpy((UINT8 *)context + lba * 512ULL, buffer, count * 512U);
    return 1;
}

int main(void) {
    OS_BLOCK_MANAGER blocks = {0};
    OS_BLOCK_DEVICE *device = 0;
    OS_BLOCK_CACHE cache = {0};
    UINT8 buffer[512] = {0};
    if (!block_manager_init(&blocks) ||
        !block_register(&blocks, "mem0", 512U, TEST_BLOCKS,
                               read_disk, write_disk, 0, disk, &device) ||
        !block_cache_init(&cache, device) ||
        !block_cache_read(&cache, 0, buffer)) return 1;
    memcpy(buffer, "cached", 6U);
    if (!block_cache_write(&cache, 0, buffer)) return 2;
    /* A complete-block write miss must not issue a read before becoming dirty. */
    if (!block_cache_write(&cache, TEST_BLOCKS - 1U, buffer) ||
        read_count != 1U) return 5;
    for (UINT64 lba = 1; lba <= OS_BLOCK_CACHE_ENTRY_COUNT; ++lba) {
        if (!block_cache_read(&cache, lba, buffer)) return 3;
    }
    /* 热缓存项未必在读取过程中被淘汰；先显式写回，再检查介质内容。 */
    if (!block_cache_writeback(&cache) || memcmp(disk, "cached", 6U) != 0 ||
        !block_cache_invalidate(&cache, 1U) ||
        !block_cache_destroy(&cache) ||
        !block_unregister(&blocks, device) ||
        !block_manager_destroy(&blocks)) return 4;
    puts("cache: ok");
    return 0;
}
