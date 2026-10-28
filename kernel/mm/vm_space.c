#include <arch/x86_64/paging.h>
#include <arch/x86_64/cpu.h>
#include <kernel/console.h>
#include <kernel/mm.h>
#include <kernel/sched.h>
#include <kernel/vm.h>
#include <kernel/string.h>

#include "internal.h"

static atomic_uint_fast32_t g_next_pcid;
static atomic_uint g_vm_init_state;

typedef struct {
    uint64_t duration_ns, root, caller, tid;
} vm_lock_diagnostic_t;
static vm_lock_diagnostic_t g_vm_lock_diagnostics[2]; /* 等待、持有 */
static atomic_flag g_vm_lock_diagnostic_guard = ATOMIC_FLAG_INIT;
/* 可选的精细采样；默认保留原有的 10ms 快速门限。 */
static volatile uint64_t g_vm_lock_trace_threshold_ns = 10000000ULL;

static void vm_lock_record(unsigned kind, uint64_t ticks, uint64_t root,
                            uint64_t caller, uint64_t tid) {
    /* 仅发布较长延迟；持有地址空间锁时绝不分配内存、打印，
     * 也不等待其他诊断写入者。 */
    uint64_t threshold = g_vm_lock_trace_threshold_ns;
    if (threshold == 0U) threshold = 10000000ULL;
    if (threshold < 100000ULL) threshold = 100000ULL;
    if (threshold > 1000000000ULL) threshold = 1000000000ULL;
    if (threshold == 10000000ULL &&
        ticks < x86_boot_cpu_features.tsc_hz / 100U) return;
    uint64_t ns = x86_tsc_to_ns(ticks);
    if (ns < threshold) return;
    if (atomic_flag_test_and_set_explicit(&g_vm_lock_diagnostic_guard,
                                          memory_order_acquire)) return;
    if (ns > g_vm_lock_diagnostics[kind].duration_ns) {
        g_vm_lock_diagnostics[kind] =
            (vm_lock_diagnostic_t){ns, root, caller, tid};
    }
    atomic_flag_clear_explicit(&g_vm_lock_diagnostic_guard, memory_order_release);
}

/* 当前 rwlock 先提供正确的互斥语义，读侧并发会在 SMP 阶段细化。 */
void map_lock(vm_space_t *space) {
    uint64_t saved_flags = 0;
    bool enabled_delivery = false;
    sched_preempt_disable();
    uint64_t wait_started = x86_read_tsc();
    unsigned expected = 0U;
    while (!atomic_compare_exchange_strong_explicit(&space->map_lock.state,
                &expected, 1U, memory_order_acquire, memory_order_relaxed)) {
        if (!enabled_delivery) {
            __asm__ volatile ("pushfq; popq %0" : "=r"(saved_flags) : : "memory");
            x86_tlb_wait_begin();
            /* 地址空间锁持有者可能正在等待本 CPU 的 TLB 确认。 */
            __asm__ volatile ("sti" : : : "memory");
            enabled_delivery = true;
        }
        /* 轮询共享缓存行，不要反复争取写所有权。仅在持有者释放后重试获取操作。
         * 等待期间保持 CPU 固定，并启用 TLB 消息接收。 */
        do {
            __asm__ volatile ("pause");
        } while (atomic_load_explicit(&space->map_lock.state,
                                     memory_order_relaxed) != 0U);
        expected = 0U;
    }
    if (enabled_delivery) {
        if ((saved_flags & (1ULL << 9)) == 0) {
            __asm__ volatile ("cli" : : : "memory");
        }
        x86_tlb_wait_end();
    }
    thread_t *thread = sched_current_thread();
    uint64_t tid = thread != 0 ? thread->tid : 0U;
    uint64_t caller = (uint64_t)(uintptr_t)__builtin_return_address(0);
    vm_lock_record(0U, x86_read_tsc() - wait_started,
                    space->root_table.value, caller, tid);
    space->map_lock_started = x86_read_tsc();
    space->map_lock_caller = caller;
    space->map_lock_tid = tid;
}

void map_unlock(vm_space_t *space) {
    vm_lock_record(1U, x86_read_tsc() - space->map_lock_started,
                    space->root_table.value, space->map_lock_caller,
                    space->map_lock_tid);
    atomic_store_explicit(&space->map_lock.state, 0U, memory_order_release);
    sched_preempt_enable();
}

