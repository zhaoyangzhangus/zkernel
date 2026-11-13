#include <arch/x86_64/paging.h>
#include <kernel/iommu.h>
#include <kernel/irq.h>
#include <kernel/pci.h>

#define PCI_CONFIG_SPACE_SIZE 0x1000U
#define PCI_CONFIG_WINDOW_VA  (X86_64_MMIO_BASE + 0x00000000ULL)
#define PCI_VENDOR_INVALID    0xFFFFU
#define PCI_STATUS_CAP_LIST   (1U << 4)
#define PCI_CAP_ID_MSIX       0x11U
#define PCI_CAP_ID_MSI        0x05U
#define PCI_COMMAND_OFFSET    0x04U
#define PCI_COMMAND_INTX_DISABLE (1U << 10)
#define PCI_MSIX_ENABLE       (1U << 15)
#define PCI_MSIX_FUNCTION_MASK (1U << 14)
#define PCI_MSIX_ENTRY_MASK    (1U << 0)
#define PCI_MSIX_WINDOW_VA     (X86_64_MMIO_BASE + 0x00002000ULL)

static pci_host_t g_pci_host;
static uint32_t g_pci_last_error;
static kstatus_t g_pci_last_map_status;

static void bytes_zero(void *memory, size_t size) {
    uint8_t *bytes = (uint8_t *)memory;
    while (size-- != 0) *bytes++ = 0;
}

static bool add_overflow_u64(uint64_t left, uint64_t right, uint64_t *result) {
    if (left > UINT64_MAX - right) return true;
    *result = left + right;
    return false;
}

static bool pci_config_physical(const x86_acpi_ecam_t *segment,
                                uint8_t bus, uint8_t slot, uint8_t function,
                                uint16_t offset, paddr_t *page,
                                uint32_t *page_offset) {
    if (segment == 0 || page == 0 || page_offset == 0 ||
        bus < segment->start_bus || bus > segment->end_bus || slot >= 32U ||
        function >= 8U || offset >= PCI_CONFIG_SPACE_SIZE || (offset & 3U) != 0) {
        return false;
    }
    uint64_t value = ((uint64_t)(bus - segment->start_bus) << 20) |
                     ((uint64_t)slot << 15) |
                     ((uint64_t)function << 12) | (uint64_t)offset;
    uint64_t physical;
    if (add_overflow_u64(segment->base, value, &physical)) return false;
    *page = paddr_make(physical & ~(uint64_t)(PAGE_SIZE - 1ULL));
    *page_offset = (uint32_t)(physical & (PAGE_SIZE - 1ULL));
    return true;
}

static kstatus_t pci_config_map(paddr_t physical, volatile uint8_t **mapped) {
    if (mapped == 0 || physical.value == 0 ||
        (physical.value & (PAGE_SIZE - 1ULL)) != 0) {
        g_pci_last_map_status = K_EINVAL;
        return K_EINVAL;
    }
    paddr_t root = x86_current_root_table();
    kstatus_t status = x86_map_page(root, (vaddr_t)PCI_CONFIG_WINDOW_VA,
                                    physical, X86_PAGE_WRITE | X86_PAGE_GLOBAL,
                                    X86_CACHE_UC);
    g_pci_last_map_status = status;
    if (status != K_OK) return status;
    *mapped = (volatile uint8_t *)(uintptr_t)PCI_CONFIG_WINDOW_VA;
    return K_OK;
}

static uint32_t pci_config_read32(const x86_acpi_ecam_t *segment,
                                  uint8_t bus, uint8_t slot, uint8_t function,
                                  uint16_t offset) {
    paddr_t physical;
    uint32_t page_offset;
    if (!pci_config_physical(segment, bus, slot, function, offset,
                             &physical, &page_offset)) return UINT32_MAX;
    volatile uint8_t *mapped = 0;
    if (pci_config_map(physical, &mapped) != K_OK) return UINT32_MAX;
    uint32_t value = *(volatile const uint32_t *)(mapped + page_offset);
    __asm__ volatile ("mfence" : : : "memory");
    (void)x86_unmap_page(x86_current_root_table(),
                         (vaddr_t)PCI_CONFIG_WINDOW_VA, 0);
    return value;
}

static kstatus_t pci_config_write32(const x86_acpi_ecam_t *segment,
                                     uint8_t bus, uint8_t slot, uint8_t function,
                                     uint16_t offset, uint32_t value) {
    paddr_t physical;
    uint32_t page_offset;
    if (!pci_config_physical(segment, bus, slot, function, offset,
                             &physical, &page_offset)) return K_EINVAL;
    volatile uint8_t *mapped = 0;
    kstatus_t status = pci_config_map(physical, &mapped);
    if (status != K_OK) return status;
    *(volatile uint32_t *)(mapped + page_offset) = value;
    __asm__ volatile ("mfence" : : : "memory");
    return x86_unmap_page(x86_current_root_table(),
                          (vaddr_t)PCI_CONFIG_WINDOW_VA, 0);
}

