#include "internal.h"

/* REFACTOR_P7A_INPUT_MOTION_OWNER：批量指针转换和刷新策略。 */

/* 指针坐标属于输入状态，而不是窗口或合成器状态。将坐标修改集中在此所有者中，使普通移动和拖动批处理
 * 共用同一个发布边界。 */
void window_input_set_pointer_locked(uint32_t x, uint32_t y) {
    g_window_server.pointer_x = x;
    g_window_server.pointer_y = y;
}

/*
 * 将累积的相对轴数据重新送入常规窗口路由路径。使用 int64_t 累加可避免一批原始事件在拆分为一个或多个
 * ABI 大小的 int32_t 事件之前溢出。
 */
void window_route_motion_axis_locked(
    const window_motion_batch_t *batch,
    uint32_t code,
    int64_t delta) {
    input_event_t event = {0};

    if (batch == 0 || delta == 0) return;

    event.timestamp = batch->timestamp;
    event.device_id = batch->device_id;
    event.type = INPUT_EVENT_RELATIVE;
    event.flags = batch->flags;
    event.code = code;

    while (delta != 0) {
        int64_t chunk;

        if (delta > 2147483647LL) {
            chunk = 2147483647LL;
        } else if (delta < -2147483648LL) {
            chunk = -2147483648LL;
        } else {
            chunk = delta;
        }

        event.value = (int32_t)chunk;
        route_input_locked(&event);
        delta -= chunk;
    }
}

void window_flush_motion_batch_locked(window_motion_batch_t *batch) {
    if (batch == 0 || !batch->active) return;

    /* 装饰区移动由 Ring0 处理，因此可以安全地将 X/Y 合并为一次几何更新。 */
    if (window_route_drag_motion_batch_locked(batch)) {
        batch->active = false;
        batch->delta_x = 0;
        batch->delta_y = 0;
        return;
    }

    /* 普通的、面向客户端的相对输入保持原有行为不变。 */
    window_route_motion_axis_locked(batch, INPUT_REL_X, batch->delta_x);
    window_route_motion_axis_locked(batch, INPUT_REL_Y, batch->delta_y);
    batch->active = false;
    batch->delta_x = 0;
    batch->delta_y = 0;
}

/*
 * 消费一个内核私有的 HID 指针事务。Ring3 仍接收不变的按钮/相对事件 ABI；合成器则在一次
 * 几何状态转换中应用报告中的 X/Y 坐标对。
 */
void window_route_pointer_transaction_locked(const input_event_t *event) {
    static const uint32_t button_codes[] = {
        INPUT_BUTTON_LEFT,
        INPUT_BUTTON_RIGHT,
        INPUT_BUTTON_MIDDLE,
    };
    window_motion_batch_t motion = {0};

    if (event == 0 || event->type != INPUT_EVENT_POINTER) return;

    for (uint32_t bit = 0U; bit < 3U; ++bit) {
        uint16_t mask = (uint16_t)(1U << bit);
        if ((event->code & mask) == 0U) continue;
        input_event_t button = {
            .timestamp = event->timestamp,
            .device_id = event->device_id,
            .type = INPUT_EVENT_BUTTON,
            .flags = event->flags,
            .code = button_codes[bit],
            .value = (event->value & mask) != 0U ?
                     INPUT_VALUE_PRESS : INPUT_VALUE_RELEASE,
        };
        route_input_locked(&button);
    }

    if (event->value2 != 0 || event->value3 != 0) {
        motion.active = true;
        motion.device_id = event->device_id;
        motion.flags = event->flags;
        motion.timestamp = event->timestamp;
        motion.delta_x = event->value2;
        motion.delta_y = event->value3;
        if (!window_route_drag_motion_batch_locked(&motion)) {
            window_route_motion_axis_locked(&motion, INPUT_REL_X,
                                            motion.delta_x);
            window_route_motion_axis_locked(&motion, INPUT_REL_Y,
                                            motion.delta_y);
        }
    }

    if (event->value4 != 0) {
        input_event_t wheel = {
            .timestamp = event->timestamp,
            .device_id = event->device_id,
            .type = INPUT_EVENT_RELATIVE,
            .flags = event->flags,
            .code = INPUT_REL_WHEEL,
            .value = event->value4,
        };
        route_input_locked(&wheel);
    }
    window_coalesce_damage_locked();
}
