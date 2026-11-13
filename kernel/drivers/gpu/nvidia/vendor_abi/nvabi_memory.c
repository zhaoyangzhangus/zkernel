#include "nvabi_internal.h"

#include <arch/x86_64/paging.h>
#include <kernel/mm.h>

static atomic_bool g_nvabi_large_mapping_trace = ATOMIC_VAR_INIT(false);

NV_STATUS NV_API_CALL os_lock_user_pages(void *address, NvU64 page_count,
                                         void **page_array, NvU32 flags) {
    if (page_array == NULL) return NV_ERR_INVALID_ARGUMENT;
    *page_array = NULL;
    uint64_t start = (uint64_t)(uintptr_t)address;
    if (page_count == 0U || page_count > UINT32_MAX ||
        page_count > SIZE_MAX / sizeof(page_t *) ||
        page_count > SIZE_MAX / PAGE_SIZE ||
        (start & NVABI_PAGE_MASK) != 0U ||
        !nv_port_user_range_valid(address, (size_t)page_count * PAGE_SIZE))
        return NV_ERR_INVALID_ARGUMENT;
    thread_t *thread = sched_current_thread();
    if (thread == NULL || thread->process == NULL ||
        thread->process->vm == NULL) return NV_ERR_NOT_SUPPORTED;
    page_t **pages = kmalloc((size_t)page_count * sizeof(*pages), 0U);
    if (pages == NULL) return NV_ERR_NO_MEMORY;
    bool write = FLD_TEST_DRF(_LOCK_USER_PAGES, _FLAGS, _WRITE, _YES, flags);
    for (NvU64 i = 0; i < page_count; ++i) {
        kstatus_t status = vm_pin_user_page(thread->process->vm,
            (vaddr_t)(start + i * PAGE_SIZE), write, &pages[i]);
        if (status != K_OK) {
            while (i != 0U) vm_unpin_user_page(pages[--i]);
            kfree(pages);
            return nvabi_status_from_kstatus(status);
        }
    }
    *page_array = pages;
    return NV_OK;
}

NV_STATUS NV_API_CALL os_unlock_user_pages(NvU64 page_count, void *page_array,
                                           NvU32 flags) {
    (void)flags;
    if (page_array == NULL || page_count == 0U || page_count > UINT32_MAX)
        return NV_ERR_INVALID_ARGUMENT;
    page_t **pages = page_array;
    for (NvU64 i = 0; i < page_count; ++i) vm_unpin_user_page(pages[i]);
    kfree(pages);
    return NV_OK;
}

NV_STATUS NV_API_CALL nv_register_user_pages(nv_state_t *nv, NvU64 page_count,
    NvU64 *physical, void *import_private, void **private_data,
    NvBool unencrypted) {
    (void)nv;
    if (physical == NULL || private_data == NULL || *private_data == NULL ||
        page_count == 0U || page_count > UINT32_MAX ||
        page_count > SIZE_MAX / PAGE_SIZE) return NV_ERR_INVALID_ARGUMENT;
    if (unencrypted) return NV_ERR_NOT_SUPPORTED;
    nvabi_page_allocation_t *allocation = kzalloc(sizeof(*allocation), 0U);
    if (allocation == NULL) return NV_ERR_NO_MEMORY;
    /* 导入的普通 RAM 保持 WB；绝不将合成器共享缓冲区的 CPU 别名失效或改为 UC。
     * 页面由 pin 引用持有。 */
    kstatus_t status = nv_port_mem_wrap_pages(*private_data,
        (uint32_t)page_count, (size_t)page_count * PAGE_SIZE,
        NV_PORT_CACHE_WB, &allocation->memory);
    if (status != K_OK) {
        kfree(allocation);
        return nvabi_status_from_kstatus(status);
    }
    allocation->magic = NVABI_PAGE_ALLOCATION_MAGIC;
    allocation->page_count = (uint32_t)page_count;
    allocation->cache_type = NV_MEMORY_CACHED;
    allocation->contiguous = allocation->memory.contiguous ? NV_TRUE : NV_FALSE;
    allocation->imported_user_pages = *private_data;
    allocation->import_private = import_private;
    for (NvU64 i = 0; i < page_count; ++i)
        physical[i] = allocation->memory.phys_pages[i];
    *private_data = allocation;
    return NV_OK;
}