static bool bar_is_memory64(uint32_t low) {
    return (low & 1U) == 0 && ((low >> 1) & 3U) == 2U;
}

static uint64_t pci_bar_length_from_masks(uint32_t mask_low,
                                          uint32_t mask_high,
                                          bool memory, bool is_64) {
    uint32_t low_mask = memory ? (mask_low & ~0xFU) : (mask_low & ~3U);
    if (is_64) {
        uint64_t mask = (uint64_t)low_mask | ((uint64_t)mask_high << 32);
        return mask != 0U ? (~mask + 1ULL) : 0U;
    }
    /* 32 位 BAR 必须在 32 位地址空间中确定大小。按 32 位回绕完成取反和加法后再提升
     * 位宽；若先提升，一个普通的 64 MiB BAR 会变成 0xffffffff04000000。 */
    return low_mask != 0U ? (uint64_t)(~low_mask + 1U) : 0U;
}

static bool read_bar(const x86_acpi_ecam_t *segment, pci_device_t *device,
                     uint32_t index) {
    uint16_t offset = (uint16_t)(0x10U + index * 4U);
    uint32_t saved_command = 0U;
    bool command_disabled = false;
    uint32_t low = pci_config_read32(segment, device->bus, device->slot,
                                     device->function, offset);
    if (low == UINT32_MAX || low == 0U) return true;

    /* 只有关闭 I/O 和内存解码时，PCI BAR 大小探测才有定义。固件常为启动介质访问保持
     * xHCI 启用；探测活动 BAR 可能返回其当前地址，而不是掩码。 */
    saved_command = pci_config_read32(segment, device->bus, device->slot,
                                      device->function, 4U);
    if (saved_command != UINT32_MAX &&
        pci_config_write32(segment, device->bus, device->slot, device->function,
                           4U, saved_command & ~0x3U) == K_OK) {
        command_disabled = true;
    }

    uint32_t original_high = 0;
    bool memory = (low & 1U) == 0;
    bool is_64 = memory && bar_is_memory64(low) && index + 1U < PCI_MAX_BARS;
    if (is_64) {
        original_high = pci_config_read32(segment, device->bus, device->slot,
                                          device->function, (uint16_t)(offset + 4U));
    }
    uint32_t mask_low = 0;
    uint32_t mask_high = 0;
    if (pci_config_write32(segment, device->bus, device->slot, device->function,
                           offset, UINT32_MAX) == K_OK) {
        mask_low = pci_config_read32(segment, device->bus, device->slot,
                                     device->function, offset);
        if (is_64 && pci_config_write32(segment, device->bus, device->slot,
                                        device->function, (uint16_t)(offset + 4U),
                                        UINT32_MAX) == K_OK) {
            mask_high = pci_config_read32(segment, device->bus, device->slot,
                                          device->function, (uint16_t)(offset + 4U));
        }
    }
    (void)pci_config_write32(segment, device->bus, device->slot,
                             device->function, offset, low);
    if (is_64) {
        (void)pci_config_write32(segment, device->bus, device->slot,
                                 device->function, (uint16_t)(offset + 4U),
                                 original_high);
    }
    if (command_disabled) {
        (void)pci_config_write32(segment, device->bus, device->slot,
                                 device->function, 4U, saved_command);
    }

    pci_bar_t *bar = &device->bars[index];
    bar->flags = memory ? PCI_RESOURCE_MEMORY : PCI_RESOURCE_IO;
    if (is_64) bar->flags |= PCI_RESOURCE_64BIT;
    if (memory && (low & (1U << 3)) != 0) bar->flags |= PCI_RESOURCE_PREFETCH;
    uint64_t address = memory ? (uint64_t)(low & ~0xFU) : (uint64_t)(low & ~3U);
    if (is_64) {
        address |= (uint64_t)original_high << 32;
    }
    uint64_t length = pci_bar_length_from_masks(mask_low, mask_high,
                                                memory, is_64);
    /* 部分固件会将未分配的 64 位 BAR 留在物理地址空间顶部。将此类 BAR 视为不存在，
     * 避免其回绕后的范围导致其他设备枚举中止。 */
    if (length != 0 && address > UINT64_MAX - (length - 1ULL)) {
        address = 0;
        length = 0;
    }
    bar->address = address;
    bar->length = length;
    return true;
}

