#include <usb/device.h>
#include <usb/hub.h>
#include "internal.h"

/* REFACTOR_P8_XHCI_DEVICE_LIFECYCLE_OWNER：设备上下文 DMA 生命周期、Slot 发布和
 * 控制器拆除由同一个私有所有者负责。 */

#define XHCI_STOP_ENDPOINT_TYPE 15U
#define XHCI_DISABLE_SLOT_TYPE 10U

static bool xhci_stop_pending_endpoints_device(
    xhci_state_t *state,
    xhci_device_context_t *device);

void xhci_zero_device_context(xhci_device_context_t *context) {
    xhci_device_context_clear(context, sizeof(*context));
}


/*
 * V3.10.6B8B
 *
 * 已移除 xhci_copy_device_context()。
 * 不会在所有权存储之间复制已发布的设备上下文。
 */


void xhci_clear_device_flags(void) {
    xhci_clear_topology();
}

/*
 * V3.10.6B9C-FIX1
 *
 * 已移除 xhci_set_device_kind()。
 * 已发布的运行时状态由 Slot Table 重新计算。
 */


/*
 * V3.10.6B6C 显式设备资源拆除
 *
 * DMA/资源生命周期属于显式设备上下文。
 */

static bool xhci_free_device_resources_device(
    xhci_device_context_t *device) {
    bool released = true;
    if (device == 0) return false;
    if (device->audio_stream != 0) {
        /* 热拔出时先将统一音频对象切换到 DISCONNECTED，再释放其端点 DMA。 */
        audio_stream_t *stream = device->audio_stream;
        device->audio_stream = 0;
        device->audio_stream_running = false;
        device->audio_stream_queued = false;
        (void)audio_stream_disconnect(stream);
    }
    if (device->bt_controller != 0) {
        (void)bt_controller_disconnect(device->bt_controller);
        bt_controller_destroy(device->bt_controller);
        device->bt_controller = 0;
    }
    if (device->msc_configured && device->device_slot != 0U) {
        xhci_msc_transport_release(device->device_slot);
        device->msc_interface = 0U;
        device->msc_bulk_in_endpoint = 0U;
        device->msc_bulk_out_endpoint = 0U;
        device->msc_bulk_in_max_burst = 0U;
        device->msc_bulk_out_max_burst = 0U;
        device->msc_bulk_in_max_packet = 0U;
        device->msc_bulk_out_max_packet = 0U;
        device->msc_configured = false;
    }
    device->msc_uas_present = false;
    device->msc_uas_interface = 0U;
    device->msc_uas_alternate = 0U;
    device->msc_uas_command_endpoint = 0U;
    device->msc_uas_status_endpoint = 0U;
    device->msc_uas_data_in_endpoint = 0U;
    device->msc_uas_data_out_endpoint = 0U;
    device->msc_uas_command_address = 0U;
    device->msc_uas_status_address = 0U;
    device->msc_uas_data_in_address = 0U;
    device->msc_uas_data_out_address = 0U;
    device->msc_uas_command_max_burst = 0U;
    device->msc_uas_status_max_burst = 0U;
    device->msc_uas_data_in_max_burst = 0U;
    device->msc_uas_data_out_max_burst = 0U;
    device->msc_uas_status_max_streams = 0U;
    device->msc_uas_data_in_max_streams = 0U;
    device->msc_uas_data_out_max_streams = 0U;
    device->msc_uas_command_max_packet = 0U;
    device->msc_uas_status_max_packet = 0U;
    device->msc_uas_data_in_max_packet = 0U;
    device->msc_uas_data_out_max_packet = 0U;

    if(!xhci_free_page(
           &device->descriptor_buffer))
    {
        released =
            false;
    }
    if(!xhci_free_page(
           &device->hid_report))
    {
        released =
            false;
    }
    if(!xhci_free_page(
           &device->hid_ring))
    {
        released =
            false;
    }
    if(!xhci_free_page(
           &device->hid_secondary.report))
    {
        released =
            false;
    }
    if(!xhci_free_page(
           &device->hid_secondary.ring))
    {
        released =
            false;
    }
    if(!xhci_free_page(
           &device->audio_buffer))
    {
        released =
            false;
    }
    if(!xhci_free_page(
           &device->audio_ring))
    {
        released =
            false;
    }
    if(!xhci_free_page(
           &device->bt_acl_out_ring))
    {
        released =
            false;
    }
    if(!xhci_free_page(
           &device->bt_acl_out_buffer))
    {
        released =
            false;
    }
    if(!xhci_free_page(
           &device->bt_acl_in_buffer))
    {
        released =
            false;
    }
    if(!xhci_free_page(
           &device->bt_acl_in_ring))
    {
        released =
            false;
    }
    if(!xhci_free_page(
           &device->bt_event_buffer))
    {
        released =
            false;
    }
    if(!xhci_free_page(
           &device->bt_event_ring))
    {
        released =
            false;
    }
    if(!xhci_free_page(
           &device->hub_report))
    {
        released =
            false;
    }
    if(!xhci_free_page(
           &device->hub_ring))
    {
        released =
            false;
    }
    if(!xhci_free_page(
           &device->ep0_ring))
    {
        released =
            false;
    }
    if(!xhci_free_page(
           &device->input_context))
    {
        released =
            false;
    }
    if(!xhci_free_page(
           &device->output_context))
    {
        released =
            false;
    }
    if (!released) return false;
    device->device_slot = 0;
    device->device_port = 0;
    device->device_speed = 0;
    device->root_port = 0U;
    device->parent_slot = 0U;
    device->parent_port = 0U;
    device->route_string = 0U;
    device->ep0_enqueue = 0;
    device->configuration_value = 0;
    device->hid_interface = 0U;
    device->hid_protocol = 0U;
    device->hid_endpoint = 0;
    device->hid_max_packet = 0;
    device->hid_interval = 0;
    device->hid_enqueue = 0;
    device->hid_cycle = 1U;
    device->hid_transfer_pending = false;
    device->hid_previous_modifier = 0U;
    for (uint32_t i = 0; i < 6U; ++i) device->hid_previous_keys[i] = 0U;
    device->hid_previous_buttons = 0U;
    device->hid_secondary.interface_number = 0U;
    device->hid_secondary.protocol = 0U;
    device->hid_secondary.endpoint = 0U;
    device->hid_secondary.max_packet = 0U;
    device->hid_secondary.interval = 0U;
    device->hid_secondary.enqueue = 0U;
    device->hid_secondary.cycle = 1U;
    device->hid_secondary.transfer_pending = false;
    device->hid_secondary.previous_modifier = 0U;
    for (uint32_t i = 0; i < 6U; ++i) {
        device->hid_secondary.previous_keys[i] = 0U;
    }
    device->hid_secondary.previous_buttons = 0U;
    device->audio_interface = 0U;
    device->audio_alt_setting = 0U;
    device->audio_endpoint = 0U;
    device->audio_interval = 0U;
    device->audio_max_packet = 0U;
    device->audio_channels = 0U;
    device->audio_bit_resolution = 0U;
    device->audio_sample_rate = 0U;
    device->audio_endpoint_in = false;
    device->audio_enqueue = 0U;
    device->audio_cycle = 1U;
    device->audio_transfer_pending = false;
    device->audio_completed = 0U;
    device->audio_stream_generation = 0U;
    device->audio_transfer_generation = 0U;
    device->audio_transfer_first = 0U;
    device->audio_transfer_packets = 0U;
    device->audio_transfer_failed = false;
    device->audio_stream = 0;
    device->audio_period = 0U;
    device->audio_frames = 0U;
    device->audio_stream_running = false;
    device->audio_stream_queued = false;
    device->hub_interface = 0U;
    device->hub_interface_present = false;
    device->hub_port_count = 0U;
    device->hub_protocol = 0U;
    device->hub_tt_think_time = 0U;
    device->hub_configured = false;
    device->hub_endpoint = 0U;
    device->hub_max_packet = 0U;
    device->hub_interval = 0U;
    device->hub_enqueue = 0U;
    device->hub_cycle = 1U;
    device->hub_transfer_pending = false;
    device->hub_change_bitmap = 0U;
    device->bt_event_endpoint = 0U;
    device->bt_acl_in_endpoint = 0U;
    device->bt_acl_out_endpoint = 0U;
    device->bt_event_interval = 0U;
    device->bt_event_max_packet = 0U;
    device->bt_acl_in_max_packet = 0U;
    device->bt_acl_out_max_packet = 0U;
    device->bt_event_enqueue = 0U;
    device->bt_event_cycle = 1U;
    device->bt_event_transfer_pending = false;
    device->bt_acl_in_enqueue = 0U;
    device->bt_acl_in_cycle = 1U;
    device->bt_acl_in_transfer_pending = false;
    device->bt_acl_out_enqueue = 0U;
    device->bt_acl_out_cycle = 1U;
    device->bt_acl_out_transfer_pending = false;
    return true;
}