void NV_API_CALL nv_unregister_user_pages(nv_state_t *nv, NvU64 page_count,
    void **import_private, void **private_data) {
    (void)nv;
    nvabi_page_allocation_t *allocation =
        private_data != NULL ? *private_data : NULL;
    if (allocation == NULL || allocation->magic != NVABI_PAGE_ALLOCATION_MAGIC ||
        allocation->page_count != page_count ||
        allocation->imported_user_pages == NULL) {
        nvabi_log_invalid("nv_unregister_user_pages", "nv-memory");
        return;
    }
    *private_data = allocation->imported_user_pages;
    if (import_private != NULL) *import_private = allocation->import_private;
    allocation->magic = 0U;
    nv_port_mem_free(&allocation->memory);
    kfree(allocation);
}

static uint32_t nvabi_cache_type(NvU32 cache_type, bool *valid) {
    if (valid != NULL) *valid = true;
    switch (cache_type) {
    case NV_MEMORY_CACHED:
        return NV_PORT_CACHE_WB;
    case NV_MEMORY_WRITECOMBINED:
        return NV_PORT_CACHE_WC;
    case NV_MEMORY_UNCACHED:
    case NV_MEMORY_UNCACHED_WEAK:
        return NV_PORT_CACHE_UC;
    default:
        if (valid != NULL) *valid = false;
        return NV_PORT_CACHE_UC;
    }
}

static bool nvabi_page_allocation_valid(const nvabi_page_allocation_t *allocation,
                                        const char *function) {
    if (allocation == NULL || allocation->magic !=
        NVABI_PAGE_ALLOCATION_MAGIC || allocation->page_count == 0U ||
        allocation->memory.page_count != allocation->page_count ||
        allocation->memory.phys_pages == NULL ||
        allocation->memory.page_objects == NULL) {
        nvabi_log_invalid(function, "nv-memory");
        return false;
    }
    return true;
}

NV_STATUS NV_API_CALL nvabi_get_system_mapping_pages(
    void *pAllocPrivate, NvU64 pageIndex, NvU64 pageCount,
    const uint64_t **pages, NvU32 *cache_mode) {
    nvabi_page_allocation_t *allocation =
        (nvabi_page_allocation_t *)pAllocPrivate;
    bool valid_cache;

    if (pages == NULL || cache_mode == NULL || pageCount == 0U ||
        !nvabi_page_allocation_valid(allocation,
                                     "nvabi_get_system_mapping_pages") ||
        pageIndex > allocation->page_count ||
        pageCount > (NvU64)allocation->page_count - pageIndex) {
        return NV_ERR_INVALID_ARGUMENT;
    }
    *cache_mode = nvabi_cache_type(allocation->cache_type, &valid_cache);
    if (!valid_cache) {
        nvabi_log_invalid("nvabi_get_system_mapping_pages",
                          "nv-memory-cache");
        return NV_ERR_INVALID_ARGUMENT;
    }
    *pages = &allocation->memory.phys_pages[(uint32_t)pageIndex];
    return NV_OK;
}