static void read_msix(const x86_acpi_ecam_t *segment, pci_device_t *device) {
    /* Header Type 的第 7 位表示多功能设备；只有低 7 位决定头部布局。若将 0x80
     * 当作不支持的头部，多功能 AMD xHCI 功能的能力列表就会被隐藏。 */
    if ((device->status & PCI_STATUS_CAP_LIST) == 0 ||
        (device->header_type & 0x7FU) != 0U) {
        return;
    }
    uint8_t offset = (uint8_t)(pci_config_read32(segment, device->bus, device->slot,
                                                 device->function, 0x34U) & 0xFFU);
    for (uint32_t count = 0; offset >= 0x40U && count < 48U; ++count) {
        uint32_t header = pci_config_read32(segment, device->bus, device->slot,
                                            device->function, offset);
        uint8_t next = (uint8_t)((header >> 8) & 0xFFU);
        if ((header & 0xFFU) == PCI_CAP_ID_MSIX) {
            uint32_t control = (header >> 16) & 0xFFFFU;
            uint32_t table = pci_config_read32(segment, device->bus, device->slot,
                                               device->function, (uint16_t)(offset + 4U));
            device->msix_capability = offset;
            device->msix_table_size = (uint16_t)((control & 0x7FFU) + 1U);
            device->msix_table_bar = (uint8_t)(table & 7U);
            device->msix_table_offset = table & ~7U;
            return;
        }
        if (next == offset) break;
        offset = next;
    }
}

static void read_msi(const x86_acpi_ecam_t *segment, pci_device_t *device) {
    if ((device->status & PCI_STATUS_CAP_LIST) == 0 ||
        (device->header_type & 0x7FU) != 0U) {
        return;
    }
    uint8_t offset = (uint8_t)(pci_config_read32(segment, device->bus, device->slot,
                                                 device->function, 0x34U) & 0xFFU);
    for (uint32_t count = 0; offset >= 0x40U && count < 48U; ++count) {
        uint32_t header = pci_config_read32(segment, device->bus, device->slot,
                                            device->function, offset);
        uint8_t next = (uint8_t)((header >> 8) & 0xFFU);
        if ((header & 0xFFU) == PCI_CAP_ID_MSI) {
            device->msi_capability = offset;
            return;
        }
        if (next == offset) break;
        offset = next;
    }
}

static bool enumerate_function(pci_host_t *host, const x86_acpi_ecam_t *segment,
                               uint8_t bus, uint8_t slot, uint8_t function) {
    uint32_t id = pci_config_read32(segment, bus, slot, function, 0);
    if ((id & 0xFFFFU) == PCI_VENDOR_INVALID || id == UINT32_MAX) return true;
    if (host->device_count >= PCI_MAX_DEVICES) return false;

    pci_device_t *device = &host->devices[host->device_count];
    bytes_zero(device, sizeof(*device));
    uint32_t class_data = pci_config_read32(segment, bus, slot, function, 8U);
    uint32_t header_data = pci_config_read32(segment, bus, slot, function, 0x0CU);
    uint32_t command_status = pci_config_read32(segment, bus, slot, function, 4U);
    if (class_data == UINT32_MAX || header_data == UINT32_MAX ||
        command_status == UINT32_MAX) return false;

    device->segment = segment->segment;
    device->bus = bus;
    device->slot = slot;
    device->function = function;
    device->ecam_start_bus = segment->start_bus;
    device->ecam_end_bus = segment->end_bus;
    device->vendor_id = (uint16_t)id;
    device->device_id = (uint16_t)(id >> 16);
    device->revision = (uint8_t)class_data;
    device->prog_if = (uint8_t)(class_data >> 8);
    device->subclass = (uint8_t)(class_data >> 16);
    device->class_code = (uint8_t)(class_data >> 24);
    device->header_type = (uint8_t)(header_data >> 16);
    device->command = (uint16_t)command_status;
    device->status = (uint16_t)(command_status >> 16);
    for (uint32_t bar = 0; bar < PCI_MAX_BARS; ++bar) {
        if (!read_bar(segment, device, bar)) return false;
        if (bar_is_memory64(pci_config_read32(segment, bus, slot, function,
                                              (uint16_t)(0x10U + bar * 4U)))) ++bar;
    }
    read_msix(segment, device);
    read_msi(segment, device);

    device->device.device_id = ((uint64_t)device->segment << 32) |
                               ((uint64_t)bus << 16) |
                               ((uint64_t)slot << 8) | function;
    device_object_init(&device->device, device->device.device_id,
                       ((uint32_t)device->class_code << 16) |
                       ((uint32_t)device->subclass << 8) | device->prog_if,
                       0, device);
    for (uint32_t bar = 0; bar < PCI_MAX_BARS; ++bar) {
        device->resources[bar].type =
            (device->bars[bar].flags & PCI_RESOURCE_IO) != 0 ?
            PCI_RESOURCE_IO : PCI_RESOURCE_MEMORY;
        device->resources[bar].flags = device->bars[bar].flags;
        device->resources[bar].start = device->bars[bar].address;
        device->resources[bar].length = device->bars[bar].length;
    }
    device->device.resources = device->resources;
    device->device.resource_count = PCI_MAX_BARS;
    if (iommu_attach_pci_device(&device->device, device->segment, device->bus,
                                device->slot, device->function) != K_OK) {
        return false;
    }
    ++host->device_count;
    if (device_register(&device->device) != K_OK) return false;
    return true;
}

