#include "nv_port.h"

#ifdef NV_PORT_HOST_TEST
#include <stdlib.h>
#ifdef _WIN32
#include <malloc.h>
#endif
#endif

static bool nv_port_power_of_two(size_t value) {
    return value != 0U && (value & (value - 1U)) == 0U;
}

static bool nv_port_get_order(size_t page_size, uint8_t *order_out) {
    uint64_t pages;
    uint8_t order = 0U;
    uint64_t block_pages = 1U;

    if (page_size < PAGE_SIZE || order_out == 0) return false;
    if ((uint64_t)page_size > UINT64_MAX - (PAGE_SIZE - 1ULL)) {
        return false;
    }
    pages = ((uint64_t)page_size + PAGE_SIZE - 1ULL) >> PAGE_SHIFT;
    while (block_pages < pages) {
        if (order >= BUDDY_MAX_ORDER) return false;
        ++order;
        block_pages <<= 1U;
    }
    *order_out = order;
    return true;
}

static bool nv_port_round_pages(size_t size, uint32_t *page_count) {
    uint64_t rounded;

    if (size == 0U || page_count == 0) return false;
    if ((uint64_t)size > UINT64_MAX - (PAGE_SIZE - 1ULL)) return false;
    rounded = ((uint64_t)size + PAGE_SIZE - 1ULL) >> PAGE_SHIFT;
    if (rounded == 0U || rounded > UINT32_MAX) return false;
    *page_count = (uint32_t)rounded;
    return true;
}

static bool nv_port_cache_type_valid(uint32_t cache_type) {
    return cache_type == NV_PORT_CACHE_WB ||
           cache_type == NV_PORT_CACHE_WC ||
           cache_type == NV_PORT_CACHE_UC;
}

static void nv_port_mem_clear(nv_port_mem_t *memory) {
    if (memory == 0) return;
    *memory = (nv_port_mem_t){0};
}

/*
 * System 会让 RAM 直接映射保持 WB。NVIDIA 的不可缓存和写合并分配还会通过 vmalloc 映射以所需
 * PAT 模式公开，因此设备开始对这些页面执行 DMA 时，直接映射别名不得保留脏数据或过期缓存行。
 * Linux 会在此转换期间修改页面映射类型；System 移植层无法在此处重写全局直接映射，因此要在
 * 所有权边界驱逐其缓存别名。
 */
static void nv_port_mem_invalidate_direct_aliases(
    const nv_port_mem_t *memory) {
#ifndef NV_PORT_HOST_TEST
    if (memory == 0 || memory->cache_type == NV_PORT_CACHE_WB ||
        memory->phys_pages == 0) return;

    for (uint32_t index = 0U; index < memory->page_count; ++index) {
        void *direct = phys_to_direct(
            paddr_make(memory->phys_pages[index]));
        if (direct != 0)
            nv_port_cache_invalidate(direct, PAGE_SIZE);
    }
#else
    (void)memory;
#endif
}

#ifndef NV_PORT_HOST_TEST
static page_alloc_flags_t nv_port_page_alloc_flags(uint32_t flags) {
    page_alloc_flags_t page_flags = 0U;

    if ((flags & NV_PORT_MEM_ZERO) != 0U) page_flags |= PAGE_ALLOC_ZERO;
    if ((flags & NV_PORT_MEM_DMA32) != 0U) page_flags |= PAGE_ALLOC_DMA32;
    if ((flags & NV_PORT_MEM_ATOMIC) != 0U) page_flags |= PAGE_ALLOC_ATOMIC;
    return page_flags;
}

static void nv_port_mem_release_pages(nv_port_mem_t *memory) {
    if (memory == 0 || !memory->owns_pages || memory->page_objects == 0) {
        return;
    }

    uint32_t block_pages = memory->allocation_block_pages;
    if (block_pages == 0U) block_pages = 1U;
    for (uint32_t index = 0U; index < memory->page_count;) {
        if (memory->page_objects[index] != 0) {
            page_free(memory->page_objects[index]);
        }
        if (block_pages > memory->page_count - index) break;
        index += block_pages;
    }
}

static bool nv_port_page_can_pin(const page_t *page) {
    if (page == 0 || (page->flags & PAGE_FREE) != 0U) return false;
    return page_to_phys(page).value != UINT64_MAX;
}
#else
typedef struct nv_port_host_memory {
    void *allocation;
    size_t allocation_size;
} nv_port_host_memory_t;