NV_STATUS NV_API_CALL nv_alloc_pages(
    nv_state_t *nv,
    NvU32 page_count,
    NvU64 page_size,
    NvBool contiguous,
    NvU32 cache_type,
    NvBool zeroed,
    NvBool unencrypted,
    NvS32 node_id,
    NvU64 *pte_array,
    void **priv_data) {
    bool valid_cache;
    uint32_t port_cache;
    uint32_t flags = 0U;
    uint64_t size;
    nvabi_page_allocation_t *allocation;
    kstatus_t status;

    (void)nv;
    if (page_count == 0U || page_size < NVABI_PAGE_SIZE ||
        pte_array == NULL || priv_data == NULL || unencrypted ||
        node_id >= 0) {
        kernel_serial_printf_serial_only(
            "OS_NVIDIA_NV_ALLOC_INVALID pages=%u page_size=%llx contig=%u cache=%u zero=%u unencrypted=%u node=%d pte=%llx priv=%llx\r\n",
            (unsigned)page_count, (unsigned long long)page_size,
            contiguous != NV_FALSE ? 1U : 0U,
            (unsigned)cache_type, zeroed != NV_FALSE ? 1U : 0U,
            unencrypted != NV_FALSE ? 1U : 0U, (int)node_id,
            (unsigned long long)(uintptr_t)pte_array,
            (unsigned long long)(uintptr_t)priv_data);
        return nvabi_invalid_status("nv_alloc_pages", "nv-memory");
    }
    if ((uint64_t)page_count > UINT64_MAX / page_size) {
        return NV_ERR_INVALID_ARGUMENT;
    }
    port_cache = nvabi_cache_type(cache_type, &valid_cache);
    if (!valid_cache) return nvabi_invalid_status(
        "nv_alloc_pages", "nv-memory-cache");
    if (contiguous) flags |= NV_PORT_MEM_CONTIGUOUS;
    if (zeroed) flags |= NV_PORT_MEM_ZERO;
    allocation = (nvabi_page_allocation_t *)kzalloc(sizeof(*allocation), 0U);
    if (allocation == NULL) return NV_ERR_NO_MEMORY;
    size = (uint64_t)page_count * NVABI_PAGE_SIZE;
    if (size > (uint64_t)SIZE_MAX) {
        kfree(allocation);
        return NV_ERR_INVALID_ARGUMENT;
    }
    status = nv_port_mem_alloc_ordered((size_t)size, (size_t)page_size,
                                       PAGE_SIZE, flags, port_cache,
                                       &allocation->memory);
    if (status != K_OK) {
        kfree(allocation);
        return nvabi_status_from_kstatus(status);
    }
    allocation->magic = NVABI_PAGE_ALLOCATION_MAGIC;
    allocation->page_count = page_count;
    allocation->cache_type = cache_type;
    allocation->contiguous = contiguous;
    if (contiguous) {
        pte_array[0] = allocation->memory.phys_pages[0];
    } else {
        for (NvU32 index = 0U; index < page_count; ++index) {
            pte_array[index] = allocation->memory.phys_pages[index];
        }
    }
    *priv_data = allocation;
    kernel_serial_printf_serial_only(
        "OS_NVIDIA_NV_ALLOC_OK pages=%u page_size=%llx phys0=%llx\r\n",
        (unsigned)page_count, (unsigned long long)page_size,
        (unsigned long long)allocation->memory.phys_pages[0]);
    return NV_OK;
}

NV_STATUS NV_API_CALL nv_free_pages(
    nv_state_t *nv,
    NvU32 page_count,
    NvBool contiguous,
    NvU32 cache_type,
    void *priv_data) {
    nvabi_page_allocation_t *allocation =
        (nvabi_page_allocation_t *)priv_data;
    (void)nv;
    if (!nvabi_page_allocation_valid(allocation, "nv_free_pages") ||
        allocation->page_count != page_count ||
        allocation->contiguous != contiguous ||
        allocation->cache_type != cache_type) {
        nvabi_log_invalid("nv_free_pages", "nv-memory");
        return NV_ERR_INVALID_ARGUMENT;
    }
    allocation->magic = 0U;
    nv_port_mem_free(&allocation->memory);
    kfree(allocation);
    return NV_OK;
}

NV_STATUS NV_API_CALL nv_get_num_phys_pages(void *memory, NvU32 *count) {
    nvabi_page_allocation_t *allocation =
        (nvabi_page_allocation_t *)memory;
    if (count == NULL) return nvabi_invalid_status(
        "nv_get_num_phys_pages", "nv-memory");
    if (!nvabi_page_allocation_valid(allocation, "nv_get_num_phys_pages")) {
        return NV_ERR_INVALID_ARGUMENT;
    }
    *count = allocation->page_count;
    return NV_OK;
}

NV_STATUS NV_API_CALL nv_get_phys_pages(void *memory, void *pages,
                                        NvU32 *count) {
    nvabi_page_allocation_t *allocation =
        (nvabi_page_allocation_t *)memory;
    NvU32 capacity;
    if (count == NULL || pages == NULL) return nvabi_invalid_status(
        "nv_get_phys_pages", "nv-memory");
    if (!nvabi_page_allocation_valid(allocation, "nv_get_phys_pages")) {
        return NV_ERR_INVALID_ARGUMENT;
    }
    capacity = *count;
    if (capacity < allocation->page_count) return NV_ERR_INSUFFICIENT_RESOURCES;
    NvU64 *output = (NvU64 *)pages;
    for (NvU32 index = 0U; index < allocation->page_count; ++index) {
        output[index] = allocation->memory.phys_pages[index];
    }
    *count = allocation->page_count;
    return NV_OK;
}

