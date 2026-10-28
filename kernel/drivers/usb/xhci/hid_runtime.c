#include "internal.h"

#include <kernel/console.h>
#include <kernel/realtest.h>

#include <stdatomic.h>

#ifndef OS_REALTEST
#define OS_REALTEST 0
#endif

/* REFACTOR_P8_XHCI_HID_RUNTIME_OWNER：HID 报告状态传递、中断排队和
 * 停止端点恢复由同一个直接所有者负责。 */

#define XHCI_RING_TRB_COUNT 256U
#define XHCI_LINK_TRB_TYPE 6U
#define XHCI_SET_TR_DEQUEUE_POINTER_TYPE 16U
#define XHCI_TRB_CYCLE (1U << 0)
#define XHCI_TRB_CHAIN (1U << 4)
#define XHCI_TRB_INTERRUPT_ON_COMPLETION (1U << 5)
#define XHCI_TRB_LINK_TOGGLE_CYCLE (1U << 1)
/* 对 interrupt-IN 报告而言，Short Packet 属于成功完成。 */
#define XHCI_COMPLETION_SHORT_PACKET 13U

static volatile uint32_t g_xhci_hid_recovery_attempts;
static volatile uint32_t g_xhci_hid_recovery_failures;
static volatile uint32_t g_xhci_hid_recovery_stage;
static volatile uint32_t g_xhci_hid_recovery_command_error;
static atomic_bool g_xhci_hid_transfer_seen;
static atomic_bool g_xhci_hid_arm_seen;
static atomic_bool g_xhci_hid_secondary_arm_seen;
static atomic_bool g_xhci_hid_secondary_transfer_seen;
static atomic_bool g_xhci_hid_lookup_failure_seen;
static atomic_uint g_xhci_hid_report_protocols;
static atomic_uint g_xhci_hid_completion_count;
static atomic_uint g_xhci_hid_completion_milestone;

void xhci_hid_runtime_initialize(void) {
    atomic_init(&g_xhci_hid_transfer_seen, false);
    atomic_init(&g_xhci_hid_arm_seen, false);
    atomic_init(&g_xhci_hid_secondary_arm_seen, false);
    atomic_init(&g_xhci_hid_secondary_transfer_seen, false);
    atomic_init(&g_xhci_hid_lookup_failure_seen, false);
    atomic_init(&g_xhci_hid_report_protocols, 0U);
    atomic_init(&g_xhci_hid_completion_count, 0U);
    atomic_init(&g_xhci_hid_completion_milestone, 0U);
}

void xhci_hid_runtime_reset_completion_counters(void) {
    atomic_store_explicit(&g_xhci_hid_completion_count, 0U,
                          memory_order_release);
    atomic_store_explicit(&g_xhci_hid_completion_milestone, 0U,
                          memory_order_release);
}

#if OS_REALTEST
static bool xhci_hid_report_diag_once(uint32_t protocol) {
    uint32_t bit;
    uint32_t observed;

    if (protocol >= 32U) return false;
    bit = 1U << protocol;
    observed = atomic_load_explicit(&g_xhci_hid_report_protocols,
                                    memory_order_acquire);
    for (;;) {
        if ((observed & bit) != 0U) return false;
        if (atomic_compare_exchange_weak_explicit(
                &g_xhci_hid_report_protocols, &observed, observed | bit,
                memory_order_acq_rel, memory_order_acquire)) {
            return true;
        }
    }
}
#endif

void xhci_hid_consume_report(xhci_device_context_t *device,
                                    uint32_t report_length) {
    xhci_hid_report_context_t context;

    if (device == 0 || device->hid_report.cpu == 0) return;
    context.report = (const uint8_t *)device->hid_report.cpu;
    context.protocol = device->hid_protocol;
    context.device_slot = device->device_slot;
    context.previous_modifier = device->hid_previous_modifier;
    for (uint32_t index = 0U; index < 6U; ++index) {
        context.previous_keys[index] = device->hid_previous_keys[index];
    }
    context.previous_buttons = device->hid_previous_buttons;

    xhci_hid_consume(&context, report_length);

    device->hid_previous_modifier = context.previous_modifier;
    for (uint32_t index = 0U; index < 6U; ++index) {
        device->hid_previous_keys[index] = context.previous_keys[index];
    }
    device->hid_previous_buttons = context.previous_buttons;
}

