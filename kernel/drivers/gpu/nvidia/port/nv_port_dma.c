#include "nv_port.h"

#ifndef NV_PORT_HOST_TEST
#include <kernel/kmem.h>
#else
#include <stdlib.h>
#endif

static bool nv_port_dma_direction_valid(uint32_t direction) {
    return direction == DMA_TO_DEVICE || direction == DMA_FROM_DEVICE ||
           direction == DMA_BIDIRECTIONAL;
}

static bool nv_port_dma_page_valid(uint64_t physical) {
    return (physical & (PAGE_SIZE - 1ULL)) == 0U;
}

kstatus_t nv_port_dma_build_segments(const uint64_t *phys_pages,
                                     uint32_t page_count,
                                     nv_port_dma_segment_t *segments,
                                     uint32_t segment_capacity,
                                     uint32_t *segment_count) {
    uint32_t count = 0U;

    if (phys_pages == 0 || page_count == 0U || segments == 0 ||
        segment_capacity == 0U || segment_count == 0) return K_EINVAL;
    for (uint32_t index = 0U; index < page_count; ++index) {
        uint64_t physical = phys_pages[index];
        if (!nv_port_dma_page_valid(physical) ||
            physical > UINT64_MAX - PAGE_SIZE) return K_EINVAL;
        if (count == 0U ||
            phys_pages[index - 1U] > UINT64_MAX - PAGE_SIZE ||
            physical != phys_pages[index - 1U] + PAGE_SIZE) {
            if (count == segment_capacity) return K_ENOSPC;
            segments[count].dma_addr = physical;
            segments[count].phys_addr = physical;
            segments[count].length = PAGE_SIZE;
            ++count;
        } else {
            if (segments[count - 1U].length > UINT64_MAX - PAGE_SIZE) {
                return K_EOVERFLOW;
            }
            segments[count - 1U].length += PAGE_SIZE;
        }
    }
    *segment_count = count;
    return K_OK;
}

static void nv_port_dma_release_segments(nv_port_dma_mapping_t *mapping) {
    if (mapping == 0 || mapping->segments == 0) return;
#ifdef NV_PORT_HOST_TEST
    free(mapping->segments);
#else
    kfree(mapping->segments);
#endif
    mapping->segments = 0;
    mapping->segment_count = 0U;
}

static kstatus_t nv_port_dma_alloc_segments(uint32_t count,
                                             nv_port_dma_segment_t **out) {
    if (out == 0 || count == 0U) return K_EINVAL;
#ifdef NV_PORT_HOST_TEST
    *out = (nv_port_dma_segment_t *)calloc(count, sizeof(**out));
#else
    *out = (nv_port_dma_segment_t *)kzalloc(
        (size_t)count * sizeof(**out), 0U);
#endif
    return *out == 0 ? K_ENOMEM : K_OK;
}

#ifdef NV_PORT_HOST_TEST
static kstatus_t nv_port_dma_map_host_physical(
    const uint64_t *phys_pages, uint32_t page_count, uint32_t direction,
    bool coherent, nv_port_dma_mapping_t *out) {
    kstatus_t status;
    uint32_t segment_count = 0U;

    if (out == 0 || phys_pages == 0 || page_count == 0U ||
        !nv_port_dma_direction_valid(direction)) return K_EINVAL;
    *out = (nv_port_dma_mapping_t){0};
    status = nv_port_dma_alloc_segments(page_count, &out->segments);
    if (status != K_OK) return status;
    status = nv_port_dma_build_segments(phys_pages, page_count, out->segments,
                                        page_count, &segment_count);
    if (status != K_OK) {
        nv_port_dma_release_segments(out);
        return status;
    }
    out->segment_count = segment_count;
    out->direction = direction;
    out->coherent = coherent;
    out->active = true;
    return K_OK;
}
#endif