static void vm_global_initialize(void) {
    if (atomic_load_explicit(&g_vm_init_state, memory_order_acquire) == 2U) return;
    unsigned expected = 0U;
    if (atomic_compare_exchange_strong_explicit(&g_vm_init_state, &expected, 1U,
                                                 memory_order_acq_rel,
                                                 memory_order_acquire)) {
        atomic_init(&g_next_pcid, 1U);
        atomic_store_explicit(&g_vm_init_state, 2U, memory_order_release);
        return;
    }
    while (atomic_load_explicit(&g_vm_init_state, memory_order_acquire) != 2U) {
        __asm__ volatile ("pause");
    }
}

void area_release(vm_area_t *area) {
    if (area == 0) return;
    vm_object_put(area->object);
    vm_object_put(area->private_object);
    kfree(area);
}

uint32_t hardware_flags(uint32_t prot, bool cow) {
    uint32_t flags = 0;
    if ((prot & VM_PROT_USER) != 0) flags |= X86_PAGE_USER;
    if ((prot & VM_PROT_EXEC) != 0) flags |= X86_PAGE_EXEC;
    if ((prot & VM_PROT_WRITE) != 0 && !cow) flags |= X86_PAGE_WRITE;
    return flags;
}

/* 根据具体页判断私有文件映射是否已经拥有可写的 shadow 页。 */
uint32_t vm_area_page_flags(const vm_area_t *area, uint64_t page_address,
                            uint32_t prot) {
    bool cow = (area->flags & VM_AREA_COW) != 0;
    if (area->private_object != 0 && area->object != 0 &&
        area->object->type == VM_OBJECT_FILE && page_address >= area->start &&
        page_address < area->end) {
        uint64_t index = (area->object_offset + page_address - area->start) >> PAGE_SHIFT;
        if (anon_page_lookup(area->private_object, index) == 0) cow = true;
    }
    return hardware_flags(prot, cow);
}

/*
 * 只有在远端 CPU 确认完成 TLB 失效后，调用方才能释放 VMA 或其对象。
 * x86_unmap_page 在失效失败时会恢复 PTE，因此这里保留 VMA 元数据并把
 * 错误返回给上层，避免后备页被过早复用。
 */
kstatus_t vm_space_create(vm_space_t **out) {
    if (out == 0) return K_EINVAL;
    vm_global_initialize();
    vm_space_t *space = (vm_space_t *)kzalloc(sizeof(vm_space_t), 0);
    if (space == 0) return K_ENOMEM;
    page_t *root_page = page_alloc(0, PAGE_ALLOC_ZERO | PAGE_ALLOC_DMA32);
    if (root_page == 0) {
        kfree(space);
        return K_ENOMEM;
    }
    root_page->owner = PAGE_OWNER_PAGETABLE;
    paddr_t root_physical = page_to_phys(root_page);
    uint64_t *new_root = (uint64_t *)phys_to_direct(root_physical);
    paddr_t current_root_physical = x86_current_root_table();
    uint64_t *current_root = (uint64_t *)phys_to_direct(current_root_physical);
    if (new_root == 0 || current_root == 0) {
        page_free(root_page);
        kfree(space);
        return K_EIO;
    }
    /* 用户地址空间不继承低端恒等映射，只共享 PML4 的内核半区。 */
    memcpy(new_root + 256U, current_root + 256U,
           256U * sizeof(*new_root));
    refcount_init(&space->refs, 1U);
    space->root_table = root_physical;
    uint32_t pcid = atomic_fetch_add_explicit(&g_next_pcid, 1U, memory_order_relaxed);
    /* PCID 不循环复用；耗尽后使用 PCID 0，由 CR3 写入触发刷新。 */
    space->pcid = pcid <= 4094U ? (uint16_t)pcid : 0U;
    root_page->private_data = space->pcid;
    atomic_init(&space->map_lock.state, 0U);
    space->areas.root = 0;
    list_init(&space->area_list);
    for (uint32_t i = 0; i < VM_PT_LOCK_COUNT; ++i) {
        atomic_init(&space->pt_locks[i].state, 0U);
    }
    atomic_init(&space->tlb_generation, 1U);
    atomic_init(&space->advice_generation, 1U);
    atomic_init(&space->rss_pages, 0U);
    atomic_init(&space->commit_pages, 0U);
    *out = space;
    return K_OK;
}

void vm_space_get(vm_space_t *space) {
    if (space != 0) atomic_fetch_add_explicit(&space->refs.value, 1U,
                                              memory_order_relaxed);
}

