#pragma once

#include <arch/x86_64/paging.h>
#include <kernel/mm.h>
#include <kernel/vm.h>

#define VM_USER_BASE          0x0000000000010000ULL
#define VM_USER_END           (X86_64_USER_TOP + 1ULL)
#define VM_AREA_COW           (1U << 31)
/* clone_cow 回滚标记，不会暴露给用户态。 */
#define VM_AREA_CLONE_OLD_COW (1U << 30)

#define VM_OBJECT_INTERNAL     (1U << 31)

/* 匿名后备存储由 MM 内部唯一的所有者 object.c 管理。 */
typedef struct anon_page_node {
    uint64_t index;
    page_t *page;
    struct anon_page_node *next;
    /* 稳定的所有者链接允许索引查找后以 O(1) 时间移除。 */
    struct anon_page_node **list_link;
    rb_node_t index_node;
} anon_page_node_t;

typedef struct anon_store {
    spinlock_t lock;
    anon_page_node_t *pages;
    rb_root_t page_index;
} anon_store_t;

void vm_object_stats_init(vm_object_t *object);
/* 通用匿名后备；共享调用方还要求满足对齐条件。 */
kstatus_t vm_object_create_anon_store(size_t size, uint32_t type,
                                      vm_object_t **out);

/* VM 空间实现单元之间共享的私有 VMA 辅助函数。 */
vm_area_t *area_from_list(list_head_t *node);
vm_area_t *area_from_rb(rb_node_t *node);
void rebuild_area_tree(vm_space_t *space);
void insert_area_tree(vm_space_t *space, vm_area_t *area);
void update_area_tree(vm_area_t *area);
void erase_area_tree(vm_space_t *space, vm_area_t *area);
void vm_area_merge_range(vm_space_t *space, vm_area_t *first,
                          list_head_t *range_end, list_head_t *retired);
vm_area_t *find_area(const vm_space_t *space, uint64_t address);
vm_area_t *find_area_at_or_after(const vm_space_t *space, uint64_t address);
bool range_size_valid(uint64_t size);
bool range_valid(uint64_t address, uint64_t size, uint64_t *end);
uint64_t find_gap(const vm_space_t *space, uint64_t hint, uint64_t size);
vm_area_t *area_allocate(void);
vm_area_t *vm_area_copy_tail(const vm_area_t *area, uint64_t start);
vm_area_t *vm_area_split(vm_area_t *area, uint64_t split);

void map_lock(vm_space_t *space);
void map_unlock(vm_space_t *space);
void area_release(vm_area_t *area);
uint32_t hardware_flags(uint32_t prot, bool cow);
uint32_t vm_area_page_flags(const vm_area_t *area, uint64_t page_address,
                            uint32_t prot);
/* pin 为真时在存储锁内取得独立引用，调用方必须用 anon_page_unpin 释放。 */
kstatus_t anon_page_get(vm_object_t *object, uint64_t index, bool create,
                        bool pin, page_t **out);
page_t *anon_page_lookup(vm_object_t *object, uint64_t index);
bool anon_page_pin(vm_object_t *object, uint64_t index, page_t *expected);
void anon_page_unpin(page_t *page);
kstatus_t anon_page_discard_range(vm_object_t *object, uint64_t first_index,
                                  uint64_t end_index);
/* 在映射保护下摘除；释放保护后再释放物理页。调用方独立拥有此列表，不依赖原有后备对象。 */
kstatus_t anon_page_retire_range(vm_object_t *object, uint64_t first_index,
                                  uint64_t end_index,
                                  anon_page_node_t **retired_out);
void anon_page_release_retired(anon_page_node_t *retired);
kstatus_t private_file_shadow_page(vm_area_t *area, uint64_t index,
                                   paddr_t source, page_t **out);
kstatus_t anon_page_cow(vm_object_t *object, uint64_t index, page_t **out);
kstatus_t vm_object_clone_anon(vm_object_t *source, vm_object_t **out);

kstatus_t vm_tlb_unmap_range(vm_space_t *space, uint64_t start, uint64_t end);
void vm_tlb_bump_generation(vm_space_t *space);
