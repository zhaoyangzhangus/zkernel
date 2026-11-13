#include <arch/x86_64/cpu.h>
#include <arch/x86_64/paging.h>
#include <kernel/console.h>
#include <kernel/kmem.h>
#include <kernel/pci.h>
#include <kernel/realtest.h>

#include "internal.h"

/* REFACTOR_P8_XHCI_LIFECYCLE_OWNER：MMIO、DMA、控制器启动和控制器停止确认。 */

#define XHCI_MMIO_VA          (X86_64_MMIO_BASE + 0x14000000ULL)
#define XHCI_MMIO_MAX_SIZE    0x00100000ULL
#define XHCI_RING_TRB_COUNT   256U
#define XHCI_LINK_TRB_TYPE    6U

#define XHCI_USBCMD           0x00U
#define XHCI_USBSTS           0x04U
#define XHCI_CRCR             0x18U
#define XHCI_DCBAAP           0x30U
#define XHCI_CONFIG           0x38U

#define XHCI_USBCMD_RUN       (1U << 0)
#define XHCI_USBCMD_HCRST     (1U << 1)
#define XHCI_USBCMD_INTE      (1U << 2)
#define XHCI_USBSTS_HCH       (1U << 0)
#define XHCI_USBSTS_CNR       (1U << 11)

#define XHCI_EXT_CAP_ID_MASK   0xFFU
#define XHCI_EXT_CAP_NEXT_SHIFT 8U
#define XHCI_EXT_CAP_LEGACY    1U
#define XHCI_LEGACY_BIOS_OWNED (1U << 16)
#define XHCI_LEGACY_OS_OWNED   (1U << 24)
#define XHCI_LEGACY_SMI_EVENTS ((1U << 29) | (1U << 30) | (1U << 31))

#define XHCI_RUNTIME_IMAN      0x00U
#define XHCI_RUNTIME_ERSTSZ    0x08U
#define XHCI_RUNTIME_ERSTBA    0x10U
#define XHCI_RUNTIME_ERDP      0x18U
#define XHCI_RUNTIME_INTR0     0x20U
#define XHCI_IMAN_IE           (1U << 1)

#define XHCI_TRB_CYCLE         (1U << 0)
#define XHCI_TRB_TYPE_SHIFT    10U
#define XHCI_MAX_SCRATCHPAD_BUFFERS 1024U

typedef struct __attribute__((packed)) xhci_lifecycle_erst_entry {
    uint64_t ring_segment_base;
    uint16_t ring_segment_size;
    uint16_t reserved0;
    uint32_t reserved1;
} xhci_lifecycle_erst_entry_t;

_Static_assert(sizeof(xhci_lifecycle_erst_entry_t) == 16U,
               "xHCI ERST ABI");

uint64_t xhci_dma_address(const dma_mapping_t *mapping) {
    return mapping != 0 && mapping->segment_count != 0U ?
           mapping->segments[0].addr.value : 0U;
}

uint32_t xhci_controller_read32(const xhci_state_t *state, uint32_t offset) {
    if (state == 0 || state->mmio == 0 ||
        offset > state->mmio_span - sizeof(uint32_t)) {
        return UINT32_MAX;
    }
    return *(volatile const uint32_t *)(state->mmio + offset);
}

uint64_t xhci_controller_timeout_deadline(uint64_t timeout_ns) {
    if (x86_boot_cpu_features.tsc_hz == 0U) return UINT64_MAX;
    uint64_t ticks = x86_timeout_ns_to_tsc(timeout_ns);
    uint64_t now = x86_read_tsc();
    return ticks > UINT64_MAX - now ? UINT64_MAX : now + ticks;
}

bool xhci_controller_timeout_reached(uint64_t deadline) {
    return deadline != UINT64_MAX &&
           (int64_t)(x86_read_tsc() - deadline) >= 0;
}

void xhci_controller_delay_ns(uint64_t delay_ns) {
    if (x86_boot_cpu_features.tsc_hz == 0U) {
        for (uint32_t spin = 0; spin < 100000U; ++spin) {
            __asm__ volatile ("pause");
        }
        return;
    }
    uint64_t deadline = xhci_controller_timeout_deadline(delay_ns);
    while (!xhci_controller_timeout_reached(deadline)) {
        __asm__ volatile ("pause");
    }
}

