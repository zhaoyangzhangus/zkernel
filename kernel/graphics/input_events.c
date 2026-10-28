#include "internal.h"

/* REFACTOR_P7A_WINDOW_EVENT_OWNER：队列存储、事件转换和解锁后的唤醒发布。 */

static int32_t window_pointer_clamp_i32(
    int64_t value) {

    if (value < -2147483648LL) {
        return (-2147483647 - 1);
    }

    if (value > 2147483647LL) {
        return 2147483647;
    }

    return (int32_t)value;
}


/*
 * 将全局 Ring0 指针位置转换为应用客户端 surface 使用的坐标系。
 *
 * 标题栏和外边框不属于 Ring3 坐标范围。
 */
static void window_event_set_pointer_locked(
    const window_server_window_t *window,
    os_window_event_t *event) {

    int64_t local_x;
    int64_t local_y;

    if (window == 0 ||
        event == 0) {
        return;
    }

    local_x =
        (int64_t)g_window_server.pointer_x -
        (int64_t)window->x -
        (int64_t)window_client_offset_x(window->flags);

    local_y =
        (int64_t)g_window_server.pointer_y -
        (int64_t)window->y -
        (int64_t)window_client_offset_y(window->flags);

    event->pointer_x =
        window_pointer_clamp_i32(
            local_x);

    event->pointer_y =
        window_pointer_clamp_i32(
            local_y);
}

void window_event_reset_ready_locked(void) {
    for (uint32_t index = 0U;
         index < WINDOW_SERVER_MAX_WINDOWS;
         ++index) {
        g_window_server.event_ready_windows[index] = 0;
    }

    g_window_server.event_ready_count = 0U;
    g_window_server.event_ready_overflow = false;
}


/*
 * 记录一个窗口，以便解锁后唤醒等待者。
 *
 * 常规路径为 O(1)。临时对象引用可在并发关闭/进程销毁期间保持内嵌等待队列存活。
 */
void window_event_schedule_wake_locked(
    window_server_window_t *window) {

    if (window == 0 ||
        window->event_wake_pending) {
        return;
    }

    if (g_window_server.event_ready_count >=
        WINDOW_SERVER_MAX_WINDOWS) {

        g_window_server.event_ready_overflow =
            true;

        return;
    }

    object_get(window);

    window->event_wake_pending = true;

    g_window_server.event_ready_windows[
        g_window_server.event_ready_count++] =
        window;
}


/*
 * 在 window_lock 下分离就绪集合，再于锁外执行 wake_one/object_put。
 *
 * 只有极少见的溢出路径会扫描注册表。
 */
static void window_event_publish_wake(window_server_window_t *window) {
    if (window == 0) return;
    (void)wake_one(&window->event_waitq);
    object_notify_signaled(window);
    object_put(window);
}

bool window_flush_event_wakes(void) {
    window_server_window_t *ready[WINDOW_SERVER_MAX_WINDOWS];
    window_server_window_t *overflow_ready[WINDOW_SERVER_MAX_WINDOWS];
    uint32_t overflow_count = 0U;

    window_lock();
    uint32_t ready_count = g_window_server.event_ready_count;
    if (ready_count > WINDOW_SERVER_MAX_WINDOWS)
        ready_count = WINDOW_SERVER_MAX_WINDOWS;
    bool overflow = g_window_server.event_ready_overflow;

    for (uint32_t index = 0U; index < ready_count; ++index) {
        window_server_window_t *window = g_window_server.event_ready_windows[index];
        ready[index] = window;
        g_window_server.event_ready_windows[index] = 0;
        // 常规批次在一轮中分离并清除成员标记。溢出批次则保留标记，直到注册表去重完成。
        if (!overflow && window != 0) window->event_wake_pending = false;
    }
    g_window_server.event_ready_count = 0U;
    g_window_server.event_ready_overflow = false;

    if (overflow) {
        for (uint32_t index = 0U;
             index < g_window_server.scene.count &&
             overflow_count < WINDOW_SERVER_MAX_WINDOWS; ++index) {
            window_server_window_t *window = g_window_server.scene.children[index];
            if (window == 0 || window->event_wake_pending ||
                window->event_count == 0U) continue;
            object_get(window);
            overflow_ready[overflow_count++] = window;
        }
        // 解锁前清除成员标记，使并发入队可在锁外发布期间再次安排唤醒。
        for (uint32_t index = 0U; index < ready_count; ++index) {
            if (ready[index] != 0) ready[index]->event_wake_pending = false;
        }
    }
    window_unlock();

    for (uint32_t index = 0U; index < ready_count; ++index)
        window_event_publish_wake(ready[index]);
    for (uint32_t index = 0U; index < overflow_count; ++index)
        window_event_publish_wake(overflow_ready[index]);
    return ready_count != 0U || overflow_count != 0U;
}