void vm_space_put(vm_space_t *space) {
    if (space == 0 || atomic_fetch_sub_explicit(&space->refs.value, 1U,
                                                memory_order_acq_rel) != 1U) return;
    list_head_t retired;
    bool failed = false;
    list_init(&retired);
    map_lock(space);
    while (!list_empty(&space->area_list)) {
        vm_area_t *area = area_from_list(space->area_list.next);
        if (vm_tlb_unmap_range(space, area->start, area->end) != K_OK) {
            /*
             * 最后一个引用已经被消费，但此时绝不能释放地址空间。保留
             * 一个不可见引用和尚未撤销的 VMA/PTE 是安全的泄漏，后备页不会在
             * 远端 CPU 仍可能使用时被回收；正常路径不会触发这里。
             */
            atomic_store_explicit(&space->refs.value, 1U, memory_order_release);
            failed = true;
            break;
        }
        list_del(&area->ordered_node);
        list_add_tail(&retired, &area->ordered_node);
    }
    /* 最后一个引用意味着不再有活动修改者。TLB 操作失败时，要为有意保留且仍有后备的 VMA 保留有效索引；
     * 索引不得指向下方已成功回收的区域。 */
    if (!failed) space->areas.root = 0;
    else if (!list_empty(&retired)) rebuild_area_tree(space);
    map_unlock(space);
    /* 后备对象析构可能释放数千个页面。与常规取消映射一样，只能在 TLB 确认后摘除；并在 map_lock 的
     * 抢占保护之外释放，以便回收期间其他进程的界面仍可运行。 */
    while (!list_empty(&retired)) {
        vm_area_t *area = area_from_list(retired.next);
        list_del(&area->ordered_node);
        area_release(area);
    }
    if (failed) return;
    page_t *root_page = phys_to_page(space->root_table);
    if (root_page != 0) page_free(root_page);
    kfree(space);
}


/* 拆分后的 VMA 仍引用同一后备对象。每次 fork 只克隆一次，既保留子进程中的别名关系，也避免为每个
 * 权限片段都复制页索引。条目借用子进程 VMA 所持有的引用。 */
typedef struct vm_clone_entry {
    vm_object_t *source;
    vm_object_t *copy;
    rb_node_t index_node;
} vm_clone_entry_t;

static kstatus_t clone_anon_cached(rb_root_t *cache,
                                  vm_object_t *source, vm_object_t **out) {
    rb_node_t **link = &cache->root;
    rb_node_t *parent = 0;
    while (*link != 0) {
        vm_clone_entry_t *entry = (vm_clone_entry_t *)
            ((uint8_t *)*link - __builtin_offsetof(vm_clone_entry_t, index_node));
        parent = *link;
        if ((uintptr_t)source < (uintptr_t)entry->source) link = &parent->left;
        else if ((uintptr_t)source > (uintptr_t)entry->source) link = &parent->right;
        else {
            vm_object_get(entry->copy);
            *out = entry->copy;
            return K_OK;
        }
    }
    vm_clone_entry_t *entry = kmalloc(sizeof(*entry), 0);
    if (entry == 0) return K_ENOMEM;
    kstatus_t status = vm_object_clone_anon(source, out);
    if (status != K_OK) {
        kfree(entry);
        return status;
    }
    entry->source = source;
    entry->copy = *out;
    rb_tree_node_init(&entry->index_node);
    rb_tree_set_parent(&entry->index_node, parent);
    rb_tree_set_color(&entry->index_node, RB_TREE_RED);
    *link = &entry->index_node;
    rb_tree_insert_rebalance(cache, &entry->index_node);
    return K_OK;
}

/* 私有平衡树：后序释放为线性复杂度，递归深度受红黑树高度限制。条目借用活动子进程 VMA 的引用。 */
static void clone_cache_release_node(rb_node_t *node) {
    if (node == 0) return;
    clone_cache_release_node(node->left);
    clone_cache_release_node(node->right);
    kfree((uint8_t *)node - __builtin_offsetof(vm_clone_entry_t, index_node));
}

static void clone_cache_release(rb_root_t *cache) {
    clone_cache_release_node(cache->root);
    cache->root = 0;
}

