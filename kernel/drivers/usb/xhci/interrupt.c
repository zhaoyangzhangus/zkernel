#include <arch/x86_64/cpu.h>
#include <kernel/console.h>
#include <kernel/irq.h>
#include <kernel/pci.h>
#include <kernel/xhci.h>
#include "internal.h"

/* REFACTOR_P8_XHCI_INTERRUPT_OWNER：MSI-X 绑定、ISR 确认、延迟工作触发、
 * 待处理中断边消费及中断诊断。 */

#define XHCI_RUNTIME_INTR0 (0x20U)
#define XHCI_IMAN_IE      (1U << 1)
#define XHCI_IMAN_IP      (1U << 0)

static atomic_bool g_xhci_irq_pending;
static atomic_bool g_xhci_irq_seen;
static atomic_bool g_xhci_primary_irq_seen;
static atomic_bool g_xhci_aux_irq_seen;
static uint32_t g_xhci_msix_failure_stage;

void xhci_interrupt_reset_state(void) {
    atomic_init(&g_xhci_irq_pending, false);
    atomic_init(&g_xhci_irq_seen, false);
    atomic_init(&g_xhci_primary_irq_seen, false);
    atomic_init(&g_xhci_aux_irq_seen, false);
    g_xhci_msix_failure_stage = 0U;
}

bool xhci_interrupt_take_pending(void) {
    return atomic_exchange_explicit(&g_xhci_irq_pending, false,
                                    memory_order_acq_rel);
}

uint32_t xhci_msix_failure_stage(void) {
    return g_xhci_msix_failure_stage;
}

static void xhci_msix_handler(uint8_t vector, struct arch_trap_frame *frame,
                              void *context) {
    xhci_state_t *state = (xhci_state_t *)context;
    (void)vector;
    (void)frame;
    if (state == 0 || !state->initialized || state->irq_vector == 0U) return;
    if (!atomic_exchange_explicit(&g_xhci_irq_seen, true,
                                  memory_order_acq_rel)) {
        kernel_serial_write_serial_only("OS_XHCI_IRQ_FIRED\r\n");
    }
    atomic_bool *controller_irq_seen =
        state == xhci_hid_controller_state() ?
            &g_xhci_aux_irq_seen : &g_xhci_primary_irq_seen;
    if (!atomic_exchange_explicit(controller_irq_seen, true,
                                  memory_order_acq_rel)) {
        kernel_serial_printf_serial_only(
            "OS_DIAG_XHCI_IRQ_CONTROLLER=%s VECTOR=%u\r\n",
            state == xhci_hid_controller_state() ? "AUX" : "PRIMARY",
            vector);
    }

    /* ISR 仅确认中断器并排入有界工作；绝不在中断级遍历事件环或生成用户输入。 */
    /* IMAN.IP 为 RW1C。清除待处理位的同时保留 IE；首次 HID 完成后若 IP 仍置位，
     * QEMU 会屏蔽后续中断边，使 hid_transfer_pending 永远无法清除。 */
    (void)xhci_controller_write32(state, state->runtime_offset + XHCI_RUNTIME_INTR0,
                       XHCI_IMAN_IP | XHCI_IMAN_IE);
    atomic_store_explicit(&g_xhci_irq_pending, true, memory_order_release);
    (void)xhci_schedule_deferred_work();
}

static void xhci_report_msix_metadata(const pci_device_t *device) {
    uint32_t address_low = 0U;
    uint32_t address_high = 0U;
    uint32_t length_low = 0U;
    uint32_t length_high = 0U;

    if (device == 0) return;
    if (device->msix_table_bar < PCI_MAX_BARS) {
        const pci_bar_t *bar = &device->bars[device->msix_table_bar];
        address_low = (uint32_t)bar->address;
        address_high = (uint32_t)(bar->address >> 32);
        length_low = (uint32_t)bar->length;
        length_high = (uint32_t)(bar->length >> 32);
    }
    kernel_serial_printf_serial_only(
        "OS_XHCI_MSIX_DETAIL CAP=%u BAR=%u OFF=%u ENTRIES=%u "
        "ADDR_LO=%u ADDR_HI=%u LEN_LO=%u LEN_HI=%u\r\n",
        device->msix_capability, device->msix_table_bar,
        device->msix_table_offset, device->msix_table_size,
        address_low, address_high, length_low, length_high);
}

