#include <arch/x86_64/cpu.h>
#include <kernel/pci.h>
#include <kernel/sched.h>
#include <kernel/realtest.h>

#include "internal.h"

/* REFACTOR_P8_XHCI_CORE_OWNER: controller initialization, teardown, and the
 * explicit private boundaries shared by the xHCI protocol Owners. */

#define XHCI_CONFIG            0x38U
#define XHCI_USBSTS           0x04U
#define XHCI_USBSTS_CNR       (1U << 11)
#define XHCI_USBSTS_RW1C_MASK (0x1FFCU)
#define XHCI_RUNTIME_ERDP      0x18U
#define XHCI_RUNTIME_INTR0     0x20U

static xhci_state_t g_xhci;
static xhci_state_t g_xhci_hid;
static bool g_xhci_hid_active;

xhci_state_t *xhci_controller_state(void) {
    return &g_xhci;
}

xhci_state_t *xhci_hid_controller_state(void) {
    return &g_xhci_hid;
}

bool xhci_hid_controller_active(void) {
    return g_xhci_hid_active;
}

void xhci_hid_controller_set_active(bool active) {
    g_xhci_hid_active = active;
}

/* Keep the Hub and event-dispatch reverse dependencies explicit.  The
 * callbacks are private contracts, not a second public driver API. */
bool xhci_restart_hub_status_endpoint(xhci_state_t *state,
                                       xhci_device_context_t *hub);
bool xhci_publish_working_device(xhci_state_t *state);
bool xhci_remove_device_subtree(xhci_state_t *state, uint8_t slot);
bool xhci_handle_bt_transfer_event(xhci_state_t *state,
                                   const xhci_trb_t *event);
bool xhci_handle_root_port_event(xhci_state_t *state, uint8_t port,
                                 uint32_t portsc);

const xhci_hub_runtime_ops_t g_xhci_hub_runtime_ops = {
    .queue_status = xhci_queue_hub_status_device,
    .restart_status = xhci_restart_hub_status_endpoint,
    .submit_command = xhci_submit_command,
    .enumerate_device = xhci_enumerate_device,
    .free_device_resources = xhci_free_device_resources,
    .publish_working_device = xhci_publish_working_device,
    .remove_device_subtree = xhci_remove_device_subtree,
    .find_child = xhci_topology_find_child,
    .child_route = xhci_topology_child_route,
    .zero_device_context = xhci_zero_device_context,
    .clear_device_flags = xhci_clear_device_flags,
};

const xhci_event_dispatch_ops_t g_xhci_event_dispatch_ops = {
    .handle_root_port_event = xhci_handle_root_port_event,
    .hub_runtime = &g_xhci_hub_runtime_ops,
};

/* REFACTOR_P8_XHCI_EVENT_LOCK_OWNER: one preemption-safe owner for the
 * controller event stream shared by synchronous transfers and deferred IRQ
 * processing. Interrupts stay enabled so xHCI completion delivery remains
 * possible while a synchronous caller polls the event ring. */
#define X86_RFLAGS_INTERRUPT (1ULL << 9)

static uint64_t xhci_event_save_flags(void) {
    uint64_t flags;
    __asm__ volatile ("pushfq; popq %0" : "=r"(flags) : : "memory");
    return flags;
}

static void xhci_event_enable_interrupts(void) {
    __asm__ volatile ("sti" : : : "memory");
}

static void xhci_event_restore_flags(uint64_t flags) {
    if ((flags & X86_RFLAGS_INTERRUPT) == 0U) {
        __asm__ volatile ("cli" : : : "memory");
    }
}

void xhci_event_lock(xhci_state_t *state) {
    if (state == 0) return;
    sched_preempt_disable();
    spinlock_lock(&state->event_lock);
    __atomic_store_n(&state->event_lock_owner, x86_current_cpu_index(),
                     __ATOMIC_RELEASE);
    state->event_lock_saved_flags = xhci_event_save_flags();
    xhci_event_enable_interrupts();
}

bool xhci_event_try_lock(xhci_state_t *state) {
    if (state == 0) return false;
    sched_preempt_disable();
    if (!spinlock_try_lock(&state->event_lock)) {
        sched_preempt_enable();
        return false;
    }
    __atomic_store_n(&state->event_lock_owner, x86_current_cpu_index(),
                     __ATOMIC_RELEASE);
    state->event_lock_saved_flags = xhci_event_save_flags();
    xhci_event_enable_interrupts();
    return true;
}

void xhci_event_unlock(xhci_state_t *state) {
    if (state == 0) return;
    uint64_t saved_flags = state->event_lock_saved_flags;
    __atomic_store_n(&state->event_lock_owner, UINT32_MAX, __ATOMIC_RELEASE);
    spinlock_unlock(&state->event_lock);
    bool resume = atomic_exchange_explicit(&state->event_work_waiting, false,
                                           memory_order_acq_rel);
    xhci_event_restore_flags(saved_flags);
    sched_preempt_enable();
    if (resume) xhci_runtime_resume_events();
}

