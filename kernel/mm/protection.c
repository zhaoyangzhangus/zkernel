#include <arch/x86_64/paging.h>
#include <uapi/mm.h>
#include "internal.h"

/* 调用方持有 map_lock：空洞仍由 VMA 权限约束，之后由缺页处理程序填充，
 * 而不是由 mprotect 填充。 */
static kstatus_t protect_resident_pages(vm_space_t *space,
                                        const vm_area_t *area, uint32_t prot,
                                        bool *deferred_changed) {
    for (uint64_t va = area->start; va < area->end; va += PAGE_SIZE) {
        vaddr_t resident;
        kstatus_t status = x86_next_present_page(
            space->root_table, (vaddr_t)va, area->end, &resident);
        if (status == K_ENOENT) return K_OK;
        if (status != K_OK) return status;
        va = resident;
        uint32_t flags = vm_area_page_flags(area, va, prot);
        if ((flags & (X86_PAGE_USER | X86_PAGE_GLOBAL)) == X86_PAGE_USER) {
            bool changed = false;
            status = x86_protect_user_page_deferred(space->root_table,
                       (vaddr_t)va, flags, X86_CACHE_WB, &changed);
            *deferred_changed |= changed;
        } else {
            /* 保留原生 supervisor/global 映射语义。 */
            status = x86_protect_page(space->root_table, (vaddr_t)va,
                                       flags, X86_CACHE_WB);
        }
        if (status != K_OK && status != K_ENOENT) return status;
    }
    return K_OK;
}

static bool range_fully_mapped(const vm_space_t *space,
                               uint64_t start, uint64_t end,
                               vm_area_t **first, vm_area_t **last) {
    /* map_lock 可保持两个索引稳定。先查找一次，再验证相邻关系，
     * 不必遍历无关的低地址 VMA。 */
    vm_area_t *start_area = find_area(space, start);
    if (start_area == 0) return false;
    uint64_t cursor = start;
    for (list_head_t *item = &start_area->ordered_node;
         item != &space->area_list; item = item->next) {
        vm_area_t *area = area_from_list(item);
        if (area->end <= cursor) continue;
        if (area->start > cursor) return false;
        if (area->end > cursor) cursor = area->end;
        if (cursor >= end) {
            if (first != 0) *first = start_area;
            if (last != 0) *last = area;
            return true;
        }
    }
    return false;
}

static bool vm_advice_supported(uint32_t advice) {
    switch (advice) {
        case OS_VM_ADVICE_NORMAL:
        case OS_VM_ADVICE_RANDOM:
        case OS_VM_ADVICE_SEQUENTIAL:
        case OS_VM_ADVICE_WILLNEED:
        case OS_VM_ADVICE_DONTNEED:
        case OS_VM_ADVICE_DONTDUMP:
        case OS_VM_ADVICE_DODUMP:
            return true;
        default:
            return false;
    }
}

bool vm_range_is_mapped(vm_space_t *space, vaddr_t address, size_t size) {
    uint64_t end;
    bool mapped;
    if (space == 0 || !range_valid((uint64_t)address, size, &end)) return false;
    map_lock(space);
    mapped = range_fully_mapped(space, (uint64_t)address, end, 0, 0);
    map_unlock(space);
    return mapped;
}

