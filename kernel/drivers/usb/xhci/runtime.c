#include <kernel/console.h>
#include <kernel/deferred.h>
#include <kernel/xhci.h>
#include "internal.h"

/* REFACTOR_P8_XHCI_RUNTIME_OWNER：运行时事件排空、延迟工作合并及
 * 事件后的中断器诊断。 */
/* REFACTOR_P8_XHCI_RUNTIME_STATE_OWNER：运行时就绪状态仅属于本编译单元；
 * 控制器自测和事件消费者使用下方的窄接口。 */

static bool g_xhci_runtime_ready;
static uint32_t g_xhci_runtime_event_batches;
static atomic_bool g_xhci_work_queued;
static atomic_bool g_xhci_resume_pending;

bool xhci_runtime_ready(void) {
    return g_xhci_runtime_ready;
}

void xhci_runtime_set_ready(bool ready) {
    g_xhci_runtime_ready = ready;
}

void xhci_runtime_reset_state(void) {
    g_xhci_runtime_ready = false;
    g_xhci_runtime_event_batches = 0U;
    atomic_init(&g_xhci_work_queued, false);
    atomic_init(&g_xhci_resume_pending, false);
    xhci_interrupt_reset_state();
}

void xhci_runtime_stop(void) {
    g_xhci_runtime_ready = false;
    g_xhci_runtime_event_batches = 0U;
}

/* 发布等待意图后重试一次，关闭“解锁恰好发生在登记之前”的丢失唤醒窗口。
 * 两次都失败就交回 CPU，持锁者解锁时负责通知，不读取别人正在修改的事件环状态。 */
static bool xhci_runtime_acquire_events(xhci_state_t *state) {
    if (state == 0 || !state->initialized) return false;
    if (!xhci_event_try_lock(state)) {
        atomic_store_explicit(&state->event_work_waiting, true, memory_order_release);
        if (!xhci_event_try_lock(state)) return false;
    }
    atomic_store_explicit(&state->event_work_waiting, false, memory_order_release);
    return true;
}

static bool xhci_runtime_events_pending(xhci_state_t *state) {
    if (!xhci_runtime_acquire_events(state)) return false;
    bool pending = xhci_event_pending(state);
    xhci_event_unlock(state);
    return pending;
}

static bool xhci_process_primary_events(uint32_t budget) {
    xhci_state_t *state = xhci_controller_state();
    bool consumed;

    if (!xhci_runtime_acquire_events(state)) {
        return false;
    }
    consumed = xhci_process_event_ring(state, budget,
                                       &g_xhci_event_dispatch_ops);
    xhci_event_handler_complete(state);
    xhci_event_unlock(state);
    return consumed;
}

static bool xhci_process_aux_hid_events(uint32_t budget) {
    xhci_state_t *state = xhci_hid_controller_state();
    bool consumed = false;

    if (!xhci_hid_controller_active() || !xhci_runtime_acquire_events(state)) {
        return false;
    }
    for (uint32_t index = 0U; index < budget; ++index) {
        xhci_trb_t event;
        uint32_t type;

        if (!xhci_next_event(state, &event)) break;
        type = (event.control >> XHCI_TRB_TYPE_SHIFT) & 0x3FU;
        if (type == XHCI_TRANSFER_EVENT_TYPE &&
            xhci_handle_hid_transfer_event(state, &event)) {
            consumed = true;
        }
    }
    xhci_event_handler_complete(state);
    xhci_event_unlock(state);
    return consumed;
}

bool xhci_process_events(uint32_t budget) {
    bool primary_consumed;
    bool aux_consumed;

    if (budget == 0U) budget = 1U;
    if (!g_xhci_runtime_ready) return false;
    ++g_xhci_runtime_event_batches;
    if (g_xhci_runtime_event_batches == 1U) {
        kernel_serial_write_serial_only("OS_USB_RUNTIME_IRQ_OK\r\n");
    }
    /* 两个独立的 xHCI 事件环由同一个合并后的工作线程排空，但各自保留锁和 ERDP 状态。 */
    primary_consumed = xhci_process_primary_events(budget);
    aux_consumed = xhci_process_aux_hid_events(budget);
    return primary_consumed || aux_consumed;
}

bool xhci_schedule_deferred_work(void) {
    bool expected = false;
    if (!g_xhci_runtime_ready) {
        return false;
    }
    if (!atomic_compare_exchange_strong_explicit(&g_xhci_work_queued, &expected,
                                                 true, memory_order_acq_rel,
                                                 memory_order_acquire)) {
        return false;
    }
    /* xHCI 是预留关键槽位的所有者。HID 和 Hub 事件不能排在无关的普通延迟工作之后；
     * 普通队列仍作为关键槽位罕见竞争时的有界后备路径。 */
    if (deferred_schedule_critical(xhci_deferred_work, 0)) {
        return true;
    }
    if (deferred_try_schedule(xhci_deferred_work, 0)) {
        return true;
    }
    atomic_store_explicit(&g_xhci_work_queued, false,
                          memory_order_release);
    return false;
}

/* 独立保存交接边沿：通知到达时工作项可能尚未释放合并标志，不能只尝试排队一次。 */
void xhci_runtime_resume_events(void) {
    atomic_store_explicit(&g_xhci_resume_pending, true, memory_order_release);
    (void)xhci_schedule_deferred_work();
}

void xhci_deferred_work(void *argument) {
    bool consumed = false;
    bool pending = false;
    bool raced_irq = false;
    uint32_t pass = 0U;
    (void)argument;
    /* 排空事件环前先消费触发此工作项的中断。旧代码只在末尾检查同一标志，
     * 因而会把触发工作的中断误认为新中断；即使事件环为空，工作线程也会不断重新排队。 */
    (void)xhci_interrupt_take_pending();
    (void)atomic_exchange_explicit(&g_xhci_resume_pending, false, memory_order_acq_rel);
    do {
        consumed = xhci_process_events(64U);
        ++pass;
    } while (consumed && pass < 8U);
    xhci_report_hid_completion_milestones();

    /* Release queue ownership before the final IRQ handshake.  An MSI-X can
     * arrive while this worker is draining the ring; the worker inherits that
     * coalesced IRQ before deciding whether to schedule another bounded pass. */
    atomic_store_explicit(&g_xhci_work_queued, false,
                          memory_order_release);

    pending = xhci_runtime_events_pending(xhci_controller_state());
    if (xhci_hid_controller_active() &&
        xhci_runtime_events_pending(xhci_hid_controller_state())) {
        pending = true;
    }
    raced_irq = xhci_interrupt_take_pending();

    bool resumed = atomic_exchange_explicit(&g_xhci_resume_pending, false,
                                            memory_order_acq_rel);
    if (pending || raced_irq || resumed) {
        (void)xhci_schedule_deferred_work();
    }
}