kstatus_t pci_ecam_init(pci_host_t *host) {
    g_pci_last_error = 0;
    g_pci_last_map_status = K_OK;
    if (host == 0 || host->initialized) {
        g_pci_last_error = 1U;
        return K_EINVAL;
    }
    const x86_acpi_platform_t *platform = x86_acpi_platform();
    if (platform == 0 || platform->ecam_count == 0) {
        g_pci_last_error = 2U;
        return K_ENOENT;
    }
    bytes_zero(host, sizeof(*host));
    host->ecam_count = platform->ecam_count;
    for (uint32_t i = 0; i < host->ecam_count; ++i) host->ecam[i] = platform->ecam[i];

    for (uint32_t segment_index = 0; segment_index < host->ecam_count; ++segment_index) {
        const x86_acpi_ecam_t *segment = &host->ecam[segment_index];
        for (uint32_t bus = segment->start_bus; bus <= segment->end_bus; ++bus) {
            for (uint32_t slot = 0; slot < 32U; ++slot) {
                uint32_t id = pci_config_read32(segment, (uint8_t)bus, (uint8_t)slot, 0, 0);
                if (id == UINT32_MAX && g_pci_last_map_status != K_OK) {
                    g_pci_last_error = 0x100U | (uint32_t)(-g_pci_last_map_status);
                    return K_EIO;
                }
                if ((id & 0xFFFFU) == PCI_VENDOR_INVALID || id == UINT32_MAX) continue;
                uint32_t header = pci_config_read32(segment, (uint8_t)bus, (uint8_t)slot, 0, 0x0CU);
                if (header == UINT32_MAX) {
                    g_pci_last_error = 3U;
                    return K_EIO;
                }
                uint32_t functions = ((header >> 16) & 0x80U) != 0 ? 8U : 1U;
                for (uint32_t function = 0; function < functions; ++function) {
                    if (!enumerate_function(host, segment, (uint8_t)bus,
                                             (uint8_t)slot, (uint8_t)function)) {
                        g_pci_last_error = 4U;
                        return K_EIO;
                    }
                }
            }
            if (bus == 255U) break;
        }
    }
    host->initialized = true;
    return K_OK;
}

const pci_device_t *pci_find_device(const pci_host_t *host, uint16_t vendor_id,
                                    uint16_t device_id) {
    if (host == 0 || !host->initialized) return 0;
    for (uint32_t i = 0; i < host->device_count; ++i) {
        const pci_device_t *device = &host->devices[i];
        if (device->vendor_id == vendor_id && device->device_id == device_id) return device;
    }
    return 0;
}

const pci_device_t *pci_find_class(const pci_host_t *host, uint8_t class_code,
                                   uint8_t subclass, uint8_t prog_if) {
    if (host == 0 || !host->initialized) return 0;
    for (uint32_t i = 0; i < host->device_count; ++i) {
        const pci_device_t *device = &host->devices[i];
        if (device->class_code == class_code && device->subclass == subclass &&
            (prog_if == 0xFFU || device->prog_if == prog_if)) return device;
    }
    return 0;
}

kstatus_t pci_msix_table(const pci_device_t *device, paddr_t *physical,
                         uint16_t *entry_count) {
    if (device == 0 || physical == 0 || entry_count == 0 ||
        device->msix_capability == 0 || device->msix_table_size == 0 ||
        device->msix_table_bar >= PCI_MAX_BARS) return K_ENOENT;
    const pci_bar_t *bar = &device->bars[device->msix_table_bar];
    uint64_t bytes = (uint64_t)device->msix_table_size * 16ULL;
    if ((bar->flags & PCI_RESOURCE_MEMORY) == 0 || bar->length < bytes ||
        device->msix_table_offset > bar->length - bytes ||
        bar->address > UINT64_MAX - device->msix_table_offset) return K_EIO;
    *physical = paddr_make(bar->address + device->msix_table_offset);
    *entry_count = device->msix_table_size;
    return K_OK;
}