static atomic_uint_fast64_t g_nv_port_host_next_physical =
    ATOMIC_VAR_INIT(0x00100000ULL);

static void *nv_port_host_aligned_alloc(size_t alignment, size_t size) {
    if (size % alignment != 0U) {
        size += alignment - (size % alignment);
    }
#ifdef _WIN32
    return _aligned_malloc(size, alignment);
#else
    return aligned_alloc(alignment, size);
#endif
}

static uint64_t nv_port_host_physical_span(uint32_t page_count) {
    return (uint64_t)page_count * PAGE_SIZE;
}

static bool nv_port_host_assign_physical(nv_port_mem_t *memory,
                                         uint32_t flags) {
    uint64_t span;
    uint64_t base;

    if (memory == 0 || memory->page_count == 0U ||
        memory->phys_pages == 0) return false;
    span = nv_port_host_physical_span(memory->page_count);
    base = atomic_fetch_add_explicit(&g_nv_port_host_next_physical,
                                     span + PAGE_SIZE,
                                     memory_order_relaxed);
    if (base > UINT64_MAX - span) return false;
    if ((flags & NV_PORT_MEM_DMA32) != 0U &&
        (base >= (1ULL << 32) || span > (1ULL << 32) - base)) return false;
    for (uint32_t index = 0U; index < memory->page_count; ++index) {
        memory->phys_pages[index] = base + (uint64_t)index * PAGE_SIZE;
    }
    return true;
}

static void nv_port_host_release_backend(nv_port_mem_t *memory) {
    if (memory == 0 || memory->backend_data == 0) return;
    nv_port_host_memory_t *backend =
        (nv_port_host_memory_t *)memory->backend_data;
#ifdef _WIN32
    _aligned_free(backend->allocation);
#else
    free(backend->allocation);
#endif
    free(backend);
    memory->backend_data = 0;
}
#endif

static bool nv_port_mem_set_physical_pages(nv_port_mem_t *memory) {
#ifndef NV_PORT_HOST_TEST
    if (memory == 0 || memory->phys_pages == 0 ||
        memory->page_objects == 0) return false;
    for (uint32_t index = 0U; index < memory->page_count; ++index) {
        if (memory->page_objects[index] == 0) return false;
        paddr_t physical = page_to_phys(memory->page_objects[index]);
        if (physical.value == UINT64_MAX) return false;
        memory->phys_pages[index] = physical.value;
    }
#else
    (void)memory;
#endif
    return true;
}

static bool nv_port_mem_is_contiguous(const nv_port_mem_t *memory) {
    if (memory == 0 || memory->page_count == 0U ||
        memory->phys_pages == 0) return false;
    for (uint32_t index = 1U; index < memory->page_count; ++index) {
        if (memory->phys_pages[index - 1U] >
            UINT64_MAX - PAGE_SIZE ||
            memory->phys_pages[index] !=
                memory->phys_pages[index - 1U] + PAGE_SIZE) return false;
    }
    return true;
}

