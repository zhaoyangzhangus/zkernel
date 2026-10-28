#include <kernel/console.h>
#include <kernel/realtest.h>
#include <kernel/xhci.h>
#include "internal.h"

/* REFACTOR_P8_XHCI_SELF_TEST_OWNER：控制器和运行时验证。 */
static bool xhci_hardware_self_test_one(const pci_device_t *pci) {
    /* V3.10.3D1 已内联自测激活逻辑。
     * V3.10.3C 之后不再保留自测激活辅助函数。
     */

    xhci_clear_device_flags();
    xhci_runtime_reset_state();
    xhci_hid_runtime_initialize();
    xhci_hid_init();
    if (pci == 0) return false;
    xhci_clear_error();
    if (!xhci_core_initialize(xhci_controller_state(), pci)) return false;
    uint8_t selected_slot = 0U;
    bool has_device =
        xhci_probe_connected_ports(
            xhci_controller_state(),
            &selected_slot);
    if (!has_device && xhci_last_error() != 0U) {
        xhci_core_destroy(xhci_controller_state());
        return false;
    }
    uint8_t slot = 0;
    bool keep_controller = false;
    bool success;
    if(has_device)
    {
        success =
            false;

        if(selected_slot != 0U)
        {
            xhci_slot_device_t *slot_dev =
                xhci_topology_slot(selected_slot);

            if(slot_dev->used &&
               slot_dev->context.device_slot ==
                   selected_slot)
            {
                /*
                 * V3.10.6B8C 自测直接使用 Slot
                 *
                 * selected_slot 已指向永久的规范上下文。
                 */
                xhci_recompute_topology(
                    xhci_controller_state());

                success =
                    true;

                /* 探测成功时已经建立了生产环境所需的控制器状态。此处重置并重新枚举同一设备，
                 * 会在首个 SCSI 命令前使刚配置的 UAS 端点失效。保留稳定 Slot；显式重新枚举辅助函数
                 * 仍供需要专门测试分离/重新连接生命周期的测试使用。 */
                keep_controller =
                    true;
                realtest_mark("XHCI_STABLE_SLOT_RETAINED");
            }
        }

        if(!success)
        {
            xhci_set_error(69U);
        }
    } else {
        success = xhci_submit_command(xhci_controller_state(), XHCI_COMMAND_RING_TYPE, 0, 0, &slot) &&
                  slot != 0U;
    }
    uint32_t saved_error = success ? 0U : xhci_last_error();
    if (success && keep_controller) {
        xhci_recompute_topology(xhci_controller_state());

        /*
         * Hub 状态 TD 会在运行时前预先挂起。此时 MSI-X 仍对运行时消费者屏蔽，先排空所有设置阶段
         * 的完成事件，再确保每个 Hub 恰好有一个待处理状态 TD。
         */
        if(!xhci_drain_startup_events(
               xhci_controller_state(),
               &g_xhci_event_dispatch_ops))
        {
            if(xhci_last_error() == 0U)
            {
                xhci_set_error(76U);
            }

            kernel_serial_write(
                "OS_XHCI_EVENT_DRAIN_FAIL\r\n");

            success = false;
            keep_controller = false;
        }
        else if(!xhci_hub_runtime_start(
                    xhci_controller_state(),
                    &g_xhci_hub_runtime_ops))
        {
            /* 可选 Hub 故障不能导致直接连接的根 MSC 设备不可用。保留存储和 HID 运行时功能；
             * 此控制器的 Hub 热插拔路径保持禁用。 */
            if(xhci_usb_mass_storage_configured())
            {
                realtest_mark(
                    "XHCI_HUB_RUNTIME_OPTIONAL_FAIL_WITH_MSC");
                kernel_serial_write(
                    "OS_XHCI_HUB_RUNTIME_OPTIONAL_FAIL\r\n");
                xhci_clear_error();
            }
            else
            {
                if(xhci_last_error() == 0U)
                {
                    xhci_set_error(76U);
                }

                kernel_serial_write(
                    "OS_XHCI_HUB_RUNTIME_FAIL\r\n");

                success = false;
                keep_controller = false;
            }
        }

    /* xHCI 运行时 I/O 由中断驱动。没有 MSI 或 MSI-X 路由的控制器会记为不可用，而不是轮询。 */
    xhci_runtime_set_ready(success);
    if (success && !xhci_core_bind_msix(xhci_controller_state())) {
            uint32_t irq_error = xhci_last_error();
            uint32_t irq_stage = xhci_msix_failure_stage();
            realtest_mark_number("XHCI_PRIMARY_MSIX_ERROR",
                                        irq_error);
            realtest_mark_number("XHCI_PRIMARY_MSIX_STAGE",
                                        irq_stage);
            kernel_serial_write("OS_XHCI_MSIX_FAIL_STAGE=");
            kernel_serial_write_u32(irq_stage);
            kernel_serial_write("\r\n");
            success = false;
            keep_controller = false;
            xhci_runtime_set_ready(false);
            realtest_mark_number("XHCI_PRIMARY_IRQ_MODE", 0U);
            kernel_serial_write("OS_XHCI_IRQ_UNAVAILABLE\r\n");
        } else if(success) {
            realtest_mark_number("XHCI_PRIMARY_IRQ_MODE", 1U);
            kernel_serial_write("OS_XHCI_MSIX_OK\r\n");
            kernel_serial_write(
                "OS_XHCI_HUB_RUNTIME_OK\r\n");

            /* 仅统计 MSI-X 启用后的运行时 HID 完成事件。 */
            xhci_hid_runtime_reset_completion_counters();

            /*
             * 完成事件可能落在启动事件排空之后、MSI-X 取消屏蔽之前的短暂绑定窗口。
             * 安排一次有界延迟处理，避免事件滞留。
             */
            (void)xhci_schedule_deferred_work();
        }
    }
    if (slot != 0U && !keep_controller &&
        !xhci_submit_command(xhci_controller_state(), XHCI_DISABLE_SLOT_TYPE, slot, 0, 0)) {
        if (success) saved_error = xhci_last_error();
        success = false;
    }
    if (!success && saved_error == 0U) saved_error = xhci_last_error() != 0U ? xhci_last_error() : 23U;
    if (!success) xhci_set_error(saved_error);
    if (!success) xhci_runtime_set_ready(false);
    if (!success || !keep_controller) xhci_core_destroy(xhci_controller_state());
    return success;
}