/*
 * 供尚未发布/新设备枚举路径使用的兼容包装函数。
 */
bool xhci_free_device_resources(
    xhci_state_t *state)
{
    if(state == 0)
    {
        return false;
    }

    return
        xhci_free_device_resources_device(
            &state->device);
}

/* 辅助 HID 控制器有自己的私有工作上下文，但不在主控制器的 Slot Table 或 USB Core 中。
 * 拆除它时不得触碰主控制器已发布的设备。 */
bool xhci_free_hid_device_resources(xhci_state_t *state) {
    xhci_device_context_t *device;
    bool released = true;

    if (state == 0) return false;
    device = &state->device;
    if (device->device_slot == 0U) return true;

    if (!xhci_stop_pending_endpoints_device(state, device)) {
        released = false;
    }
    if (!xhci_submit_command(state, XHCI_DISABLE_SLOT_TYPE,
                             device->device_slot, 0U, 0)) {
        released = false;
    }
    if (!xhci_free_device_resources_device(device)) {
        released = false;
    }
    return released;
}


/*
 * 释放端点环之前，先停止仍有 TRB 在途的端点。Disable Slot 会使设备失效，但显式 Stop Endpoint
 * 可将“控制器仍可能消费 DMA 环”的生命周期边界提前。即使设备突然拔出，停止命令失败也只影响诊断，
 * 后续仍会执行尽可能安全的资源回收。
 *
 * V3.10.6B6C 显式端点停止
 */