static kstatus_t nv_port_mem_alloc_internal(size_t size, size_t alignment,
                                             uint32_t flags,
                                             uint32_t cache_type,
                                             uint8_t requested_order,
                                             nv_port_mem_t *out) {
    uint32_t page_count;
    size_t normalized_alignment = alignment == 0U ? PAGE_SIZE : alignment;
    nv_port_mem_t memory = {0};

    if (out == 0 || size == 0U || !nv_port_round_pages(size, &page_count) ||
        !nv_port_power_of_two(normalized_alignment) ||
        normalized_alignment < PAGE_SIZE ||
        !nv_port_cache_type_valid(cache_type) ||
        (flags & ~(NV_PORT_MEM_ZERO | NV_PORT_MEM_CONTIGUOUS |
                   NV_PORT_MEM_DMA32 | NV_PORT_MEM_ATOMIC)) != 0U) {
        return K_EINVAL;
    }
    if ((uint64_t)page_count * PAGE_SIZE > UINT64_MAX) return K_EOVERFLOW;

    memory.size = size;
    memory.page_count = page_count;
    memory.page_shift = PAGE_SHIFT;
    memory.cache_type = cache_type;
    memory.owns_pages = true;
    memory.contiguous = (flags & NV_PORT_MEM_CONTIGUOUS) != 0U;
    memory.allocation_order = requested_order == UINT8_MAX ? 0U : requested_order;
    memory.allocation_block_pages = 1U << memory.allocation_order;

#ifdef NV_PORT_HOST_TEST
    nv_port_host_memory_t *backend =
        (nv_port_host_memory_t *)calloc(1U, sizeof(*backend));
    if (backend == 0) return K_ENOMEM;
    size_t storage_size = (size_t)page_count * PAGE_SIZE;
    backend->allocation = nv_port_host_aligned_alloc(normalized_alignment,
                                                      storage_size);
    backend->allocation_size = storage_size;
    if (backend->allocation == 0) {
        free(backend);
        return K_ENOMEM;
    }
    memory.backend_data = backend;
    memory.phys_pages = (uint64_t *)calloc(page_count,
                                           sizeof(*memory.phys_pages));
    if (memory.phys_pages == 0 ||
        !nv_port_host_assign_physical(&memory, flags)) {
        free(memory.phys_pages);
        nv_port_host_release_backend(&memory);
        return K_ENOMEM;
    }
    if ((flags & NV_PORT_MEM_ZERO) != 0U) {
        uint8_t *bytes = (uint8_t *)backend->allocation;
        for (size_t index = 0U; index < storage_size; ++index) bytes[index] = 0U;
    }
    memory.cpu_va = cache_type == NV_PORT_CACHE_WB ? backend->allocation : 0;
#else
    memory.phys_pages = (uint64_t *)kmalloc(
        (size_t)page_count * sizeof(*memory.phys_pages), 0U);
    memory.page_objects = (page_t **)kzalloc(
        (size_t)page_count * sizeof(*memory.page_objects), 0U);
    if (memory.phys_pages == 0 || memory.page_objects == 0) {
        kfree(memory.phys_pages);
        kfree(memory.page_objects);
        return K_ENOMEM;
    }
    page_alloc_flags_t page_flags = nv_port_page_alloc_flags(flags);
    if (memory.contiguous) {
        uint8_t order = 0U;
        uint64_t required_pages = page_count;
        uint64_t alignment_pages = normalized_alignment >> PAGE_SHIFT;
        uint64_t required = required_pages > alignment_pages ?
                            required_pages : alignment_pages;
        while ((1ULL << order) < required && order < BUDDY_MAX_ORDER) {
            ++order;
        }
        if ((1ULL << order) < required || order > BUDDY_MAX_ORDER) {
            kfree(memory.phys_pages);
            kfree(memory.page_objects);
            return K_EOVERFLOW;
        }
        page_t *head = page_alloc(order, page_flags);
        if (head == 0) {
            kfree(memory.phys_pages);
            kfree(memory.page_objects);
            return K_ENOMEM;
        }
        memory.page_objects[0] = head;
        for (uint32_t index = 1U; index < page_count; ++index) {
            memory.page_objects[index] = phys_to_page(
                paddr_make(page_to_phys(head).value +
                           (uint64_t)index * PAGE_SIZE));
            if (memory.page_objects[index] == 0) {
                page_free(head);
                kfree(memory.phys_pages);
                kfree(memory.page_objects);
                return K_EIO;
            }
        }
        memory.allocation_order = order;
        memory.allocation_block_pages = 1U << order;
    } else {
        uint8_t order = requested_order == UINT8_MAX ? 0U : requested_order;
        uint32_t block_pages = 1U << order;
        uint32_t block_count =
            (page_count - 1U) / block_pages + 1U;

        memory.allocation_order = order;
        memory.allocation_block_pages = block_pages;
        for (uint32_t block = 0U; block < block_count; ++block) {
            uint32_t base = block * block_pages;
            uint32_t remaining = page_count - base;
            uint32_t pages_in_block = remaining < block_pages ?
                                      remaining : block_pages;
            page_t *head = page_alloc(order, page_flags);
            if (head == 0) {
                nv_port_mem_release_pages(&memory);
                kfree(memory.phys_pages);
                kfree(memory.page_objects);
                return K_ENOMEM;
            }
            uint64_t head_physical = page_to_phys(head).value;
            for (uint32_t offset = 0U; offset < pages_in_block; ++offset) {
                memory.page_objects[base + offset] = phys_to_page(
                    paddr_make(head_physical + (uint64_t)offset * PAGE_SIZE));
                if (memory.page_objects[base + offset] == 0) {
                    nv_port_mem_release_pages(&memory);
                    kfree(memory.phys_pages);
                    kfree(memory.page_objects);
                    return K_EIO;
                }
            }
        }
    }
    if (!nv_port_mem_set_physical_pages(&memory)
#ifdef NV_PORT_HOST_TEST
        || !nv_port_host_assign_physical(&memory, 0U)
#endif
    ) {
        nv_port_mem_release_pages(&memory);
        kfree(memory.phys_pages);
        kfree(memory.page_objects);
        return K_EIO;
    }
    nv_port_mem_invalidate_direct_aliases(&memory);
    if (memory.contiguous && cache_type == NV_PORT_CACHE_WB) {
        memory.cpu_va = phys_to_direct(paddr_make(memory.phys_pages[0]));
    }
#endif

    *out = memory;
    return K_OK;
}

