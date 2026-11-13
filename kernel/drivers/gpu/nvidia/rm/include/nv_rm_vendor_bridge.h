#pragma once

#include <stdbool.h>
#include <stdint.h>

#include <kernel/base.h>
#include <kernel/pci.h>

/*
 * 厂商边界特意只公开不透明句柄和从厂商 RM 获取的信息。nv_state_t、OBJGPU 及生成的
 * NVOC 类型均保留在此接口的厂商一侧。
 */
typedef struct nv_rm_vendor_result {
    void *context;
    void *rm_gpu;
    void *rm_device;
    uint32_t chip_arch;
    uint32_t chip_impl;
    uint32_t hal_impl;
    bool blackwell;
    bool gb205;
    bool hal_valid;
    uint64_t vram_bytes;
} nv_rm_vendor_result_t;

typedef struct nv_rm_vendor_frontend_mapping {
    paddr_t physical;
    const uint64_t *physical_pages;
    uint64_t page_count;
    uint64_t length;
    uint32_t cache_mode;
    bool physical_pages_owned;
} nv_rm_vendor_frontend_mapping_t;

kstatus_t nv_rm_vendor_start(const pci_device_t *device,
                             void **context_out,
                             nv_rm_vendor_result_t *result);

void nv_rm_vendor_stop(void *context);

/* 通用 System 前端负责 fd、映射和进程的生命周期。这些钩子会隐藏厂商 RM 状态，同时允许
 * 针对 595.84 的桥接层只执行真实 RM/UAPI 操作。未链接厂商 RM 时，普通映像中的弱实现
 * 会安全失败。 */
kstatus_t nv_rm_vendor_frontend_open(bool control, uint32_t minor,
                                     void **file_out);
kstatus_t nv_rm_vendor_frontend_abi_private(void *file, void **private_out);
bool nv_rm_vendor_frontend_is_control(void *file);
uint32_t nv_rm_vendor_frontend_poll_state(void *file, bool consume_dataless);
void *nv_rm_vendor_frontend_wait_object(void *file);
void *nv_rm_vendor_ctl_state(void);
kstatus_t nv_rm_vendor_frontend_register_fd(void *file, void *control_file);
kstatus_t nv_rm_vendor_frontend_ioctl(void *file, uint32_t request,
                                      void *argument, uint32_t argument_size);
kstatus_t nv_rm_vendor_frontend_mmap(void *file, uint64_t offset,
                                     uint64_t length, uint32_t protection,
                                     nv_rm_vendor_frontend_mapping_t *mapping);
void nv_rm_vendor_frontend_close(void *file);

/*
 * NVKMS 使用官方内核级 RMAPI ABI。RM 调用栈、nv_state_t 和所有生成的 NVOC 类型都应
 * 保留在厂商桥接层内部；显示模式端口只能通过这些入口传递不透明操作包和 GPU 信息。
 */
kstatus_t nv_rm_vendor_kms_call_rm(void *context, void *ops);
kstatus_t nv_rm_vendor_kms_open_gpu(void *context, uint32_t gpu_id,
                                    bool reset_aware);
void nv_rm_vendor_kms_close_gpu(void *context, uint32_t gpu_id,
                                bool reset_aware);
uint32_t nv_rm_vendor_kms_enumerate_gpus(void *context, void *gpu_info);
bool nv_rm_vendor_kms_allow_write_combining(void *context);