static bool xhci_stop_pending_endpoints_device(
    xhci_state_t *state,
    xhci_device_context_t *device) {
    uint8_t endpoints[8];
    uint32_t count = 0U;
    bool stopped = true;
    if (state == 0 ||
        device == 0 ||
        device->device_slot == 0U) return true;
    if (device->hid_transfer_pending && device->hid_endpoint != 0U) {
        endpoints[count++] = (uint8_t)(device->hid_endpoint * 2U + 1U);
    }
    if (device->hid_secondary.transfer_pending &&
        device->hid_secondary.endpoint != 0U) {
        endpoints[count++] = (uint8_t)(device->hid_secondary.endpoint * 2U + 1U);
    }
    if (device->audio_transfer_pending && device->audio_endpoint != 0U) {
        endpoints[count++] = (uint8_t)(device->audio_endpoint * 2U +
                                       (device->audio_endpoint_in ? 1U : 0U));
    }
    if (device->hub_transfer_pending && device->hub_endpoint != 0U) {
        endpoints[count++] = (uint8_t)(device->hub_endpoint * 2U + 1U);
    }
    if (device->bt_event_transfer_pending && device->bt_event_endpoint != 0U) {
        endpoints[count++] = (uint8_t)(device->bt_event_endpoint * 2U + 1U);
    }
    if (device->bt_acl_in_transfer_pending && device->bt_acl_in_endpoint != 0U) {
        endpoints[count++] = (uint8_t)(device->bt_acl_in_endpoint * 2U + 1U);
    }
    if (device->bt_acl_out_transfer_pending && device->bt_acl_out_endpoint != 0U) {
        endpoints[count++] = (uint8_t)(device->bt_acl_out_endpoint * 2U);
    }
    for (uint32_t i = 0U; i < count; ++i) {
        bool duplicate = false;
        for (uint32_t previous = 0U; previous < i; ++previous) {
            if (endpoints[previous] == endpoints[i]) duplicate = true;
        }
        if (duplicate || endpoints[i] > 31U) continue;
        if (!xhci_submit_command_ex(state, XHCI_STOP_ENDPOINT_TYPE,
                                    device->device_slot, endpoints[i], 0U, 0)) {
            stopped = false;
        }
    }
    return stopped;
}

/*
 * 未发布工作上下文的兼容包装函数。
 */
/*
 * V3.10.6B9E-FIX1 已移除 STOP ENDPOINT 包装函数
 *
 * 端点拆除始终接收显式的 xhci_device_context_t *。
 */