kstatus_t nv_port_mem_alloc(size_t size, size_t alignment, uint32_t flags,
                            uint32_t cache_type, nv_port_mem_t *out) {
    return nv_port_mem_alloc_internal(size, alignment, flags, cache_type,
                                      UINT8_MAX, out);
}

kstatus_t nv_port_mem_alloc_ordered(size_t size, size_t page_size,
                                    size_t alignment, uint32_t flags,
                                    uint32_t cache_type, nv_port_mem_t *out) {
    uint8_t order;
    if (!nv_port_get_order(page_size, &order)) return K_EINVAL;
    return nv_port_mem_alloc_internal(size, alignment, flags, cache_type,
                                      order, out);
}

kstatus_t nv_port_mem_wrap_pages(page_t **pages, uint32_t page_count,
                                 size_t size, uint32_t cache_type,
                                 nv_port_mem_t *out) {
    nv_port_mem_t memory = {0};

    if (out == 0 || pages == 0 || page_count == 0U || size == 0U ||
        (uint64_t)size > (uint64_t)page_count * PAGE_SIZE ||
        !nv_port_cache_type_valid(cache_type)) return K_EINVAL;

#ifdef NV_PORT_HOST_TEST
    memory.phys_pages = (uint64_t *)calloc(
        (size_t)page_count, sizeof(*memory.phys_pages));
    memory.page_objects = (page_t **)calloc(
        (size_t)page_count, sizeof(*memory.page_objects));
#else
    memory.phys_pages = (uint64_t *)kmalloc(
        (size_t)page_count * sizeof(*memory.phys_pages), 0U);
    memory.page_objects = (page_t **)kmalloc(
        (size_t)page_count * sizeof(*memory.page_objects), 0U);
#endif
    if (memory.phys_pages == 0 || memory.page_objects == 0) {
#ifdef NV_PORT_HOST_TEST
        free(memory.phys_pages);
        free(memory.page_objects);
#else
        kfree(memory.phys_pages);
        kfree(memory.page_objects);
#endif
        return K_ENOMEM;
    }
    for (uint32_t index = 0U; index < page_count; ++index) {
        memory.page_objects[index] = pages[index];
    }
    memory.size = size;
    memory.page_count = page_count;
    memory.page_shift = PAGE_SHIFT;
    memory.cache_type = cache_type;
    memory.allocation_order = 0U;
    memory.allocation_block_pages = 1U;
    memory.owns_pages = false;
    if (!nv_port_mem_set_physical_pages(&memory)
#ifdef NV_PORT_HOST_TEST
        || !nv_port_host_assign_physical(&memory, 0U)
#endif
    ) {
#ifdef NV_PORT_HOST_TEST
        free(memory.phys_pages);
        free(memory.page_objects);
#else
        kfree(memory.phys_pages);
        kfree(memory.page_objects);
#endif
        return K_EINVAL;
    }
    nv_port_mem_invalidate_direct_aliases(&memory);
    memory.contiguous = nv_port_mem_is_contiguous(&memory);
#ifndef NV_PORT_HOST_TEST
    if (memory.contiguous && cache_type == NV_PORT_CACHE_WB) {
        memory.cpu_va = phys_to_direct(paddr_make(memory.phys_pages[0]));
    }
#endif
    *out = memory;
    return K_OK;
}

kstatus_t nv_port_mem_pin(nv_port_mem_t *memory) {
    if (memory == 0 || memory->page_count == 0U) return K_EINVAL;
#ifndef NV_PORT_HOST_TEST
    if (memory->page_objects == 0) return K_EINVAL;
#endif
    if (memory->pin_count == UINT32_MAX) return K_EOVERFLOW;
#ifndef NV_PORT_HOST_TEST
    for (uint32_t index = 0U; index < memory->page_count; ++index) {
        if (!nv_port_page_can_pin(memory->page_objects[index])) return K_EINVAL;
    }
    for (uint32_t index = 0U; index < memory->page_count; ++index) {
        page_t *page = memory->page_objects[index];
        atomic_fetch_add_explicit(&page->refs, 1U, memory_order_acq_rel);
        __atomic_add_fetch(&page->private_data, 1ULL, __ATOMIC_ACQ_REL);
        page->flags |= PAGE_PINNED;
    }
#endif
    ++memory->pin_count;
    return K_OK;
}

