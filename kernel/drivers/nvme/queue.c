#include "internal.h"

#define NVME_REG_DBS 0x1000U

static void nvme_queue_write32(const nvme_controller_t *controller,
                               uint32_t offset, uint32_t value) {
    volatile uint32_t *address;
    if (controller == 0 || controller->registers == 0) return;
    address = (volatile uint32_t *)(controller->registers + offset);
    /* NVMe doorbell 要求执行 32 位 MMIO 事务。 */
    __asm__ volatile ("movl %1, %0" : "=m"(*address) : "r"(value) : "memory");
}

void nvme_queue_ring_submission(nvme_controller_t *controller,
                                const nvme_queue_t *queue) {
    uint32_t offset;
    if (controller == 0 || queue == 0) return;
    offset = NVME_REG_DBS +
             (uint32_t)queue->queue_id * 2U * controller->doorbell_stride;
    nvme_queue_write32(controller, offset, queue->submission_tail);
}

void nvme_queue_ring_completion(nvme_controller_t *controller,
                                const nvme_queue_t *queue) {
    uint32_t offset;
    if (controller == 0 || queue == 0) return;
    offset = NVME_REG_DBS +
             ((uint32_t)queue->queue_id * 2U + 1U) *
                 controller->doorbell_stride;
    nvme_queue_write32(controller, offset, queue->completion_head);
}