void xhci_hid_consume_report_secondary(xhci_device_context_t *device,
                                       uint32_t report_length) {
    xhci_hid_report_context_t context;

    if (device == 0 || device->hid_secondary.report.cpu == 0) return;
    context.report = (const uint8_t *)device->hid_secondary.report.cpu;
    context.protocol = device->hid_secondary.protocol;
    context.device_slot = device->device_slot;
    context.previous_modifier = device->hid_secondary.previous_modifier;
    for (uint32_t index = 0U; index < 6U; ++index) {
        context.previous_keys[index] =
            device->hid_secondary.previous_keys[index];
    }
    context.previous_buttons = device->hid_secondary.previous_buttons;

    xhci_hid_consume(&context, report_length);

    device->hid_secondary.previous_modifier = context.previous_modifier;
    for (uint32_t index = 0U; index < 6U; ++index) {
        device->hid_secondary.previous_keys[index] =
            context.previous_keys[index];
    }
    device->hid_secondary.previous_buttons = context.previous_buttons;
}


bool xhci_queue_hid_report(xhci_state_t *state,
                                  xhci_device_context_t *device) {
    xhci_trb_t *ring;
    uint32_t index;
    uint32_t endpoint_id;
    if (state == 0 || device == 0 || !state->initialized ||
        device->hid_ring.cpu == 0 || device->hid_report.cpu == 0 ||
        device->hid_transfer_pending || device->device_slot == 0U) return false;
    index = device->hid_enqueue;
    if (index >= XHCI_RING_TRB_COUNT - 1U) return false;
    endpoint_id = (uint32_t)device->hid_endpoint * 2U + 1U;
    if (device->hid_endpoint == 0U || endpoint_id > 31U) return false;
    ring = (xhci_trb_t *)device->hid_ring.cpu;
    if (!xhci_transfer_encode_normal(
            &ring[index], xhci_dma_address(&device->hid_report.mapping),
            device->hid_max_packet, XHCI_TRB_INTERRUPT_ON_COMPLETION,
            device->hid_cycle)) return false;
    ++index;
    if (index == XHCI_RING_TRB_COUNT - 1U) {
        ring[XHCI_RING_TRB_COUNT - 1U].control =
            (XHCI_LINK_TRB_TYPE << XHCI_TRB_TYPE_SHIFT) |
            device->hid_cycle | XHCI_TRB_LINK_TOGGLE_CYCLE;
        device->hid_enqueue = 0U;
        device->hid_cycle ^= 1U;
    } else {
        device->hid_enqueue = index;
    }
    device->hid_transfer_pending = true;
    dma_sync_for_device(&device->hid_ring.mapping);
    dma_sync_for_device(&device->hid_report.mapping);
    dma_wmb();
    *(volatile uint32_t *)(state->mmio + state->doorbell_offset +
                           (uint32_t)device->device_slot * sizeof(uint32_t)) = endpoint_id;
    __asm__ volatile ("mfence" : : : "memory");
    if (!atomic_exchange_explicit(&g_xhci_hid_arm_seen, true,
                                  memory_order_acq_rel)) {
        realtest_mark_number("XHCI_HID_ARM_SLOT",
                                    device->device_slot);
        realtest_mark_number("XHCI_HID_ARM_ENDPOINT", endpoint_id);
        realtest_mark_number("XHCI_HID_ARM_PROTOCOL",
                                    device->hid_protocol);
        kernel_serial_printf_serial_only(
            "OS_DIAG_HID_ARM_SLOT=%u EP=%u MAX=%u\r\n",
            device->device_slot, endpoint_id, device->hid_max_packet);
    }
    return true;
}

