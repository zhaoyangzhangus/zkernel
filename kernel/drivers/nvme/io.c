#include <arch/x86_64/paging.h>
#include <kernel/block.h>
#include <kernel/dma.h>
#include <kernel/io.h>
#include <kernel/kmem.h>
#include <kernel/nvme_core.h>

#include "internal.h"

/* REFACTOR_P8_NVME_IO_OWNER：请求准备、提交、取消和待处理资源释放。 */

nvme_pending_io_t *nvme_pending_for_id_locked(nvme_queue_t *queue,
                                                       uint16_t command_id) {
    if (queue == 0) return 0;
    for (list_head_t *node = queue->pending_ios.next;
         node != &queue->pending_ios; node = node->next) {
        nvme_pending_io_t *pending = (nvme_pending_io_t *)((uint8_t *)node -
            __builtin_offsetof(nvme_pending_io_t, node));
        if (pending->command_id == command_id) return pending;
    }
    return 0;
}

void nvme_pending_unlink_locked(nvme_queue_t *queue,
                                        nvme_pending_io_t *pending) {
    if (queue == 0 || pending == 0) return;
    if (pending->node.next != &pending->node && pending->node.prev != &pending->node) {
        pending->node.prev->next = pending->node.next;
        pending->node.next->prev = pending->node.prev;
        list_init(&pending->node);
        atomic_fetch_sub_explicit(&queue->pending_count, 1U, memory_order_release);
    }
}

void nvme_pending_release(nvme_pending_io_t *pending,
                                 kstatus_t completion_status,
                                 uint64_t completion_bytes) {
    io_request_t *request;
    bool complete_request;
    if (pending == 0) return;
    request = pending->request;
    if (pending->mapping != 0) {
        /* CQ 已经确认设备不再访问缓冲区，此时才允许同步和解除映射。 */
        dma_sync_for_cpu(pending->mapping);
        (void)nvme_release_transient_dma(pending->mapping, 0);
        pending->mapping = 0;
    }
    complete_request = request != 0 && !io_request_is_terminal(request);
    /* pending 的引用不能在 io_complete 返回后释放，栈请求可能已被复用。 */
    if (request != 0) object_put(request);
    if (complete_request) io_complete(request, completion_status, completion_bytes);
    kfree(pending);
}

static void nvme_cancel_request(io_request_t *request) {
    device_t *device;
    nvme_controller_t *controller;
    if (request == 0 || request->device == 0) return;
    device = request->device;
    controller = (nvme_controller_t *)device->driver_data;
    if (controller == 0) return;
    for (uint16_t index = 0U; index < controller->io_queue_count; ++index) {
        nvme_queue_t *queue = &controller->io_queues[index];
        nvme_lock(&queue->lock);
        for (list_head_t *node = queue->pending_ios.next;
             node != &queue->pending_ios; node = node->next) {
            nvme_pending_io_t *pending = (nvme_pending_io_t *)((uint8_t *)node -
                __builtin_offsetof(nvme_pending_io_t, node));
            if (pending->request == request) {
                pending->cancelled = true;
                nvme_unlock(&queue->lock);
                return;
            }
        }
        nvme_unlock(&queue->lock);
    }
}

static void nvme_publish_pending_locked(nvme_queue_t *queue,
                                        nvme_pending_io_t *pending,
                                        const nvme_command_t *command) {
    uint16_t slot;
    nvme_command_t submission;
    if (queue == 0 || pending == 0 || command == 0) return;
    slot = queue->submission_tail;
    submission = *command;
    submission.opcode_flags = (submission.opcode_flags & 0x0000FFFFU) |
                              ((uint32_t)pending->command_id << 16);
    queue->submission[slot] = submission;
    dma_sync_for_device(&queue->submission_dma);
    dma_wmb();
    queue->submission_tail = (uint16_t)((slot + 1U) % queue->depth);
    pending->node.next = &queue->pending_ios;
    pending->node.prev = queue->pending_ios.prev;
    queue->pending_ios.prev->next = &pending->node;
    queue->pending_ios.prev = &pending->node;
    atomic_fetch_add_explicit(&queue->pending_count, 1U, memory_order_release);
    nvme_ring_io_submission(queue->controller, queue);
}

static uint16_t nvme_allocate_io_command_id_locked(nvme_controller_t *controller,
                                                   nvme_queue_t *queue) {
    if (controller == 0 || queue == 0) return 0U;
    for (uint32_t attempt = 0U; attempt < 0xFFFFU; ++attempt) {
        uint16_t candidate = nvme_next_command_id(controller);
        if (candidate != 0U && nvme_pending_for_id_locked(queue, candidate) == 0) {
            return candidate;
        }
    }
    return 0U;
}