kstatus_t pci_msix_read_entry(const pci_device_t *device, uint16_t entry,
                              uint64_t *message_address,
                              uint32_t *message_data,
                              uint32_t *vector_control) {
    paddr_t table;
    uint16_t entry_count;
    kstatus_t status;
    uint64_t physical;
    uint64_t page;
    uint32_t offset;
    paddr_t root;
    volatile uint8_t *mapped;

    if (message_address == 0 || message_data == 0 || vector_control == 0) {
        return K_EINVAL;
    }
    status = pci_msix_table(device, &table, &entry_count);
    if (status != K_OK) return status;
    if (entry >= entry_count || table.value > UINT64_MAX -
        (uint64_t)entry * 16ULL) return K_EINVAL;
    physical = table.value + (uint64_t)entry * 16ULL;
    page = physical & ~(uint64_t)(PAGE_SIZE - 1ULL);
    offset = (uint32_t)(physical & (PAGE_SIZE - 1ULL));
    if (offset + 16U > PAGE_SIZE) return K_EIO;
    root = x86_current_root_table();
    if (x86_map_page(root, (vaddr_t)PCI_MSIX_WINDOW_VA, paddr_make(page),
                     X86_PAGE_WRITE | X86_PAGE_GLOBAL, X86_CACHE_UC) != K_OK) {
        return K_EIO;
    }
    mapped = (volatile uint8_t *)(uintptr_t)PCI_MSIX_WINDOW_VA + offset;
    *message_address = *(volatile uint64_t *)mapped;
    *message_data = *(volatile uint32_t *)(mapped + 8U);
    *vector_control = *(volatile uint32_t *)(mapped + 12U);
    __asm__ volatile ("mfence" : : : "memory");
    (void)x86_unmap_page(root, (vaddr_t)PCI_MSIX_WINDOW_VA, 0);
    return K_OK;
}

static bool pci_device_segment(const pci_device_t *device,
                               x86_acpi_ecam_t *segment) {
    if (device == 0 || segment == 0 || device->ecam_start_bus > device->ecam_end_bus) {
        return false;
    }
    segment->base = 0;
    segment->segment = device->segment;
    segment->start_bus = device->ecam_start_bus;
    segment->end_bus = device->ecam_end_bus;
    for (uint32_t i = 0; i < g_pci_host.ecam_count; ++i) {
        const x86_acpi_ecam_t *candidate = &g_pci_host.ecam[i];
        if (candidate->segment == device->segment &&
            candidate->start_bus == device->ecam_start_bus &&
            candidate->end_bus == device->ecam_end_bus) {
            *segment = *candidate;
            return true;
        }
    }
    return false;
}

static kstatus_t pci_device_config_read32(const pci_device_t *device,
                                          uint16_t offset, uint32_t *value) {
    if (value == 0) return K_EINVAL;
    x86_acpi_ecam_t segment;
    if (!pci_device_segment(device, &segment)) return K_ENOENT;
    uint32_t read = pci_config_read32(&segment, device->bus, device->slot,
                                      device->function, offset);
    if (read == UINT32_MAX) return K_EIO;
    *value = read;
    return K_OK;
}

static kstatus_t pci_device_config_write32(const pci_device_t *device,
                                           uint16_t offset, uint32_t value) {
    x86_acpi_ecam_t segment;
    if (!pci_device_segment(device, &segment)) return K_ENOENT;
    return pci_config_write32(&segment, device->bus, device->slot,
                             device->function, offset, value);
}

kstatus_t pci_read_config32(const pci_device_t *device, uint16_t offset,
                             uint32_t *value) {
    if (value == 0 || offset >= PCI_CONFIG_SPACE_SIZE ||
        (offset & 3U) != 0U) return K_EINVAL;
    return pci_device_config_read32(device, offset, value);
}

kstatus_t pci_write_config32(const pci_device_t *device, uint16_t offset,
                              uint32_t value) {
    if (offset >= PCI_CONFIG_SPACE_SIZE || (offset & 3U) != 0U) {
        return K_EINVAL;
    }
    return pci_device_config_write32(device, offset, value);
}

kstatus_t pci_read_config16(const pci_device_t *device, uint16_t offset,
                             uint16_t *value) {
    uint32_t word;
    uint16_t aligned;
    if (value == 0 || offset >= PCI_CONFIG_SPACE_SIZE - 1U) return K_EINVAL;
    aligned = (uint16_t)(offset & ~3U);
    kstatus_t status = pci_read_config32(device, aligned, &word);
    if (status != K_OK) return status;
    *value = (uint16_t)(word >> ((offset & 2U) * 8U));
    return K_OK;
}

kstatus_t pci_read_config8(const pci_device_t *device, uint16_t offset,
                            uint8_t *value) {
    uint32_t word;
    if (value == 0 || offset >= PCI_CONFIG_SPACE_SIZE) return K_EINVAL;
    kstatus_t status = pci_read_config32(device, (uint16_t)(offset & ~3U),
                                          &word);
    if (status != K_OK) return status;
    *value = (uint8_t)(word >> ((offset & 3U) * 8U));
    return K_OK;
}

static kstatus_t pci_write_config_partial(const pci_device_t *device,
                                          uint16_t offset, uint32_t value,
                                          uint32_t mask) {
    uint32_t word;
    uint16_t aligned = (uint16_t)(offset & ~3U);
    kstatus_t status = pci_read_config32(device, aligned, &word);
    if (status != K_OK) return status;
    word = (word & ~mask) | (value & mask);
    return pci_write_config32(device, aligned, word);
}

kstatus_t pci_write_config16(const pci_device_t *device, uint16_t offset,
                              uint16_t value) {
    if (offset >= PCI_CONFIG_SPACE_SIZE - 1U) return K_EINVAL;
    uint32_t shift = (uint32_t)(offset & 2U) * 8U;
    return pci_write_config_partial(device, offset, (uint32_t)value << shift,
                                    0xFFFFU << shift);
}