bool xhci_queue_hid_report_secondary(xhci_state_t *state,
                                     xhci_device_context_t *device) {
    xhci_hid_endpoint_t *hid;
    xhci_trb_t *ring;
    uint32_t index;
    uint32_t endpoint_id;

    if (state == 0 || device == 0 || !state->initialized) return false;
    hid = &device->hid_secondary;
    if (hid->ring.cpu == 0 || hid->report.cpu == 0 ||
        hid->transfer_pending || device->device_slot == 0U) return false;
    index = hid->enqueue;
    if (index >= XHCI_RING_TRB_COUNT - 1U || hid->endpoint == 0U) return false;
    endpoint_id = (uint32_t)hid->endpoint * 2U + 1U;
    if (endpoint_id > 31U) return false;
    ring = (xhci_trb_t *)hid->ring.cpu;
    if (!xhci_transfer_encode_normal(&ring[index],
            xhci_dma_address(&hid->report.mapping), hid->max_packet,
            XHCI_TRB_INTERRUPT_ON_COMPLETION, hid->cycle)) return false;
    ++index;
    if (index == XHCI_RING_TRB_COUNT - 1U) {
        ring[XHCI_RING_TRB_COUNT - 1U].control =
            (XHCI_LINK_TRB_TYPE << XHCI_TRB_TYPE_SHIFT) |
            hid->cycle | XHCI_TRB_LINK_TOGGLE_CYCLE;
        hid->enqueue = 0U;
        hid->cycle ^= 1U;
    } else {
        hid->enqueue = index;
    }
    hid->transfer_pending = true;
    dma_sync_for_device(&hid->ring.mapping);
    dma_sync_for_device(&hid->report.mapping);
    dma_wmb();
    *(volatile uint32_t *)(state->mmio + state->doorbell_offset +
                           (uint32_t)device->device_slot * sizeof(uint32_t)) =
        endpoint_id;
    __asm__ volatile ("mfence" : : : "memory");
    if (!atomic_exchange_explicit(&g_xhci_hid_secondary_arm_seen, true,
                                  memory_order_acq_rel)) {
        kernel_serial_printf_serial_only(
            "OS_DIAG_HID2_ARM_SLOT=%u EP=%u MAX=%u\r\n",
            device->device_slot, endpoint_id, hid->max_packet);
    }
    return true;
}

/* 拓扑转换（或控制器侧端点停止）可能使 interrupt-IN TRB 以 "Stopped" 完成。仅清除
 * hid_transfer_pending 不够：端点仍处于停止状态，后续 doorbell 都会被忽略，用户输入
 * 等待者可能永远无法唤醒。重新启用下一份报告前先复位端点。此处理由事件驱动；
 * 不在 HID 路径中加入轮询循环。 */
