#include "internal.h"

/*
 * 映射策略负责 VMA 的插入和移除。页表失效操作委托给 tlb.c，因此本文件仅在
 * 架构操作成功后修改 VMA/对象状态。
 */
typedef struct vm_retired {
    list_head_t areas; /* 必须是首字段，以维持 VMA 链表辅助函数的约定。 */
    struct anon_page_node *pages;
} vm_retired_t;

static kstatus_t vm_unmap_locked(vm_space_t *space, vaddr_t address,
                                  uint64_t end, list_head_t *retired,
                                  list_head_t **replacement_position);

static void vm_unmap_discard_backing(vm_area_t *area,
                                     uint64_t start, uint64_t end,
                                     list_head_t *retired) {
    vm_object_t *objects[2] = {area->object, area->private_object};
    uint64_t first = (area->object_offset + start - area->start) >> PAGE_SHIFT;
    uint64_t last = first + ((end - start) >> PAGE_SHIFT);
    for (unsigned i = 0; i < 2U; ++i) {
        vm_object_t *object = objects[i];
        if (object != 0 && object->type == VM_OBJECT_ANON &&
            (object->flags & VM_OBJECT_FLAG_MAP_OWNED) != 0U)
            (void)anon_page_retire_range(object, first, last,
                                         &((vm_retired_t *)retired)->pages);
    }
}

static void map_unlock_retired(vm_space_t *space, list_head_t *retired) {
    /* 分离的 VMA 在 PTE 失效期间继续持有后备对象引用。
     * 最终释放后备对象时不能长期占用地址空间锁。 */
    map_unlock(space);
    anon_page_release_retired(((vm_retired_t *)retired)->pages);
    ((vm_retired_t *)retired)->pages = 0;
    while (retired->next != retired) {
        vm_area_t *area = area_from_list(retired->next);
        list_del(&area->ordered_node);
        area_release(area);
    }
}

static kstatus_t map_fail_unlock(vm_space_t *space, list_head_t *retired,
                                 vm_object_t *owned_object,
                                 vm_object_t *private_object,
                                 kstatus_t status) {
    /* 此事务只拥有新建的后备对象引用。完成已退役 VMA 的处理后，
     * 在地址空间锁外释放这些引用。 */
    map_unlock_retired(space, retired);
    vm_object_put(owned_object);
    vm_object_put(private_object);
    return status;
}

