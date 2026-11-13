#include "nvabi_internal.h"

typedef struct nvabi_pci_handle {
    uint32_t magic;
    const pci_device_t *device;
} nvabi_pci_handle_t;

#define NVABI_PCI_HANDLE_MAGIC 0x50434948U

static const pci_device_t *nvabi_find_bdf(NvU32 domain, NvU8 bus,
                                           NvU8 slot, NvU8 function) {
    const pci_host_t *host = pci_current_host();
    if (host == NULL || !host->initialized) return NULL;
    for (uint32_t index = 0U; index < host->device_count; ++index) {
        const pci_device_t *device = &host->devices[index];
        if (device->segment == domain && device->bus == bus &&
            device->slot == slot && device->function == function) {
            return device;
        }
    }
    return NULL;
}

static nvabi_pci_handle_t *nvabi_pci_get(void *handle, const char *function) {
    nvabi_pci_handle_t *value = (nvabi_pci_handle_t *)handle;
    if (value == NULL || value->magic != NVABI_PCI_HANDLE_MAGIC ||
        value->device == NULL) {
        nvabi_log_invalid(function, "os-pci");
        return NULL;
    }
    return value;
}

void *NV_API_CALL os_pci_init_handle(NvU32 domain, NvU8 bus, NvU8 slot,
                                     NvU8 function, NvU16 *vendor,
                                     NvU16 *device_id) {
    const pci_device_t *device = nvabi_find_bdf(domain, bus, slot, function);
    /* RM 枚举 PCI 总线时会探测任意 BDF。设备不存在是此查找的正常否定结果，不代表
     * System 不支持该操作；保持探测静默，让厂商代码继续运行。 */
    if (device == NULL) return NULL;
    nvabi_pci_handle_t *handle = (nvabi_pci_handle_t *)kzalloc(
        sizeof(*handle), 0U);
    if (handle == NULL) return NULL;
    handle->magic = NVABI_PCI_HANDLE_MAGIC;
    handle->device = device;
    if (vendor != NULL) *vendor = device->vendor_id;
    if (device_id != NULL) *device_id = device->device_id;
    return handle;
}

NV_STATUS NV_API_CALL os_pci_read_byte(void *handle, NvU32 offset,
                                       NvU8 *value) {
    nvabi_pci_handle_t *pci = nvabi_pci_get(handle, "os_pci_read_byte");
    if (pci == NULL) return NV_ERR_INVALID_ARGUMENT;
    return nvabi_status_from_kstatus(nv_port_pci_read8(
        pci->device, (uint16_t)offset, value));
}

NV_STATUS NV_API_CALL os_pci_read_word(void *handle, NvU32 offset,
                                       NvU16 *value) {
    nvabi_pci_handle_t *pci = nvabi_pci_get(handle, "os_pci_read_word");
    if (pci == NULL) return NV_ERR_INVALID_ARGUMENT;
    return nvabi_status_from_kstatus(nv_port_pci_read16(
        pci->device, (uint16_t)offset, value));
}

NV_STATUS NV_API_CALL os_pci_read_dword(void *handle, NvU32 offset,
                                        NvU32 *value) {
    nvabi_pci_handle_t *pci = nvabi_pci_get(handle, "os_pci_read_dword");
    if (pci == NULL) return NV_ERR_INVALID_ARGUMENT;
    return nvabi_status_from_kstatus(nv_port_pci_read32(
        pci->device, (uint16_t)offset, value));
}

NV_STATUS NV_API_CALL os_pci_write_byte(void *handle, NvU32 offset,
                                        NvU8 value) {
    nvabi_pci_handle_t *pci = nvabi_pci_get(handle, "os_pci_write_byte");
    if (pci == NULL) return NV_ERR_INVALID_ARGUMENT;
    return nvabi_status_from_kstatus(nv_port_pci_write8(
        (pci_device_t *)pci->device, (uint16_t)offset, value));
}

NV_STATUS NV_API_CALL os_pci_write_word(void *handle, NvU32 offset,
                                        NvU16 value) {
    nvabi_pci_handle_t *pci = nvabi_pci_get(handle, "os_pci_write_word");
    if (pci == NULL) return NV_ERR_INVALID_ARGUMENT;
    return nvabi_status_from_kstatus(nv_port_pci_write16(
        (pci_device_t *)pci->device, (uint16_t)offset, value));
}

NV_STATUS NV_API_CALL os_pci_write_dword(void *handle, NvU32 offset,
                                         NvU32 value) {
    nvabi_pci_handle_t *pci = nvabi_pci_get(handle, "os_pci_write_dword");
    if (pci == NULL) return NV_ERR_INVALID_ARGUMENT;
    return nvabi_status_from_kstatus(nv_port_pci_write32(
        (pci_device_t *)pci->device, (uint16_t)offset, value));
}

NvBool NV_API_CALL os_pci_remove_supported(void) {
    return NV_TRUE;
}

void NV_API_CALL os_pci_remove(void *handle) {
    nvabi_pci_handle_t *pci = nvabi_pci_get(handle, "os_pci_remove");
    if (pci == NULL) return;
    pci->magic = 0U;
    kfree(pci);
}
