/*
 * pmm_test.c —— 不启动 QEMU，验证早期物理内存分配器。
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../mm/pmm.h"

/* OVMF 常见 DescriptorSize=48；结构本体只有 40 字节，顺便验证步进逻辑。 */
#define TEST_DESC_SIZE 48u
#define TEST_DESC_COUNT 3u

static uint8_t map_bytes[TEST_DESC_SIZE * TEST_DESC_COUNT];

static BOOT_MEMORY_DESCRIPTOR *desc(unsigned index)
{
    return (BOOT_MEMORY_DESCRIPTOR *)(void *)
        (map_bytes + (uint64_t)index * TEST_DESC_SIZE);
}

static int checks;
static int failures;

static void check(int condition, const char *what)
{
    checks++;
    if (!condition) {
        failures++;
        fprintf(stderr, "FAIL: %s\n", what);
    }
}

int main(void)
{
    BOOT_INFO bi = { 0 };

    memset(map_bytes, 0, sizeof(map_bytes));

    /* LoaderData 必须完全跳过。 */
    desc(0)->type = MEM_LOADER_DATA;
    desc(0)->physical_start = 0x00100000;
    desc(0)->number_of_pages = 100;

    /* 两段真正可供 PMM 使用的 ConventionalMemory。 */
    desc(1)->type = MEM_CONVENTIONAL;
    desc(1)->physical_start = 0x00200000;
    desc(1)->number_of_pages = 5;

    desc(2)->type = MEM_CONVENTIONAL;
    desc(2)->physical_start = 0x00400000;
    desc(2)->number_of_pages = 10;

    bi.mmap_addr = (uint64_t)(uintptr_t)map_bytes;
    bi.mmap_size = sizeof(map_bytes);
    bi.mmap_desc_size = TEST_DESC_SIZE;
    bi.mmap_desc_count = TEST_DESC_COUNT;

    check(pmm_init(&bi) == 0, "pmm_init");
    check(pmm_alloc(0) == 0, "size 0 must fail");
    check(pmm_alloc(UINT64_MAX) == 0, "size alignment overflow must fail");

    /* 1 字节也必须占整整一页。 */
    check(pmm_alloc(1) == 0x00200000, "1 byte rounds to one page");
    check(desc(1)->physical_start == 0x00201000,
          "first range advances by one page");
    check(desc(1)->number_of_pages == 4,
          "first range keeps 4 pages");

    /* 8193 字节向上对齐为 12288 字节，也就是 3 页。 */
    check(pmm_alloc(8193) == 0x00201000,
          "8193 bytes round to 3 pages");
    check(desc(1)->physical_start == 0x00204000,
          "first range advances by 3 more pages");
    check(desc(1)->number_of_pages == 1,
          "first range keeps 1 page");

    /* 4097 字节需要 2 页，第一段剩 1 页放不下，必须去下一段。 */
    check(pmm_alloc(4097) == 0x00400000,
          "4097 bytes use 2 pages from next fitting range");

    /* 扫描从头开始，因此第一段最后 1 页仍然可以利用。 */
    check(pmm_alloc(4096) == 0x00204000,
          "exact page consumes first range remainder");

    /* 第二段此前用了 2 页，还剩 8 页。 */
    check(pmm_alloc(8 * PMM_PAGE_SIZE) == 0x00402000,
          "exactly consume second range remainder");
    check(pmm_alloc(1) == 0, "allocation fails after exhaustion");

    check(desc(0)->physical_start == 0x00100000 &&
          desc(0)->number_of_pages == 100,
          "LoaderData is never consumed");

    if (failures == 0) {
        printf("pmm 测试通过：%d 项\n", checks);
        return 0;
    }

    printf("pmm 测试失败：%d/%d 项不通过\n", failures, checks);
    return 1;
}