kstatus_t vm_space_clone_cow(vm_space_t *source, vm_space_t **out) {
    if (source == 0 || out == 0) return K_EINVAL;
    uint64_t started = x86_read_tsc();
    uint64_t areas_copied = 0, ptes_visited = 0;
    bool protection_changed = false;
    bool cow_metadata_changed = false;
    rb_root_t clone_cache = {0};
    vm_space_t *child = 0;
    kstatus_t status = vm_space_create(&child);
    if (status != K_OK) return status;
    map_lock(source);
    uint64_t locked = x86_read_tsc();

    /*
     * 第一阶段只复制 VMA 和对象，不修改父地址空间。这样在内存不足时
     * 可以直接销毁半成品子空间，不会留下半个 COW fork。
     */
    for (list_head_t *item = source->area_list.next; item != &source->area_list;
         item = item->next) {
        vm_area_t *area = area_from_list(item);
        vm_area_t *copy = area_allocate();
        if (copy == 0) {
            status = K_ENOMEM;
            break;
        }
        copy->start = area->start;
        copy->end = area->end;
        copy->object_offset = area->object_offset;
        copy->prot = area->prot;
        copy->flags = area->flags;
        if ((area->flags & VM_MAP_PRIVATE) != 0 && area->object != 0 &&
            area->object->type == VM_OBJECT_ANON) {
            status = clone_anon_cached(&clone_cache, area->object, &copy->object);
            if (status != K_OK) {
                kfree(copy);
                break;
            }
        } else {
            copy->object = area->object;
            vm_object_get(copy->object);
        }
        if (status == K_OK && area->private_object != 0) {
            status = clone_anon_cached(&clone_cache, area->private_object,
                                       &copy->private_object);
        }
        if (status != K_OK) {
            area_release(copy);
            break;
        }
        list_add_before(&child->area_list, &copy->ordered_node);
        ++areas_copied;
    }
    uint64_t objects_done = x86_read_tsc();
    list_head_t *cow_first = &source->area_list;
    list_head_t *cow_end = &source->area_list;
    list_head_t *old_cow_first = &source->area_list;
    list_head_t *old_cow_end = &source->area_list;

    /*
     * 第二阶段才修改父空间的 PTE。即使 VMA 已经是 COW，页级写故障也可能
     * 让其中部分 PTE 再次可写，因此每次克隆都必须重新设为只读。旧 COW
     * 标记只用于失败回滚；失败后保留这些 PTE 为只读仍是安全状态。
     */
    if (status == K_OK) {
        list_head_t *source_item = source->area_list.next;
        list_head_t *child_item = child->area_list.next;
        while (source_item != &source->area_list &&
               child_item != &child->area_list) {
            vm_area_t *area = area_from_list(source_item);
            vm_area_t *copy = area_from_list(child_item);
            source_item = source_item->next;
            child_item = child_item->next;
            if ((area->flags & VM_MAP_PRIVATE) == 0 || area->object == 0 ||
                (area->object->type != VM_OBJECT_ANON && area->private_object == 0)) {
                continue;
            }

            /* 任何 PTE 操作可能失败前，先记录当前区域。未修改的现有 COW 区域没有 OLD_COW 标记；若回滚
             * 整个列表，会错误地将其共享页面改为可写。 */
            if (cow_first == &source->area_list)
                cow_first = &area->ordered_node;
            cow_end = source_item;
            bool was_cow = (area->flags & VM_AREA_COW) != 0;
            if (was_cow) {
                area->flags |= VM_AREA_CLONE_OLD_COW;
                if (old_cow_first == &source->area_list)
                    old_cow_first = &area->ordered_node;
                old_cow_end = source_item;
            } else cow_metadata_changed = true;
            /* 更改首个 PTE 前先发布 COW，即使后续 protect 失败也如此。回滚必须覆盖该部分修改的区域。 */
            area->flags |= VM_AREA_COW;
            copy->flags |= VM_AREA_COW;
            if ((area->prot & VM_PROT_WRITE) != 0) {
                for (uint64_t va = area->start; va < area->end; va += PAGE_SIZE) {
                    vaddr_t resident;
                    kstatus_t scan = x86_next_present_page(
                        source->root_table, (vaddr_t)va, area->end, &resident);
                    if (scan == K_ENOENT) break;
                    if (scan != K_OK) {
                        status = scan;
                        break;
                    }
                    va = resident;
                    ++ptes_visited;
                    bool changed = false;
                    kstatus_t protect = x86_protect_user_page_deferred(source->root_table,
                                                          (vaddr_t)va,
                                                          hardware_flags(area->prot, true),
                                                          X86_CACHE_WB, &changed);
                    protection_changed |= changed;
                    if (protect != K_OK && protect != K_ENOENT) {
                        status = protect;
                        break;
                    }
                }
            }
            if (status != K_OK) break;
        }
    }

    /* 通过一次已确认的用户根页表刷新，发布全部 COW 权限限制，而不是为每个驻留页单独往返一次 IPI。
     * 此时子进程仍为私有状态，map_lock 可阻止并发修改 VM 元数据。 */
    if (status == K_OK && protection_changed &&
        !x86_tlb_shootdown_root(source->root_table)) status = K_EIO;
    uint64_t protection_done = x86_read_tsc();
    if (status != K_OK) {
        /* 尽力恢复父空间；恢复失败时保留 COW 标志仍然是安全状态。 */
        /* 尚未完成的当前区域没有旧 COW 标记，但可能已经部分改成只读。 */
        for (list_head_t *item = cow_first;
             item != cow_end; item = item->next) {
            vm_area_t *area = area_from_list(item);
            bool was_cow = (area->flags & VM_AREA_CLONE_OLD_COW) != 0;
            bool cow_backed = (area->flags & VM_MAP_PRIVATE) != 0 &&
                area->object != 0 && (area->object->type == VM_OBJECT_ANON ||
                                     area->private_object != 0);
            /* 在同一次遍历中消费回滚标记，并在恢复权限前将其含义保存在局部变量中。 */
            if (was_cow && cow_backed) area->flags &= ~VM_AREA_CLONE_OLD_COW;
            if ((area->flags & VM_AREA_COW) == 0 ||
                was_cow ||
                (area->prot & VM_PROT_WRITE) == 0 || !cow_backed) {
                continue;
            }
            bool restored = true;
            area->flags &= ~VM_AREA_COW;
            for (uint64_t va = area->start; va < area->end; va += PAGE_SIZE) {
                vaddr_t resident;
                kstatus_t scan = x86_next_present_page(
                    source->root_table, (vaddr_t)va, area->end, &resident);
                if (scan == K_ENOENT) break;
                if (scan != K_OK) {
                    restored = false;
                    break;
                }
                va = resident;
                kstatus_t restore = x86_protect_page(source->root_table,
                                                      (vaddr_t)va,
                                                      vm_area_page_flags(area, va,
                                                                         area->prot),
                                                      X86_CACHE_WB);
                if (restore != K_OK && restore != K_ENOENT) restored = false;
            }
            if (!restored) area->flags |= VM_AREA_COW;
        }
    } else {
        /* 失败的克隆已在上方回滚时消费标记。首次成功 fork 不会设置标记；这里只清除实际的旧标记范围。 */
        for (list_head_t *item = old_cow_first;
             item != old_cow_end; item = item->next) {
            vm_area_t *area = area_from_list(item);
            area->flags &= ~VM_AREA_CLONE_OLD_COW;
        }
    }
    if (status == K_OK) {
        atomic_store_explicit(&child->commit_pages,
                              atomic_load_explicit(&source->commit_pages,
                                                   memory_order_relaxed),
                              memory_order_relaxed);
        if (cow_metadata_changed || protection_changed)
            vm_tlb_bump_generation(source);
    }
    map_unlock(source);
    /* Fork 会改变父进程的权限/COW 标志，但不会改变其 VMA 边界或地址索引摘要。下方设置 *out 前，子进程
     * 仍为私有状态；构建其索引时无需延长父进程的抢占保护。 */
    if (status == K_OK) rebuild_area_tree(child);
    /* 缓存条目借用对象引用，并且仅属于此次 fork。分配缓存不必延长源映射的抢占保护；记账完成前要保留
     * 子进程 VMA 引用。 */
    clone_cache_release(&clone_cache);
    uint64_t finished = x86_read_tsc();
    if (x86_tsc_to_ns(finished - started) >= 50000000ULL) {
        (void)kernel_serial_printf_serial_only(
            "OS_SLOW_COW root=%llx areas=%llu ptes=%llu "
            "lock_ns=%llu objects_ns=%llu protect_ns=%llu finish_ns=%llu status=%lld\r\n",
            (unsigned long long)source->root_table.value,
            (unsigned long long)areas_copied, (unsigned long long)ptes_visited,
            (unsigned long long)x86_tsc_to_ns(locked - started),
            (unsigned long long)x86_tsc_to_ns(objects_done - locked),
            (unsigned long long)x86_tsc_to_ns(protection_done - objects_done),
            (unsigned long long)x86_tsc_to_ns(finished - protection_done),
            (long long)status);
    }
    if (status != K_OK) {
        vm_space_put(child);
        return status;
    }
    *out = child;
    return K_OK;
}
