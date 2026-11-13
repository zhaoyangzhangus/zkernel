#include <arch/x86_64/acpi.h>
#include <arch/x86_64/paging.h>
#include <kernel/mm.h>
#include <kernel/mm_boot.h>

#include <stdint.h>

#define ACPI_SDT_HEADER_SIZE 36U
#define ACPI_TABLE_MAX_SIZE  (1024U * 1024U)

typedef struct __attribute__((packed)) {
    char signature[8];
    uint8_t checksum;
    char oem_id[6];
    uint8_t revision;
    uint32_t rsdt_address;
    uint32_t length;
    uint64_t xsdt_address;
    uint8_t extended_checksum;
    uint8_t reserved[3];
} acpi_rsdp_t;

typedef struct __attribute__((packed)) {
    char signature[4];
    uint32_t length;
    uint8_t revision;
    uint8_t checksum;
    char oem_id[6];
    char oem_table_id[8];
    uint32_t oem_revision;
    uint32_t creator_id;
    uint32_t creator_revision;
} acpi_sdt_header_t;

typedef struct __attribute__((packed)) {
    uint8_t space_id;
    uint8_t bit_width;
    uint8_t bit_offset;
    uint8_t access_size;
    uint64_t address;
} acpi_generic_address_t;

typedef struct __attribute__((packed)) {
    acpi_sdt_header_t header;
    uint32_t lapic_address;
    uint32_t flags;
    uint8_t entries[];
} acpi_madt_t;

typedef struct __attribute__((packed)) {
    acpi_sdt_header_t header;
    uint64_t reserved;
    uint8_t entries[];
} acpi_mcfg_t;

typedef struct __attribute__((packed)) {
    uint64_t base;
    uint16_t segment;
    uint8_t start_bus;
    uint8_t end_bus;
    uint32_t reserved;
} acpi_mcfg_entry_t;

typedef struct __attribute__((packed)) {
    acpi_sdt_header_t header;
    uint8_t host_address_width;
    uint8_t flags;
    uint8_t reserved[10];
    uint8_t entries[];
} acpi_dmar_t;

typedef struct __attribute__((packed)) {
    uint16_t type;
    uint16_t length;
} acpi_dmar_header_t;

typedef struct __attribute__((packed)) {
    acpi_dmar_header_t header;
    uint8_t flags;
    uint8_t reserved;
    uint16_t segment;
    uint64_t register_base;
} acpi_dmar_drhd_t;

/* ACPI FADT 的固定字段；只声明到我们实际使用的 Flags。 */
typedef struct __attribute__((packed)) {
    acpi_sdt_header_t header;
    uint32_t firmware_control;
    uint32_t dsdt;
    uint8_t reserved0;
    uint8_t preferred_pm_profile;
    uint16_t sci_interrupt;
    uint32_t smi_command;
    uint8_t acpi_enable;
    uint8_t acpi_disable;
    uint8_t s4bios_request;
    uint8_t pstate_control;
    uint32_t pm1a_event_block;
    uint32_t pm1b_event_block;
    uint32_t pm1a_control_block;
    uint32_t pm1b_control_block;
    uint32_t pm2_control_block;
    uint32_t pm_timer_block;
    uint32_t gpe0_block;
    uint32_t gpe1_block;
    uint8_t pm1_event_length;
    uint8_t pm1_control_length;
    uint8_t pm2_control_length;
    uint8_t pm_timer_length;
    uint8_t gpe0_block_length;
    uint8_t gpe1_block_length;
    uint8_t gpe1_base;
    uint8_t cstate_control;
    uint16_t worst_c2_latency;
    uint16_t worst_c3_latency;
    uint16_t flush_size;
    uint16_t flush_stride;
    uint8_t duty_offset;
    uint8_t duty_width;
    uint8_t day_alarm;
    uint8_t month_alarm;
    uint8_t century;
    uint16_t iapc_boot_arch;
    uint8_t reserved1;
    uint32_t flags;
    acpi_generic_address_t reset_register;
    uint8_t reset_value;
} acpi_fadt_t;

