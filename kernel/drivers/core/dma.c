#include <kernel/dma.h>
#include <kernel/device.h>
#include <kernel/iommu.h>
#include <kernel/kmem.h>
#include <kernel/sched.h>

#define DMA_MAPPING_ACTIVE_FLAG (1U << 0)
#define DMA_MAPPING_IOMMU_FLAG  (1U << 1)
#define DMA_MAPPING_UNMAPPING_FLAG (1U << 2)
#define DMA_MAPPING_LIMIT       256U
#define DMA_IOVA_BASE           0x10000000ULL
#define DMA_IOVA_LIMIT          0x0000800000000000ULL

static spinlock_t g_dma_lock;
static atomic_uint g_dma_init_state;
static dma_mapping_t *g_dma_mappings[DMA_MAPPING_LIMIT];
static uint32_t g_dma_next_free_slot;
static uint64_t g_dma_next_iova = DMA_IOVA_BASE;

static void dma_lock(void) {
    /* 其他 CPU 自旋等待期间，确保注册表所有者仍可运行。 */
    sched_preempt_disable();
    spinlock_lock(&g_dma_lock);
}

static void dma_unlock(void) {
    spinlock_unlock(&g_dma_lock);
    sched_preempt_enable();
}

static void dma_initialize(void) {
    if (atomic_load_explicit(&g_dma_init_state, memory_order_acquire) == 2U) return;
    unsigned expected = 0U;
    if (atomic_compare_exchange_strong_explicit(&g_dma_init_state, &expected, 1U,
                                                memory_order_acq_rel,
                                                memory_order_acquire)) {
        spinlock_init(&g_dma_lock);
        for (uint32_t i = 0; i < DMA_MAPPING_LIMIT; ++i) g_dma_mappings[i] = 0;
        g_dma_next_free_slot = 0U;
        g_dma_next_iova = DMA_IOVA_BASE;
        atomic_store_explicit(&g_dma_init_state, 2U, memory_order_release);
        return;
    }
    while (atomic_load_explicit(&g_dma_init_state, memory_order_acquire) != 2U) {
        __asm__ volatile ("pause");
    }
}

static bool dma_range_overlaps(uint64_t start, uint64_t length,
                               const dma_mapping_t *mapping) {
    if (mapping == 0 || mapping->segment_count == 0) return false;
    if (length == 0U || start > UINT64_MAX - length) return true;
    uint64_t end = start + length;
    uint64_t other_start = mapping->segments[0].addr.value;
    if (mapping->mapped_length == 0U ||
        other_start > UINT64_MAX - mapping->mapped_length) return true;
    uint64_t other_end = other_start + mapping->mapped_length;
    return start < other_end && other_start < end;
}

static bool dma_choose_iova(uint64_t length, uint64_t *out) {
    if (out == 0 || length == 0 || length > DMA_IOVA_LIMIT - DMA_IOVA_BASE) {
        return false;
    }
    uint64_t candidate = (g_dma_next_iova + PAGE_SIZE - 1ULL) &
                         ~(PAGE_SIZE - 1ULL);
    for (uint32_t pass = 0; pass < DMA_MAPPING_LIMIT + 1U; ++pass) {
        bool restart = false;
        uint64_t scanned_end = 0U;
        for (uint32_t i = 0; i < DMA_MAPPING_LIMIT; ++i) {
            dma_mapping_t *mapping = g_dma_mappings[i];
            if (mapping == 0 || mapping->segment_count == 0U) continue;
            if (mapping->mapped_length == 0U ||
                mapping->segments[0].addr.value > UINT64_MAX - mapping->mapped_length) {
                return false;
            }
            uint64_t end = mapping->segments[0].addr.value + mapping->mapped_length;
            if (dma_range_overlaps(candidate, length, mapping)) {
                if (end > UINT64_MAX - (PAGE_SIZE - 1ULL)) return false;
                candidate = (end + PAGE_SIZE - 1ULL) & ~(PAGE_SIZE - 1ULL);
                /* 仅当新候选范围位于所有已扫描范围末端之后时，较早的范围才安全。否则立即重新开始扫描。 */
                if (scanned_end > candidate) {
                    restart = true;
                    break;
                }
            }
            if (end > scanned_end) scanned_end = end;
        }
        if (restart) continue;
        if (candidate <= DMA_IOVA_LIMIT && length <= DMA_IOVA_LIMIT - candidate) {
            *out = candidate;
            g_dma_next_iova = candidate + length;
            if (g_dma_next_iova >= DMA_IOVA_LIMIT) g_dma_next_iova = DMA_IOVA_BASE;
            return true;
        }
        candidate = DMA_IOVA_BASE;
        g_dma_next_iova = DMA_IOVA_BASE;
    }
    return false;
}