bool xhci_restart_hid_endpoint(xhci_state_t *state,
                                      xhci_device_context_t *device) {
    uint32_t endpoint_id;
    xhci_trb_t *ring;
    if (state == 0 || device == 0 || device->device_slot == 0U ||
        device->hid_endpoint == 0U || device->hid_ring.cpu == 0) return false;
    ++g_xhci_hid_recovery_attempts;
    g_xhci_hid_recovery_stage = 1U;
    endpoint_id = (uint32_t)device->hid_endpoint * 2U + 1U;
    if (endpoint_id > 31U) {
        ++g_xhci_hid_recovery_failures;
        g_xhci_hid_recovery_stage = 2U;
        return false;
    }
    /* 完成码为 Stopped 的传输事件会使端点保持停止状态。此时 Reset Endpoint 无效
     *（QEMU 返回 Context State Error）；规定的恢复方式是 Set TR Dequeue Pointer。
     * 重新初始化生产者环，将控制器指向 DCS=1 的首个 TRB，然后在下方敲响端点 doorbell。 */
    ring = (xhci_trb_t *)device->hid_ring.cpu;
    for (uint32_t i = 0U; i < XHCI_RING_TRB_COUNT - 1U; ++i) {
        ring[i].parameter = 0U;
        ring[i].status = 0U;
        ring[i].control = 0U;
    }
    xhci_ring_init_link(device->hid_ring.cpu,
                        xhci_dma_address(&device->hid_ring.mapping),
                        XHCI_RING_TRB_COUNT);
    device->hid_enqueue = 0U;
    device->hid_cycle = 1U;
    device->hid_transfer_pending = false;
    dma_sync_for_device(&device->hid_ring.mapping);
    g_xhci_hid_recovery_stage = 2U;
    if (!xhci_submit_command_ex(state, XHCI_SET_TR_DEQUEUE_POINTER_TYPE,
                                device->device_slot, (uint8_t)endpoint_id,
                                xhci_dma_address(&device->hid_ring.mapping) | 1ULL,
                                0)) {
        g_xhci_hid_recovery_command_error = xhci_last_error();
        /* 延迟工作线程观察事件时，QEMU 可能已恢复该传输却仍报告 Stopped。
         * 此暂态下 Set TR Dequeue Pointer 会正确返回 Context State Error；端点仍可用，
         * 因此只重试生产者，不再发出第二个状态变更命令。 */
        g_xhci_hid_recovery_stage = 3U;
        device->hid_transfer_pending = false;
        if (!xhci_queue_hid_report(state, device)) {
            ++g_xhci_hid_recovery_failures;
            g_xhci_hid_recovery_stage = 4U;
            return false;
        }
        xhci_clear_error();
        g_xhci_hid_recovery_stage = 0U;
        return true;
    }
    g_xhci_hid_recovery_stage = 4U;
    if (!xhci_queue_hid_report(state, device)) {
        ++g_xhci_hid_recovery_failures;
        g_xhci_hid_recovery_stage = 5U;
        return false;
    }
    g_xhci_hid_recovery_stage = 0U;
    return true;
}

bool xhci_restart_hid_endpoint_secondary(xhci_state_t *state,
                                         xhci_device_context_t *device) {
    xhci_hid_endpoint_t *hid;
    uint32_t endpoint_id;
    xhci_trb_t *ring;

    if (state == 0 || device == 0 || device->device_slot == 0U) return false;
    hid = &device->hid_secondary;
    if (hid->endpoint == 0U || hid->ring.cpu == 0) return false;
    endpoint_id = (uint32_t)hid->endpoint * 2U + 1U;
    if (endpoint_id > 31U) return false;
    ring = (xhci_trb_t *)hid->ring.cpu;
    for (uint32_t index = 0U; index < XHCI_RING_TRB_COUNT - 1U; ++index) {
        ring[index].parameter = 0U;
        ring[index].status = 0U;
        ring[index].control = 0U;
    }
    xhci_ring_init_link(hid->ring.cpu, xhci_dma_address(&hid->ring.mapping),
                        XHCI_RING_TRB_COUNT);
    hid->enqueue = 0U;
    hid->cycle = 1U;
    hid->transfer_pending = false;
    dma_sync_for_device(&hid->ring.mapping);
    if (!xhci_submit_command_ex(state, XHCI_SET_TR_DEQUEUE_POINTER_TYPE,
                                device->device_slot, (uint8_t)endpoint_id,
                                xhci_dma_address(&hid->ring.mapping) | 1ULL,
                                0)) {
        hid->transfer_pending = false;
        if (!xhci_queue_hid_report_secondary(state, device)) return false;
        xhci_clear_error();
        return true;
    }
    return xhci_queue_hid_report_secondary(state, device);
}

/* HID context lookup is independent of context storage.  A published Slot
 * context wins; the enumeration context is only a bounded pre-publication
 * fallback for the first interrupt completion. */
static xhci_device_context_t *xhci_find_hid_context(
    xhci_state_t *state,
    uint8_t slot,
    uint32_t endpoint_id) {
    xhci_device_context_t *device = 0;
    xhci_slot_device_t *slot_dev;

    if (state == 0 || slot == 0U) return 0;
    /* Auxiliary controllers intentionally do not publish into the primary
     * controller's Slot Table; their working context is authoritative. */
    if (state == xhci_hid_controller_state() &&
        state->device.device_slot == slot &&
        (state->device.hid_endpoint != 0U ||
         state->device.hid_secondary.endpoint != 0U)) {
        return &state->device;
    }
    slot_dev = xhci_topology_slot(slot);
    if (slot_dev->used && slot_dev->context.device_slot == slot) {
        device = &slot_dev->context;
    } else if (state->device.device_slot == slot) {
        device = &state->device;
    }
    if (device == 0 ||
        (endpoint_id != (uint32_t)device->hid_endpoint * 2U + 1U &&
         endpoint_id != (uint32_t)device->hid_secondary.endpoint * 2U + 1U)) {
        return 0;
    }
    return device;
}

