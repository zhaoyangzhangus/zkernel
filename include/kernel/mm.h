#pragma once
#pragma once
#include "base.h"
#include "list.h"
#include "refcount.h"

#define BUDDY_MAX_ORDER 18u
#define PAGE_SECTION_SHIFT 18u /* 2^18 pages = 1 GiB physical span */

enum page_zone_id {
    PAGE_ZONE_DMA32 = 0,
    PAGE_ZONE_NORMAL = 1,
    PAGE_ZONE_COUNT,
};

enum page_owner_kind {
    PAGE_OWNER_NONE = 0,
    PAGE_OWNER_BUDDY,
    PAGE_OWNER_SLAB,
    PAGE_OWNER_ANON,
    PAGE_OWNER_FILE,
    PAGE_OWNER_PAGETABLE,
    PAGE_OWNER_DMA,
    PAGE_OWNER_DEVICE,
};

enum page_flags {
    PAGE_FREE          = 1u << 0,
    PAGE_DIRTY         = 1u << 1,
    PAGE_UPTODATE      = 1u << 2,
    PAGE_PINNED        = 1u << 3,
    PAGE_COMPOUND_HEAD = 1u << 4,
    PAGE_COMPOUND_TAIL = 1u << 5,
    PAGE_WRITEBACK     = 1u << 6,
};

typedef struct page {
    atomic_uint refs;
    atomic_int mapcount;
    uint32_t flags;
    uint16_t owner;
    uint8_t order;  /* valid for buddy head or allocated compound head */
    uint8_t zone;

    union {
        list_head_t free_node;
        struct { void *cache; void *slab; } slab;
        struct { void *mapping; uint64_t index; } file;
        struct { struct page *head; uint64_t index; } compound;
    } u;

    uint64_t private_data;
} page_t;

_Static_assert(sizeof(page_t) <= CACHELINE_SIZE, "page_t must fit within one cache line");

typedef uint32_t page_alloc_flags_t;
enum {
    PAGE_ALLOC_ZERO  = 1u << 0,
    PAGE_ALLOC_DMA32 = 1u << 1,
    PAGE_ALLOC_ATOMIC= 1u << 2,
};

typedef struct page_section {
    pfn_t base_pfn;
    page_t *pages;
    uint32_t present_pages;
    uint32_t flags;
} page_section_t;

page_t *page_alloc(uint8_t order, page_alloc_flags_t flags);
void page_free(page_t *head);
kstatus_t page_memory_get_stats(uint64_t *total_pages, uint64_t *free_pages);
page_t *phys_to_page(paddr_t pa);
paddr_t page_to_phys(const page_t *page);
void *phys_to_direct(paddr_t pa);
paddr_t direct_to_phys(const void *va);

void *kmalloc(size_t size, uint32_t flags);
/* 仅适用于独占的大块分配。在现有物理块内扩展，不分配或复制内存；新增字节按 kmalloc 的方式
 * 初始化，并移动尾部保护区。返回 false 时分配保持不变（收缩或小块分配也如此）。指针必须指向
 * 有效的 kmalloc 分配。 */
bool kmalloc_try_grow(void *pointer, size_t size);
/* 尽力从现有全局 slab 中执行中断安全的小块分配。绝不初始化、等待锁、访问 CPU 缓存或分配页面。
 * 返回 NULL 也表示缓存竞争或耗尽。应在线程上下文中释放。 */
void *kmalloc_try_cached(size_t size, uint32_t flags);
void *kzalloc(size_t size, uint32_t flags);
void kfree(void *ptr);
void *vmalloc(size_t size);
void vfree(void *ptr);
/* 将调用方拥有的物理页面映射到内核 vmalloc 区域。映射不会取得页面所有权；此形式下，vfree 只会
 * 拆除虚拟映射。 */
kstatus_t vmalloc_map_pages(page_t *const *pages, uint32_t page_count,
                            uint32_t cache_mode, void **out);