bool xhci_controller_write32(const xhci_state_t *state, uint32_t offset,
                             uint32_t value) {
    if (state == 0 || state->mmio == 0 ||
        offset > state->mmio_span - sizeof(uint32_t)) {
        return false;
    }
    *(volatile uint32_t *)(state->mmio + offset) = value;
    __asm__ volatile ("mfence" : : : "memory");
    return true;
}

bool xhci_controller_write64(const xhci_state_t *state, uint32_t offset,
                             uint64_t value) {
    return xhci_controller_write32(state, offset, (uint32_t)value) &&
           xhci_controller_write32(state, offset + sizeof(uint32_t),
                                    (uint32_t)(value >> 32));
}

bool xhci_controller_map_mmio(xhci_state_t *state) {
    uint32_t bar_index = PCI_MAX_BARS;
    uint64_t virtual_base = XHCI_MMIO_VA;
    if (state == 0 || state->pci == 0) return false;
    /* 存储控制器仍使用传统窗口。HID 专用控制器必须有独立的虚拟映射，因为多个 PCI xHCI
     * 功能可能同时处于活动状态。 */
    if (state == xhci_hid_controller_state()) {
        virtual_base += XHCI_MMIO_MAX_SIZE;
    }
    for (uint32_t i = 0; i < PCI_MAX_BARS; ++i) {
        if ((state->pci->bars[i].flags & PCI_RESOURCE_MEMORY) != 0 &&
            state->pci->bars[i].length != 0U) {
            bar_index = i;
            break;
        }
    }
    if (bar_index == PCI_MAX_BARS) {
        xhci_set_error(10U);
        return false;
    }
    uint64_t span = state->pci->bars[bar_index].length;
    if (span > XHCI_MMIO_MAX_SIZE) span = XHCI_MMIO_MAX_SIZE;
    span = (span + PAGE_SIZE - 1ULL) & ~(uint64_t)(PAGE_SIZE - 1ULL);
    if (span == 0U || span > X86_64_MMIO_END - virtual_base + 1ULL ||
        state->pci->bars[bar_index].address > UINT64_MAX - span) {
        xhci_set_error(11U);
        return false;
    }
    uint64_t mapped = 0U;
    while (mapped < span) {
        if (x86_map_page(x86_current_root_table(), virtual_base + mapped,
                         paddr_make(state->pci->bars[bar_index].address + mapped),
                         X86_PAGE_WRITE | X86_PAGE_GLOBAL,
                         X86_CACHE_UC) != K_OK) {
            while (mapped != 0U) {
                mapped -= PAGE_SIZE;
                (void)x86_unmap_page(x86_current_root_table(),
                                     virtual_base + mapped, 0);
            }
            xhci_set_error(12U);
            return false;
        }
        mapped += PAGE_SIZE;
    }
    state->mmio = (volatile uint8_t *)(uintptr_t)virtual_base;
    state->mmio_span = span;
    return true;
}

void xhci_controller_unmap_mmio(xhci_state_t *state) {
    if (state == 0 || state->mmio == 0) return;
    uint64_t virtual_base = (uint64_t)(uintptr_t)state->mmio;
    for (uint64_t offset = 0U; offset < state->mmio_span;
         offset += PAGE_SIZE) {
        (void)x86_unmap_page(x86_current_root_table(),
                             virtual_base + offset, 0);
    }
    state->mmio = 0;
    state->mmio_span = 0U;
}

bool xhci_alloc_page(xhci_state_t *state, xhci_dma_page_t *dma,
                     enum dma_direction direction) {
    if (state == 0 || state->pci == 0 || dma == 0) return false;
    dma->page = page_alloc(0, PAGE_ALLOC_ZERO | PAGE_ALLOC_DMA32);
    if (dma->page == 0) return false;
    page_t *pages[1] = {dma->page};
    if (dma_map_pages((device_t *)&state->pci->device, pages, 1U, direction,
                      &dma->mapping) != K_OK) {
        page_free(dma->page);
        dma->page = 0;
        return false;
    }
    dma->cpu = phys_to_direct(page_to_phys(dma->page));
    if (dma->cpu == 0 || xhci_dma_address(&dma->mapping) == 0U) {
        if (dma_unmap_checked(&dma->mapping) != K_OK) {
            dma->cpu = 0;
            return false;
        }
        page_free(dma->page);
        dma->page = 0;
        dma->cpu = 0;
        return false;
    }
    return true;
}