bool xhci_core_initialize(xhci_state_t *state, const pci_device_t *pci) {
    if (state == 0 || pci == 0 || state->initialized) return false;

    for (size_t i = 0; i < sizeof(*state); ++i) {
        ((uint8_t *)state)[i] = 0U;
    }
    spinlock_init(&state->event_lock);
    atomic_init(&state->event_work_waiting, false);
    state->event_lock_owner = UINT32_MAX;
    state->pci = pci;

    if (pci_enable_memory_busmaster((pci_device_t *)pci) != K_OK ||
        !xhci_controller_map_mmio(state)) {
        xhci_set_error(20U);
        return false;
    }

    uint32_t capability = xhci_controller_read32(state, 0U);
    uint8_t cap_length = (uint8_t)(capability & 0xFFU);
    state->hci_version = (uint16_t)(capability >> 16);
    uint32_t hcs_params1 = xhci_controller_read32(state, 0x04U);
    uint32_t hcs_params2 = xhci_controller_read32(state, 0x08U);
    uint32_t hcc_params1 = xhci_controller_read32(state, 0x10U);
    uint32_t doorbell_offset = xhci_controller_read32(state, 0x14U) & ~3U;
    uint32_t runtime_offset = xhci_controller_read32(state, 0x18U) & ~31U;

    state->operational_offset = cap_length;
    state->doorbell_offset = doorbell_offset;
    state->runtime_offset = runtime_offset;
    state->max_slots = hcs_params1 & 0xFFU;
    state->max_ports = (hcs_params1 >> 24) & 0xFFU;
    state->scratchpad_count =
        (((hcs_params2 >> 27) & 0x1FU) << 5) |
        ((hcs_params2 >> 21) & 0x1FU);
    state->context_size = (hcc_params1 & (1U << 2)) != 0U ? 64U : 32U;
    uint8_t max_psa_exponent = (uint8_t)((hcc_params1 >> 12) & 0x0FU);
    state->max_primary_stream_array_size = 1U << (max_psa_exponent + 1U);
    realtest_mark_number("XHCI_HCS_PARAMS1", hcs_params1);
    realtest_mark_number("XHCI_HCS_PARAMS2", hcs_params2);
    realtest_mark_number("XHCI_SCRATCHPAD_COUNT",
                                state->scratchpad_count);
    realtest_mark_number("XHCI_HCC_PARAMS1", hcc_params1);
    realtest_mark_number("XHCI_MAX_PSA_EXPONENT", max_psa_exponent);
    realtest_mark_number("XHCI_MAX_PSA_SIZE",
                                state->max_primary_stream_array_size);
    realtest_mark_number("XHCI_VERSION", state->hci_version);
    realtest_mark_number("XHCI_CONTEXT_SIZE", state->context_size);

    if (cap_length < 0x20U || state->max_slots == 0U ||
        state->max_ports == 0U || doorbell_offset >= state->mmio_span ||
        runtime_offset >= state->mmio_span ||
        state->operational_offset + XHCI_CONFIG + sizeof(uint32_t) >
            state->mmio_span ||
        state->doorbell_offset + sizeof(uint32_t) > state->mmio_span ||
        state->runtime_offset + XHCI_RUNTIME_INTR0 + XHCI_RUNTIME_ERDP +
            sizeof(uint64_t) > state->mmio_span) {
        xhci_set_error(21U);
        xhci_controller_unmap_mmio(state);
        return false;
    }

    if (!xhci_controller_handoff_legacy(state, hcc_params1)) {
        xhci_set_error(23U);
        xhci_controller_unmap_mmio(state);
        return false;
    }
    uint64_t ready_deadline =
        xhci_controller_timeout_deadline(5000000000ULL);
    uint32_t ready_spins = 0U;
    for (;;) {
        uint32_t status = xhci_controller_read32(
            state, state->operational_offset + XHCI_USBSTS);
        if ((status & XHCI_USBSTS_CNR) == 0U) break;
        if ((ready_deadline != UINT64_MAX &&
             xhci_controller_timeout_reached(ready_deadline)) ||
            (ready_deadline == UINT64_MAX && ++ready_spins >= 5000000U)) {
            xhci_set_error(24U);
            xhci_controller_unmap_mmio(state);
            return false;
        }
        __asm__ volatile ("pause");
    }
    (void)xhci_controller_write32(
        state, state->operational_offset + XHCI_USBSTS,
        XHCI_USBSTS_RW1C_MASK);
    if (state->max_slots > 255U) state->max_slots = 255U;

    if (!xhci_controller_reset(state) || !xhci_controller_setup_rings(state)) {
        xhci_set_error(22U);
        xhci_controller_free_rings(state);
        xhci_controller_unmap_mmio(state);
        return false;
    }

    state->event_index = 0U;
    state->event_cycle = 1U;
    state->command_index = 0U;
    state->command_cycle = 1U;
    state->initialized = true;
    return true;
}

/* xHCI commands need a running controller, so Slot teardown precedes the
 * controller halt and ring/MMIO release. Preserve the original failure code
 * if cleanup is entered after an earlier initialization error. */
bool xhci_core_destroy(xhci_state_t *state) {
    if (state == 0) return false;

    uint32_t original_error = xhci_last_error();
    bool released = true;
    bool hid_controller = state == &g_xhci_hid;

    if (!hid_controller) xhci_runtime_stop();
    if (hid_controller) g_xhci_hid_active = false;
    xhci_core_unbind_msix(state);

    if (hid_controller) {
        if (!xhci_free_hid_device_resources(state)) released = false;
    } else if (!xhci_free_slot_resources(state)) {
        released = false;
    }

    if (!xhci_controller_halt(state)) released = false;

    if (!xhci_controller_free_rings(state)) released = false;
    xhci_zero_device_context(&state->device);
    if (!hid_controller) xhci_clear_device_flags();
    xhci_controller_unmap_mmio(state);
    state->initialized = false;
    state->pci = 0;

    if (original_error != 0U) xhci_set_error(original_error);
    return released;
}