_Static_assert(__builtin_offsetof(acpi_fadt_t, pm1a_control_block) == 64U,
               "ACPI FADT PM1a control offset");
_Static_assert(__builtin_offsetof(acpi_fadt_t, pm1_control_length) == 89U,
               "ACPI FADT PM1 control length offset");
_Static_assert(__builtin_offsetof(acpi_fadt_t, reset_register) == 116U,
               "ACPI FADT reset register offset");
_Static_assert(__builtin_offsetof(acpi_fadt_t, reset_value) == 128U,
               "ACPI FADT reset value offset");

static bool parse_aml_integer(const uint8_t **cursor, const uint8_t *end,
                              uint8_t *value) {
    const uint8_t *current;
    uint64_t number = 0U;
    uint32_t bytes;
    if (cursor == 0 || *cursor == 0 || value == 0) return false;
    current = *cursor;
    if (current >= end) return false;
    switch (*current++) {
        case 0x00U: number = 0U; break;
        case 0x01U: number = 1U; break;
        case 0x0AU: if (current >= end) return false; number = *current++; break;
        case 0x0BU: bytes = 2U; goto read_bytes;
        case 0x0CU: bytes = 4U; goto read_bytes;
        case 0x0EU: bytes = 8U; goto read_bytes;
        default: return false;
    }
    if (number > 7U) return false;
    *value = (uint8_t)number;
    *cursor = current;
    return true;

read_bytes:
    if ((size_t)(end - current) < bytes) return false;
    for (uint32_t index = 0U; index < bytes; ++index) {
        number |= (uint64_t)current[index] << (index * 8U);
    }
    if (number > 7U) return false;
    current += bytes;
    *value = (uint8_t)number;
    *cursor = current;
    return true;
}

static bool parse_dsdt_sleep(const acpi_sdt_header_t *header,
                             uint8_t *s3, uint8_t *s4) {
    const uint8_t *cursor;
    const uint8_t *end;
    if (header == 0 || s3 == 0 || s4 == 0 || header->length < ACPI_SDT_HEADER_SIZE) {
        return false;
    }
    cursor = (const uint8_t *)header + ACPI_SDT_HEADER_SIZE;
    end = (const uint8_t *)header + header->length;
    while ((size_t)(end - cursor) >= 6U) {
        if (cursor[0] == 0x08U && cursor[1] == '_' && cursor[2] == 'S' &&
            cursor[3] == '5' && cursor[4] == '_') {
            const uint8_t *package = cursor + 5U;
            if (package < end && *package++ == 0x12U && package < end) {
                uint8_t package_length = *package++;
                uint32_t extra = (package_length >> 6) & 3U;
                uint32_t length = package_length & 0x3FU;
                if ((size_t)(end - package) < extra) return false;
                for (uint32_t index = 0U; index < extra; ++index) {
                    length |= (uint32_t)package[index] << (6U + index * 8U);
                }
                package += extra;
                if (length > (size_t)(end - package) || package >= end) return false;
                const uint8_t *package_end = package + length;
                if (package < package_end) {
                    uint8_t elements = *package++;
                    if (elements >= 2U && parse_aml_integer(&package, package_end, s3) &&
                        parse_aml_integer(&package, package_end, s4)) return true;
                }
            }
        }
        ++cursor;
    }
    return false;
}

static x86_acpi_platform_t g_platform;
static bool g_discovered;

static const acpi_sdt_header_t *map_sdt(uint64_t physical);

