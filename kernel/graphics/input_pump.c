#include "internal.h"
#include <kernel/power.h>

/* REFACTOR_P7K_INPUT_PUMP_OWNER：输入消费和帧泵策略。 */

static inline bool window_input_router_is_relative_motion(
    const input_event_t *event) {
    return event != 0 &&
           event->type == INPUT_EVENT_RELATIVE &&
           (event->code == INPUT_REL_X ||
            event->code == INPUT_REL_Y);
}

/* 验证实际输入泵使用的事件分类，不向窗口服务器注入合成状态。 */
bool window_input_router_self_test(void) {
    input_event_t relative_x = {0};
    input_event_t relative_y = {0};
    input_event_t key = {0};

    relative_x.type = INPUT_EVENT_RELATIVE;
    relative_x.code = INPUT_REL_X;
    relative_y.type = INPUT_EVENT_RELATIVE;
    relative_y.code = INPUT_REL_Y;
    key.type = INPUT_EVENT_KEY;

    return window_input_router_is_relative_motion(&relative_x) &&
           window_input_router_is_relative_motion(&relative_y) &&
           !window_input_router_is_relative_motion(&key) &&
           WINDOW_COMPOSITOR_DRAG_INPUT_EVENTS >= WINDOW_EVENT_CAPACITY &&
           desktop_chrome_launch_guard_self_test();
}