static int32_t dma_find_slot(void) {
    for (uint32_t i = g_dma_next_free_slot; i < DMA_MAPPING_LIMIT; ++i) {
        if (g_dma_mappings[i] == 0) {
            g_dma_next_free_slot = i;
            return (int32_t)i;
        }
    }
    g_dma_next_free_slot = DMA_MAPPING_LIMIT;
    return -1;
}

static void dma_publish_slot(uint32_t slot, dma_mapping_t *mapping) {
    g_dma_mappings[slot] = mapping;
    g_dma_next_free_slot = slot + 1U;
}

static void dma_clear_slot(uint32_t slot) {
    g_dma_mappings[slot] = 0;
    if (slot < g_dma_next_free_slot) g_dma_next_free_slot = slot;
}

static void dma_unpin_pages(page_t **pages, uint32_t page_count) {
    if (pages == 0) return;
    for (uint32_t i = 0; i < page_count; ++i) {
        if (pages[i] == 0) continue;
        atomic_fetch_sub_explicit(&pages[i]->refs, 1U, memory_order_release);
        uint64_t previous = __atomic_fetch_sub(&pages[i]->private_data, 1ULL,
                                               __ATOMIC_ACQ_REL);
        if (previous <= 1ULL) pages[i]->flags &= ~PAGE_PINNED;
    }
}

static bool dma_contains_range(const dma_mapping_t *mapping, uint64_t address,
                               uint64_t length) {
    if (mapping == 0 || mapping->segment_count == 0 || length == 0) return false;
    uint64_t cursor = address;
    uint64_t remaining = length;
    for (uint32_t i = 0; i < mapping->segment_count && remaining != 0; ++i) {
        uint64_t segment_start = mapping->segments[i].addr.value;
        uint64_t segment_length = mapping->segments[i].length;
        if (segment_length == 0 || segment_start > UINT64_MAX - segment_length) {
            return false;
        }
        uint64_t segment_end = segment_start + segment_length;
        if (cursor < segment_start || cursor >= segment_end) continue;
        uint64_t available = segment_end - cursor;
        if (available >= remaining) return true;
        remaining -= available;
        cursor = segment_end;
    }
    return remaining == 0;
}

static uint32_t iommu_access_for_direction(enum dma_direction direction) {
    if (direction == DMA_TO_DEVICE) return IOMMU_MAP_DEVICE_READ;
    if (direction == DMA_FROM_DEVICE) return IOMMU_MAP_DEVICE_WRITE;
    return IOMMU_MAP_DEVICE_READ | IOMMU_MAP_DEVICE_WRITE;
}

bool dma_mapping_active(const dma_mapping_t *mapping) {
    return mapping != 0 && (mapping->flags & DMA_MAPPING_ACTIVE_FLAG) != 0 &&
           (mapping->flags & DMA_MAPPING_UNMAPPING_FLAG) == 0;
}