static kstatus_t vm_map_object_impl(vm_space_t *space, vm_object_t *object,
                        vaddr_t *inout_address, uint64_t offset, size_t size,
                        uint32_t prot, uint32_t flags, bool replace_existing) {
    const uint32_t valid_prot = VM_PROT_READ | VM_PROT_WRITE |
                                VM_PROT_EXEC | VM_PROT_USER;
    const uint32_t valid_flags = VM_MAP_PRIVATE | VM_MAP_SHARED | VM_MAP_FIXED |
                                 VM_MAP_STACK | VM_MAP_GUARD |
                                 VM_MAP_ALLOW_RWX | VM_MAP_LOW32;
    if (space == 0 || inout_address == 0 || !range_size_valid(size) ||
        (replace_existing && (flags & VM_MAP_FIXED) == 0) ||
        (offset & (PAGE_SIZE - 1ULL)) != 0 || (prot & ~valid_prot) != 0 ||
        (flags & ~valid_flags) != 0 ||
        ((prot & (VM_PROT_WRITE | VM_PROT_EXEC)) ==
         (VM_PROT_WRITE | VM_PROT_EXEC) &&
         (flags & VM_MAP_ALLOW_RWX) == 0U) ||
        ((flags & VM_MAP_PRIVATE) != 0 && (flags & VM_MAP_SHARED) != 0)) {
        return K_EINVAL;
    }
    if ((flags & (VM_MAP_PRIVATE | VM_MAP_SHARED)) == 0) {
        flags |= VM_MAP_PRIVATE;
    }
    if (object != 0 && object->type == VM_OBJECT_SHARED &&
        (flags & VM_MAP_SHARED) == 0) {
        return K_EINVAL;
    }
    if (object != 0 &&
        (object->flags & VM_OBJECT_FLAG_MAP_OWNED) != 0U) return K_EINVAL;
    if (object != 0 && (offset > object->size || size > object->size - offset)) {
        return K_EINVAL;
    }

    vm_retired_t retirement = {0};
    list_head_t *retired = &retirement.areas;
    list_init(retired);
    bool created_object = false;
    vm_object_t *private_object = 0;
    if (object == 0 && (flags & VM_MAP_GUARD) == 0) {
        if (offset > UINT64_MAX - size ||
            vm_object_create_anon((size_t)(offset + size), &object) != K_OK) {
            return K_ENOMEM;
        }
        created_object = true;
        if ((flags & VM_MAP_PRIVATE) != 0U)
            object->flags |= VM_OBJECT_FLAG_MAP_OWNED;
    }
    if (object != 0 && object->type == VM_OBJECT_FILE &&
        (prot & VM_PROT_WRITE) != 0 && (flags & VM_MAP_SHARED) == 0) {
        if (vm_object_create_anon(object->size, &private_object) != K_OK) {
            if (created_object) vm_object_put(object);
            return K_ENOMEM;
        }
        private_object->flags |= VM_OBJECT_FLAG_MAP_OWNED;
    }

    /* 每个映射都需要一个新的 VMA。在 map_lock 禁止抢占前先完成分配；分配器补充/回收页面不能阻塞此 VM
     * 中的缺页处理。发布前，此空节点由本地退役链表持有，因此所有验证/替换失败路径都在映射锁外释放它。 */
    vm_area_t *area = area_allocate();
    if (area != 0) list_add_before(retired, &area->ordered_node);

    map_lock(space);
    uint64_t address = (uint64_t)*inout_address;
    uint64_t end;
    if ((flags & VM_MAP_FIXED) != 0) {
        if (!range_valid(address, size, &end)) {
            return map_fail_unlock(space, retired,
                created_object ? object : 0, private_object, K_EINVAL);
        }
    } else {
        if ((flags & VM_MAP_LOW32) != 0U) address = 0U;
        address = find_gap(space, address, size);
        if (address == 0 || !range_valid(address, size, &end)) {
            return map_fail_unlock(space, retired,
                created_object ? object : 0, private_object, K_ENOMEM);
        }
        if ((flags & VM_MAP_LOW32) != 0U && end > 0x80000000ULL) {
            return map_fail_unlock(space, retired,
                created_object ? object : 0, private_object, K_ENOMEM);
        }
    }

    /* 移除任何内容前先预留替换 VMA。移除和插入期间持续持有 map_lock，确保其他线程绝不会观察到空闲间隙。 */
    list_head_t *position = &space->area_list;
    if (replace_existing) {
        if (area == 0) {
            return map_fail_unlock(space, retired,
                created_object ? object : 0, private_object, K_ENOMEM);
        }
        kstatus_t status = vm_unmap_locked(space, (vaddr_t)address, end, retired,
                                            &position);
        if (status != K_OK) {
            return map_fail_unlock(space, retired,
                created_object ? object : 0, private_object, status);
        }
    }
    /* 复用移除操作返回的后继项，不再重复搜索。移除操作已在同一 map_lock 下发布有效索引。 */
    vm_area_t *candidate = replace_existing ? 0 :
        find_area_at_or_after(space, address);
    if (candidate != 0) {
        if (candidate->start < end) {
            return map_fail_unlock(space, retired,
                created_object ? object : 0, private_object, K_EBUSY);
        }
        position = &candidate->ordered_node;
    }

    /* 即使预分配失败，也要保持范围/重叠错误的优先级；尤其是 NOREPLACE 仍必须报告映射冲突。 */
    if (area == 0) {
        return map_fail_unlock(space, retired,
            created_object ? object : 0, private_object, K_ENOMEM);
    }
    list_del(&area->ordered_node);
    area->start = address;
    area->end = end;
    area->object_offset = offset;
    area->prot = prot;
    area->flags = flags;
    area->object = object;
    area->private_object = private_object;
    if (!created_object) vm_object_get(object);
    list_add_before(position, &area->ordered_node);
    insert_area_tree(space, area);
    atomic_fetch_add_explicit(&space->commit_pages, size >> PAGE_SHIFT,
                              memory_order_relaxed);
    vm_tlb_bump_generation(space);
    map_unlock_retired(space, retired);
    *inout_address = (vaddr_t)address;
    return K_OK;
}

kstatus_t vm_map_object(vm_space_t *space, vm_object_t *object,
                        vaddr_t *address, uint64_t offset, size_t size,
                        uint32_t prot, uint32_t flags) {
    return vm_map_object_impl(space, object, address, offset, size, prot, flags,
                              false);
}

kstatus_t vm_map_object_replace(vm_space_t *space, vm_object_t *object,
                                vaddr_t *address, uint64_t offset, size_t size,
                                uint32_t prot, uint32_t flags) {
    return vm_map_object_impl(space, object, address, offset, size, prot, flags,
                              true);
}

