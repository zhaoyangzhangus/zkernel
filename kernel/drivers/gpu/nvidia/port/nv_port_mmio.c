#include "nv_port.h"

#include <arch/x86_64/paging.h>
#include <kernel/console.h>

#ifdef NV_PORT_HOST_TEST
#include <stdlib.h>
#ifdef _WIN32
#include <malloc.h>
#endif

static void *nv_port_mmio_host_aligned_alloc(size_t size) {
#ifdef _WIN32
    return _aligned_malloc(size, PAGE_SIZE);
#else
    return aligned_alloc(PAGE_SIZE, size);
#endif
}

static void nv_port_mmio_host_free(void *storage) {
#ifdef _WIN32
    _aligned_free(storage);
#else
    free(storage);
#endif
}
#endif

#define NV_PORT_MMIO_SLOT_COUNT 1024U
#define NV_PORT_MMIO_SLOT_SIZE  (1ULL << 30)
#define NV_PORT_MMIO_FIRST_SLOT 1U
#define NV_PORT_MMIO_LAST_SLOT  (NV_PORT_MMIO_SLOT_COUNT - 1U)

#ifndef NV_PORT_HOST_TEST
static atomic_uint g_nv_port_mmio_init_state;
static nv_port_spinlock_t g_nv_port_mmio_lock;
static uint64_t g_nv_port_mmio_slots[NV_PORT_MMIO_SLOT_COUNT / 64U];
#endif

static bool nv_port_mmio_cache_valid(uint32_t cache_type) {
    return cache_type == NV_PORT_CACHE_WB ||
           cache_type == NV_PORT_CACHE_WC ||
           cache_type == NV_PORT_CACHE_UC;
}

#ifndef NV_PORT_HOST_TEST
static void nv_port_mmio_initialize(void) {
    unsigned expected = 0U;
    if (atomic_compare_exchange_strong_explicit(
            &g_nv_port_mmio_init_state, &expected, 1U, memory_order_acq_rel,
            memory_order_acquire)) {
        nv_port_spin_init(&g_nv_port_mmio_lock);
        for (uint32_t index = 0U;
             index < NV_PORT_MMIO_SLOT_COUNT / 64U; ++index) {
            g_nv_port_mmio_slots[index] = 0U;
        }
        atomic_store_explicit(&g_nv_port_mmio_init_state, 2U,
                              memory_order_release);
        return;
    }
    while (atomic_load_explicit(&g_nv_port_mmio_init_state,
                                memory_order_acquire) != 2U) {
        __asm__ volatile ("pause");
    }
}

static bool nv_port_mmio_slot_used(uint32_t slot) {
    return (g_nv_port_mmio_slots[slot >> 6] & (1ULL << (slot & 63U))) != 0U;
}

static void nv_port_mmio_set_slot(uint32_t slot, bool used) {
    uint64_t bit = 1ULL << (slot & 63U);
    if (used) g_nv_port_mmio_slots[slot >> 6] |= bit;
    else g_nv_port_mmio_slots[slot >> 6] &= ~bit;
}

static int32_t nv_port_mmio_take_slot(void) {
    uint64_t flags = nv_port_spin_lock_irqsave(&g_nv_port_mmio_lock);
    int32_t result = -1;
    for (uint32_t slot = NV_PORT_MMIO_FIRST_SLOT;
         slot <= NV_PORT_MMIO_LAST_SLOT; ++slot) {
        if (!nv_port_mmio_slot_used(slot)) {
            nv_port_mmio_set_slot(slot, true);
            result = (int32_t)slot;
            break;
        }
    }
    nv_port_spin_unlock_irqrestore(&g_nv_port_mmio_lock, flags);
    return result;
}

static void nv_port_mmio_release_slot(uint32_t slot) {
    if (slot < NV_PORT_MMIO_FIRST_SLOT || slot > NV_PORT_MMIO_LAST_SLOT) return;
    uint64_t flags = nv_port_spin_lock_irqsave(&g_nv_port_mmio_lock);
    nv_port_mmio_set_slot(slot, false);
    nv_port_spin_unlock_irqrestore(&g_nv_port_mmio_lock, flags);
}
#endif

#ifndef NV_PORT_HOST_TEST
static enum x86_cache_mode nv_port_mmio_cache_mode(uint32_t cache_type) {
    return cache_type == NV_PORT_CACHE_WC ? X86_CACHE_WC :
           cache_type == NV_PORT_CACHE_UC ? X86_CACHE_UC : X86_CACHE_WB;
}
#endif

