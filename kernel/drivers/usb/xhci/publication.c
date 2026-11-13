#include "internal.h"

#include <kernel/console.h>
#include <usb/storage.h>

/* REFACTOR_P8_XHCI_PUBLICATION_OWNER：工作上下文中的 DMA 所有权转移与规范
 * Slot.context 发布共用同一个状态边界。 */

static bool xhci_move_device_context(xhci_device_context_t *destination,
                                     xhci_device_context_t *source) {
    dma_mapping_t *destination_mappings[] = {
        &destination->input_context.mapping,
        &destination->output_context.mapping,
        &destination->ep0_ring.mapping,
        &destination->descriptor_buffer.mapping,
        &destination->hid_ring.mapping,
        &destination->hid_report.mapping,
        &destination->hid_secondary.ring.mapping,
        &destination->hid_secondary.report.mapping,
        &destination->audio_ring.mapping,
        &destination->audio_buffer.mapping,
        &destination->bt_event_ring.mapping,
        &destination->bt_event_buffer.mapping,
        &destination->bt_acl_in_ring.mapping,
        &destination->bt_acl_in_buffer.mapping,
        &destination->bt_acl_out_ring.mapping,
        &destination->bt_acl_out_buffer.mapping,
        &destination->hub_ring.mapping,
        &destination->hub_report.mapping,
    };
    dma_mapping_t *source_mappings[] = {
        &source->input_context.mapping,
        &source->output_context.mapping,
        &source->ep0_ring.mapping,
        &source->descriptor_buffer.mapping,
        &source->hid_ring.mapping,
        &source->hid_report.mapping,
        &source->hid_secondary.ring.mapping,
        &source->hid_secondary.report.mapping,
        &source->audio_ring.mapping,
        &source->audio_buffer.mapping,
        &source->bt_event_ring.mapping,
        &source->bt_event_buffer.mapping,
        &source->bt_acl_in_ring.mapping,
        &source->bt_acl_in_buffer.mapping,
        &source->bt_acl_out_ring.mapping,
        &source->bt_acl_out_buffer.mapping,
        &source->hub_ring.mapping,
        &source->hub_report.mapping,
    };
    uint32_t mapping_count =
        (uint32_t)(sizeof(destination_mappings) /
                   sizeof(destination_mappings[0]));
    uint8_t slot;

    if (destination == 0 || source == 0 || destination == source ||
        source->device_slot == 0U || destination->device_slot != 0U) {
        return false;
    }
    for (uint32_t i = 0U; i < mapping_count; ++i) {
        dma_mapping_t *mapping = destination_mappings[i];
        if (mapping->device != 0 || mapping->pages != 0 ||
            mapping->page_count != 0U || mapping->segments != 0 ||
            mapping->segment_count != 0U || mapping->mapped_length != 0U ||
            mapping->flags != 0U) {
            return false;
        }
    }

    slot = source->device_slot;
    *destination = *source;
    destination->device_slot = 0U;
    for (uint32_t i = 0U; i < mapping_count; ++i) {
        *destination_mappings[i] = (dma_mapping_t){0};
    }

    uint32_t moved = 0U;
    for (; moved < mapping_count; ++moved) {
        if (dma_mapping_move(destination_mappings[moved],
                             source_mappings[moved]) != K_OK) {
            while (moved != 0U) {
                --moved;
                if (dma_mapping_move(source_mappings[moved],
                                     destination_mappings[moved]) != K_OK) {
                    return false;
                }
            }
            xhci_zero_device_context(destination);
            return false;
        }
    }

    destination->device_slot = slot;
    xhci_zero_device_context(source);
    return true;
}

bool xhci_publish_working_device(xhci_state_t *state) {
    uint8_t slot;
    xhci_slot_device_t *slot_dev;

    if (state == 0 || state->device.device_slot == 0U) return false;
    slot = state->device.device_slot;
    slot_dev = xhci_topology_slot(slot);
    if (!slot_dev->used ||
        !xhci_move_device_context(&slot_dev->context, &state->device)) {
        return false;
    }

    xhci_topology_publish_slot(slot);

    xhci_clear_device_flags();
    xhci_recompute_topology(state);
    if (slot_dev->context.msc_configured &&
        !usb_msc_schedule_attach(slot)) {
        kernel_serial_write("OS_USB_MSC_ATTACH_QUEUE_FAIL\r\n");
    }
    return true;
}
