#pragma once

#include <kernel/base.h>
#include <kernel/spinlock.h>
#include <kernel/wait.h>

/* 统一输入事件：USB HID、PS/2 和未来的蓝牙输入都只向这里提交事件。 */
enum input_event_type {
    INPUT_EVENT_KEY = 1,
    INPUT_EVENT_BUTTON = 2,
    INPUT_EVENT_RELATIVE = 3,
    INPUT_EVENT_ABSOLUTE = 4,
    /* 内核内部 HID 报告事务；不会暴露为 Ring3 ABI。 */
    INPUT_EVENT_POINTER = 5,
};

enum input_event_value {
    INPUT_VALUE_RELEASE = 0,
    INPUT_VALUE_PRESS = 1,
    INPUT_VALUE_REPEAT = 2,
};

/*
 * 由 HID 后端和用户态客户端共用的稳定指针代码。
 * 取值遵循常见 evdev 相对轴和按键范围，使新增输入后端无需仅为报告基本鼠标事件
 * 再增加转换层。
 */
enum input_relative_code {
    INPUT_REL_X = 0,
    INPUT_REL_Y = 1,
    INPUT_REL_WHEEL = 8,
};

enum input_button_code {
    INPUT_BUTTON_LEFT = 0x110,
    INPUT_BUTTON_RIGHT = 0x111,
    INPUT_BUTTON_MIDDLE = 0x112,
};

typedef struct input_event {
    uint64_t timestamp;
    uint32_t device_id;
    uint16_t type;
    uint16_t flags;
    uint32_t code;
    int32_t value;
    int32_t value2;
    int32_t value3;
    int32_t value4;
} input_event_t;

typedef struct input_pointer_motion {
    uint64_t timestamp;
    uint32_t device_id;
    uint16_t flags;
    uint16_t buttons_changed;
    uint8_t buttons;
    uint8_t reserved[3];
    int32_t dx;
    int32_t dy;
    int32_t wheel;
} input_pointer_motion_t;

#define INPUT_CORE_CAPACITY 256U

bool input_core_init(void);
/* 绑定通用消费者等待队列；输入所有者不会依赖 Graphics 等客户端子系统。 */
void input_core_bind_wakeup(wait_queue_t *wait_queue);
kstatus_t input_core_push(const input_event_t *event);
kstatus_t input_core_push_pointer(const input_pointer_motion_t *motion);
kstatus_t input_core_pop(input_event_t *event);
/* 非阻塞地从 FIFO 中提取事件；请求无效或队列为空时返回零。 */
size_t input_core_pop_batch(input_event_t *events, size_t capacity);
/* 仅提取开头连续的 X/Y 或无滚轮指针事件；绝不越过输入屏障。 */
size_t input_core_pop_cursor_motion(input_event_t *events, size_t capacity);
kstatus_t input_core_wait(uint64_t timeout_ns);
kstatus_t input_core_read(input_event_t *event, uint64_t timeout_ns);
uint32_t input_core_pending(void);
uint64_t input_core_dropped(void);
bool input_core_self_test(void);