bool xhci_handle_hid_transfer_event(xhci_state_t *state,
                                    const xhci_trb_t *event) {
    xhci_device_context_t *device;
    uint8_t slot;
    uint32_t endpoint_id;
    uint32_t completion;
    uint32_t residual;
    uint32_t report_length;
    bool secondary;
    uint16_t max_packet;
    xhci_dma_page_t *report_page;
    bool *transfer_pending;

    if (state == 0 || event == 0 ||
        ((event->control >> XHCI_TRB_TYPE_SHIFT) & 0x3FU) !=
            XHCI_TRANSFER_EVENT_TYPE) {
        return false;
    }
    slot = (uint8_t)(event->control >> XHCI_TRB_SLOT_SHIFT);
    endpoint_id = (event->control >> XHCI_TRB_ENDPOINT_SHIFT) & 0x1FU;
    device = xhci_find_hid_context(state, slot, endpoint_id);
    if (device == 0) {
        if (slot != 0U &&
            xhci_topology_slot(slot)->used &&
            (xhci_topology_slot(slot)->context.hid_endpoint != 0U ||
             xhci_topology_slot(slot)->context.hid_secondary.endpoint != 0U) &&
            !atomic_exchange_explicit(&g_xhci_hid_lookup_failure_seen, true,
                                      memory_order_acq_rel)) {
            kernel_serial_printf_serial_only(
                "OS_DIAG_HID_LOOKUP_FAIL_SLOT=%u EP=%u EXPECT=%u\r\n",
                slot, endpoint_id,
                (uint32_t)xhci_topology_slot(slot)->context.hid_endpoint *
                    2U + 1U);
        }
        return false;
    }

    secondary = device->hid_secondary.endpoint != 0U &&
                endpoint_id ==
                    (uint32_t)device->hid_secondary.endpoint * 2U + 1U;
    max_packet = secondary ? device->hid_secondary.max_packet :
                              device->hid_max_packet;
    report_page = secondary ? &device->hid_secondary.report :
                              &device->hid_report;
    transfer_pending = secondary ? &device->hid_secondary.transfer_pending :
                                    &device->hid_transfer_pending;

    completion = event->status >> XHCI_COMPLETION_SHIFT;
    residual = event->status & 0x00FFFFFFU;
    if (secondary &&
        !atomic_exchange_explicit(&g_xhci_hid_secondary_transfer_seen, true,
                                  memory_order_acq_rel)) {
        kernel_serial_printf_serial_only(
            "OS_DIAG_HID2_TRANSFER_SLOT=%u EP=%u CC=%u RESIDUAL=%u\r\n",
            slot, endpoint_id, completion, residual);
    }
    if (!atomic_exchange_explicit(&g_xhci_hid_transfer_seen, true,
                                  memory_order_acq_rel)) {
        kernel_serial_write_serial_only("OS_XHCI_HID_TRANSFER_OK\r\n");
    }
    (void)atomic_fetch_add_explicit(&g_xhci_hid_completion_count, 1U,
                                    memory_order_relaxed);

    *transfer_pending = false;
    if ((completion == XHCI_COMPLETION_SUCCESS ||
         completion == XHCI_COMPLETION_SHORT_PACKET) &&
        residual <= max_packet) {
#if OS_REALTEST
        const uint8_t *report;
#endif
        report_length = max_packet - residual;
        dma_sync_for_cpu(&report_page->mapping);
#if OS_REALTEST
        report = (const uint8_t *)report_page->cpu;
        uint32_t protocol = secondary ? device->hid_secondary.protocol :
                                        device->hid_protocol;
        if (xhci_hid_report_diag_once(protocol)) {
            kernel_serial_printf_serial_only(
                "OS_DIAG_HID_REPORT SLOT=%u EP=%u PROTO=%u LEN=%u "
                "B0=%u B1=%u B2=%u B3=%u\r\n",
                slot, endpoint_id, protocol, report_length,
                report_length > 0U ? report[0] : 0U,
                report_length > 1U ? report[1] : 0U,
                 report_length > 2U ? report[2] : 0U,
                 report_length > 3U ? report[3] : 0U);
        }
#endif
        if (secondary) {
            xhci_hid_consume_report_secondary(device, report_length);
        } else {
            xhci_hid_consume_report(device, report_length);
        }
        if (secondary ? !xhci_queue_hid_report_secondary(state, device) :
                        !xhci_queue_hid_report(state, device)) {
            xhci_set_error(90U);
        }
    } else if (completion != 0U) {
        /* A stopped endpoint must be reinitialized before the next doorbell;
         * otherwise the input waiter can remain asleep forever. */
        if (secondary ? !xhci_restart_hid_endpoint_secondary(state, device) :
                        !xhci_restart_hid_endpoint(state, device)) {
            xhci_set_error(60U + completion);
        } else {
            xhci_clear_error();
        }
    }
    return true;
}

