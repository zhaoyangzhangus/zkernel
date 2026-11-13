#pragma once

#include <stdint.h>

#define PTE_PRESENT           (1ULL << 0)
#define PTE_WRITE             (1ULL << 1)
#define PTE_USER              (1ULL << 2)
#define PTE_WRITE_THROUGH     (1ULL << 3)
#define PTE_CACHE_DISABLE     (1ULL << 4)
#define PTE_ACCESSED          (1ULL << 5)
#define PTE_DIRTY             (1ULL << 6)
#define PTE_LARGE             (1ULL << 7)
#define PTE_GLOBAL            (1ULL << 8)
#define PTE_PAT_LARGE         (1ULL << 12)
#define PTE_NO_EXECUTE        (1ULL << 63)
#define PTE_ADDRESS_MASK      0x000FFFFFFFFFF000ULL

/* 序列化锁由 arch/x86_64/mm/page_table.c 持有；MMU 初始化只负责初始化该锁。 */
void x86_page_table_arch_init(void);