kstatus_t nv_port_mmio_map(paddr_t physical, uint64_t length,
                           uint32_t cache_type,
                           nv_port_mmio_mapping_t *out) {
    uint64_t offset;
    uint64_t mapped_length;

    if (out == 0 || length == 0U || physical.value > UINT64_MAX - length ||
        !nv_port_mmio_cache_valid(cache_type)) return K_EINVAL;
    offset = physical.value & (PAGE_SIZE - 1ULL);
    if (length > UINT64_MAX - offset) return K_EOVERFLOW;
    if (length + offset > UINT64_MAX - (PAGE_SIZE - 1ULL)) return K_EOVERFLOW;
    mapped_length = (length + offset + PAGE_SIZE - 1ULL) &
                    ~(PAGE_SIZE - 1ULL);
    if (mapped_length == 0U || mapped_length > NV_PORT_MMIO_SLOT_SIZE) {
        return K_EOVERFLOW;
    }
#ifdef NV_PORT_HOST_TEST
    void *storage = nv_port_mmio_host_aligned_alloc((size_t)mapped_length);
    if (storage == 0) return K_ENOMEM;
    *out = (nv_port_mmio_mapping_t){
        .base = (uint8_t *)storage + offset,
        .virtual_base = (vaddr_t)(uintptr_t)storage,
        .physical = physical,
        .length = length,
        .mapped_length = mapped_length,
        .cache_type = cache_type,
        .active = true,
        .host_storage = storage,
    };
    return K_OK;
#else
    nv_port_mmio_initialize();
    int32_t slot = nv_port_mmio_take_slot();
    if (slot < 0) return K_ENOSPC;
    paddr_t root = x86_current_root_table();
    vaddr_t virtual_base = (vaddr_t)X86_64_MMIO_BASE +
                           (uint64_t)slot * NV_PORT_MMIO_SLOT_SIZE;
    uint64_t mapped = 0U;
    kstatus_t status = K_OK;
    for (; mapped < mapped_length; mapped += PAGE_SIZE) {
        status = x86_map_page(
            root, virtual_base + mapped,
            paddr_make((physical.value & ~(PAGE_SIZE - 1ULL)) + mapped),
            X86_PAGE_WRITE | X86_PAGE_GLOBAL, nv_port_mmio_cache_mode(cache_type));
        if (status != K_OK) break;
    }
    if (status != K_OK) {
        kernel_serial_printf_serial_only(
            "OS_NVIDIA_PORT_MMIO_FAIL slot=%d va=%llx "
            "physical=%llx length=%llx mapped=%llx status=%d\r\n",
            (int)slot, (unsigned long long)virtual_base,
            (unsigned long long)physical.value,
            (unsigned long long)length,
            (unsigned long long)mapped, (int)status);
        while (mapped != 0U) {
            mapped -= PAGE_SIZE;
            (void)x86_unmap_page(root, virtual_base + mapped, 0);
        }
        nv_port_mmio_release_slot((uint32_t)slot);
        return status;
    }
    *out = (nv_port_mmio_mapping_t){
        .base = (void *)(uintptr_t)(virtual_base + offset),
        .virtual_base = virtual_base,
        .physical = physical,
        .length = length,
        .mapped_length = mapped_length,
        .cache_type = cache_type,
        .active = true,
        .root_table = root,
        .slot = (uint32_t)slot,
    };
    return K_OK;
#endif
}

kstatus_t nv_port_mmio_map_bar(const pci_device_t *device, uint32_t bar_index,
                               uint32_t cache_type,
                               nv_port_mmio_mapping_t *out) {
    if (device == 0 || out == 0 || bar_index >= PCI_MAX_BARS) return K_EINVAL;
    const pci_bar_t *bar = &device->bars[bar_index];
    if ((bar->flags & PCI_RESOURCE_MEMORY) == 0U || bar->length == 0U) {
        return K_ENOENT;
    }
    /* BAR0 是寄存器空间；其他类似帧缓冲区的 BAR 可能采用 WC/UC 属性。 */
    if ((bar_index == 0U && cache_type != NV_PORT_CACHE_UC) ||
        (bar_index != 0U && cache_type == NV_PORT_CACHE_WB)) return K_EINVAL;
    return nv_port_mmio_map(paddr_make(bar->address), bar->length,
                            cache_type, out);
}