bool xhci_core_bind_msix(xhci_state_t *state) {
    paddr_t table;
    uint16_t entries;
    uint8_t vector = state == xhci_hid_controller_state() ? 0x59U : 0x58U;
    bool use_msix;
    g_xhci_msix_failure_stage = 0U;
    if (state == 0 || state->pci == 0 || vector > IRQ_VECTOR_LAST) {
        g_xhci_msix_failure_stage = 1U;
        return false;
    }
    use_msix = pci_msix_table(state->pci, &table, &entries) == K_OK &&
               entries != 0U;
    if (!use_msix && state->pci->msi_capability == 0U) {
        g_xhci_msix_failure_stage = 2U;
        xhci_report_msix_metadata(state->pci);
        return false;
    }
    /* MSI-X 禁用期间，枚举过程可能使中断器保持待处理状态。启用 PCI 向量前先清除该旧中断边；
     * 随后的新传输完成便会产生新的中断。 */
    xhci_event_handler_complete(state);
    if (!xhci_controller_write32(state, state->runtime_offset + XHCI_RUNTIME_INTR0,
                      XHCI_IMAN_IP | XHCI_IMAN_IE)) {
        g_xhci_msix_failure_stage = 3U;
        return false;
    }
    if (irq_register(vector, xhci_msix_handler, state) != K_OK) {
        g_xhci_msix_failure_stage = 4U;
        return false;
    }
    state->irq_vector = vector;
    kstatus_t status = use_msix ?
        pci_msix_configure((pci_device_t *)state->pci, 0U,
                           x86_current_apic_id(), vector) :
        pci_msi_configure((pci_device_t *)state->pci,
                          x86_current_apic_id(), vector);
    if (status != K_OK) {
        g_xhci_msix_failure_stage = 5U;
        if (use_msix) xhci_report_msix_metadata(state->pci);
        kernel_serial_printf_serial_only(use_msix ?
            "OS_XHCI_MSIX_CONFIG_STATUS=%u\r\n" :
            "OS_XHCI_MSI_CONFIG_STATUS=%u\r\n",
            (uint32_t)(-status));
        (void)irq_unregister(vector, xhci_msix_handler, state);
        state->irq_vector = 0U;
        return false;
    }
    if (use_msix) {
        status = pci_msix_mask((pci_device_t *)state->pci, 0U, false);
        if (status != K_OK) {
            g_xhci_msix_failure_stage = 6U;
            xhci_report_msix_metadata(state->pci);
            kernel_serial_printf_serial_only(
                "OS_XHCI_MSIX_MASK_STATUS=%u\r\n",
                (uint32_t)(-status));
            (void)irq_unregister(vector, xhci_msix_handler, state);
            state->irq_vector = 0U;
            return false;
        }
    }
    state->irq_msi = !use_msix;
    if (!use_msix) {
        kernel_serial_write_serial_only("OS_XHCI_MSI_FALLBACK_OK\r\n");
    }
    return true;
}

void xhci_core_unbind_msix(xhci_state_t *state) {
    if (state == 0 || state->irq_vector == 0U) return;
    if (state->irq_msi) {
        (void)pci_msi_disable((pci_device_t *)state->pci);
    } else {
        (void)pci_msix_mask((pci_device_t *)state->pci, 0U, true);
    }
    (void)irq_unregister(state->irq_vector, xhci_msix_handler, state);
    state->irq_vector = 0U;
    state->irq_msi = false;
}
