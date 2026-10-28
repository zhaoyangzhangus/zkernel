#include "internal.h"

/* REFACTOR_P8_XHCI_COMMAND_RUNTIME_OWNER：命令环提交和完成处理与控制器生命周期及协议所有者分离。 */

#define XHCI_COMMAND_RING_TRB_COUNT 256U
#define XHCI_LINK_TRB_TYPE 6U
#define XHCI_COMMAND_COMPLETION_TYPE 33U
#define XHCI_USBSTS 0x04U
#define XHCI_USBSTS_HCH (1U << 0)
#define XHCI_ADDRESS_DEVICE_TYPE 11U
#define XHCI_ADDRESS_DEVICE_BSR (1U << 9)

static bool xhci_submit_command_flags(xhci_state_t *state, uint32_t type,
                                       uint8_t slot, uint8_t endpoint,
                                       uint64_t parameter,
                                       uint32_t command_flags,
                                       uint8_t *result_slot) {
    xhci_trb_t *command_ring = (xhci_trb_t *)state->command_ring.cpu;
    uint32_t command_index = state->command_index;
    uint64_t command_address;
    if (state->command_index >= XHCI_COMMAND_RING_TRB_COUNT - 1U) {
        state->command_index = 0U;
        state->command_cycle ^= 1U;
        command_index = 0U;
    }
    command_address = xhci_dma_address(&state->command_ring.mapping) +
                      (uint64_t)command_index * sizeof(xhci_trb_t);

    xhci_trb_t *command = &command_ring[state->command_index];
    xhci_command_encode(command, type, slot, endpoint, parameter,
                        state->command_cycle);
    command->control |= command_flags;

    uint32_t next_command = state->command_index + 1U;
    if (next_command == XHCI_COMMAND_RING_TRB_COUNT - 1U) {
        command_ring[XHCI_COMMAND_RING_TRB_COUNT - 1U].control =
            (XHCI_LINK_TRB_TYPE << XHCI_TRB_TYPE_SHIFT) |
            state->command_cycle | (1U << 1);
        state->command_index = 0U;
        state->command_cycle ^= 1U;
    } else {
        state->command_index = next_command;
    }

    dma_sync_for_device(&state->command_ring.mapping);
    dma_wmb();
    *(volatile uint32_t *)(state->mmio + state->doorbell_offset) = 0;
    __asm__ volatile ("mfence" : : : "memory");

    uint64_t deadline = xhci_controller_timeout_deadline(5000000000ULL);
    for (;;) {
        xhci_trb_t event;
        if (!xhci_next_ring_event(state, &event)) {
            if (xhci_controller_timeout_reached(deadline)) break;
            __asm__ volatile ("pause");
            continue;
        }

        uint32_t control = event.control;
        uint32_t event_type = (control >> XHCI_TRB_TYPE_SHIFT) & 0x3FU;
        if (event_type != XHCI_COMMAND_COMPLETION_TYPE) {
            (void)xhci_defer_event(state, &event);
            continue;
        }
        if (event.parameter != command_address) {
            (void)xhci_defer_event(state, &event);
            continue;
        }

        uint32_t completion = event.status >> XHCI_COMPLETION_SHIFT;
        uint8_t completed_slot = (uint8_t)(control >> XHCI_TRB_SLOT_SHIFT);
        if (result_slot != 0) *result_slot = completed_slot;
        if (completion != XHCI_COMPLETION_SUCCESS) {
            xhci_set_error(32U + completion);
            xhci_event_handler_complete(state);
            return false;
        }

        xhci_clear_error();
        xhci_event_handler_complete(state);
        return true;
    }

    if ((xhci_controller_read32(state, state->operational_offset + XHCI_USBSTS) &
         XHCI_USBSTS_HCH) != 0U) {
        xhci_set_error(26U);
    } else {
        xhci_set_error(27U);
    }
    return false;
}

bool xhci_submit_command_ex(xhci_state_t *state, uint32_t type,
                            uint8_t slot, uint8_t endpoint,
                            uint64_t parameter, uint8_t *result_slot) {
    return xhci_submit_command_flags(state, type, slot, endpoint, parameter,
                                      0U, result_slot);
}

bool xhci_submit_address_device(xhci_state_t *state, uint8_t slot,
                                uint64_t input_context,
                                bool context_only) {
    return xhci_submit_command_flags(
        state, XHCI_ADDRESS_DEVICE_TYPE, slot, 0U, input_context,
        context_only ? XHCI_ADDRESS_DEVICE_BSR : 0U, 0);
}

bool xhci_submit_command(xhci_state_t *state, uint32_t type,
                         uint8_t slot, uint64_t parameter,
                         uint8_t *result_slot) {
    return xhci_submit_command_ex(state, type, slot, 0U, parameter, result_slot);
}