NvU64 NV_API_CALL nv_get_kern_phys_address(NvU64 address) {
    paddr_t physical = direct_to_phys((const void *)(uintptr_t)address);
    return physical.value;
}

void *NV_API_CALL nv_alloc_kernel_mapping(
    nv_state_t *nv,
    void *pAllocPrivate,
    NvU64 pageIndex,
    NvU32 pageOffset,
    NvU64 size,
    void **pPrivate) {
    nvabi_page_allocation_t *allocation =
        (nvabi_page_allocation_t *)pAllocPrivate;
    uint64_t span;
    uint64_t map_pages;
    bool valid_cache;
    uint32_t port_cache;
    void *mapped = NULL;

    (void)nv;
    if (pPrivate == NULL) {
        nvabi_log_invalid("nv_alloc_kernel_mapping", "nv-memory-map");
        return NULL;
    }
    *pPrivate = NULL;
    if (!nvabi_page_allocation_valid(allocation, "nv_alloc_kernel_mapping") ||
        pageOffset >= NVABI_PAGE_SIZE ||
        pageIndex >= allocation->page_count ||
        size > UINT64_MAX - (uint64_t)pageOffset) {
        nvabi_log_invalid("nv_alloc_kernel_mapping", "nv-memory-map");
        return NULL;
    }

    port_cache = nvabi_cache_type(allocation->cache_type, &valid_cache);
    if (!valid_cache) {
        nvabi_log_invalid("nv_alloc_kernel_mapping", "nv-memory-cache");
        return NULL;
    }

    span = size + (uint64_t)pageOffset;
    map_pages = (span >> PAGE_SHIFT) +
                ((span & (PAGE_SIZE - 1ULL)) != 0U ? 1ULL : 0ULL);
    /* System 的直接映射采用 WB 属性，因此只适用于 WB 分配。即使厂商 UC/WC 分配范围
     * 只有一个页面，也必须使用具有所需缓存属性的映射。 */
    if (span <= NVABI_PAGE_SIZE && port_cache == NV_PORT_CACHE_WB) {
        void *direct = phys_to_direct(paddr_make(
            allocation->memory.phys_pages[(uint32_t)pageIndex]));
        if (direct == NULL) {
            nvabi_log_invalid("nv_alloc_kernel_mapping", "nv-memory-direct");
            return NULL;
        }
        return (void *)((uint8_t *)direct + pageOffset);
    }
    if (map_pages == 0U || map_pages > UINT32_MAX ||
        map_pages > (uint64_t)allocation->page_count - pageIndex) {
        nvabi_log_invalid("nv_alloc_kernel_mapping", "nv-memory-map");
        return NULL;
    }
    if (nv_port_mem_map_kernel_pages(
            &allocation->memory, (uint32_t)pageIndex, (uint32_t)map_pages,
            port_cache, false, &mapped) != K_OK || mapped == NULL) {
        nvabi_log_invalid("nv_alloc_kernel_mapping", "nv-memory-map");
        return NULL;
    }
    if (map_pages >= 256U &&
        !atomic_exchange_explicit(&g_nvabi_large_mapping_trace, true,
                                  memory_order_acq_rel)) {
        paddr_t translated = paddr_make(0U);
        uint64_t entry_flags = 0U;
        kstatus_t translate_status = x86_translate_page(
            x86_current_root_table(), (vaddr_t)(uintptr_t)mapped,
            &translated, &entry_flags);
        kernel_serial_printf_serial_only(
            "OS_NVIDIA_NV_MAP_TRACE cache=%u port_cache=%u "
            "page_index=%llx page_offset=%x size=%llx pages=%llx "
            "va=%llx translate=%d phys=%llx pte=%llx\r\n",
            (unsigned)allocation->cache_type, (unsigned)port_cache,
            (unsigned long long)pageIndex, (unsigned)pageOffset,
            (unsigned long long)size, (unsigned long long)map_pages,
            (unsigned long long)(uintptr_t)mapped, (int)translate_status,
            (unsigned long long)translated.value,
            (unsigned long long)entry_flags);
    }
    *pPrivate = (void *)(uintptr_t)map_pages;
    return (void *)((uint8_t *)mapped + pageOffset);
}