/* 控件 MOVE 只保存位置快照。队列满时先腾出这类槽位，保留按键和点击的顺序。 */
static bool window_event_discard_motion_locked(window_server_window_t *window) {
    for(uint32_t offset=0;offset<window->event_count;++offset) {
        uint32_t slot=(window->event_read+offset)%WINDOW_EVENT_CAPACITY;
        const os_window_event_t *event=window->events+slot;
        if(event->identifier!=window->identifier || event->type!=OS_WINDOW_EVENT_CONTROL ||
            event->control.action!=OS_CONTROL_MOVE)continue;
        for(uint32_t next=offset+1U;next<window->event_count;++next) {
            uint32_t from=(window->event_read+next)%WINDOW_EVENT_CAPACITY;
            window->events[slot]=window->events[from];slot=from;
        }
        window->event_write=(window->event_write+WINDOW_EVENT_CAPACITY-1U)%WINDOW_EVENT_CAPACITY;
        --window->event_count;
        return true;
    }
    return false;
}

/* 所有生产者共用槽位淘汰、清理和负载写入后的发布流程。 */
static os_window_event_t *window_event_reserve_locked(window_server_window_t *window) {
    if (window->event_count >= WINDOW_EVENT_CAPACITY &&
        !window_event_discard_motion_locked(window)) {
        window->event_read = (window->event_read + 1U) % WINDOW_EVENT_CAPACITY;
        --window->event_count;
    }
    os_window_event_t *queued = &window->events[window->event_write];
    *queued = (os_window_event_t){0};
    queued->identifier = window->identifier;
    return queued;
}

static void window_event_commit_locked(window_server_window_t *window) {
    window->event_write = (window->event_write + 1U) % WINDOW_EVENT_CAPACITY;
    ++window->event_count;
    window_event_schedule_wake_locked(window);
}

static void window_enqueue_client_input_mode_locked(window_server_window_t *window,
    const os_input_event_t *event, bool hover_prepared) {
    if (window == 0 || event == 0) return;
    os_window_event_t pointer = {0};
    window_event_set_pointer_locked(window, &pointer);
    if (window_control_route_locked(window, event, pointer.pointer_x, pointer.pointer_y,
            event->type == OS_INPUT_EVENT_KEY ||
                (hover_prepared
                    ? g_window_server.scene.hovered_window_identifier == window->identifier
                    : window_scene_pointer_window_locked() == window), hover_prepared)) return;
    os_window_event_t *queued = window_event_reserve_locked(window);
    queued->type = OS_WINDOW_EVENT_INPUT;
    queued->input = *event;
    window_event_set_pointer_locked(window, queued);
    window_event_commit_locked(window);
}

void window_enqueue_client_input_locked(window_server_window_t *window,
                                       const os_input_event_t *event) {
    window_enqueue_client_input_mode_locked(window, event, false);
}

/* 物理输入已在场景入口完成悬停命中，直接复用结果；定向注入走上方独立入口。 */
void window_enqueue_event_locked(window_server_window_t *window,
                                 const input_event_t *event) {
    if (window == 0 || event == 0) return;
    os_input_event_t input = {.timestamp=event->timestamp, .device_id=event->device_id,
        .type=event->type, .flags=event->flags, .code=event->code, .value=event->value};
    window_enqueue_client_input_mode_locked(window, &input, true);
}