bool xhci_free_page(xhci_dma_page_t *dma) {
    if (dma == 0) return false;
    if (dma->mapping.device != 0 &&
        dma_unmap_checked(&dma->mapping) != K_OK) {
        return false;
    }
    if (dma->page != 0) page_free(dma->page);
    dma->page = 0;
    dma->cpu = 0;
    return true;
}

bool xhci_controller_free_rings(xhci_state_t *state) {
    if (state == 0) return false;
    bool released = xhci_controller_free_scratchpads(state);
    if (!xhci_free_page(&state->erst)) released = false;
    if (!xhci_free_page(&state->event_ring)) released = false;
    if (!xhci_free_page(&state->command_ring)) released = false;
    if (!xhci_free_page(&state->dcbaa)) released = false;
    return released;
}

bool xhci_controller_free_scratchpads(xhci_state_t *state) {
    bool released = true;
    if (state == 0) return false;
    if (!xhci_free_dma_region(&state->scratchpad_buffers)) released = false;
    if (!xhci_free_dma_region(&state->scratchpad_array)) released = false;
    return released;
}

bool xhci_alloc_dma_region(xhci_state_t *state, xhci_dma_region_t *region,
                           uint32_t page_count) {
    uint8_t order = 0U;
    uint32_t allocated_pages = 1U;
    if (state == 0 || state->pci == 0 || region == 0 || page_count == 0U) {
        return false;
    }
    while (allocated_pages < page_count && order < BUDDY_MAX_ORDER) {
        ++order;
        allocated_pages <<= 1U;
    }
    if (allocated_pages < page_count) return false;
    *region = (xhci_dma_region_t){0};
    region->head = page_alloc(order, PAGE_ALLOC_ZERO | PAGE_ALLOC_DMA32);
    if (region->head == 0) return false;
    region->pages = (page_t **)kzalloc(
        (size_t)page_count * sizeof(*region->pages), 0U);
    if (region->pages == 0) {
        page_free(region->head);
        *region = (xhci_dma_region_t){0};
        return false;
    }
    paddr_t physical = page_to_phys(region->head);
    for (uint32_t index = 0U; index < page_count; ++index) {
        region->pages[index] = phys_to_page(
            paddr_make(physical.value + (uint64_t)index * PAGE_SIZE));
        if (region->pages[index] == 0) {
            (void)xhci_free_dma_region(region);
            return false;
        }
    }
    if (dma_map_pages((device_t *)&state->pci->device, region->pages,
                      page_count, DMA_BIDIRECTIONAL,
                      &region->mapping) != K_OK) {
        (void)xhci_free_dma_region(region);
        return false;
    }
    if (region->mapping.segment_count != 1U ||
        region->mapping.segments[0].length <
            (uint64_t)page_count * PAGE_SIZE) {
        (void)xhci_free_dma_region(region);
        return false;
    }
    region->page_count = page_count;
    region->allocation_order = order;
    region->cpu = phys_to_direct(physical);
    if (region->cpu == 0) {
        (void)xhci_free_dma_region(region);
        return false;
    }
    return true;
}

bool xhci_free_dma_region(xhci_dma_region_t *region) {
    bool success = true;
    if (region == 0) return false;
    if (region->mapping.flags != 0U &&
        dma_unmap_checked(&region->mapping) != K_OK) success = false;
    if (success && region->pages != 0) {
        kfree(region->pages);
        region->pages = 0;
    }
    if (success && region->head != 0) {
        page_free(region->head);
        region->head = 0;
    }
    if (success) *region = (xhci_dma_region_t){0};
    return success;
}