static bool xhci_start_auxiliary_hid(const pci_device_t *pci) {
    xhci_state_t *state = xhci_hid_controller_state();

    if (state == 0 || pci == 0 || state->initialized) return false;
    xhci_hid_controller_set_active(false);
    xhci_clear_error();
    realtest_mark_number("XHCI_AUX_DEVICE", pci->device_id);
    realtest_mark_number("XHCI_AUX_BUS", pci->bus);
    realtest_mark_number("XHCI_AUX_PCI_STATUS", pci->status);
    realtest_mark_number("XHCI_AUX_PCI_COMMAND", pci->command);
    realtest_mark_number("XHCI_AUX_MSI_CAP", pci->msi_capability);
    realtest_mark_number("XHCI_AUX_MSIX_CAP", pci->msix_capability);
    realtest_mark_number("XHCI_AUX_MSIX_ENTRIES",
                                pci->msix_table_size);
    realtest_mark_number("XHCI_AUX_MSIX_BAR", pci->msix_table_bar);
    realtest_mark_number("XHCI_AUX_MSIX_OFFSET",
                                pci->msix_table_offset);
    if (pci->msix_table_bar < PCI_MAX_BARS) {
        realtest_mark_number("XHCI_AUX_MSIX_BAR_FLAGS",
                                    pci->bars[pci->msix_table_bar].flags);
        realtest_mark_number("XHCI_AUX_MSIX_BAR_LENGTH_LO",
                                    (uint32_t)pci->bars[pci->msix_table_bar].length);
    }
    if (!xhci_core_initialize(state, pci)) {
        realtest_mark_number("XHCI_AUX_INIT_ERROR", xhci_last_error());
        kernel_serial_write("OS_XHCI_AUX_HID_FAIL\r\n");
        return false;
    }
    if (!xhci_probe_hid_controller(state)) {
        realtest_mark_number("XHCI_AUX_PROBE_ERROR", xhci_last_error());
        kernel_serial_write("OS_XHCI_AUX_HID_FAIL\r\n");
        (void)xhci_core_destroy(state);
        return false;
    }
    if (!xhci_core_bind_msix(state)) {
        uint32_t irq_stage = xhci_msix_failure_stage();
        realtest_mark_number("XHCI_AUX_MSIX_ERROR", xhci_last_error());
        realtest_mark_number("XHCI_AUX_MSIX_STAGE",
                                    irq_stage);
        /* 仅完成枚举和端点设置还不足以支持运行时 HID：没有中断路由就没有事件驱动的完成路径，
         * 因此释放该控制器并记录失败标记。 */
        xhci_hid_controller_set_active(false);
        realtest_mark_number("XHCI_AUX_HID_IRQ_MODE", 0U);
        realtest_mark("XHCI_AUX_HID_IRQ_UNAVAILABLE");
        kernel_serial_write("OS_XHCI_AUX_HID_IRQ_UNAVAILABLE\r\n");
        (void)xhci_core_destroy(state);
        return false;
    }
    xhci_hid_controller_set_active(true);
    realtest_mark_number("XHCI_AUX_HID_IRQ_MODE", 1U);
    realtest_mark("XHCI_AUX_HID_READY");
    kernel_serial_write("OS_XHCI_AUX_HID_MSIX_OK\r\n");
    (void)xhci_schedule_deferred_work();
    return true;
}