static void parse_fadt(const acpi_sdt_header_t *header) {
    const acpi_fadt_t *fadt;
    const acpi_sdt_header_t *dsdt;
    uint8_t s3 = 0U;
    uint8_t s4 = 0U;
    if (header == 0 ||
        header->length < __builtin_offsetof(acpi_fadt_t, flags) +
                          sizeof(fadt->flags)) return;
    fadt = (const acpi_fadt_t *)header;
    if (header->length >= __builtin_offsetof(acpi_fadt_t, reset_value) +
                              sizeof(fadt->reset_value) &&
        (fadt->reset_register.space_id == 0U ||
         fadt->reset_register.space_id == 1U) &&
        fadt->reset_register.address != 0U &&
        fadt->reset_register.bit_offset == 0U &&
        (fadt->reset_register.bit_width == 8U ||
         fadt->reset_register.bit_width == 16U ||
         fadt->reset_register.bit_width == 32U) &&
        (fadt->reset_register.access_size == 0U ||
         fadt->reset_register.access_size <= 3U)) {
        g_platform.reset_address = fadt->reset_register.address;
        g_platform.reset_space_id = fadt->reset_register.space_id;
        g_platform.reset_bit_width = fadt->reset_register.bit_width;
        g_platform.reset_access_size = fadt->reset_register.access_size;
        g_platform.reset_value = fadt->reset_value;
        g_platform.reset_supported = true;
    }
    if (fadt->pm1a_control_block > UINT16_MAX || fadt->pm1_control_length < 2U) return;
    dsdt = map_sdt(fadt->dsdt);
    if (dsdt == 0 || !parse_dsdt_sleep(dsdt, &s3, &s4)) return;
    g_platform.pm1a_control_block = (uint16_t)fadt->pm1a_control_block;
    g_platform.pm1b_control_block = fadt->pm1b_control_block <= UINT16_MAX ?
                                     (uint16_t)fadt->pm1b_control_block : 0U;
    g_platform.pm1_control_length = fadt->pm1_control_length;
    g_platform.sleep_type_s3 = s3;
    g_platform.sleep_type_s4 = s4;
    g_platform.sleep_supported = true;
}

static void bytes_zero(void *memory, size_t size) {
    uint8_t *bytes = (uint8_t *)memory;
    while (size-- != 0) *bytes++ = 0;
}

static bool bytes_equal(const char *left, const char *right, size_t size) {
    while (size-- != 0) {
        if (*left++ != *right++) return false;
    }
    return true;
}

static bool checksum_valid(const void *memory, size_t size) {
    const uint8_t *bytes = (const uint8_t *)memory;
    uint8_t sum = 0;
    while (size-- != 0) sum = (uint8_t)(sum + *bytes++);
    return sum == 0;
}

static const void *map_ram(uint64_t physical, size_t size) {
    if (!direct_map_range_is_ram(paddr_make(physical), size)) return 0;
    return phys_to_direct(paddr_make(physical));
}

static const acpi_sdt_header_t *map_sdt(uint64_t physical) {
    const acpi_sdt_header_t *header =
        (const acpi_sdt_header_t *)map_ram(physical, sizeof(acpi_sdt_header_t));
    if (header == 0 || header->length < ACPI_SDT_HEADER_SIZE ||
        header->length > ACPI_TABLE_MAX_SIZE) return 0;
    header = (const acpi_sdt_header_t *)map_ram(physical, header->length);
    return header != 0 && checksum_valid(header, header->length) ? header : 0;
}

static bool cpu_known(uint32_t apic_id) {
    for (uint32_t i = 0; i < g_platform.cpu_count; ++i) {
        if (g_platform.cpus[i].apic_id == apic_id) return true;
    }
    return false;
}

static void add_cpu(uint32_t apic_id, uint32_t uid, bool x2apic, uint32_t flags) {
    if ((flags & 1U) == 0 || cpu_known(apic_id) || g_platform.cpu_count >= MAX_CPUS) return;
    x86_acpi_cpu_t *cpu = &g_platform.cpus[g_platform.cpu_count++];
    cpu->apic_id = apic_id;
    cpu->acpi_uid = uid;
    cpu->x2apic = x2apic;
    cpu->enabled = true;
}