static bool xhci_controller_setup_scratchpads(xhci_state_t *state) {
    uint32_t array_pages;
    if (state == 0 || state->scratchpad_count == 0U) return true;
    if (state->scratchpad_count > XHCI_MAX_SCRATCHPAD_BUFFERS) return false;
    array_pages = (state->scratchpad_count * sizeof(uint64_t) + PAGE_SIZE - 1U) /
                  PAGE_SIZE;
    if (!xhci_alloc_dma_region(state, &state->scratchpad_array, array_pages) ||
        !xhci_alloc_dma_region(state, &state->scratchpad_buffers,
                               state->scratchpad_count)) {
        (void)xhci_controller_free_scratchpads(state);
        return false;
    }
    uint64_t *entries = (uint64_t *)state->scratchpad_array.cpu;
    for (uint32_t index = 0U; index < state->scratchpad_count; ++index) {
        entries[index] = state->scratchpad_buffers.mapping.segments[0].addr.value +
                         (uint64_t)index * PAGE_SIZE;
    }
    dma_sync_for_device(&state->scratchpad_array.mapping);
    return true;
}

bool xhci_controller_reset(xhci_state_t *state) {
    uint32_t command = xhci_controller_read32(
        state, state->operational_offset + XHCI_USBCMD);
    if (command == UINT32_MAX ||
        !xhci_controller_write32(state,
                                 state->operational_offset + XHCI_USBCMD,
                                 command & ~XHCI_USBCMD_RUN)) {
        return false;
    }
    for (uint32_t spin = 0U; spin < 100000U; ++spin) {
        uint32_t status = xhci_controller_read32(
            state, state->operational_offset + XHCI_USBSTS);
        if ((status & XHCI_USBSTS_HCH) != 0U) break;
        __asm__ volatile ("pause");
    }
    command = xhci_controller_read32(
        state, state->operational_offset + XHCI_USBCMD);
    if (command == UINT32_MAX ||
        !xhci_controller_write32(state,
                                 state->operational_offset + XHCI_USBCMD,
                                 command | XHCI_USBCMD_HCRST)) {
        return false;
    }
    for (uint32_t spin = 0U; spin < 100000U; ++spin) {
        command = xhci_controller_read32(
            state, state->operational_offset + XHCI_USBCMD);
        uint32_t status = xhci_controller_read32(
            state, state->operational_offset + XHCI_USBSTS);
        if ((command & XHCI_USBCMD_HCRST) == 0U &&
            (status & XHCI_USBSTS_CNR) == 0U) {
            return true;
        }
        __asm__ volatile ("pause");
    }
    return false;
}

bool xhci_controller_halt(xhci_state_t *state) {
    if (state == 0 || state->mmio == 0) return true;
    if (!xhci_controller_write32(
            state, state->operational_offset + XHCI_USBCMD, 0U)) {
        return false;
    }
    for (uint32_t spin = 0U; spin < 10000U; ++spin) {
        if ((xhci_controller_read32(
                 state, state->operational_offset + XHCI_USBSTS) &
             XHCI_USBSTS_HCH) != 0U) {
            return true;
        }
        __asm__ volatile ("pause");
    }
    return false;
}

bool xhci_controller_handoff_legacy(xhci_state_t *state,
                                     uint32_t hcc_params1) {
    if (state == 0) return false;
    uint32_t offset = ((hcc_params1 >> 16) & 0xFFFFU) << 2;
    for (uint32_t count = 0U; offset != 0U && count < 256U; ++count) {
        if (offset > state->mmio_span - sizeof(uint32_t)) return false;
        uint32_t capability = xhci_controller_read32(state, offset);
        if ((capability & XHCI_EXT_CAP_ID_MASK) == XHCI_EXT_CAP_LEGACY) {
            if ((capability & XHCI_LEGACY_OS_OWNED) == 0U &&
                !xhci_controller_write32(state, offset,
                                         capability | XHCI_LEGACY_OS_OWNED)) {
                return false;
            }
            if ((capability & XHCI_LEGACY_BIOS_OWNED) != 0U) {
                uint64_t deadline =
                    xhci_controller_timeout_deadline(1000000000ULL);
                while ((xhci_controller_read32(state, offset) &
                        XHCI_LEGACY_BIOS_OWNED) != 0U &&
                       !xhci_controller_timeout_reached(deadline)) {
                    __asm__ volatile ("pause");
                }
            }
            uint32_t after = xhci_controller_read32(state, offset);
            if ((after & XHCI_LEGACY_BIOS_OWNED) != 0U) {
                kernel_serial_write("OS_XHCI_LEGACY_HANDOFF_TIMEOUT BIOS=");
                kernel_serial_write_u32(after);
                kernel_serial_write("\r\n");
                (void)xhci_controller_write32(
                    state, offset,
                    (after | XHCI_LEGACY_OS_OWNED) &
                        ~XHCI_LEGACY_BIOS_OWNED);
            }
            if (offset <= state->mmio_span - 2U * sizeof(uint32_t)) {
                uint32_t control = xhci_controller_read32(
                    state, offset + sizeof(uint32_t));
                (void)xhci_controller_write32(
                    state, offset + sizeof(uint32_t),
                    (control & ~0x0000FFFFU) | XHCI_LEGACY_SMI_EVENTS);
            }
            return true;
        }
        uint32_t next = (capability >> XHCI_EXT_CAP_NEXT_SHIFT) & 0xFFU;
        offset = next == 0U ? 0U : offset + (next << 2);
    }
    return true;
}

