#pragma once

#include <kernel/nvme_core.h>

struct arch_trap_frame;

#define NVME_IO_FLUSH 0x00U
#define NVME_IO_WRITE 0x01U
#define NVME_IO_READ  0x02U

typedef struct nvme_pending_io {
    list_head_t node;
    nvme_queue_t *queue;
    io_request_t *request;
    dma_mapping_t *mapping;
    uint64_t bytes;
    uint16_t command_id;
    bool cancelled;
} nvme_pending_io_t;

/* Core 所有者会将注册表存储保留在 core.c 内部。 */
nvme_controller_t *nvme_controller_at(uint32_t index);
bool nvme_pci_is_controller(const pci_device_t *pci);
bool nvme_all_controllers_ready(void);

/* 队列 doorbell 仅由 NVMe 队列单元使用。 */
void nvme_queue_ring_submission(nvme_controller_t *controller,
                                const nvme_queue_t *queue);
void nvme_queue_ring_completion(nvme_controller_t *controller,
                                const nvme_queue_t *queue);
void nvme_ring_io_submission(nvme_controller_t *controller,
                             const nvme_queue_t *queue);
void nvme_ring_io_completion(nvme_controller_t *controller,
                             const nvme_queue_t *queue);

void nvme_record_completion_status(uint16_t status);
void nvme_lock(spinlock_t *lock);
bool nvme_try_lock(spinlock_t *lock);
void nvme_unlock(spinlock_t *lock);
void nvme_stop_completion_work(nvme_queue_t *queue);
void nvme_abort_queue_pending(nvme_queue_t *queue, kstatus_t status);
void nvme_msix_handler(uint8_t vector, struct arch_trap_frame *frame,
                       void *context);
void nvme_write32(const nvme_controller_t *controller, uint32_t offset,
                  uint32_t value);
bool nvme_deadline_reached(uint64_t deadline);
uint64_t nvme_timeout_deadline(const nvme_controller_t *controller);
uint16_t nvme_next_command_id(nvme_controller_t *controller);
kstatus_t nvme_admin_submit(nvme_controller_t *controller,
                            const nvme_command_t *command,
                            nvme_completion_t *result);
kstatus_t nvme_release_transient_dma(dma_mapping_t *mapping, page_t *page);
kstatus_t nvme_identify_controller(nvme_controller_t *controller);
kstatus_t nvme_identify_namespace(nvme_controller_t *controller);
bool nvme_rebind_msix(nvme_controller_t *controller, nvme_queue_t *queue,
                      uint16_t msix_entry, uint8_t new_vector);
bool nvme_msix_rebind_self_test(const nvme_controller_t *controller);
bool nvme_read_lba0_self_test(const nvme_controller_t *controller,
                              uint32_t forced_queue);

nvme_pending_io_t *nvme_pending_for_id_locked(nvme_queue_t *queue,
                                               uint16_t command_id);
void nvme_pending_unlink_locked(nvme_queue_t *queue,
                                nvme_pending_io_t *pending);
void nvme_pending_release(nvme_pending_io_t *pending,
                          kstatus_t completion_status,
                          uint64_t completion_bytes);
kstatus_t nvme_submit_io(device_t *device, io_request_t *request);