static bool parse_madt(const acpi_sdt_header_t *header) {
    if (header->length < sizeof(acpi_madt_t)) return false;
    const acpi_madt_t *madt = (const acpi_madt_t *)header;
    g_platform.lapic_address = madt->lapic_address;
    g_platform.flags = madt->flags;
    const uint8_t *cursor = madt->entries;
    const uint8_t *end = (const uint8_t *)madt + madt->header.length;
    while (cursor < end) {
        if ((size_t)(end - cursor) < 2U || cursor[1] < 2U || cursor[1] > end - cursor) {
            return false;
        }
        uint8_t type = cursor[0];
        uint8_t length = cursor[1];
        if (type == 0U && length >= 8U) {
            uint32_t flags = *(const uint32_t *)(cursor + 4U);
            add_cpu(cursor[3], cursor[2], false, flags);
        } else if (type == 1U && length >= 12U &&
                   g_platform.ioapic_count < X86_MAX_IOAPICS) {
            x86_acpi_ioapic_t *ioapic =
                &g_platform.ioapics[g_platform.ioapic_count++];
            ioapic->id = cursor[2];
            ioapic->address = *(const uint32_t *)(cursor + 4U);
            ioapic->gsi_base = *(const uint32_t *)(cursor + 8U);
        } else if (type == 5U && length >= 12U) {
            g_platform.lapic_address = *(const uint64_t *)(cursor + 4U);
        } else if (type == 9U && length >= 16U) {
            uint32_t apic_id = *(const uint32_t *)(cursor + 4U);
            uint32_t flags = *(const uint32_t *)(cursor + 8U);
            uint32_t uid = *(const uint32_t *)(cursor + 12U);
            add_cpu(apic_id, uid, true, flags);
        }
        cursor += length;
    }
    return cursor == end && g_platform.cpu_count != 0 && g_platform.lapic_address != 0;
}

static bool parse_mcfg(const acpi_sdt_header_t *header) {
    if (header->length < sizeof(acpi_mcfg_t)) return false;
    const uint8_t *cursor = ((const acpi_mcfg_t *)header)->entries;
    const uint8_t *end = (const uint8_t *)header + header->length;
    while ((size_t)(end - cursor) >= sizeof(acpi_mcfg_entry_t) &&
           g_platform.ecam_count < X86_MAX_ECAM_SEGMENTS) {
        const acpi_mcfg_entry_t *source = (const acpi_mcfg_entry_t *)cursor;
        if (source->base != 0 && source->start_bus <= source->end_bus) {
            x86_acpi_ecam_t *target = &g_platform.ecam[g_platform.ecam_count++];
            target->base = source->base;
            target->segment = source->segment;
            target->start_bus = source->start_bus;
            target->end_bus = source->end_bus;
        }
        cursor += sizeof(*source);
    }
    return cursor == end;
}

static bool parse_dmar(const acpi_sdt_header_t *header) {
    if (header->length < sizeof(acpi_dmar_t)) return false;
    const acpi_dmar_t *dmar = (const acpi_dmar_t *)header;
    const uint8_t *cursor = dmar->entries;
    const uint8_t *end = (const uint8_t *)header + header->length;
    while (cursor < end) {
        if ((size_t)(end - cursor) < sizeof(acpi_dmar_header_t)) return false;
        const acpi_dmar_header_t *entry = (const acpi_dmar_header_t *)cursor;
        if (entry->length < sizeof(*entry) || entry->length > (size_t)(end - cursor)) {
            return false;
        }
        if (entry->type == 0U && entry->length >= sizeof(acpi_dmar_drhd_t) &&
            g_platform.iommu_count < X86_MAX_IOMMUS) {
            const acpi_dmar_drhd_t *drhd = (const acpi_dmar_drhd_t *)cursor;
            if (drhd->register_base == 0) return false;
            x86_acpi_iommu_t *iommu = &g_platform.iommus[g_platform.iommu_count++];
            iommu->base = drhd->register_base;
            iommu->segment = drhd->segment;
            iommu->flags = drhd->flags;
        }
        cursor += entry->length;
    }
    return cursor == end;
}

