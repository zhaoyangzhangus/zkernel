#pragma once

#include <stdbool.h>
#include <stdint.h>

#include <kernel/base.h>
#include <kernel/pci.h>

#include "../../port/include/nv_port.h"

typedef enum nv_gpu_state {
    NV_GPU_NEW = 0,
    NV_GPU_PCI_READY,
    NV_GPU_BAR_READY,
    NV_GPU_IRQ_READY,
    NV_GPU_RM_READY,
    NV_GPU_HAL_READY,
    NV_GPU_GSP_READY,
    NV_GPU_FAILED,
} nv_gpu_state_t;

typedef struct nv_gpu_device {
    nv_gpu_state_t state;
    pci_device_t *pci;

    uint8_t bar0_index;
    uint8_t bar1_index;
    uint64_t bar0_pa;
    uint64_t bar0_size;
    void *bar0_va;
    uint64_t bar1_pa;
    uint64_t bar1_size;

    nv_port_mmio_mapping_t bar0_mapping;
    void *irq_set;
    void *rm_gpu;
    void *rm_device;
    void *vendor_context;

    uint16_t segment;
    uint8_t bus;
    uint8_t slot;
    uint8_t function;
    uint16_t vendor_id;
    uint16_t device_id;

    uint32_t failure_stage;
    kstatus_t failure_status;
} nv_gpu_device_t;

/*
 * NVIDIA OS ABI 使用的固件帧缓冲区描述副本。厂商 RM 会向宿主 OS 查询此信息，以确定 BAR1 是否
 * 包含由客户端拥有的 UEFI 控制台。此处只保留经过验证的标量值；RM 永远不会收到引导信息指针。
 */
typedef struct nv_rm_screen_info {
    uint64_t physical_base;
    uint32_t width;
    uint32_t height;
    uint32_t depth;
    uint32_t pitch;
    uint64_t size;
    bool valid;
} nv_rm_screen_info_t;

kstatus_t nv_rm_driver_probe(void);

const nv_gpu_device_t *nv_rm_driver_current(void);
/* 只读初始化时发布的显存容量；不进入 RM，不触发设备初始化。 */
uint64_t nv_rm_driver_memory_bytes(const pci_device_t *device);

void nv_rm_driver_set_screen_info(uint64_t physical_base, uint32_t width,
                                  uint32_t height, uint32_t depth,
                                  uint32_t pitch, uint64_t size);
bool nv_rm_driver_get_screen_info(nv_rm_screen_info_t *info);