kstatus_t vm_sync(vm_space_t *space, vaddr_t address, size_t size,
                  uint32_t flags) {
    typedef struct sync_request {
        struct sync_request *next;
        vm_object_t *object;
        uint64_t first_page, page_count;
    } sync_request_t;
    sync_request_t *requests = 0;
    sync_request_t **tail = &requests;
    const uint32_t valid_flags = OS_VM_SYNC_ASYNC |
                                 OS_VM_SYNC_INVALIDATE |
                                 OS_VM_SYNC_SYNC;
    uint64_t end;
    kstatus_t status = K_OK;
    vm_area_t *first_area, *last_area;
    if (space == 0 || !range_valid((uint64_t)address, size, &end) ||
        (flags & ~valid_flags) != 0U ||
        ((flags & (OS_VM_SYNC_ASYNC | OS_VM_SYNC_SYNC)) == 0U) ||
        ((flags & (OS_VM_SYNC_ASYNC | OS_VM_SYNC_SYNC)) ==
         (OS_VM_SYNC_ASYNC | OS_VM_SYNC_SYNC))) {
        return K_EINVAL;
    }

    map_lock(space);
    if (!range_fully_mapped(space, (uint64_t)address, end, &first_area, &last_area)) {
        map_unlock(space);
        return K_ENOENT;
    }
    list_head_t *range_end = last_area->ordered_node.next;
    for (list_head_t *item = &first_area->ordered_node;
         item != range_end; item = item->next) {
        vm_area_t *area = area_from_list(item);
        uint64_t overlap_start;
        uint64_t overlap_end;
        uint64_t object_offset;
        uint64_t file_offset;
        uint64_t page_count;
        const vm_file_ops_t *ops;

        if ((area->flags & VM_MAP_GUARD) != 0U) {
            status = K_EINVAL;
            break;
        }
        if ((area->flags & VM_MAP_SHARED) == 0U || area->object == 0 ||
            area->object->type != VM_OBJECT_FILE) continue;

        overlap_start = area->start > address ? area->start : address;
        overlap_end = area->end < end ? area->end : end;
        if (overlap_end <= overlap_start ||
            area->object_offset > UINT64_MAX -
                (overlap_start - area->start)) {
            status = K_EOVERFLOW;
            break;
        }
        object_offset = area->object_offset +
                        (overlap_start - area->start);
        if (area->object->u.file.file_offset > UINT64_MAX - object_offset) {
            status = K_EOVERFLOW;
            break;
        }
        file_offset = area->object->u.file.file_offset + object_offset;
        page_count = (overlap_end - overlap_start) >> PAGE_SHIFT;
        ops = area->object->u.file.ops;
        if (page_count == 0U || ops == 0 || ops->sync == 0) continue;
        sync_request_t *request = kmalloc(sizeof(*request), 0U);
        if (request == 0) {
            status = K_ENOMEM;
            break;
        }
        vm_object_get(area->object);
        *request = (sync_request_t){
            .object = area->object,
            .first_page = file_offset >> PAGE_SHIFT,
            .page_count = page_count,
        };
        *tail = request;
        tail = &request->next;
    }
    map_unlock(space);
    /* 不再使用 VMA/链表指针。每个请求都会持有不可变的文件对象/映射，直到回调结束；
     * 即使 I/O 期间另一颗 CPU 解映射、拆分或替换了原地址范围也不受影响。 */
    while (requests != 0) {
        sync_request_t *request = requests;
        requests = request->next;
        vm_object_t *object = request->object;
        if (status == K_OK) {
            status = object->u.file.ops->sync(object->u.file.mapping,
                request->first_page, request->page_count);
        }
        vm_object_put(object);
        kfree(request);
    }
    return status;
}

kstatus_t vm_advise(vm_space_t *space, vaddr_t address, size_t size,
                    uint32_t advice) {
    uint64_t end;
    vm_area_t *first_area, *last_area;
    if (space == 0 || !range_valid((uint64_t)address, size, &end) ||
        !vm_advice_supported(advice)) return K_EINVAL;
    map_lock(space);
    if (!range_fully_mapped(space, (uint64_t)address, end, &first_area, &last_area)) {
        map_unlock(space);
        return K_ENOENT;
    }
    list_head_t *range_end = last_area->ordered_node.next;
    for (list_head_t *item = &first_area->ordered_node;
         item != range_end; item = item->next) {
        vm_area_t *area = area_from_list(item);
        if ((area->flags & VM_MAP_GUARD) != 0U) {
            map_unlock(space);
            return K_EINVAL;
        }
    }
    if (advice != OS_VM_ADVICE_DONTNEED) {
        /* 这些是提示；System 支持 core dump 前，DONTDUMP/DODUMP 不执行操作，
         * 但仍须允许 Linux 调用方使用它们。 */
        map_unlock(space);
        return K_OK;
    }

    /* DONTNEED 先丢弃常驻映射。所有 shootdown 成功前都保留后备对象，
     * 避免解映射失败时丢失用户数据。 */
    /* 整个区间已确认均有映射且不含保护区。允许稀疏取消映射跨相邻 VMA 批量处理驻留页面，避免每个
     * 小 VMA 都单独进行一次 IPI/确认往返。 */
    kstatus_t unmap_status = vm_tlb_unmap_range(space, address, end);
    if (unmap_status != K_OK) {
        map_unlock(space);
        return unmap_status;
    }

    /* 在此操作前已采样后备页的缺页处理程序，必须在安装页面前重试。映射锁会阻止新快照，直到下方的
     * 后备页范围完成丢弃。 */
    atomic_fetch_add_explicit(&space->advice_generation, 1U,
                              memory_order_release);
    anon_page_node_t *retired = 0;
    for (list_head_t *item = &first_area->ordered_node;
         item != range_end; item = item->next) {
        vm_area_t *area = area_from_list(item);
        vm_object_t *discard_object = 0;
        if ((area->flags & VM_MAP_PRIVATE) == 0U) continue;
        if (area->object->type == VM_OBJECT_ANON) {
            discard_object = area->object;
        } else if (area->object->type == VM_OBJECT_FILE &&
                   area->private_object != 0) {
            discard_object = area->private_object;
        }
        if (discard_object == 0) continue;

        uint64_t overlap_start = area->start > address ? area->start : address;
        uint64_t overlap_end = area->end < end ? area->end : end;
        uint64_t byte_offset = area->object_offset +
                               (overlap_start - area->start);
        uint64_t first_index = byte_offset >> PAGE_SHIFT;
        uint64_t page_count = (overlap_end - overlap_start) >> PAGE_SHIFT;
        if (first_index > UINT64_MAX - page_count) {
            map_unlock(space);
            anon_page_release_retired(retired);
            return K_EOVERFLOW;
        }
        kstatus_t status = anon_page_retire_range(
            discard_object, first_index, first_index + page_count, &retired);
        if (status != K_OK) {
            map_unlock(space);
            anon_page_release_retired(retired);
            return status;
        }
    }
    vm_tlb_bump_generation(space);
    map_unlock(space);
    anon_page_release_retired(retired);
    return K_OK;
}