/* 同步读盘者已持有事件环锁；正常 HID 报告必须立即消费并重新投递，不能等整次
 * 存储会话结束。异常完成仍留给延迟工作，避免端点恢复的命令等待嵌套消费存储完成。 */
bool xhci_handle_hid_io_event(xhci_state_t *state, const xhci_trb_t *event) {
    if (state == 0 || event == 0 || !xhci_runtime_ready() ||
        ((event->control >> XHCI_TRB_TYPE_SHIFT) & 0x3fU) !=
            XHCI_TRANSFER_EVENT_TYPE) return false;
    uint32_t completion = event->status >> XHCI_COMPLETION_SHIFT;
    if (completion != XHCI_COMPLETION_SUCCESS &&
        completion != XHCI_COMPLETION_SHORT_PACKET) return false;
    uint8_t slot = (uint8_t)(event->control >> XHCI_TRB_SLOT_SHIFT);
    uint32_t endpoint = (event->control >> XHCI_TRB_ENDPOINT_SHIFT) & 0x1fU;
    xhci_device_context_t *device = xhci_find_hid_context(state, slot, endpoint);
    if (device == 0) return false;
    bool secondary = device->hid_secondary.endpoint != 0U &&
        endpoint == (uint32_t)device->hid_secondary.endpoint * 2U + 1U;
    if (!secondary && (device->hid_endpoint == 0U ||
        endpoint != (uint32_t)device->hid_endpoint * 2U + 1U)) return false;
    uint16_t maximum = secondary ? device->hid_secondary.max_packet :
                                  device->hid_max_packet;
    if ((event->status & 0x00ffffffU) > maximum) return false;
    return xhci_handle_hid_transfer_event(state, event);
}

void xhci_report_hid_completion_milestones(void) {
    uint32_t completed = atomic_load_explicit(&g_xhci_hid_completion_count,
                                              memory_order_acquire);
    unsigned target = completed >= XHCI_RING_TRB_COUNT * 2U ? 2U :
                      completed >= XHCI_RING_TRB_COUNT ? 1U : 0U;
    unsigned observed = atomic_load_explicit(&g_xhci_hid_completion_milestone,
                                             memory_order_acquire);
    while (observed < target) {
        unsigned next = observed + 1U;
        if (!atomic_compare_exchange_weak_explicit(
                &g_xhci_hid_completion_milestone, &observed, next,
                memory_order_acq_rel, memory_order_acquire)) {
            continue;
        }
        if (next == 1U) {
            kernel_serial_write_serial_only("OS_XHCI_HID_EVENTS_256\r\n");
        } else if (next == 2U) {
            kernel_serial_write_serial_only("OS_XHCI_HID_EVENTS_512\r\n");
        }
    }
}
