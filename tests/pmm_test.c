/*
 * pmm_test.c —— 不启动 QEMU，验证早期物理页分配器。
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
    check(pmm_alloc_pages(0) == 0, "0 pages must fail");

    check(pmm_alloc_pages(3) == 0x00200000, "allocate 3 pages");
    check(desc(1)->physical_start == 0x00203000,
          "first range advances by 3 pages");
    check(desc(1)->number_of_pages == 2,
          "first range keeps 2 pages");

    /* 2 页那段放不下 4 页，必须跳到下一段。 */
    check(pmm_alloc_pages(4) == 0x00400000,
          "allocate 4 pages from next fitting range");

    /* 扫描从头开始，因此剩下的 2 页仍然可以被利用。 */
    check(pmm_alloc_pages(2) == 0x00203000,
          "allocate exact remainder of first range");

    check(pmm_alloc_pages(6) == 0x00404000,
          "allocate exact remainder of second range");
    check(pmm_alloc_page() == 0, "allocation fails after exhaustion");

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