kstatus_t pci_write_config8(const pci_device_t *device, uint16_t offset,
                             uint8_t value) {
    if (offset >= PCI_CONFIG_SPACE_SIZE) return K_EINVAL;
    uint32_t shift = (uint32_t)(offset & 3U) * 8U;
    return pci_write_config_partial(device, offset, (uint32_t)value << shift,
                                    0xFFU << shift);
}

static kstatus_t pci_set_intx_disabled(pci_device_t *device, bool disabled) {
    uint32_t command_status;
    uint16_t command;

    if (device == 0) return K_EINVAL;
    if (pci_device_config_read32(device, PCI_COMMAND_OFFSET,
                                 &command_status) != K_OK) {
        return K_EIO;
    }
    command = (uint16_t)command_status;
    if (disabled) command |= (uint16_t)PCI_COMMAND_INTX_DISABLE;
    else command &= (uint16_t)~PCI_COMMAND_INTX_DISABLE;
    /* 高 16 位是 PCI Status（写 1 清除）；绝不能将其写回。 */
    return pci_device_config_write32(device, PCI_COMMAND_OFFSET, command);
}

static kstatus_t pci_set_msi_enabled(pci_device_t *device, bool enabled) {
    uint32_t header;
    uint16_t control;

    if (device == 0 || device->msi_capability == 0U) return K_EINVAL;
    if (pci_device_config_read32(device, device->msi_capability,
                                 &header) != K_OK) return K_EIO;
    control = (uint16_t)(header >> 16);
    if (enabled) control |= 1U;
    else control &= (uint16_t)~1U;
    return pci_device_config_write32(
        device, device->msi_capability,
        (header & 0x0000FFFFU) | ((uint32_t)control << 16));
}

static kstatus_t pci_set_msix_enabled(pci_device_t *device, bool enabled,
                                       bool function_masked) {
    uint32_t header;
    uint16_t control;

    if (device == 0 || device->msix_capability == 0U) return K_EINVAL;
    if (pci_device_config_read32(device, device->msix_capability,
                                 &header) != K_OK) return K_EIO;
    control = (uint16_t)(header >> 16);
    if (enabled) control |= PCI_MSIX_ENABLE;
    else control &= (uint16_t)~PCI_MSIX_ENABLE;
    if (function_masked) control |= PCI_MSIX_FUNCTION_MASK;
    else control &= (uint16_t)~PCI_MSIX_FUNCTION_MASK;
    return pci_device_config_write32(
        device, device->msix_capability,
        (header & 0x0000FFFFU) | ((uint32_t)control << 16));
}

static kstatus_t pci_msix_write_entry(const pci_device_t *device, uint16_t entry,
                                      uint64_t message_address, uint32_t message_data,
                                      uint32_t vector_control) {
    paddr_t table;
    uint16_t entry_count;
    kstatus_t status = pci_msix_table(device, &table, &entry_count);
    if (status != K_OK || entry >= entry_count) return status != K_OK ? status : K_EINVAL;
    uint64_t entry_offset = (uint64_t)entry * 16ULL;
    if (table.value > UINT64_MAX - entry_offset) return K_EIO;
    uint64_t physical = table.value + entry_offset;
    uint64_t page = physical & ~(uint64_t)(PAGE_SIZE - 1ULL);
    uint32_t offset = (uint32_t)(physical & (PAGE_SIZE - 1ULL));
    paddr_t root = x86_current_root_table();
    if (x86_map_page(root, (vaddr_t)PCI_MSIX_WINDOW_VA, paddr_make(page),
                     X86_PAGE_WRITE | X86_PAGE_GLOBAL, X86_CACHE_UC) != K_OK) {
        return K_EIO;
    }
    uint32_t next_page = offset + 16U > PAGE_SIZE ? 1U : 0U;
    if (next_page != 0 && x86_map_page(root, (vaddr_t)(PCI_MSIX_WINDOW_VA + PAGE_SIZE),
                                        paddr_make(page + PAGE_SIZE),
                                        X86_PAGE_WRITE | X86_PAGE_GLOBAL,
                                        X86_CACHE_UC) != K_OK) {
        (void)x86_unmap_page(root, (vaddr_t)PCI_MSIX_WINDOW_VA, 0);
        return K_EIO;
    }
    volatile uint8_t *mapped = (volatile uint8_t *)(uintptr_t)PCI_MSIX_WINDOW_VA;
    volatile uint8_t *second = (volatile uint8_t *)(uintptr_t)(PCI_MSIX_WINDOW_VA + PAGE_SIZE);
    volatile uint8_t *entry_bytes = mapped + offset;
    if (next_page == 0) {
        *(volatile uint64_t *)entry_bytes = message_address;
        *(volatile uint32_t *)(entry_bytes + 8U) = message_data;
        *(volatile uint32_t *)(entry_bytes + 12U) = vector_control;
    } else {
        uint64_t values[2] = {message_address, (uint64_t)message_data |
                              ((uint64_t)vector_control << 32)};
        const uint8_t *bytes = (const uint8_t *)values;
        for (uint32_t i = 0; i < 16U; ++i) {
            if (offset + i < PAGE_SIZE) mapped[offset + i] = bytes[i];
            else second[offset + i - PAGE_SIZE] = bytes[i];
        }
    }
    __asm__ volatile ("mfence" : : : "memory");
    if (next_page != 0) (void)x86_unmap_page(root, (vaddr_t)(PCI_MSIX_WINDOW_VA + PAGE_SIZE), 0);
    (void)x86_unmap_page(root, (vaddr_t)PCI_MSIX_WINDOW_VA, 0);
    return K_OK;
}