kstatus_t nv_port_dma_map_pages(struct device *device, page_t **pages,
                                uint32_t page_count, uint32_t direction,
                                bool coherent, nv_port_dma_mapping_t *out) {
    if (device == 0 || pages == 0 || page_count == 0U || out == 0 ||
        !nv_port_dma_direction_valid(direction) || out->active) return K_EINVAL;
#ifdef NV_PORT_HOST_TEST
    (void)coherent;
    return K_ENOSYS;
#else
    *out = (nv_port_dma_mapping_t){0};
    kstatus_t status = dma_map_pages(
        device, pages, page_count, (enum dma_direction)direction,
        &out->core_mapping);
    if (status != K_OK) return status;
    status = nv_port_dma_alloc_segments(page_count, &out->segments);
    if (status != K_OK) {
        (void)dma_unmap_checked(&out->core_mapping);
        return status;
    }

    uint64_t *physical_pages = (uint64_t *)kzalloc(
        (size_t)page_count * sizeof(*physical_pages), 0U);
    if (physical_pages == 0) {
        nv_port_dma_release_segments(out);
        (void)dma_unmap_checked(&out->core_mapping);
        return K_ENOMEM;
    }
    for (uint32_t index = 0U; index < page_count; ++index) {
        physical_pages[index] = page_to_phys(pages[index]).value;
    }
    uint32_t physical_count = 0U;
    /* 任意散布输入都需要按页面数量分配大小合适的临时缓冲区。 */
    nv_port_dma_segment_t *physical_list = (nv_port_dma_segment_t *)kzalloc(
        (size_t)page_count * sizeof(*physical_list), 0U);
    if (physical_list == 0) {
        kfree(physical_pages);
        nv_port_dma_release_segments(out);
        (void)dma_unmap_checked(&out->core_mapping);
        return K_ENOMEM;
    }
    status = nv_port_dma_build_segments(physical_pages, page_count,
                                        physical_list, page_count,
                                        &physical_count);
    if (status != K_OK) {
        kfree(physical_list);
        kfree(physical_pages);
        nv_port_dma_release_segments(out);
        (void)dma_unmap_checked(&out->core_mapping);
        return status;
    }
    if (out->core_mapping.segment_count == 1U && physical_count != 1U) {
        /* IOMMU 基本约定：核心提供一段连续 IOVA，而后备页面的物理地址可能分散。应在端口描述符中
         * 明确保留每个页面的物理标识。 */
        for (uint32_t index = 0U; index < page_count; ++index) {
            out->segments[index].phys_addr = physical_pages[index];
            out->segments[index].dma_addr =
                out->core_mapping.segments[0].addr.value +
                (uint64_t)index * PAGE_SIZE;
            out->segments[index].length = PAGE_SIZE;
        }
        out->segment_count = page_count;
    } else {
        for (uint32_t index = 0U; index < physical_count; ++index) {
            out->segments[index] = physical_list[index];
            if (index < out->core_mapping.segment_count) {
                out->segments[index].dma_addr =
                    out->core_mapping.segments[index].addr.value;
            }
        }
        out->segment_count = physical_count;
    }
    out->direction = direction;
    out->coherent = coherent;
    out->active = true;
    kfree(physical_list);
    kfree(physical_pages);
    return K_OK;
#endif
}

kstatus_t nv_port_dma_map_mem(struct device *device, nv_port_mem_t *memory,
                              uint32_t direction, bool coherent,
                              nv_port_dma_mapping_t *out) {
    if (device == 0 || memory == 0 || out == 0 || memory->page_count == 0U ||
        !nv_port_dma_direction_valid(direction) || out->active) return K_EINVAL;
#ifdef NV_PORT_HOST_TEST
    (void)device;
    return nv_port_dma_map_host_physical(memory->phys_pages,
                                         memory->page_count, direction,
                                         coherent, out);
#else
    return nv_port_dma_map_pages(device, memory->page_objects,
                                 memory->page_count, direction, coherent, out);
#endif
}