kstatus_t nv_port_mmio_unmap(nv_port_mmio_mapping_t *mapping) {
    if (mapping == 0 || !mapping->active) return K_EINVAL;
#ifdef NV_PORT_HOST_TEST
    nv_port_mmio_host_free(mapping->host_storage);
    *mapping = (nv_port_mmio_mapping_t){0};
    return K_OK;
#else
    if (mapping->root_table.value == 0U || mapping->mapped_length == 0U) {
        return K_EINVAL;
    }
    kstatus_t status = K_OK;
    for (uint64_t offset = 0U; offset < mapping->mapped_length;
         offset += PAGE_SIZE) {
        kstatus_t unmap = x86_unmap_page(
            mapping->root_table, mapping->virtual_base + offset, 0);
        if (unmap != K_OK && unmap != K_ENOENT && status == K_OK) status = unmap;
    }
    if (status != K_OK) return status;
    nv_port_mmio_release_slot(mapping->slot);
    *mapping = (nv_port_mmio_mapping_t){0};
    return K_OK;
#endif
}

uint32_t nv_port_mmio_read32(void *base, uint64_t offset) {
    if (base == 0 || (offset & 3U) != 0U) return 0U;
    nv_port_rmb();
    uint32_t value = *(volatile const uint32_t *)((uint8_t *)base + offset);
    nv_port_rmb();
    return value;
}

void nv_port_mmio_write32(void *base, uint64_t offset, uint32_t value) {
    if (base == 0 || (offset & 3U) != 0U) return;
    *(volatile uint32_t *)((uint8_t *)base + offset) = value;
    nv_port_wmb();
}

uint64_t nv_port_mmio_read64(void *base, uint64_t offset) {
    if (base == 0 || (offset & 7U) != 0U) return 0U;
    nv_port_rmb();
    uint64_t value = *(volatile const uint64_t *)((uint8_t *)base + offset);
    nv_port_rmb();
    return value;
}

void nv_port_mmio_write64(void *base, uint64_t offset, uint64_t value) {
    if (base == 0 || (offset & 7U) != 0U) return;
    *(volatile uint64_t *)((uint8_t *)base + offset) = value;
    nv_port_wmb();
}

static bool nv_port_mmio_range_valid(const nv_port_mmio_mapping_t *mapping,
                                     uint64_t offset, size_t length) {
    return mapping != 0 && mapping->active && offset <= mapping->length &&
           (uint64_t)length <= mapping->length - offset;
}

kstatus_t nv_port_mmio_read_bytes(const nv_port_mmio_mapping_t *mapping,
                                  uint64_t offset, void *destination,
                                  size_t length) {
    if (destination == 0 || !nv_port_mmio_range_valid(mapping, offset, length)) {
        return K_EINVAL;
    }
    volatile const uint8_t *source =
        (volatile const uint8_t *)mapping->base + offset;
    uint8_t *bytes = (uint8_t *)destination;
    nv_port_rmb();
    for (size_t index = 0U; index < length; ++index) bytes[index] = source[index];
    nv_port_rmb();
    return K_OK;
}

kstatus_t nv_port_mmio_write_bytes(const nv_port_mmio_mapping_t *mapping,
                                   uint64_t offset, const void *source,
                                   size_t length) {
    if (source == 0 || !nv_port_mmio_range_valid(mapping, offset, length)) {
        return K_EINVAL;
    }
    volatile uint8_t *destination = (volatile uint8_t *)mapping->base + offset;
    const uint8_t *bytes = (const uint8_t *)source;
    for (size_t index = 0U; index < length; ++index) destination[index] = bytes[index];
    nv_port_wmb();
    return K_OK;
}

bool nv_port_mmio_self_test(void) {
#ifdef NV_PORT_HOST_TEST
    nv_port_mmio_mapping_t mapping = {0};
    uint32_t value32;
    uint64_t value64;
    if (nv_port_mmio_map(paddr_make(0x1234U), PAGE_SIZE + 9U,
                         NV_PORT_CACHE_UC, &mapping) != K_OK ||
        nv_port_mmio_write_bytes(&mapping, 1U, "mmio", 4U) != K_OK ||
        nv_port_mmio_read_bytes(&mapping, 1U, &value32, 4U) != K_OK ||
        value32 != 0x6F696D6DU) {
        (void)nv_port_mmio_unmap(&mapping);
        return false;
    }
    nv_port_mmio_write32(mapping.base, 4U, 0xAABBCCDDU);
    value32 = nv_port_mmio_read32(mapping.base, 4U);
    nv_port_mmio_write64(mapping.base, 8U, 0x1122334455667788ULL);
    value64 = nv_port_mmio_read64(mapping.base, 8U);
    return value32 == 0xAABBCCDDU && value64 == 0x1122334455667788ULL &&
           nv_port_mmio_unmap(&mapping) == K_OK;
#else
    return nv_port_mmio_map(paddr_make(0U), 0U, NV_PORT_CACHE_UC, 0) ==
           K_EINVAL;
#endif
}