bool xhci_controller_setup_rings(xhci_state_t *state) {
    if (!xhci_alloc_page(state, &state->dcbaa, DMA_BIDIRECTIONAL) ||
        !xhci_alloc_page(state, &state->command_ring, DMA_BIDIRECTIONAL) ||
        !xhci_alloc_page(state, &state->event_ring, DMA_FROM_DEVICE) ||
        !xhci_alloc_page(state, &state->erst, DMA_BIDIRECTIONAL)) {
        return false;
    }
    if (!xhci_controller_setup_scratchpads(state)) return false;

    xhci_trb_t *command_ring = (xhci_trb_t *)state->command_ring.cpu;
    command_ring[XHCI_RING_TRB_COUNT - 1U].parameter =
        xhci_dma_address(&state->command_ring.mapping);
    command_ring[XHCI_RING_TRB_COUNT - 1U].control =
        (XHCI_LINK_TRB_TYPE << XHCI_TRB_TYPE_SHIFT) | XHCI_TRB_CYCLE;
    xhci_lifecycle_erst_entry_t *erst =
        (xhci_lifecycle_erst_entry_t *)state->erst.cpu;
    erst[0].ring_segment_base = xhci_dma_address(&state->event_ring.mapping);
    erst[0].ring_segment_size = XHCI_RING_TRB_COUNT;
    uint64_t dcbaa = xhci_dma_address(&state->dcbaa.mapping);
    if (state->scratchpad_count != 0U) {
        ((uint64_t *)state->dcbaa.cpu)[0] =
            state->scratchpad_array.mapping.segments[0].addr.value;
    }
    uint64_t command_ring_address =
        xhci_dma_address(&state->command_ring.mapping) | 1ULL;
    uint64_t erst_address = xhci_dma_address(&state->erst.mapping);
    uint64_t event_ring_address = xhci_dma_address(&state->event_ring.mapping);
    uint32_t runtime = state->runtime_offset + XHCI_RUNTIME_INTR0;
    uint32_t operational = state->operational_offset;
    dma_sync_for_device(&state->dcbaa.mapping);
    dma_sync_for_device(&state->command_ring.mapping);
    dma_sync_for_device(&state->event_ring.mapping);
    dma_sync_for_device(&state->erst.mapping);
    if (!xhci_controller_write64(state, operational + XHCI_DCBAAP, dcbaa) ||
        !xhci_controller_write64(state, operational + XHCI_CRCR,
                                 command_ring_address) ||
        !xhci_controller_write32(state, operational + XHCI_CONFIG,
                                 state->max_slots) ||
        !xhci_controller_write32(state, runtime + XHCI_RUNTIME_IMAN,
                                 XHCI_IMAN_IE) ||
        !xhci_controller_write32(state, runtime + XHCI_RUNTIME_ERSTSZ, 1U) ||
        !xhci_controller_write64(state, runtime + XHCI_RUNTIME_ERSTBA,
                                 erst_address) ||
        !xhci_controller_write64(state, runtime + XHCI_RUNTIME_ERDP,
                                 event_ring_address) ||
        !xhci_controller_write32(state, operational + XHCI_USBCMD,
                                 XHCI_USBCMD_RUN | XHCI_USBCMD_INTE)) {
        return false;
    }
    for (uint32_t spin = 0U; spin < 100000U; ++spin) {
        if ((xhci_controller_read32(state, operational + XHCI_USBSTS) &
             XHCI_USBSTS_HCH) == 0U) {
            return true;
        }
        __asm__ volatile ("pause");
    }
    return false;
}