kstatus_t nv_port_dma_alloc_coherent(size_t size, size_t alignment,
                                     nv_port_mem_t *memory) {
    return nv_port_mem_alloc(size, alignment,
                             NV_PORT_MEM_ZERO | NV_PORT_MEM_CONTIGUOUS,
                             NV_PORT_CACHE_WB, memory);
}

void nv_port_dma_free_coherent(nv_port_mem_t *memory) {
    nv_port_mem_free(memory);
}

kstatus_t nv_port_dma_unmap(nv_port_dma_mapping_t *mapping) {
    if (mapping == 0 || !mapping->active) return K_EINVAL;
#ifndef NV_PORT_HOST_TEST
    kstatus_t status = dma_unmap_checked(&mapping->core_mapping);
    if (status != K_OK) return status;
#endif
    nv_port_dma_release_segments(mapping);
    *mapping = (nv_port_dma_mapping_t){0};
    return K_OK;
}

void nv_port_dma_sync_for_device(nv_port_dma_mapping_t *mapping) {
    if (mapping == 0 || !mapping->active) return;
#ifndef NV_PORT_HOST_TEST
    dma_sync_for_device(&mapping->core_mapping);
#else
    nv_port_dma_wmb();
#endif
}

void nv_port_dma_sync_for_cpu(nv_port_dma_mapping_t *mapping) {
    if (mapping == 0 || !mapping->active) return;
#ifndef NV_PORT_HOST_TEST
    dma_sync_for_cpu(&mapping->core_mapping);
#else
    nv_port_dma_rmb();
#endif
}

kstatus_t nv_port_dma_validate(struct device *device, uint64_t dma_address,
                               uint64_t length, uint32_t access,
                               const nv_port_dma_mapping_t *mapping) {
    if (device == 0 || mapping == 0 || !mapping->active || length == 0U ||
        access > DMA_DEVICE_WRITE) return K_EINVAL;
    if ((access == DMA_DEVICE_READ && mapping->direction == DMA_FROM_DEVICE) ||
        (access == DMA_DEVICE_WRITE && mapping->direction == DMA_TO_DEVICE)) {
        return K_EACCES;
    }
#ifndef NV_PORT_HOST_TEST
    return dma_validate_access(device, iova_make(dma_address), length,
                               (enum dma_device_access)access);
#else
    uint64_t cursor = dma_address;
    uint64_t remaining = length;
    for (uint32_t index = 0U; index < mapping->segment_count && remaining != 0U;
         ++index) {
        const nv_port_dma_segment_t *segment = &mapping->segments[index];
        if (segment->length == 0U || segment->dma_addr > UINT64_MAX -
            segment->length) return K_EIO;
        uint64_t end = segment->dma_addr + segment->length;
        if (cursor < segment->dma_addr || cursor >= end) continue;
        uint64_t available = end - cursor;
        if (available >= remaining) return K_OK;
        remaining -= available;
        cursor = end;
    }
    return K_EACCES;
#endif
}

bool nv_port_dma_self_test(void) {
    const uint64_t physical_pages[] = {
        0x1000U, 0x2000U, 0x3000U, 0x9000U, 0xA000U,
    };
    nv_port_dma_segment_t segments[5] = {0};
    uint32_t segment_count = 0U;

    if (nv_port_dma_build_segments(physical_pages, 5U, segments, 5U,
                                   &segment_count) != K_OK ||
        segment_count != 2U || segments[0].phys_addr != 0x1000U ||
        segments[0].length != 3U * PAGE_SIZE ||
        segments[1].phys_addr != 0x9000U ||
        segments[1].length != 2U * PAGE_SIZE ||
        nv_port_dma_build_segments(physical_pages, 5U, segments, 1U,
                                   &segment_count) != K_ENOSPC) return false;
    uint64_t invalid[] = {0x1001U};
    return nv_port_dma_build_segments(invalid, 1U, segments, 5U,
                                      &segment_count) == K_EINVAL;
}
