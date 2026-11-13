#include <kernel/deferred.h>
#include <kernel/dma.h>
#include <kernel/irq.h>

#include "internal.h"

#define NVME_COMPLETION_STOPPING 0x80000000U
#define NVME_COMPLETION_REF_MASK 0x7FFFFFFFU

/* REFACTOR_P8_NVME_COMPLETION_OWNER：IRQ、CQ 消费和延迟完成处理。 */

static void nvme_deferred_complete(void *argument);
static bool nvme_schedule_queue_completion(nvme_queue_t *queue);

static bool nvme_completion_ref_get(nvme_queue_t *queue) {
    unsigned value;

    if (queue == 0) return false;
    value = atomic_load_explicit(&queue->completion_work_refs,
                                 memory_order_acquire);
    for (;;) {
        if ((value & NVME_COMPLETION_STOPPING) != 0U ||
            (value & NVME_COMPLETION_REF_MASK) == NVME_COMPLETION_REF_MASK) {
            return false;
        }
        if (atomic_compare_exchange_weak_explicit(&queue->completion_work_refs,
                                                  &value, value + 1U,
                                                  memory_order_acq_rel,
                                                  memory_order_acquire)) {
            return true;
        }
    }
}

static void nvme_completion_ref_put(nvme_queue_t *queue) {
    if (queue == 0) return;
    atomic_fetch_sub_explicit(&queue->completion_work_refs, 1U,
                              memory_order_release);
}

void nvme_stop_completion_work(nvme_queue_t *queue) {
    unsigned value;

    if (queue == 0) return;
    atomic_fetch_or_explicit(&queue->completion_work_refs,
                             NVME_COMPLETION_STOPPING, memory_order_acq_rel);
    for (;;) {
        value = atomic_load_explicit(&queue->completion_work_refs,
                                     memory_order_acquire);
        if ((value & NVME_COMPLETION_REF_MASK) == 0U) return;
        (void)deferred_run(8U);
        __asm__ volatile ("pause");
    }
}

void nvme_msix_handler(uint8_t vector, struct arch_trap_frame *frame,
                       void *context) {
    nvme_controller_t *controller = (nvme_controller_t *)context;

    (void)frame;
    if (controller == 0 || !controller->started) return;
    for (uint16_t index = 0; index < controller->io_queue_count; ++index) {
        nvme_queue_t *queue = &controller->io_queues[index];
        if (queue->active && queue->irq_vector == vector) {
            (void)nvme_schedule_queue_completion(queue);
            return;
        }
    }
}

static bool nvme_schedule_queue_completion(nvme_queue_t *queue) {
    bool expected = false;
    bool ref_acquired;

    if (queue == 0 || !queue->active) return false;
    ref_acquired = nvme_completion_ref_get(queue);
    if (!ref_acquired ||
        !atomic_compare_exchange_strong_explicit(&queue->completion_queued,
                                                 &expected, true,
                                                 memory_order_acq_rel,
                                                 memory_order_acquire)) {
        if (ref_acquired) nvme_completion_ref_put(queue);
        return false;
    }
    if (!deferred_try_schedule(nvme_deferred_complete, queue)) {
        atomic_store_explicit(&queue->completion_queued, false,
                              memory_order_release);
        nvme_completion_ref_put(queue);
        return false;
    }
    return true;
}

