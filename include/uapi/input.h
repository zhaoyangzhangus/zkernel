#pragma once
#pragma once

#include "abi.h"

/* 用户态可见的输入事件，布局与内核输入队列保持一致。 */
enum os_input_event_type {
    OS_INPUT_EVENT_KEY = 1u,
    OS_INPUT_EVENT_BUTTON = 2u,
    OS_INPUT_EVENT_RELATIVE = 3u,
    OS_INPUT_EVENT_ABSOLUTE = 4u,
};

enum os_input_event_value {
    OS_INPUT_VALUE_RELEASE = 0u,
    OS_INPUT_VALUE_PRESS = 1u,
    OS_INPUT_VALUE_REPEAT = 2u,
};

/* 指针代码取值属于输入 ABI。 */
enum os_input_relative_code {
    OS_INPUT_REL_X = 0u,
    OS_INPUT_REL_Y = 1u,
    OS_INPUT_REL_WHEEL = 8u,
};

enum os_input_button_code {
    OS_INPUT_BUTTON_LEFT = 0x110u,
    OS_INPUT_BUTTON_RIGHT = 0x111u,
    OS_INPUT_BUTTON_MIDDLE = 0x112u,
};

typedef struct os_input_event {
    uint64_t timestamp;
    uint32_t device_id;
    uint16_t type;
    uint16_t flags;
    uint32_t code;
    int32_t value;
} os_input_event_t;