static kstatus_t nvme_prepare_request_dma(device_t *device,
                                          io_request_t *request,
                                          nvme_command_t *command,
                                          dma_mapping_t **mapping_out,
                                          uint64_t *bytes_out) {
    bio_t *bio;
    const bio_vec_t *bio_vector = 0;
    const io_vec_t *io_vector;
    page_t *page;
    uint64_t physical = 0U;
    uint64_t end_physical;
    uint64_t bytes;
    uint64_t lba;
    uint64_t sectors;
    uint64_t page_offset = 0U;
    uint64_t page_capacity = PAGE_SIZE;
    uint32_t page_count = 1U;
    enum dma_direction direction;
    enum dma_device_access access;
    dma_mapping_t *mapping;
    page_t *pages[2] = {0};
    kstatus_t status;

    if (device == 0 || request == 0 || command == 0 || mapping_out == 0 ||
        bytes_out == 0) return K_EINVAL;
    *mapping_out = 0;
    *bytes_out = 0U;
    command->namespace_id = 1U;
    if (request->opcode == IO_FLUSH) {
        command->opcode_flags = NVME_IO_FLUSH;
        return K_OK;
    }
    if (request->opcode != IO_READ && request->opcode != IO_WRITE) return K_EINVAL;

    bio = (bio_t *)request->completion_target;
    if (bio != 0) {
        if (bio->io != request || bio->vec_count != 1U || bio->vecs == 0 ||
            (bio->op != BIO_OP_READ && bio->op != BIO_OP_WRITE)) return K_EINVAL;
        bio_vector = &bio->vecs[0];
        if (bio_vector->page == 0) return K_EINVAL;
        if ((bio_vector->page->flags & PAGE_COMPOUND_HEAD) != 0U) {
            page_capacity = (uint64_t)PAGE_SIZE <<
                            bio_vector->page->order;
        }
        if ((bio_vector->page->flags & PAGE_FREE) != 0U ||
            (bio_vector->page->flags & PAGE_COMPOUND_TAIL) != 0U ||
            bio_vector->length == 0U ||
            bio_vector->offset >= page_capacity ||
            bio_vector->length > page_capacity - bio_vector->offset ||
            (bio_vector->length & 511U) != 0U) return K_EINVAL;
        bytes = bio_vector->length;
        lba = bio->lba;
        page = bio_vector->page;
        page_offset = bio_vector->offset;
        direction = bio->op == BIO_OP_READ ? DMA_FROM_DEVICE : DMA_TO_DEVICE;
    } else {
        if (request->vec_count != 1U || request->vecs == 0 ||
            request->vecs[0].base == 0 || request->vecs[0].length == 0U) {
            return K_EINVAL;
        }
        io_vector = &request->vecs[0];
        bytes = io_vector->length;
        if (bytes > PAGE_SIZE || (bytes & 511U) != 0U) return K_EINVAL;
        physical = direct_to_phys(io_vector->base).value;
        if (physical == UINT64_MAX || physical > UINT64_MAX - (bytes - 1U)) {
            return K_EACCES;
        }
        end_physical = physical + bytes - 1U;
        if ((physical >> PAGE_SHIFT) != (end_physical >> PAGE_SHIFT)) return K_EINVAL;
        page = phys_to_page(paddr_make(physical & ~(PAGE_SIZE - 1ULL)));
        if (page == 0) return K_EACCES;
        page_offset = physical & (PAGE_SIZE - 1ULL);
        lba = request->offset / 512U;
        direction = request->opcode == IO_READ ? DMA_FROM_DEVICE : DMA_TO_DEVICE;
    }
    if ((request->offset & 511U) != 0U && bio == 0) return K_EINVAL;
    sectors = bytes / 512U;
    if (sectors == 0U || sectors > 0x10000ULL || lba > UINT64_MAX - sectors) {
        return K_EINVAL;
    }
    page_count = (uint32_t)((page_offset + bytes + PAGE_SIZE - 1U) /
                            PAGE_SIZE);
    if (page_count == 0U || page_count > 2U) return K_EINVAL;
    uint64_t page_physical = page_to_phys(page).value;
    if (page_physical == UINT64_MAX ||
        page_physical > UINT64_MAX - (uint64_t)(page_count - 1U) * PAGE_SIZE) {
        return K_EACCES;
    }
    for (uint32_t index = 0U; index < page_count; ++index) {
        pages[index] = phys_to_page(paddr_make(
            page_physical + (uint64_t)index * PAGE_SIZE));
        if (pages[index] == 0 ||
            (pages[index]->flags & PAGE_FREE) != 0U) return K_EACCES;
    }
    mapping = (dma_mapping_t *)kzalloc(sizeof(*mapping), 0);
    if (mapping == 0) return K_ENOMEM;
    status = dma_map_pages(device, pages, page_count, direction, mapping);
    if (status != K_OK) {
        kfree(mapping);
        return status;
    }
    iova_t address = iova_make(mapping->segments[0].addr.value + page_offset);
    access = request->opcode == IO_READ ? DMA_DEVICE_WRITE : DMA_DEVICE_READ;
    if (mapping->segment_count != 1U ||
        mapping->segments[0].length < page_offset + bytes ||
        dma_validate_access(device, address, bytes, access) != K_OK) {
        status = nvme_release_transient_dma(mapping, 0);
        return status == K_OK ? K_EACCES : K_EIO;
    }
    dma_sync_for_device(mapping);
    command->opcode_flags = request->opcode == IO_READ ? NVME_IO_READ : NVME_IO_WRITE;
    command->prp1 = address.value;
    command->prp2 = 0U;
    if (page_offset + bytes > PAGE_SIZE) {
        if (page_count != 2U) {
            status = nvme_release_transient_dma(mapping, 0);
            return status == K_OK ? K_EINVAL : K_EIO;
        }
        command->prp2 = mapping->segments[0].addr.value + PAGE_SIZE;
    }
    command->cdw10 = (uint32_t)lba;
    command->cdw11 = (uint32_t)(lba >> 32);
    command->cdw12 = (uint32_t)(sectors - 1U);
    *mapping_out = mapping;
    *bytes_out = bytes;
    return K_OK;
}