void window_enqueue_control_event_locked(window_server_window_t *window,
    const os_input_event_t *input, uint32_t target, uint16_t action, int32_t x, int32_t y) {
    if (!window || !input) return;
    os_control_event_t control = {.timestamp=input->timestamp, .target=target,
        .code=input->code, .value=input->value, .action=action, .flags=input->flags};
    if (!window_control_event_metadata_locked(window, target, &control)) return;
    /* MOVE 是客户端坐标快照。仅覆盖队尾同目标、同修饰状态的连续移动，绝不跨越点击等屏障。 */
    if (action == OS_CONTROL_MOVE && window->event_count) {
        uint32_t slot=(window->event_write+WINDOW_EVENT_CAPACITY-1U)%WINDOW_EVENT_CAPACITY;
        os_window_event_t *last=window->events+slot;
        if (last->identifier == window->identifier && last->type == OS_WINDOW_EVENT_CONTROL &&
            last->control.action == OS_CONTROL_MOVE && last->control.target == target &&
            last->control.tree_revision == control.tree_revision &&
            last->control.node_index == control.node_index &&
            last->control.user_data == control.user_data &&
            last->control.flags == input->flags) {
            last->control=control;
            last->pointer_x=x;last->pointer_y=y;
            return;
        }
    }
    /* 全是关键事件时丢弃新位置快照，不让慢客户端的鼠标移动挤掉点击或按键。 */
    if(action==OS_CONTROL_MOVE && window->event_count>=WINDOW_EVENT_CAPACITY &&
        !window_event_discard_motion_locked(window))return;
    os_window_event_t *queued = window_event_reserve_locked(window);
    queued->type = OS_WINDOW_EVENT_CONTROL;
    queued->control = control;
    queued->pointer_x = x; queued->pointer_y = y;
    window_event_commit_locked(window);
}

/* 关闭请求采用协作处理，在消费或淘汰前会去重。正常进程关闭策略由 Ring3 管理，而非此队列。 */
void window_enqueue_close_request_locked(window_server_window_t *window) {
    if (window == 0) return;
    for (uint32_t offset = 0U; offset < window->event_count; ++offset) {
        uint32_t slot = (window->event_read + offset) % WINDOW_EVENT_CAPACITY;
        const os_window_event_t *queued = &window->events[slot];
        if (queued->identifier == window->identifier &&
            queued->type == OS_WINDOW_EVENT_CLOSE_REQUEST) return;
    }
    os_window_event_t *queued = window_event_reserve_locked(window);
    queued->type = OS_WINDOW_EVENT_CLOSE_REQUEST;
    window_event_commit_locked(window);
}

void window_enqueue_minimized_event_locked(window_server_window_t *window) {
    if (window == 0) return;
    os_window_event_t *queued = window_event_reserve_locked(window);
    queued->type = OS_WINDOW_EVENT_MINIMIZED;
    queued->minimized.minimized = window->minimized ? 1U : 0U;
    window_event_commit_locked(window);
}

void window_enqueue_resize_event_locked(window_server_window_t *window) {
    if (window == 0) return;
    /* 连续的调整大小事件会合并为最新几何信息，但不会跨越中间的输入、关闭或最小化事件。 */
    if (window->event_count != 0U) {
        uint32_t slot = (window->event_write + WINDOW_EVENT_CAPACITY - 1U) %
                        WINDOW_EVENT_CAPACITY;
        os_window_event_t *queued = &window->events[slot];
        if (queued->identifier == window->identifier &&
            queued->type == OS_WINDOW_EVENT_RESIZE) {
            queued->resize.width = window->width;
            queued->resize.height = window->height;
            queued->resize.buffer_size = window->buffer_size;
            queued->resize.reserved = 0U;
            return;
        }
    }
    os_window_event_t *queued = window_event_reserve_locked(window);
    queued->type = OS_WINDOW_EVENT_RESIZE;
    queued->resize.width = window->width;
    queued->resize.height = window->height;
    queued->resize.buffer_size = window->buffer_size;
    window_event_commit_locked(window);
}

void window_enqueue_moved_event_locked(window_server_window_t *window) {
    if (window == 0) return;
    os_window_event_t *queued = 0;
    /* 绝不跨越输入事件进行合并：客户端坐标属于事件流在该位置对应的几何信息。 */
    if (window->event_count != 0U) {
        uint32_t slot = (window->event_write + WINDOW_EVENT_CAPACITY - 1U) %
                        WINDOW_EVENT_CAPACITY;
        if (window->events[slot].identifier == window->identifier &&
            window->events[slot].type == OS_WINDOW_EVENT_MOVED) {
            queued = &window->events[slot];
        }
    }
    bool append = queued == 0;
    if (append) queued = window_event_reserve_locked(window);
    queued->type = OS_WINDOW_EVENT_MOVED;
    queued->moved.x = window_pointer_clamp_i32(
        (int64_t)window->x + window_client_offset_x(window->flags));
    queued->moved.y = window_pointer_clamp_i32(
        (int64_t)window->y + window_client_offset_y(window->flags));
    queued->moved.reserved = 0U;
    queued->moved.reserved2 = 0U;
    if (append) window_event_commit_locked(window);
}