void window_server_pump_input_mode(bool compose_now) {
    input_event_t event;
    input_event_t events[16];
    size_t batch_count = 0U;
    size_t batch_index = 0U;
    window_motion_batch_t motion = {0};
    bool wake = false;
    uint32_t consumed = 0U;
    uint32_t input_budget = WINDOW_EVENT_CAPACITY;

    if (!window_server_kernel_ready()) return;

    /* Ring0 拖动会在单个本地批次中处理更多纯移动事件。 */
    window_lock();
    if (g_window_server.dragging_identifier != 0U) {
        input_budget = WINDOW_COMPOSITOR_DRAG_INPUT_EVENTS;
    }
    window_unlock();

    /* 合并相对 X/Y 和纯指针移动报告。按钮、滚轮、按键或绝对事件仍作为顺序屏障。 */
    while (consumed < input_budget) {
        if (batch_index == batch_count) {
            size_t capacity = input_budget - consumed;
            if (capacity > sizeof(events) / sizeof(events[0]))
                capacity = sizeof(events) / sizeof(events[0]);
            batch_count = input_core_pop_batch(events, capacity);
            batch_index = 0U;
            if (batch_count == 0U) break;
        }
        event = events[batch_index++];
        bool is_motion =
            window_input_router_is_relative_motion(&event);

        ++consumed;
        wake = true;

        if (event.type == INPUT_EVENT_POINTER) {
            bool pure_pointer_motion =
                event.code == 0U &&
                event.value4 == 0;

            if (pure_pointer_motion) {
                if (event.value2 == 0 &&
                    event.value3 == 0) {
                    continue;
                }

                if (motion.active &&
                    (motion.device_id != event.device_id ||
                     motion.flags != event.flags ||
                     (motion.delta_x < 0 && event.value2 > 0) ||
                     (motion.delta_x > 0 && event.value2 < 0) ||
                     (motion.delta_y < 0 && event.value3 > 0) ||
                     (motion.delta_y > 0 && event.value3 < 0))) {

                    window_lock();
                    window_flush_motion_batch_locked(&motion);
                    window_unlock();
                }

                if (!motion.active) {
                    motion.active = true;
                    motion.device_id = event.device_id;
                    motion.flags = event.flags;
                    motion.timestamp = event.timestamp;
                    motion.delta_x = 0;
                    motion.delta_y = 0;
                }

                motion.timestamp = event.timestamp;
                motion.delta_x += event.value2;
                motion.delta_y += event.value3;
                continue;
            }

            window_lock();
            window_flush_motion_batch_locked(&motion);
            window_route_pointer_transaction_locked(&event);

            if (g_window_server.dragging_identifier != 0U) {
                input_budget = WINDOW_COMPOSITOR_DRAG_INPUT_EVENTS;
            }

            window_unlock();
            continue;
        }

        if (is_motion) {
            /* 到达屏幕边缘后的反向移动不能抵消越界前的位移。 */
            int64_t previous = event.code == INPUT_REL_X ? motion.delta_x : motion.delta_y;
            if (motion.active &&
                (motion.device_id != event.device_id ||
                 motion.flags != event.flags ||
                 (previous < 0 && event.value > 0) ||
                 (previous > 0 && event.value < 0))) {

                window_lock();
                window_flush_motion_batch_locked(&motion);
                window_unlock();
            }

            if (!motion.active) {
                motion.active = true;
                motion.device_id = event.device_id;
                motion.flags = event.flags;
                motion.timestamp = event.timestamp;
                motion.delta_x = 0;
                motion.delta_y = 0;
            }

            motion.timestamp = event.timestamp;

            if (event.code == INPUT_REL_X) {
                motion.delta_x += event.value;
            } else {
                motion.delta_y += event.value;
            }

            continue;
        }

        window_lock();
        window_flush_motion_batch_locked(&motion);
        route_input_locked(&event);
        const os_input_event_t copy_event = {
            .type = event.type, .code = event.code, .value = event.value
        };
        window_control_copy_request_t copy =
            window_control_copy_selection_locked(&copy_event);

        if (g_window_server.dragging_identifier != 0U) {
            input_budget = WINDOW_COMPOSITOR_DRAG_INPUT_EVENTS;
        }

        window_unlock();
        window_control_copy_complete(&copy);
    }

    if (motion.active) {
        window_lock();
        window_flush_motion_batch_locked(&motion);
        window_unlock();
    }

    if (wake &&
        window_flush_event_wakes()) {
        (void)wake_all(&g_window_server.event_waitq);
    }

    for (;;) {
        uint32_t app;
        uint32_t power_request;
        bool cycle_focus;

        window_lock();

        app = window_input_take_desktop_launch_locked();
        cycle_focus = window_input_take_focus_cycle_request_locked();
        power_request = g_window_server.desktop.power_request;
        g_window_server.desktop.power_request = 0U;

        window_unlock();

        if (app == DESKTOP_APP_NONE && !cycle_focus && power_request == 0U) {
            break;
        }

        /* 菜单只记录请求；关机写回和设备停止不得持有窗口锁。 */
        if (power_request != 0U) (void)power_system_request(power_request - 1U);

        if (cycle_focus) {
            desktop_cycle_window_focus();
        }

        if (app != DESKTOP_APP_NONE) {
            if (!desktop_launch_enqueue(app) &&
                !desktop_restore_minimized_app(app)) {
                /* 忙碌/不可用时，不得让输入所有者执行磁盘 I/O 和 ELF 加载。队列清空后用户可以
                 * 重试。 */
                continue;
            }
        }
    }

    if (!compose_now) return;

    compositor_frame_run();
}

/* 单一合成器；GOP 扫描线边界可抢占；场景已完整保留。不得消费点击、修改拖动几何信息、启动
 * 程序或递归执行合成。 */
void window_server_pump_publication_motion(void) {
    input_event_t events[16];
    size_t count;
    if (input_core_pending() == 0U) return;
    window_lock();
    if (g_window_server.dragging_identifier != 0U ||
        g_compositor_snapshot.dragging_identifier != 0U) {
        window_unlock();
        return;
    }
    window_unlock();
    count = input_core_pop_cursor_motion(events,
        sizeof(events) / sizeof(events[0]));
    if (count == 0U) return;
    window_lock();
    for (size_t i = 0U; i < count; ++i) {
        if (events[i].type == INPUT_EVENT_POINTER)
            window_route_pointer_transaction_locked(&events[i]);
        else
            route_input_locked(&events[i]);
    }
    window_unlock();
    if (window_flush_event_wakes())
        (void)wake_all(&g_window_server.event_waitq);
    compositor_present_cursor_direct(false);
}

void window_server_pump_input(void) {
    window_server_pump_input_mode(true);
}