kstatus_t dma_map_pages(struct device *dev, page_t **pages, uint32_t page_count,
                        enum dma_direction direction, dma_mapping_t *out) {
    dma_initialize();
    if (dev == 0 || pages == 0 || page_count == 0 || out == 0 ||
        direction > DMA_BIDIRECTIONAL || dma_mapping_active(out)) return K_EINVAL;

    /*
     * dma_mapping_t 会在映射存续期间保存页数组。调用者传入的数组可能位于
     * 栈上（NVMe/e1000 的单页映射就是这种情况），因此不能直接保存调用者
     * 的数组地址；映射必须拥有一份稳定的页指针副本。
     */
    *out = (dma_mapping_t){0};

    uint64_t mapped_length = (uint64_t)page_count * PAGE_SIZE;
    if (mapped_length == 0 || mapped_length > DMA_IOVA_LIMIT - DMA_IOVA_BASE) {
        return K_EINVAL;
    }
    uintptr_t highest_page_address = 0U;
    uint32_t segment_capacity = 0U;
    uint64_t previous_physical = 0U;
    for (uint32_t i = 0; i < page_count; ++i) {
        page_t *page = pages[i];
        if (page == 0 || (page->flags & PAGE_FREE) != 0 ||
            ((page->flags & PAGE_COMPOUND_HEAD) == 0 &&
             (page->flags & PAGE_COMPOUND_TAIL) == 0 && page->order != 0)) return K_EINVAL;
        uint64_t physical = page_to_phys(page).value;
        if (physical == UINT64_MAX) return K_EINVAL;
        if (i == 0U || physical != previous_physical + PAGE_SIZE) {
            ++segment_capacity;
        }
        previous_physical = physical;
        uintptr_t page_address = (uintptr_t)page;
        /* 新最大值不可能等于先前的指针。无序列表仍执行完整检查，不分配临时内存，也不重新排序。 */
        if (page_address > highest_page_address) {
            highest_page_address = page_address;
        } else {
            for (uint32_t j = 0; j < i; ++j) {
                if (pages[j] == page) return K_EINVAL;
            }
        }
    }

    for (uint32_t i = 0; i < page_count; ++i) {
        atomic_fetch_add_explicit(&pages[i]->refs, 1U, memory_order_acq_rel);
        __atomic_add_fetch(&pages[i]->private_data, 1ULL, __ATOMIC_ACQ_REL);
        pages[i]->flags |= PAGE_PINNED;
    }

    /* 发布映射前先设置所有页面指针和活动段字段；绝不暴露未使用的段容量。 */
    dma_segment_t *segments = (dma_segment_t *)kmalloc(
        (size_t)segment_capacity * sizeof(*segments), 0);
    if (segments == 0) {
        dma_unpin_pages(pages, page_count);
        return K_ENOMEM;
    }

    page_t **mapped_pages = (page_t **)kmalloc(
        (size_t)page_count * sizeof(*mapped_pages), 0);
    if (mapped_pages == 0) {
        kfree(segments);
        dma_unpin_pages(pages, page_count);
        return K_ENOMEM;
    }
    uint32_t physical_segment_count = 0;
    previous_physical = 0U;
    for (uint32_t i = 0; i < page_count; ++i) {
        uint64_t physical = page_to_phys(pages[i]).value;
        mapped_pages[i] = pages[i];
        if (physical_segment_count == 0 ||
            physical != previous_physical + PAGE_SIZE) {
            segments[physical_segment_count].addr = iova_make(physical);
            segments[physical_segment_count].length = PAGE_SIZE;
            ++physical_segment_count;
        } else {
            segments[physical_segment_count - 1U].length += PAGE_SIZE;
        }
        previous_physical = physical;
    }

    dma_mapping_t candidate = {0};
    candidate.device = dev;
    candidate.pages = mapped_pages;
    candidate.page_count = page_count;
    candidate.direction = (uint32_t)direction;
    candidate.segments = segments;
    candidate.segment_count = physical_segment_count;
    candidate.mapped_length = mapped_length;

    dma_lock();
    int32_t slot = dma_find_slot();
    bool use_iommu = iommu_hardware_enabled();
    uint64_t iova = 0;
    bool address_ok = slot >= 0;
    if (address_ok && use_iommu) address_ok = dma_choose_iova(mapped_length, &iova);
    if (address_ok && use_iommu) {
        segments[0].addr = iova_make(iova);
        segments[0].length = mapped_length;
        candidate.segment_count = 1U;
        candidate.flags = DMA_MAPPING_ACTIVE_FLAG | DMA_MAPPING_IOMMU_FLAG;
        *out = candidate;
        dma_publish_slot((uint32_t)slot, out);
        if (iommu_map_pages(dev, iova_make(iova), pages, page_count,
                            iommu_access_for_direction(direction)) != K_OK) {
            dma_clear_slot((uint32_t)slot);
            *out = (dma_mapping_t){0};
            address_ok = false;
        }
    } else if (address_ok) {
        candidate.flags = DMA_MAPPING_ACTIVE_FLAG;
        *out = candidate;
        dma_publish_slot((uint32_t)slot, out);
    }
    dma_unlock();

    if (!address_ok) {
        kfree(mapped_pages);
        kfree(segments);
        dma_unpin_pages(pages, page_count);
        return use_iommu ? K_EIO : K_ENOMEM;
    }
    object_get(dev);
    return K_OK;
}