/* 设备可能在 active context 和 inventory 之间迁移，因此必须同步重新计算拓扑标记。 */
/* V3.10.6B7D-FIX1 已移除 xhci_take_active_device()：运行时设备不会激活 state->device。 */



/* V3.10.6B7D-FIX1 已移除 xhci_restore_active_device()：运行时设备不会激活 state->device。 */



/*
 * V3.6.6 取消发布 Slot
 *
 * 硬件拆除仍由 xHCI 负责。
 *
 * 发出 Disable Slot 后，从以下位置移除同一设备：
 *
 *   xHCI Slot 表
 *   父 Hub 子设备映射
 *   USB Hub 核心
 *   USB 设备核心
 */
void xhci_unpublish_slot_device(
    uint8_t slot,
    uint8_t parent_slot,
    uint8_t parent_port)
{
    if(slot == 0U)
    {
        return;
    }

    /*
    * 先从父设备分离。
     */
    if(parent_slot != 0U &&
       parent_port != 0U)
    {
        usb_hub_port_disconnect(
            parent_slot,
            parent_port,
            slot);

        xhci_topology_detach_slot(
            slot,
            parent_slot,
            parent_port);
    }

    /*
    * 对非 Hub 设备也安全：
    * usb_hub_remove() 只会清除对应的类槽位。
     */
    usb_hub_remove(
        slot);

    usb_release_device(
        slot);

        /*
    * bt_controller_disconnect/destroy 会在取消发布前执行。
    * 此时可以安全地使回调绑定失效。
     */
    xhci_bt_transport_release(slot);


    xhci_topology_clear_slot(slot);
}


static bool xhci_release_device_context(
    xhci_state_t *state,
    xhci_device_context_t *device);

/*
 * V3.10.6B9D 统一显式拆除
 *
 * state->device 是尚未发布的枚举工作存储。
 *
 * 它的资源生命周期与已发布的 Slot.context 共用显式设备上下文拆除核心。
 */
bool xhci_release_working_device(
    xhci_state_t *state)
{
    if(state == 0 ||
       state->device.device_slot == 0U)
    {
        return true;
    }


    return
        xhci_release_device_context(
            state,
            &state->device);
}

/*
 * V3.10.6B6C 显式设备拆除
 *
 * 销毁一个设备上下文，但不将其安装到 state->device。
 *
 * 当前设备类型/标志记账由调用方负责。
 */
static bool xhci_release_device_context(
    xhci_state_t *state,
    xhci_device_context_t *device)
{
    if(state == 0 ||
       device == 0 ||
       device->device_slot == 0U)
    {
        return true;
    }

    /*
    * 资源拆除会清除身份字段，因此先保存软件拓扑身份信息。
     */
    uint8_t released_slot =
        device->device_slot;

    uint8_t released_parent_slot =
        device->parent_slot;

    uint8_t released_parent_port =
        device->parent_port;

    if(!xhci_stop_pending_endpoints_device(
           state,
           device))
    {
        xhci_set_error(60U);
    }

    if(!xhci_submit_command(
           state,
           XHCI_DISABLE_SLOT_TYPE,
           device->device_slot,
           0U,
           0))
    {
        /*
         * 设备断开后可能不再返回 Disable Slot 完成事件，但仍必须释放软件/DMA 所有权。
         */
        xhci_set_error(61U);
    }

    bool released =
        xhci_free_device_resources_device(
            device);

    /*
     * 硬件 Slot 生命周期已结束。
     */
    xhci_unpublish_slot_device(
        released_slot,
        released_parent_slot,
        released_parent_port);

    xhci_recompute_topology(
        state);

    return released;
}


/*
 * V3.9.2 直接释放 Slot
 *
 * 现在可直接根据 Slot ID 终止硬件/设备生命周期。
 *
 * 不再需要：
 *
 *     扫描 inventory
 *     查找 inventory 索引
 *     inventory 上下文
 *
 * 即可停止端点、Disable Slot、释放 DMA 或取消发布拓扑。
 */
/*
 * V3.10.6B6C 规范 Slot 拆除
 *
 * 已发布的 Slot.context 会原位销毁。
 * 除非 state->device 本身就是目标，否则不会触及它。
 */
/*
 * V3.10.6B9C 仅释放已发布的 Slot
 *
 * 此函数仅负责一个已发布的硬件 Slot：
 *
 *     Slot ID
 *        |
 *        v
 *   Slot.context
 *        |
 *        v
 * xhci_release_device_context()
 *
 * state->device 属于独立的未发布枚举生命周期，此处有意不处理。
 */