kstatus_t pci_msix_mask(pci_device_t *device, uint16_t entry, bool masked) {
    if (device == 0 || device->msix_capability == 0 || entry >= device->msix_table_size) {
        return K_EINVAL;
    }
    paddr_t table;
    uint16_t entry_count;
    kstatus_t status = pci_msix_table(device, &table, &entry_count);
    if (status != K_OK || entry >= entry_count) return status != K_OK ? status : K_EINVAL;
    /* 用当前消息内容重写控制字，避免破坏设备已经配置的路由。 */
    uint64_t entry_offset = (uint64_t)entry * 16ULL;
    if (table.value > UINT64_MAX - entry_offset) return K_EIO;
    uint64_t physical = table.value + entry_offset + 12ULL;
    uint64_t page = physical & ~(uint64_t)(PAGE_SIZE - 1ULL);
    uint32_t offset = (uint32_t)(physical & (PAGE_SIZE - 1ULL));
    if (offset + sizeof(uint32_t) > PAGE_SIZE) return K_EIO;
    paddr_t root = x86_current_root_table();
    if (x86_map_page(root, (vaddr_t)PCI_MSIX_WINDOW_VA, paddr_make(page),
                     X86_PAGE_WRITE | X86_PAGE_GLOBAL, X86_CACHE_UC) != K_OK) return K_EIO;
    volatile uint32_t *control = (volatile uint32_t *)(uintptr_t)(PCI_MSIX_WINDOW_VA + offset);
    *control = masked ? PCI_MSIX_ENTRY_MASK : 0U;
    __asm__ volatile ("mfence" : : : "memory");
    (void)x86_unmap_page(root, (vaddr_t)PCI_MSIX_WINDOW_VA, 0);
    return K_OK;
}

kstatus_t pci_msix_configure(pci_device_t *device, uint16_t entry,
                             uint32_t apic_id, uint8_t vector) {
    if (device == 0 || device->msix_capability == 0 || vector < 32U ||
        apic_id > 0xFFU || entry >= device->msix_table_size) return K_EINVAL;
    kstatus_t status;

    /* 同一个功能不得同时启用 MSI 和 MSI-X。 */
    if (device->msi_capability != 0U &&
        pci_set_msi_enabled(device, false) != K_OK) return K_EIO;
    status = pci_set_msix_enabled(device, false, true);
    if (status != K_OK) return status;
    uint64_t message_address = 0xFEE00000ULL | ((uint64_t)apic_id << 12);
    status = pci_msix_write_entry(device, entry, message_address, vector,
                                  PCI_MSIX_ENTRY_MASK);
    if (status != K_OK) return status;
    status = pci_set_msix_enabled(device, true, true);
    if (status != K_OK) return status;
    status = pci_msix_write_entry(device, entry, message_address, vector, 0U);
    if (status != K_OK) return status;
    status = pci_set_msix_enabled(device, true, false);
    if (status != K_OK) return status;
    return pci_set_intx_disabled(device, true);
}

kstatus_t pci_msi_configure(pci_device_t *device, uint32_t apic_id,
                            uint8_t vector) {
    uint32_t header;
    uint16_t control;
    bool address_64;
    uint16_t data_offset;
    uint32_t address_low;
    if (device == 0 || device->msi_capability == 0 || apic_id > 0xFFU ||
        vector < IRQ_VECTOR_FIRST || vector > IRQ_VECTOR_LAST) return K_EINVAL;
    if (device->msix_capability != 0U &&
        pci_set_msix_enabled(device, false, true) != K_OK) return K_EIO;
    if (pci_device_config_read32(device, device->msi_capability, &header) != K_OK) {
        return K_EIO;
    }
    control = (uint16_t)(header >> 16);
    address_64 = (control & (1U << 7)) != 0;
    address_low = 0xFEE00000U | (apic_id << 12);
    if (pci_device_config_write32(device, device->msi_capability + 4U,
                                  address_low) != K_OK) return K_EIO;
    data_offset = (uint16_t)(device->msi_capability + (address_64 ? 12U : 8U));
    if (address_64 && pci_device_config_write32(device, device->msi_capability + 8U,
                                                 0U) != K_OK) return K_EIO;
    if (pci_device_config_write32(device, data_offset,
                                  (uint32_t)vector) != K_OK) return K_EIO;
    /* 为此功能启用一个 MSI 向量，并禁用传统 INTx 中断。 */
    control &= (uint16_t)~(0x7U << 4);
    control |= 1U;
    if (pci_device_config_write32(device, device->msi_capability,
                                  (header & 0x0000FFFFU) | ((uint32_t)control << 16)) != K_OK) {
        return K_EIO;
    }
    return pci_set_intx_disabled(device, true);
}