static uint32_t nvme_poll_queue_completions(nvme_queue_t *queue,
                                             uint32_t budget,
                                             bool wait_for_lock) {
    nvme_controller_t *controller;
    uint32_t processed = 0U;

    if (queue == 0 || budget == 0U) return 0U;
    controller = queue->controller;
    if (controller == 0 || !queue->active) return 0U;

    if (wait_for_lock) {
        nvme_lock(&queue->lock);
    } else if (!nvme_try_lock(&queue->lock)) {
        return 0U;
    }
    if (queue->completion != 0) dma_sync_for_cpu(&queue->completion_dma);

    while (processed < budget && queue->completion != 0) {
        nvme_completion_t completion =
            queue->completion[queue->completion_head];

        if ((completion.status & 1U) != queue->phase) break;

        queue->completion_head =
            (uint16_t)((queue->completion_head + 1U) % queue->depth);
        if (queue->completion_head == 0U) queue->phase ^= 1U;

        nvme_ring_io_completion(controller, queue);

        nvme_pending_io_t *pending =
            nvme_pending_for_id_locked(queue, completion.command_id);
        if (pending != 0) {
            nvme_pending_unlink_locked(queue, pending);
        }

        nvme_unlock(&queue->lock);

        if (pending != 0) {
            kstatus_t status =
                ((completion.status >> 1) & 0x7FFFU) == 0U ?
                    K_OK : K_EIO;

            nvme_record_completion_status(completion.status);
            nvme_pending_release(
                pending,
                status,
                status == K_OK ? pending->bytes : 0U);
        }

        ++processed;

        if (wait_for_lock) {
            nvme_lock(&queue->lock);
        } else if (!nvme_try_lock(&queue->lock)) {
            return processed;
        }
        if (queue->completion != 0) {
            dma_sync_for_cpu(&queue->completion_dma);
        }
    }

    nvme_unlock(&queue->lock);
    return processed;
}

static void nvme_deferred_complete(void *argument) {
    nvme_queue_t *queue = (nvme_queue_t *)argument;
    uint32_t processed;

    if (queue == 0) return;
    processed = nvme_poll_queue_completions(queue, 32U, true);

    atomic_store_explicit(&queue->completion_queued, false,
                          memory_order_release);
    nvme_completion_ref_put(queue);

    if (processed == 32U && queue->active) {
        (void)nvme_schedule_queue_completion(queue);
    }
}

void nvme_abort_queue_pending(nvme_queue_t *queue, kstatus_t status) {
    if (queue == 0) return;
    for (;;) {
        nvme_pending_io_t *pending = 0;

        nvme_lock(&queue->lock);
        if (!list_empty(&queue->pending_ios)) {
            list_head_t *node = queue->pending_ios.next;
            pending = (nvme_pending_io_t *)((uint8_t *)node -
                __builtin_offsetof(nvme_pending_io_t, node));
            nvme_pending_unlink_locked(queue, pending);
        }
        nvme_unlock(&queue->lock);

        if (pending == 0) return;
        pending->cancelled = true;
        nvme_pending_release(pending, status, 0U);
    }
}

uint32_t nvme_poll_device_completions(device_t *device, uint32_t budget) {
    nvme_controller_t *controller;
    uint32_t processed = 0U;

    if (device == 0 || device->driver_data == 0 || budget == 0U) {
        return 0U;
    }

    controller = (nvme_controller_t *)device->driver_data;
    if (!controller->started) return 0U;

    for (uint16_t index = 0U;
         index < controller->io_queue_count && processed < budget;
         ++index) {
        nvme_queue_t *queue = &controller->io_queues[index];

        if (!queue->active ||
            atomic_load_explicit(&queue->pending_count,
                                 memory_order_acquire) == 0U) {
            continue;
        }

        processed += nvme_poll_queue_completions(
            queue, budget - processed, false);
    }

    return processed;
}

bool nvme_schedule_deferred_poll(void) {
    bool scheduled = false;

    for (uint32_t controller_index = 0U;
         controller_index < NVME_MAX_CONTROLLERS; ++controller_index) {
        nvme_controller_t *controller =
            nvme_controller_at(controller_index);

        if (controller == 0 || !controller->started) continue;
        for (uint16_t queue_index = 0U;
             queue_index < controller->io_queue_count; ++queue_index) {
            nvme_queue_t *queue = &controller->io_queues[queue_index];
            if (queue->active &&
                atomic_load_explicit(&queue->pending_count,
                                     memory_order_acquire) != 0U &&
                nvme_schedule_queue_completion(queue)) {
                scheduled = true;
            }
        }
    }
    return scheduled;
}