static bool parse_root(const acpi_sdt_header_t *root, bool xsdt) {
    size_t entry_size = xsdt ? sizeof(uint64_t) : sizeof(uint32_t);
    size_t payload = root->length - sizeof(*root);
    if (payload % entry_size != 0) return false;
    const uint8_t *entries = (const uint8_t *)root + sizeof(*root);
    const acpi_sdt_header_t *madt = 0;
    const acpi_sdt_header_t *mcfg = 0;
    const acpi_sdt_header_t *dmar = 0;
    const acpi_sdt_header_t *fadt = 0;
    for (size_t offset = 0; offset < payload; offset += entry_size) {
        uint64_t physical = xsdt ? *(const uint64_t *)(entries + offset) :
                                   *(const uint32_t *)(entries + offset);
        const acpi_sdt_header_t *table = map_sdt(physical);
        if (table == 0) continue;
        if (bytes_equal(table->signature, "APIC", 4U)) madt = table;
        else if (bytes_equal(table->signature, "MCFG", 4U)) mcfg = table;
        else if (bytes_equal(table->signature, "DMAR", 4U)) dmar = table;
        else if (bytes_equal(table->signature, "FACP", 4U)) fadt = table;
    }
    if (madt == 0 || !parse_madt(madt)) return false;
    if (mcfg != 0 && !parse_mcfg(mcfg)) return false;
    if (dmar != 0 && !parse_dmar(dmar)) return false;
    if (fadt != 0) parse_fadt(fadt);
    return true;
}

static uint32_t boot_apic_id(void) {
    uint32_t eax;
    uint32_t ebx;
    uint32_t ecx;
    uint32_t edx;
    __asm__ volatile ("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
                      : "a"(1U), "c"(0U));
    (void)eax;
    (void)ecx;
    (void)edx;
    return ebx >> 24;
}

bool x86_acpi_discover(const OS_BOOT_INFO *boot_info) {
    if (g_discovered || boot_info == 0 || boot_info->AcpiRsdp == 0) return false;
    bytes_zero(&g_platform, sizeof(g_platform));
    const acpi_rsdp_t *rsdp =
        (const acpi_rsdp_t *)map_ram(boot_info->AcpiRsdp, 20U);
    if (rsdp == 0 || !bytes_equal(rsdp->signature, "RSD PTR ", 8U) ||
        !checksum_valid(rsdp, 20U)) return false;

    const acpi_sdt_header_t *root = 0;
    bool xsdt = false;
    if (rsdp->revision >= 2U) {
        rsdp = (const acpi_rsdp_t *)map_ram(boot_info->AcpiRsdp, sizeof(*rsdp));
        if (rsdp == 0 || rsdp->length < sizeof(*rsdp) ||
            rsdp->length > 4096U ||
            map_ram(boot_info->AcpiRsdp, rsdp->length) == 0 ||
            !checksum_valid(rsdp, rsdp->length)) return false;
        if (rsdp->xsdt_address != 0) {
            root = map_sdt(rsdp->xsdt_address);
            xsdt = root != 0 && bytes_equal(root->signature, "XSDT", 4U);
        }
    }
    if (root == 0 && rsdp->rsdt_address != 0) {
        root = map_sdt(rsdp->rsdt_address);
        xsdt = false;
        if (root == 0 || !bytes_equal(root->signature, "RSDT", 4U)) return false;
    }
    if (root == 0 || !parse_root(root, xsdt)) return false;

    g_platform.bsp_apic_id = boot_apic_id();
    bool bsp_found = false;
    for (uint32_t i = 0; i < g_platform.cpu_count; ++i) {
        if (g_platform.cpus[i].apic_id == g_platform.bsp_apic_id) bsp_found = true;
    }
    g_discovered = bsp_found;
    return g_discovered;
}

const x86_acpi_platform_t *x86_acpi_platform(void) {
    return g_discovered ? &g_platform : 0;
}

bool x86_acpi_sleep_supported(void) {
    return g_discovered && g_platform.sleep_supported;
}

static uint8_t reset_access_bytes(const x86_acpi_platform_t *platform) {
    if (platform == 0) return 0U;
    if (platform->reset_access_size != 0U) {
        if (platform->reset_access_size == 1U) return 1U;
        if (platform->reset_access_size == 2U) return 2U;
        if (platform->reset_access_size == 3U) return 4U;
        return 0U;
    }
    if (platform->reset_bit_width <= 8U) return 1U;
    if (platform->reset_bit_width <= 16U) return 2U;
    if (platform->reset_bit_width <= 32U) return 4U;
    return 0U;
}

