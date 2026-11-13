#include "nvabi_internal.h"

typedef struct nvabi_mmio_mapping {
    uint32_t magic;
    void *returned_base;
    uint64_t requested_length;
    nv_port_mmio_mapping_t mapping;
    struct nvabi_mmio_mapping *next;
} nvabi_mmio_mapping_t;

#define NVABI_MMIO_MAPPING_MAGIC 0x4D4D494FU

static nv_port_spinlock_t g_nvabi_mmio_lock;
static atomic_bool g_nvabi_mmio_lock_initialized = ATOMIC_VAR_INIT(false);
static nvabi_mmio_mapping_t *g_nvabi_mmio_mappings;

static void nvabi_mmio_lock_init(void) {
    bool expected = false;
    if (atomic_compare_exchange_strong_explicit(
            &g_nvabi_mmio_lock_initialized, &expected, true,
            memory_order_acq_rel, memory_order_acquire)) {
        nv_port_spin_init(&g_nvabi_mmio_lock);
    }
}

static uint32_t nvabi_mmio_cache_type(NvU32 type) {
    switch (type) {
    case NV_MEMORY_CACHED:
        return NV_PORT_CACHE_WB;
    case NV_MEMORY_WRITECOMBINED:
        return NV_PORT_CACHE_WC;
    case NV_MEMORY_UNCACHED:
    case NV_MEMORY_UNCACHED_WEAK:
    default:
        return NV_PORT_CACHE_UC;
    }
}

void *NV_API_CALL os_map_kernel_space(NvU64 physical, NvU64 length,
                                      NvU32 cache_type) {
    uint64_t page_physical;
    uint64_t offset;
    uint64_t mapped_length;
    nvabi_mmio_mapping_t *record;
    kstatus_t status;

    if (length == 0U || physical > UINT64_MAX - length) {
        nvabi_log_invalid("os_map_kernel_space", "os-mmio");
        return NULL;
    }
    page_physical = physical & ~NVABI_PAGE_MASK;
    offset = physical - page_physical;
    if (length > UINT64_MAX - offset) {
        nvabi_log_invalid("os_map_kernel_space", "os-mmio");
        return NULL;
    }
    mapped_length = (length + offset + NVABI_PAGE_MASK) & ~NVABI_PAGE_MASK;
    if (mapped_length == 0U) {
        nvabi_log_invalid("os_map_kernel_space", "os-mmio");
        return NULL;
    }
    record = (nvabi_mmio_mapping_t *)kzalloc(sizeof(*record), 0U);
    if (record == NULL) return NULL;
    status = nv_port_mmio_map(paddr_make(page_physical), mapped_length,
                              nvabi_mmio_cache_type(cache_type),
                              &record->mapping);
    if (status != K_OK) {
        kernel_serial_printf_serial_only(
            "OS_NVIDIA_MMIO_MAP_FAIL physical=%llx length=%llx "
            "cache=%u status=%d\r\n",
            (unsigned long long)page_physical,
            (unsigned long long)mapped_length,
            nvabi_mmio_cache_type(cache_type), (int)status);
        kfree(record);
        nvabi_log_unsupported("os_map_kernel_space", "os-mmio-map");
        return NULL;
    }
    record->magic = NVABI_MMIO_MAPPING_MAGIC;
    record->requested_length = length;
    record->returned_base = (uint8_t *)record->mapping.base + offset;
    nvabi_mmio_lock_init();
    nv_port_spin_lock(&g_nvabi_mmio_lock);
    record->next = g_nvabi_mmio_mappings;
    g_nvabi_mmio_mappings = record;
    nv_port_spin_unlock(&g_nvabi_mmio_lock);
    return record->returned_base;
}

void NV_API_CALL os_unmap_kernel_space(void *address, NvU64 length) {
    nvabi_mmio_mapping_t *record = NULL;
    nvabi_mmio_mapping_t *previous = NULL;

    if (address == NULL || length == 0U) {
        nvabi_log_invalid("os_unmap_kernel_space", "os-mmio");
        return;
    }
    nvabi_mmio_lock_init();
    nv_port_spin_lock(&g_nvabi_mmio_lock);
    for (nvabi_mmio_mapping_t *cursor = g_nvabi_mmio_mappings;
         cursor != NULL; cursor = cursor->next) {
        if (cursor->returned_base == address) {
            record = cursor;
            break;
        }
        previous = cursor;
    }
    if (record != NULL && length <= record->requested_length) {
        if (previous == NULL) g_nvabi_mmio_mappings = record->next;
        else previous->next = record->next;
    } else {
        record = NULL;
    }
    nv_port_spin_unlock(&g_nvabi_mmio_lock);
    if (record == NULL) {
        nvabi_log_invalid("os_unmap_kernel_space", "os-mmio");
        return;
    }
    if (nv_port_mmio_unmap(&record->mapping) != K_OK) {
        nvabi_log_invalid("os_unmap_kernel_space", "os-mmio-unmap");
        /* 取消映射失败时，映射仍由端口持有。保留记录以便后续拆除时重试，避免静默泄漏。 */
        nv_port_spin_lock(&g_nvabi_mmio_lock);
        record->next = g_nvabi_mmio_mappings;
        g_nvabi_mmio_mappings = record;
        nv_port_spin_unlock(&g_nvabi_mmio_lock);
        return;
    }
    record->magic = 0U;
    kfree(record);
}