static void xhci_start_auxiliary_hid_controllers(uint32_t primary_index) {
    uint32_t count = xhci_pci_controller_count();

    for (uint32_t index = 0U; index < count; ++index) {
        const pci_device_t *controller;

        if (index == primary_index) continue;
        controller = xhci_pci_controller_at(index);
        realtest_mark_number("XHCI_AUX_CONTROLLER_BEGIN", index);
        if (xhci_start_auxiliary_hid(controller)) {
            realtest_mark_number("XHCI_AUX_CONTROLLER_READY", index);
            return;
        }
    }
}

bool xhci_hardware_self_test(void) {
    uint32_t controller_count = xhci_pci_controller_count();
    const pci_device_t *fallback_controller = 0;
    xhci_set_hardware_present(controller_count != 0U);
    if (controller_count == 0U) return true;
    for (uint32_t index = 0U; index < controller_count; ++index) {
        const pci_device_t *controller = xhci_pci_controller_at(index);
        realtest_mark_number("XHCI_CONTROLLER_BEGIN", index);
        if (controller != 0) {
            realtest_mark_number("XHCI_CONTROLLER_DEVICE",
                                        controller->device_id);
            realtest_mark_number("XHCI_CONTROLLER_BUS",
                                        controller->bus);
        }
        if (!xhci_hardware_self_test_one(controller)) {
            realtest_mark_number("XHCI_CONTROLLER_ERROR",
                                        xhci_last_error());
            continue;
        }
        /* 前一个控制器可能产生了详细的 EP0 失败信息。后续控制器接管可用 USB 拓扑后，
         * 该失败不再导致启动失败。 */
        realtest_clear_failure_state();

        /* 控制器自测通过还不足以保证真机启动：必须能通过 MSC 设备访问 FAT 卷。
         * 固件公开多个独立 xHCI 功能时，优先选择该控制器。 */
        if (xhci_usb_mass_storage_configured()) {
            realtest_mark_number("XHCI_CONTROLLER_MSC", index);
            xhci_start_auxiliary_hid_controllers(index);
            return true;
        }
        realtest_mark_number("XHCI_CONTROLLER_NO_MSC", index);
        fallback_controller = controller;
        if (index + 1U < controller_count) {
            (void)xhci_core_destroy(xhci_controller_state());
        } else {
            /* 即使没有连接 USB 大容量存储，最后一个控制器仍可用于 HID。它已初始化，
             * 不要在运行中的控制器上再次执行自测。 */
            realtest_mark("XHCI_FALLBACK_READY");
            return true;
        }
    }

    /* 没有控制器管理大容量存储时，最后保留可用的纯 HID 控制器。由于探测其他控制器会拆除
     * 共用的 xHCI 状态，因此需要重新运行该控制器自测。 */
    if (fallback_controller != 0 &&
        xhci_hardware_self_test_one(fallback_controller)) {
        realtest_clear_failure_state();
        realtest_mark("XHCI_FALLBACK_READY");
        return true;
    }
    return false;
}