/*
 * V3.10.6B9E-FIX2 DMA 映射所有权转移
 *
 * g_dma_mappings[] 保存 dma_mapping_t 对象地址。
 *
 * 因此活动映射不可复制。转移所有权时，必须在 g_dma_lock 下更新注册表指针，同时保留：
 *
 *   页面/固定引用
 *   段
 *   IOVA
 *   IOMMU 页表项
 *   设备引用
 *   方向
 *
 * 此处不会重新映射 DMA。
 */
kstatus_t dma_mapping_move(dma_mapping_t *destination, dma_mapping_t *source) {
    if (destination == 0 || source == 0) return K_EINVAL;
    if (destination == source) return K_OK;
    if (destination->device != 0 || destination->pages != 0 ||
        destination->page_count != 0U || destination->segments != 0 ||
        destination->segment_count != 0U || destination->mapped_length != 0U ||
        destination->flags != 0U) return K_EBUSY;

    dma_initialize();
    dma_lock();
    int32_t source_slot = -1;
    bool destination_registered = false;
    for (uint32_t i = 0U; i < DMA_MAPPING_LIMIT; ++i) {
        if (g_dma_mappings[i] == destination) destination_registered = true;
        if (g_dma_mappings[i] == source) source_slot = (int32_t)i;
    }

    bool active = (source->flags & DMA_MAPPING_ACTIVE_FLAG) != 0U;
    kstatus_t status = K_OK;
    if (destination_registered ||
        (active && (source->flags & DMA_MAPPING_UNMAPPING_FLAG) != 0U)) {
        status = K_EBUSY;
    } else if (active != (source_slot >= 0)) {
        /* 活动所有权必须有注册项；非活动值不得占用注册表槽位。 */
        status = K_EINVAL;
    } else {
        /* 仅迁移地址身份，不重新映射、不重复固定，也不改变设备引用。非活动聚合值同样按此方式复制。 */
        *destination = *source;
        if (active) g_dma_mappings[(uint32_t)source_slot] = destination;
        *source = (dma_mapping_t){0};
    }
    dma_unlock();
    return status;
}


