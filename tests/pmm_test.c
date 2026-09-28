/*
 * pmm_test.c —— 不启动 QEMU，验证早期物理内存分配器。
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../mm/pmm.h"

#define TEST_DESC_SIZE 48u
#define TEST_DESC_COUNT 4u

static uint8_t map_bytes[TEST_DESC_SIZE * TEST_DESC_COUNT];
static int checks;
static int failures;

static BOOT_MEMORY_DESCRIPTOR *desc(unsigned index)
{
    return (BOOT_MEMORY_DESCRIPTOR *)(void *)
        (map_bytes + (uint64_t)index * TEST_DESC_SIZE);
}

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

    /*
     * 故意让第一个 ConventionalMemory 从物理地址 0 开始：
     * 0 是合法物理地址，不能再和“失败”共用一个返回值。
     */
    desc(0)->type = MEM_CONVENTIONAL;
    desc(0)->physical_start = 0x00000000;
    desc(0)->number_of_pages = 2;

    desc(1)->type = MEM_LOADER_DATA;
    desc(1)->physical_start = 0x00100000;
    desc(1)->number_of_pages = 100;

    desc(2)->type = MEM_CONVENTIONAL;
    desc(2)->physical_start = 0x00200000;
    desc(2)->number_of_pages = 8;

    desc(3)->type = MEM_BOOT_SERVICES_DATA;
    desc(3)->physical_start = 0x00400000;
    desc(3)->number_of_pages = 100;

    bi.mmap_addr = (uint64_t)(uintptr_t)map_bytes;
    bi.mmap_size = sizeof(map_bytes);
    bi.mmap_desc_size = TEST_DESC_SIZE;
    bi.mmap_desc_count = TEST_DESC_COUNT;

    check(pmm_init(&bi) == 0, "pmm_init");
    check(pmm_alloc(0) == PMM_ALLOC_FAILED, "size 0 must fail");
    check(pmm_alloc(UINT64_MAX) == PMM_ALLOC_FAILED,
          "alignment overflow must fail");

    check(pmm_alloc(1) == 0x00000000,
          "physical address zero is a valid allocation");
    check(desc(0)->physical_start == 0x00001000 &&
          desc(0)->number_of_pages == 1,
          "zero-based descriptor advances normally");

    check(pmm_alloc(4096) == 0x00001000,
          "second low page allocates normally");

    check(pmm_alloc(4097) == 0x00200000,
          "4097 bytes round to 2 pages");
    check(desc(2)->physical_start == 0x00202000 &&
          desc(2)->number_of_pages == 6,
          "conventional descriptor shrinks by 2 pages");

    check(pmm_alloc(6 * PMM_PAGE_SIZE) == 0x00202000,
          "consume remaining conventional range");

    check(pmm_alloc(1) == PMM_ALLOC_FAILED,
          "allocator ignores non-conventional memory");

    check(desc(1)->physical_start == 0x00100000 &&
          desc(1)->number_of_pages == 100,
          "LoaderData is never consumed");
    check(desc(3)->physical_start == 0x00400000 &&
          desc(3)->number_of_pages == 100,
          "BootServicesData is never consumed");

    if (failures == 0) {
        printf("pmm 测试通过：%d 项\n", checks);
        return 0;
    }

    printf("pmm 测试失败：%d/%d 项不通过\n", failures, checks);
    return 1;
}