void NV_API_CALL nv_free_kernel_mapping(
    nv_state_t *nv,
    void *pAllocPrivate,
    void *address,
    void *pPrivate) {
    uintptr_t page_count;
    void *base;

    (void)nv;
    (void)pAllocPrivate;
    if (pPrivate == NULL) return;
    if (address == NULL) {
        nvabi_log_invalid("nv_free_kernel_mapping", "nv-memory-map");
        return;
    }
    page_count = (uintptr_t)pPrivate;
    if (page_count == 0U || page_count > UINT32_MAX) {
        nvabi_log_invalid("nv_free_kernel_mapping", "nv-memory-map");
        return;
    }
    base = (void *)((uintptr_t)address & ~(uintptr_t)NVABI_PAGE_MASK);
    nv_port_mem_unmap_kernel_pages(base, (uint32_t)page_count);
}

NV_STATUS NV_API_CALL nv_alloc_user_mapping(
    nv_state_t *nv,
    void *pAllocPrivate,
    NvU64 pageIndex,
    NvU32 pageOffset,
    NvU64 size,
    NvU32 protect,
    NvU64 *pUserAddress,
    void **ppPrivate) {
    nvabi_page_allocation_t *allocation =
        (nvabi_page_allocation_t *)pAllocPrivate;
    uint64_t byte_offset;
    uint64_t span;
    uint64_t physical;

    (void)nv;
    (void)protect;
    if (pUserAddress == NULL || ppPrivate == NULL ||
        !nvabi_page_allocation_valid(allocation, "nv_alloc_user_mapping") ||
        pageOffset >= NVABI_PAGE_SIZE ||
        pageIndex >= allocation->page_count ||
        pageIndex > UINT64_MAX / NVABI_PAGE_SIZE ||
        size > UINT64_MAX - (uint64_t)pageOffset) {
        nvabi_log_invalid("nv_alloc_user_mapping", "nv-memory-map");
        return NV_ERR_INVALID_ARGUMENT;
    }

    byte_offset = pageIndex * NVABI_PAGE_SIZE + (uint64_t)pageOffset;
    span = size + (uint64_t)pageOffset;
    if (byte_offset > allocation->memory.size ||
        span > allocation->memory.size -
            pageIndex * NVABI_PAGE_SIZE) {
        nvabi_log_invalid("nv_alloc_user_mapping", "nv-memory-map");
        return NV_ERR_INVALID_ARGUMENT;
    }

    /* 不可变的 595.84 nv-usermap.c 会在此处返回经过验证的物理地址。后续 mmap 路径会
     * 使用同一分配描述符，并将其页面映射到调用方地址空间。此适配器不会生成 System
     * 虚拟地址、VRAM 分配器、GPU VA、GMMU 状态或 GSP 事务。 */
    if (allocation->contiguous) {
        if (allocation->memory.phys_pages[0] > UINT64_MAX -
            pageIndex * NVABI_PAGE_SIZE) {
            nvabi_log_invalid("nv_alloc_user_mapping", "nv-memory-map");
            return NV_ERR_INVALID_ARGUMENT;
        }
        physical = allocation->memory.phys_pages[0] +
                   pageIndex * NVABI_PAGE_SIZE;
    } else {
        physical = allocation->memory.phys_pages[(uint32_t)pageIndex];
    }
    if (physical > UINT64_MAX - (uint64_t)pageOffset) {
        nvabi_log_invalid("nv_alloc_user_mapping", "nv-memory-map");
        return NV_ERR_INVALID_ARGUMENT;
    }
    *pUserAddress = physical + (uint64_t)pageOffset;
    *ppPrivate = NULL;
    return NV_OK;
}

void NV_API_CALL nv_free_user_mapping(
    nv_state_t *nv,
    void *pAllocPrivate,
    NvU64 userAddress,
    void *pPrivate) {
    /* 595.84 的用户映射由分配描述符表示，并随 RM 内存对象一起释放；nv-usermap.c
     * 中没有需要单独拆除的逐映射资源。 */
    (void)nv;
    (void)pAllocPrivate;
    (void)userAddress;
    (void)pPrivate;
}
