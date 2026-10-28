#pragma once

#include <stdbool.h>
#include <stdint.h>

#include <kernel/base.h>
#include <kernel/pci.h>

/*
 * The vendor boundary deliberately exposes only opaque handles and facts
 * obtained from the vendor RM.  nv_state_t, OBJGPU, and the generated NVOC
 * types stay on the vendor side of this interface.
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

/* The generic System frontend owns fd, mapping, and process lifetimes.  These
 * hooks keep vendor RM state opaque while allowing the 595.84-bound bridge to
 * perform only real RM/UAPI operations.  The weak implementations in the
 * normal image fail closed when the vendor RM is not linked. */
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
 * NVKMS consumes the official kernel-level RMAPI ABI.  Keep the RM stack,
 * nv_state_t, and all generated NVOC types private to the vendor bridge; the
 * modeset port passes only the opaque operation packet and GPU facts through
 * these entry points.
 */
kstatus_t nv_rm_vendor_kms_call_rm(void *context, void *ops);
kstatus_t nv_rm_vendor_kms_open_gpu(void *context, uint32_t gpu_id,
                                    bool reset_aware);
void nv_rm_vendor_kms_close_gpu(void *context, uint32_t gpu_id,
                                bool reset_aware);
uint32_t nv_rm_vendor_kms_enumerate_gpus(void *context, void *gpu_info);
bool nv_rm_vendor_kms_allow_write_combining(void *context);