kstatus_t pci_msi_disable(pci_device_t *device) {
    uint32_t header;
    uint16_t control;

    if (device == 0 || device->msi_capability == 0U) return K_EINVAL;
    if (pci_device_config_read32(device, device->msi_capability, &header) != K_OK)
        return K_EIO;
    control = (uint16_t)(header >> 16);
    control &= (uint16_t)~1U;
    if (pci_device_config_write32(
            device, device->msi_capability,
            (header & 0x0000FFFFU) | ((uint32_t)control << 16)) != K_OK) {
        return K_EIO;
    }
    return pci_set_intx_disabled(device, false);
}

kstatus_t pci_msix_disable(pci_device_t *device) {
    kstatus_t status;

    if (device == 0 || device->msix_capability == 0U) return K_EINVAL;
    status = pci_set_msix_enabled(device, false, true);
    if (status != K_OK) return status;
    return pci_set_intx_disabled(device, false);
}

kstatus_t pci_enable_memory_busmaster(pci_device_t *device) {
    if (device == 0) return K_EINVAL;
    uint32_t command_status;
    kstatus_t status = pci_device_config_read32(device, 4U, &command_status);
    if (status != K_OK) return status;
    uint16_t command = (uint16_t)command_status;
    command |= (1U << 1) | (1U << 2);
    status = pci_device_config_write32(device, PCI_COMMAND_OFFSET, command);
    if (status == K_OK) device->command = command;
    return status;
}

const pci_host_t *pci_current_host(void) {
    return g_pci_host.initialized ? &g_pci_host : 0;
}

bool pci_ecam_self_test(void) {
    if (pci_bar_length_from_masks(0xFC000000U, 0U, true, false) !=
            0x04000000ULL ||
        pci_bar_length_from_masks(0xFFFFFF00U, 0U, false, false) !=
            0x00000100ULL ||
        pci_bar_length_from_masks(0xFFFFFFF0U, 0xFFFFFFFFU, true, true) !=
            0x0000000000000010ULL) {
        g_pci_last_error = 0x100U;
        return false;
    }
    if (pci_ecam_init(&g_pci_host) != K_OK || !g_pci_host.initialized ||
        g_pci_host.ecam_count == 0) {
        if (g_pci_last_error == 0U) g_pci_last_error = 0x200U;
        return false;
    }
    for (uint32_t i = 0; i < g_pci_host.device_count; ++i) {
        const pci_device_t *device = &g_pci_host.devices[i];
        if (device->vendor_id == PCI_VENDOR_INVALID ||
            device->device.resources != device->resources ||
            device->device.resource_count != PCI_MAX_BARS) {
            g_pci_last_error = 0x300U | (i & 0xFFU);
            return false;
        }
        for (uint32_t bar = 0; bar < PCI_MAX_BARS; ++bar) {
            if (device->bars[bar].length != 0 &&
                device->bars[bar].address > UINT64_MAX -
                (device->bars[bar].length - 1ULL)) {
                g_pci_last_error = 0x400U | ((i & 0x1FU) << 3) | bar;
                return false;
            }
        }
        if (device->msix_capability != 0) {
            paddr_t table;
            uint16_t entries;
            if (pci_msix_table(device, &table, &entries) != K_OK ||
                table.value == 0 || entries != device->msix_table_size) {
                /* MSI-X 是可选功能。部分固件会在其他方面可用的设备上暴露过期能力；
                 * 保持设备可枚举，并让其驱动选择其他中断路径。 */
                g_pci_last_error = 0x500U | (i & 0xFFU);
                /* 为专用 MSI-X 诊断保留 xHCI 原始能力；在确认是表还是配置写入失败前，
                 * 不得静默降低控制器的能力。 */
                if (device->class_code != 0x0CU || device->subclass != 0x03U ||
                    device->prog_if != 0x30U) {
                    ((pci_device_t *)device)->msix_capability = 0U;
                    ((pci_device_t *)device)->msix_table_size = 0U;
                    ((pci_device_t *)device)->msix_table_bar = 0U;
                    ((pci_device_t *)device)->msix_table_offset = 0U;
                }
            }
        }
    }
    return true;
}

uint32_t pci_ecam_last_error(void) {
    return g_pci_last_error;
}