kstatus_t dma_unmap_checked(dma_mapping_t *mapping) {
    if (mapping == 0 || (mapping->flags & DMA_MAPPING_ACTIVE_FLAG) == 0) {
        return K_OK;
    }
    dma_initialize();
    int32_t slot = -1;
    bool iommu_mapping = (mapping->flags & DMA_MAPPING_IOMMU_FLAG) != 0;
    dma_lock();
    for (uint32_t i = 0; i < DMA_MAPPING_LIMIT; ++i) {
        if (g_dma_mappings[i] == mapping) {
            if ((mapping->flags & DMA_MAPPING_UNMAPPING_FLAG) != 0) {
                dma_unlock();
                return K_EBUSY;
            }
            /* 解除期间仍保留登记，但访问校验会拒绝新的 DMA。 */
            mapping->flags |= DMA_MAPPING_UNMAPPING_FLAG;
            slot = (int32_t)i;
            break;
        }
    }
    dma_unlock();
    if (slot < 0) return K_EINVAL;

    if (iommu_mapping) {
        kstatus_t status = iommu_unmap_pages(mapping->device,
                                             mapping->segments[0].addr,
                                             mapping->mapped_length);
        /* 设备移除时 domain 可能已回收；此时旧 IOVA 不再可达。 */
        if (status != K_OK && status != K_ENOENT) {
        /* 失败时保留 pin 和登记，调用者可以稍后重试解除。 */
            dma_lock();
            mapping->flags &= ~DMA_MAPPING_UNMAPPING_FLAG;
            dma_unlock();
            return status;
        }
    }
    dma_lock();
    /* 在释放此注册槽位前，UNMAPPING 状态会阻止所有权转移。 */
    if (g_dma_mappings[slot] == mapping) dma_clear_slot((uint32_t)slot);
    dma_unlock();
    dma_unpin_pages(mapping->pages, mapping->page_count);
    object_put(mapping->device);
    kfree(mapping->pages);
    kfree(mapping->segments);
    mapping->device = 0;
    mapping->pages = 0;
    mapping->page_count = 0;
    mapping->direction = 0;
    mapping->segments = 0;
    mapping->segment_count = 0;
    mapping->mapped_length = 0;
    mapping->flags = 0;
    return K_OK;
}

void dma_unmap(dma_mapping_t *mapping) {
    (void)dma_unmap_checked(mapping);
}

void dma_sync_for_device(dma_mapping_t *mapping) {
    if (!dma_mapping_active(mapping)) return;
    atomic_thread_fence(memory_order_seq_cst);
    __asm__ volatile ("mfence" : : : "memory");
}

void dma_sync_for_cpu(dma_mapping_t *mapping) {
    if (!dma_mapping_active(mapping)) return;
    __asm__ volatile ("mfence" : : : "memory");
    atomic_thread_fence(memory_order_seq_cst);
}

void dma_wmb(void) {
    /*
     * DMA 映射同步解决缓存可见性，写屏障解决描述符字段与 MMIO 门铃
     * 的发布顺序。两者分开提供，驱动可以在最后一次 sync 后明确表达
     * “现在允许设备读取描述符”的时刻。
     */
    atomic_thread_fence(memory_order_release);
    __asm__ volatile ("sfence" : : : "memory");
}

kstatus_t dma_validate_access(struct device *dev, iova_t address,
                              uint64_t length, enum dma_device_access access) {
    if (dev == 0 || length == 0 ||
        (access != DMA_DEVICE_READ && access != DMA_DEVICE_WRITE) ||
        address.value > UINT64_MAX - length) return K_EINVAL;
    dma_initialize();
    dma_lock();
    kstatus_t status = K_EACCES;
    for (uint32_t i = 0; i < DMA_MAPPING_LIMIT; ++i) {
        dma_mapping_t *mapping = g_dma_mappings[i];
        if (mapping == 0 || mapping->device != dev || mapping->segment_count == 0) continue;
        if (!dma_contains_range(mapping, address.value, length)) continue;
        unsigned state = atomic_load_explicit(&dev->state, memory_order_acquire);
        if ((mapping->flags & DMA_MAPPING_UNMAPPING_FLAG) != 0) {
            status = K_EACCES;
        } else if (state >= DEVICE_REMOVING) {
            status = K_EDEVREMOVED;
        } else if (mapping->direction == DMA_BIDIRECTIONAL ||
                   (access == DMA_DEVICE_READ && mapping->direction == DMA_TO_DEVICE) ||
                   (access == DMA_DEVICE_WRITE && mapping->direction == DMA_FROM_DEVICE)) {
            status = K_OK;
        }
        break;
    }
    dma_unlock();
    return status;
}