kstatus_t nv_port_mem_unpin(nv_port_mem_t *memory) {
    if (memory == 0 || memory->pin_count == 0U) return K_EINVAL;
#ifndef NV_PORT_HOST_TEST
    if (memory->page_objects == 0) return K_EINVAL;
    for (uint32_t index = 0U; index < memory->page_count; ++index) {
        page_t *page = memory->page_objects[index];
        if (page == 0) return K_EINVAL;
        atomic_fetch_sub_explicit(&page->refs, 1U, memory_order_release);
        uint64_t previous = __atomic_fetch_sub(&page->private_data, 1ULL,
                                               __ATOMIC_ACQ_REL);
        if (previous <= 1ULL) page->flags &= ~PAGE_PINNED;
    }
#endif
    --memory->pin_count;
    return K_OK;
}

void *nv_port_mem_cpu_ptr(const nv_port_mem_t *memory, uint64_t offset,
                          size_t length) {
    if (memory == 0 || memory->cpu_va == 0 || offset > memory->size ||
        (uint64_t)length > memory->size - offset) return 0;
    return (uint8_t *)memory->cpu_va + offset;
}

void *nv_port_mem_page_cpu_ptr(const nv_port_mem_t *memory, uint32_t index) {
    if (memory == 0 || index >= memory->page_count ||
        memory->cache_type != NV_PORT_CACHE_WB) return 0;
#ifdef NV_PORT_HOST_TEST
    if (memory->backend_data == 0) return 0;
    nv_port_host_memory_t *backend =
        (nv_port_host_memory_t *)memory->backend_data;
    return (uint8_t *)backend->allocation + (size_t)index * PAGE_SIZE;
#else
    if (memory->phys_pages == 0) return 0;
    return phys_to_direct(paddr_make(memory->phys_pages[index]));
#endif
}

void nv_port_mem_free(nv_port_mem_t *memory) {
    if (memory == 0 || memory->pin_count != 0U) return;
#ifndef NV_PORT_HOST_TEST
    nv_port_mem_release_pages(memory);
    kfree(memory->phys_pages);
    kfree(memory->page_objects);
#else
    free(memory->phys_pages);
    free(memory->page_objects);
    nv_port_host_release_backend(memory);
#endif
    nv_port_mem_clear(memory);
}

bool nv_port_memory_self_test(void) {
    nv_port_mem_t memory;
    nv_port_mem_t ordered = {0};
    uint8_t *bytes;

    if (nv_port_mem_alloc(PAGE_SIZE + 17U, PAGE_SIZE,
                          NV_PORT_MEM_ZERO | NV_PORT_MEM_CONTIGUOUS,
                          NV_PORT_CACHE_WB, &memory) != K_OK) return false;
    if (!memory.contiguous || memory.page_count != 2U ||
        memory.phys_pages[1] != memory.phys_pages[0] + PAGE_SIZE ||
        nv_port_mem_cpu_ptr(&memory, 0U, memory.size) == 0 ||
        nv_port_mem_page_cpu_ptr(&memory, 1U) == 0) {
        nv_port_mem_free(&memory);
        return false;
    }
    bytes = (uint8_t *)memory.cpu_va;
    if (bytes[0] != 0U || bytes[PAGE_SIZE] != 0U ||
        nv_port_mem_pin(&memory) != K_OK || nv_port_mem_unpin(&memory) != K_OK) {
        nv_port_mem_free(&memory);
        return false;
    }
    nv_port_mem_free(&memory);
    if (memory.cpu_va != 0 || memory.phys_pages != 0) return false;

    if (nv_port_mem_alloc_ordered(PAGE_SIZE * 3U, PAGE_SIZE * 2U,
                                  PAGE_SIZE, NV_PORT_MEM_ZERO,
                                  NV_PORT_CACHE_WB, &ordered) != K_OK ||
        ordered.page_count != 3U || ordered.allocation_order != 1U ||
        ordered.allocation_block_pages != 2U ||
        nv_port_mem_page_cpu_ptr(&ordered, 2U) == 0) {
        nv_port_mem_free(&ordered);
        return false;
    }
    nv_port_mem_free(&ordered);
    return ordered.cpu_va == 0 && ordered.phys_pages == 0;
}