bool x86_acpi_reset(void) {
    const x86_acpi_platform_t *platform = x86_acpi_platform();
    uint8_t bytes;
    uint32_t value;
    if (platform == 0 || !platform->reset_supported ||
        platform->reset_bit_width == 0U) return false;
    bytes = reset_access_bytes(platform);
    if (bytes == 0U) return false;
    value = platform->reset_value;
    if (platform->reset_space_id == 1U) {
        if (platform->reset_address > UINT16_MAX) return false;
        if (bytes == 1U) {
            __asm__ volatile ("outb %b0, %w1" : : "a"(value),
                              "Nd"((uint16_t)platform->reset_address));
        } else if (bytes == 2U) {
            __asm__ volatile ("outw %w0, %w1" : : "a"(value),
                              "Nd"((uint16_t)platform->reset_address));
        } else {
            __asm__ volatile ("outl %0, %w1" : : "a"(value),
                              "Nd"((uint16_t)platform->reset_address));
        }
        return true;
    }
    if (platform->reset_space_id != 0U ||
        platform->reset_address > X86_64_DIRECT_MAP_END -
                                  X86_64_DIRECT_MAP_BASE ||
        bytes > X86_64_DIRECT_MAP_END - X86_64_DIRECT_MAP_BASE -
                platform->reset_address) return false;
    volatile uint8_t *address = (volatile uint8_t *)phys_to_direct(
        paddr_make(platform->reset_address));
    if (address == 0) return false;
    if (bytes == 1U) {
        *(volatile uint8_t *)address = platform->reset_value;
    } else if (bytes == 2U) {
        *(volatile uint16_t *)address = platform->reset_value;
    } else {
        *(volatile uint32_t *)address = platform->reset_value;
    }
    __asm__ volatile ("mfence" : : : "memory");
    return true;
}

kstatus_t x86_acpi_poweroff(void) {
    /* parse_dsdt_sleep 会读取 _S5_：其中两个沿用旧名称的字段表示 PM1a/PM1b 的
     * S5 类型，不是 S3/S4 睡眠状态。 */
    if (!x86_acpi_sleep_supported() || g_platform.pm1_control_length < 2U ||
        g_platform.pm1a_control_block == 0U ||
        g_platform.sleep_type_s3 > 7U || g_platform.sleep_type_s4 > 7U)
        return K_ENOSYS;
    uint16_t ports[2] = {g_platform.pm1a_control_block, g_platform.pm1b_control_block};
    uint8_t types[2] = {g_platform.sleep_type_s3, g_platform.sleep_type_s4};
    for (unsigned i = 0U; i < 2U; ++i) {
        if (ports[i] == 0U) continue;
        uint16_t control;
        __asm__ volatile ("inw %1, %0" : "=a"(control) : "Nd"(ports[i]));
        control = (uint16_t)((control & ~((7U << 10) | (1U << 13))) |
                            ((uint16_t)types[i] << 10) | (1U << 13));
        __asm__ volatile ("outw %0, %1" : : "a"(control), "Nd"(ports[i]));
    }
    return K_OK;
}

kstatus_t x86_acpi_enter_sleep(uint8_t sleep_state) {
    uint8_t sleep_type;
    uint16_t control;
    if (!x86_acpi_sleep_supported() || g_platform.pm1_control_length < 2U ||
        g_platform.pm1a_control_block == 0U) return K_ENOSYS;
    if (sleep_state == 3U) sleep_type = g_platform.sleep_type_s3;
    else if (sleep_state == 4U) sleep_type = g_platform.sleep_type_s4;
    else return K_EINVAL;
    if (sleep_type > 7U) return K_EINVAL;
    __asm__ volatile ("inw %1, %0" : "=a"(control) : "Nd"(g_platform.pm1a_control_block));
    control = (uint16_t)((control & ~(7U << 10)) |
                         ((uint16_t)sleep_type << 10) | (1U << 13));
    __asm__ volatile ("outw %0, %1" : : "a"(control), "Nd"(g_platform.pm1a_control_block));
    if (g_platform.pm1b_control_block != 0U) {
        __asm__ volatile ("outw %0, %1" : : "a"(control),
                          "Nd"(g_platform.pm1b_control_block));
    }
    __asm__ volatile ("mfence" : : : "memory");
    return K_OK;
}