bool xhci_release_slot_device(
    xhci_state_t *state,
    uint8_t slot)
{
    if(state == 0 ||
       slot == 0U)
    {
        return false;
    }


    xhci_slot_device_t *slot_dev =
        xhci_topology_slot(slot);


    if(!slot_dev->used ||
       slot_dev->context.device_slot !=
           slot)
    {
        return false;
    }


    return
        xhci_release_device_context(
            state,
            &slot_dev->context);
}


/*
 * inventory[] 兼容封装。
 *
 * 初始枚举仍使用 inventory 元数据，但硬件拆除本身现在完全基于 Slot。
 */

/*
 * V3.10.6B8C
 *
 * 已移除 xhci_release_inventory_entry()。
 * 拆除现在基于 Slot ID。
 */




/*
 * V3.9.6 仅保存 Slot
 *
 * Slot 表是永久设备存储。
 *

*/

/*
 * V3.10.6B8C
 *
 * 已移除 xhci_save_active_device()。
 * 发布仅使用 Slot.context。
 */




/*
 * V3.10.6B2 规范自测上下文
 *
 * 已移除 xhci_load_inventory_device()。
 *
 * 硬件自测会将初始化 inventory 直接解析为规范 Slot.context，绝不将已发布设备状态安装到 state->device。
 */



/*
 * V3.9.5 Slot 表控制器拆除
 *
 * 控制器销毁完全由硬件 Slot 标识驱动。
 *
 * 不检查 inventory[]。
 */
bool xhci_free_slot_resources(
    xhci_state_t *state)
{
    bool released =
        true;


    if(state == 0)
    {
        return false;
    }


    /*
    * 首先处理当前工作上下文。
     *
    * 完成枚举的活动设备也应存在于 Slot 表中。但部分构建的枚举上下文可能尚未入表，因此要为这种情况
    * 保留原始资源回退路径。
     */
    if(state->device.device_slot != 0U)
    {
        /*
         * V3.10.6B9B 工作上下文拆除
         *
         * 枚举期间 Slot.context 尚未发布时，Slot.used 可能已经为 true。
         *
         * 因此 state->device 始终负责自己的拆除路径。
         */
        if(!xhci_release_working_device(
               state))
        {
            released =
                false;
        }
    }


    uint32_t limit =
        state->max_slots;


    if(limit >=
       XHCI_MAX_SLOT_TABLE)
    {
        limit =
            XHCI_MAX_SLOT_TABLE -
            1U;
    }


    /*
    * 其余已发布设备均保存在 Slot.context 中。
     *
    * xhci_release_slot_device() 会执行：
     *
    *     停止端点
     *        鈫?     *     Disable Slot
     *        鈫?     *     release DMA
     *        鈫?     *     unlink parent
     *        鈫?     *     remove USB Core / Hub Core object
     *        鈫?     *     clear Slot Table entry
     */
    for(uint32_t slot = 1U;
        slot <= limit;
        ++slot)
    {
        xhci_slot_device_t *slot_dev =
            xhci_topology_slot(slot);


        if(!slot_dev->used)
        {
            continue;
        }


        /*
         * 忽略尚未提交或已过期的暂定节点。
         *
         * 如果它仍拥有活动工作上下文，上方已处理。已发布且停放的 Slot 必须有匹配的规范上下文。
         */
        if(slot_dev->context.device_slot !=
           (uint8_t)slot)
        {
            xhci_topology_clear_slot((uint8_t)slot);

            released =
                false;

            continue;
        }


        if(!xhci_release_slot_device(
                state,
                (uint8_t)slot))
        {
            released =
                false;
        }
    }


    /*
    * 控制器拆除后不得残留任何已安装状态。
     */
    xhci_zero_device_context(
        &state->device);

    xhci_clear_device_flags();


    return released;
}


    /* runtime 鍖哄墠 0x20 瀛楄妭鏄?MFINDEX锛岀 0 涓?interrupter 浠?0x20 寮€濮嬨€?*/
        /* 鍒濆浜嬩欢闃熷垪灏氭湭琚帶鍒跺櫒澶勭悊锛孍RDP 涓嶅簲鎼哄甫 EHB銆?*/