static kstatus_t vm_protect_impl(vm_space_t *space, vaddr_t address,
                                 size_t size, uint32_t prot,
                                 bool allow_rwx) {
    uint64_t end;
    const uint32_t valid_prot = VM_PROT_READ | VM_PROT_WRITE |
                                VM_PROT_EXEC | VM_PROT_USER;
    if (space == 0 || !range_valid((uint64_t)address, size, &end) ||
        (prot & ~valid_prot) != 0 ||
        ((prot & (VM_PROT_WRITE | VM_PROT_EXEC)) ==
         (VM_PROT_WRITE | VM_PROT_EXEC) && !allow_rwx)) {
        return K_EINVAL;
    }

    map_lock(space);
    vm_area_t *start_area, *end_area;
    if (!range_fully_mapped(space, address, end, &start_area, &end_area)) {
        map_unlock(space);
        return K_ENOENT;
    }
    /* 已有权限无需再次切分 VMA 或遍历 PTE。COW 等受限 PTE 仍由缺页路径处理，
     * 不因重复声明可写权限而解除共享页保护。私有可写文件若尚无影子对象，
     * 则继续原有的准备事务，不能仅凭 VMA 权限跳过。 */
    bool unchanged = start_area->prot == prot;
    for (list_head_t *item = &start_area->ordered_node;
         unchanged && item != end_area->ordered_node.next; item = item->next) {
        vm_area_t *area = area_from_list(item);
        if (area->prot != prot ||
            ((prot & VM_PROT_WRITE) != 0U && area->object != 0 &&
             area->object->type == VM_OBJECT_FILE &&
             (area->flags & VM_MAP_PRIVATE) != 0U && area->private_object == 0)) {
            unchanged = false;
            break;
        }
    }
    if (unchanged) {
        map_unlock(space);
        return K_OK;
    }
    /* 先解析两个边界。在后续分配/MMU 操作可能失败并释放 map_lock 前，逐步发布每次分割结果。 */
    if (address > start_area->start) {
        vm_area_t *right = vm_area_split(start_area, address);
        if (right == 0) {
            map_unlock(space);
            return K_ENOMEM;
        }
        update_area_tree(start_area);
        insert_area_tree(space, right);
        if (end_area == start_area) end_area = right;
        start_area = right;
    }
    if (end < end_area->end) {
        vm_area_t *right = vm_area_split(end_area, end);
        if (right == 0) {
            map_unlock(space);
            return K_ENOMEM;
        }
        update_area_tree(end_area);
        insert_area_tree(space, right);
    }
    /* 后续所有阶段都使用此稳定且已完整分割的列表区间。持有 map_lock 时，Shadow/PTE 修改不能插入或移除 VMA。 */
    list_head_t *range_end = end_area->ordered_node.next;

    /* 修改 PTE 前，私有可写文件映射需要先创建影子对象。 */
    if ((prot & VM_PROT_WRITE) != 0) {
        list_head_t *shadow_begin = range_end;
        for (list_head_t *item = &start_area->ordered_node;
             item != range_end; item = item->next) {
            vm_area_t *area = area_from_list(item);
            if (area->object == 0 ||
                area->object->type != VM_OBJECT_FILE ||
                (area->flags & VM_MAP_PRIVATE) == 0 ||
                area->private_object != 0) {
                continue;
            }
            if (vm_object_create_anon(area->object->size,
                                      &area->private_object) != K_OK) {
                for (list_head_t *rollback_item = shadow_begin;
                     shadow_begin != range_end && rollback_item != item;
                     rollback_item = rollback_item->next) {
                    vm_area_t *rollback = area_from_list(rollback_item);
                    if ((rollback->flags & VM_AREA_CLONE_OLD_COW) != 0) {
                        vm_object_put(rollback->private_object);
                        rollback->private_object = 0;
                        rollback->flags &= ~VM_AREA_CLONE_OLD_COW;
                    }
                }
                map_unlock(space);
                return K_ENOMEM;
            }
            area->private_object->flags |= VM_OBJECT_FLAG_MAP_OWNED;
            area->flags |= VM_AREA_CLONE_OLD_COW;
            if (shadow_begin == range_end) shadow_begin = item;
        }
        for (list_head_t *item = shadow_begin;
             item != range_end; item = item->next) {
            area_from_list(item)->flags &= ~VM_AREA_CLONE_OLD_COW;
        }
    }

    /* 大型匿名预留区可能完全没有驻留 PTE。不要对该稀疏 VMA 中的每个 4 KiB 页面都调用
     * x86_protect_page。 */
    if (x86_page_range_has_present_pages(
            space->root_table, (vaddr_t)address, end)) {
        /* 先更新全部 PTE。在发布 VMA 权限或释放 map_lock 前，为整个事务执行一次已确认的用户根页表刷新。 */
        bool deferred_changed = false;
        kstatus_t status = K_OK;
        list_head_t *rollback_end = &start_area->ordered_node;
        for (list_head_t *item = &start_area->ordered_node;
             item != range_end; item = item->next) {
            vm_area_t *area = area_from_list(item);
            /* 失败的 VMA 中可能已经有 PTE 被修改。回滚时须包含该 VMA，但绝不扫描或重写尚未处理的后缀。 */
            rollback_end = item->next;
            status = protect_resident_pages(space, area, prot, &deferred_changed);
            if (status != K_OK) break;
        }
        if (status == K_OK && deferred_changed &&
            !x86_tlb_shootdown_root(space->root_table)) status = K_EIO;
        if (status != K_OK) {
            bool rollback_changed = false;

            for (list_head_t *rollback_item = &start_area->ordered_node;
                 rollback_item != rollback_end;
                 rollback_item = rollback_item->next) {
                vm_area_t *rollback = area_from_list(rollback_item);
                (void)protect_resident_pages(space, rollback, rollback->prot,
                                            &rollback_changed);
            }
            if ((deferred_changed || rollback_changed) &&
                !x86_tlb_shootdown_root(space->root_table)) status = K_EIO;
            map_unlock(space);
            return status;
        }
    }

    for (list_head_t *item = &start_area->ordered_node;
         item != range_end; item = item->next) {
        vm_area_t *area = area_from_list(item);
        area->prot = prot;
    }
    /* 权限事务及刷新确认成功后再合并，失败回滚仍保留原有区域边界。 */
    list_head_t retired;
    list_init(&retired);
    vm_area_merge_range(space, start_area, range_end, &retired);
    vm_tlb_bump_generation(space);
    map_unlock(space);
    while (!list_empty(&retired)) {
        vm_area_t *area = area_from_list(retired.next);
        list_del(&area->ordered_node);
        area_release(area);
    }
    return K_OK;
}

kstatus_t vm_protect(vm_space_t *space, vaddr_t address, size_t size,
                     uint32_t prot) {
    return vm_protect_impl(space, address, size, prot, false);
}

kstatus_t vm_protect_allow_rwx(vm_space_t *space, vaddr_t address,
                               size_t size, uint32_t prot) {
    return vm_protect_impl(space, address, size, prot, true);
}