kstatus_t nvme_submit_io(device_t *device, io_request_t *request) {
    nvme_controller_t *controller;
    nvme_queue_t *queue;
    nvme_pending_io_t *pending;
    nvme_command_t command = {0};
    dma_mapping_t *mapping = 0;
    uint64_t bytes = 0U;
    kstatus_t status;
    uint32_t queue_index;

    if (device == 0 || request == 0 || device->driver_data == 0) return K_EINVAL;
    request->cancel = nvme_cancel_request;
    controller = (nvme_controller_t *)device->driver_data;
    if (!controller->started || !controller->identified ||
        controller->io_queue_count == 0U) return K_EIO;
    if ((request->flags & IOREQ_BLOCK_QUEUE_VALID) != 0U) {
        queue_index = (request->flags >> IOREQ_BLOCK_QUEUE_SHIFT) &
                      IOREQ_BLOCK_QUEUE_MASK;
        queue_index %= controller->io_queue_count;
    } else {
        queue_index = atomic_fetch_add_explicit(&controller->next_io_queue, 1U,
                                                memory_order_relaxed) %
                       controller->io_queue_count;
    }
    queue = &controller->io_queues[queue_index];
    status = nvme_prepare_request_dma(device, request, &command, &mapping, &bytes);
    if (status != K_OK) return status;
    pending = (nvme_pending_io_t *)kzalloc(sizeof(*pending), 0);
    if (pending == 0) {
        (void)nvme_release_transient_dma(mapping, 0);
        return K_ENOMEM;
    }
    list_init(&pending->node);
    pending->queue = queue;
    pending->request = request;
    pending->mapping = mapping;
    pending->bytes = bytes;
    object_get(request);

    nvme_lock(&queue->lock);
    if (!queue->active || queue->controller != controller ||
        atomic_load_explicit(&request->state, memory_order_acquire) != IOREQ_SUBMITTED ||
        atomic_load_explicit(&queue->pending_count, memory_order_acquire) >=
            (unsigned)(queue->depth - 1U)) {
        nvme_unlock(&queue->lock);
        object_put(request);
        kfree(pending);
        (void)nvme_release_transient_dma(mapping, 0);
        return K_EBUSY;
    }
    pending->command_id = nvme_allocate_io_command_id_locked(controller, queue);
    if (pending->command_id == 0U) {
        nvme_unlock(&queue->lock);
        object_put(request);
        kfree(pending);
        (void)nvme_release_transient_dma(mapping, 0);
        return K_EBUSY;
    }
    nvme_publish_pending_locked(queue, pending, &command);
    nvme_unlock(&queue->lock);
    return K_OK;
}