static kstatus_t vm_unmap_locked(vm_space_t *space, vaddr_t address,
                                  uint64_t end, list_head_t *retired,
                                  list_head_t **replacement_position) {
    kstatus_t result = K_OK;
    bool mapping_changed = false;
    bool invalidation_attempted = false;
    uint64_t invalidated_end = 0U;
    vm_area_t *root_area = space->areas.root != 0 ?
                           area_from_rb(space->areas.root) : 0;
    bool clear_all = root_area != 0 &&
                     address <= root_area->subtree_min_start &&
                     end >= root_area->subtree_max_end;
    vm_area_t *first = find_area_at_or_after(space, address);
    list_head_t *item = first != 0 ? &first->ordered_node : &space->area_list;
    while (item != &space->area_list) {
        vm_area_t *area = area_from_list(item);
        if (area->start >= end) break;
        item = item->next;

        uint64_t cut_start = address > area->start ? address : area->start;
        uint64_t cut_end = end < area->end ? end : area->end;
        if (cut_start > area->start && cut_end < area->end) {
            /* 修改 PTE 范围前先准备右侧 VMA。 */
            vm_area_t *right = vm_area_copy_tail(area, cut_end);
            if (right == 0) {
                result = K_ENOMEM;
                break;
            }
            invalidation_attempted = true;
            kstatus_t status = vm_tlb_unmap_range(space, cut_start, cut_end);
            if (status != K_OK) {
                list_add_before(retired, &right->ordered_node);
                result = status;
                break;
            }
            atomic_fetch_add_explicit(&space->advice_generation, 1U,
                                      memory_order_release);
            vm_unmap_discard_backing(area, cut_start, cut_end, retired);
            area->end = cut_start;
            list_add_before(area->ordered_node.next, &right->ordered_node);
            update_area_tree(area);
            insert_area_tree(space, right);
            mapping_changed = true;
            atomic_fetch_sub_explicit(&space->commit_pages,
                                      (cut_end - cut_start) >> PAGE_SHIFT,
                                      memory_order_relaxed);
            /* 此处的中间切分完成了请求区间。新的右侧剩余区域就是替换项的直接后继。 */
            item = &right->ordered_node;
            break;
        }

        if (cut_start >= invalidated_end) {
            uint64_t flush_end = cut_end;
            /* 连续且完整移除的 VMA 共用驻留页批次，避免每个小 VMA 单独等待
             * TLB 确认。仅沿有序链表前看，不跨空洞或需要切分的尾部。
             * 确认成功前保持全部 VMA 和后备对象不变。 */
            if (cut_start == area->start && cut_end == area->end) {
                for (list_head_t *next = item; next != &space->area_list;
                     next = next->next) {
                    vm_area_t *following = area_from_list(next);
                    if (following->start != flush_end || following->end > end) break;
                    flush_end = following->end;
                }
            }
            invalidation_attempted = true;
            kstatus_t status = vm_tlb_unmap_range(space, cut_start, flush_end);
            if (status != K_OK) {
                result = status;
                break;
            }
            invalidated_end = flush_end;
        }
        atomic_fetch_add_explicit(&space->advice_generation, 1U,
                                  memory_order_release);
        vm_unmap_discard_backing(area, cut_start, cut_end, retired);
        atomic_fetch_sub_explicit(&space->commit_pages,
                                  (cut_end - cut_start) >> PAGE_SHIFT,
                                  memory_order_relaxed);
        mapping_changed = true;
        if (cut_start == area->start && cut_end < area->end) {
            area->object_offset += cut_end - area->start;
            area->start = cut_end;
            update_area_tree(area);
            item = &area->ordered_node;
            break;
        } else if (cut_start > area->start && cut_end == area->end) {
            area->end = cut_start;
            update_area_tree(area);
        } else if (cut_start == area->start && cut_end == area->end) {
            if (!clear_all)
                erase_area_tree(space, area);
            list_del(&area->ordered_node);
            list_add_before(retired, &area->ordered_node);
        }
    }
    /* 完整拆除保持线性复杂度，无需逐节点旋转。拆除失败时，仍须在解锁前重新发布保留的后缀。 */
    if (clear_all && mapping_changed) {
        if (result == K_OK) space->areas.root = 0;
        else rebuild_area_tree(space);
    }
    if (replacement_position != 0 && result == K_OK)
        *replacement_position = item;
    /* 批处理失败返回前可能已移除部分 PTE。发布已尝试的失效操作，但不要发布空洞或 MMU 操作前的失败。 */
    if (invalidation_attempted) vm_tlb_bump_generation(space);
    return result;
}

kstatus_t vm_unmap(vm_space_t *space, vaddr_t address, size_t size) {
    uint64_t end;
    if (space == 0 || !range_valid((uint64_t)address, size, &end)) {
        return K_EINVAL;
    }
    vm_retired_t retirement = {0};
    list_head_t *retired = &retirement.areas;
    list_init(retired);
    map_lock(space);
    kstatus_t status = vm_unmap_locked(space, address, end, retired, 0);
    map_unlock_retired(space, retired);
    return status;
}
