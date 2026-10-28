#pragma once

#include <kernel/base.h>
#include <kernel/object.h>
#include <kernel/spinlock.h>

#define KOBJECT_TYPE_TIMER 0x010AU

#define TIMER_RIGHT_WAIT (1U << 31)
#define TIMER_RIGHT_ALL  TIMER_RIGHT_WAIT
#define TIMER_OBJECT_LIMIT 128U

typedef struct timer_object {
    object_header_t object;
    atomic_bool canceled;
    atomic_bool fired;
    uint32_t registry_slot; /* 在定时器注册表锁保护下分配和回收 */
    /* 定时器注册表锁保护截止时间更新和对象生命周期；注册后 period_tsc 不可变。 */
    uint64_t deadline_tsc;
    uint64_t period_tsc;
} timer_object_t;

kstatus_t timer_create(uint64_t delay_ns, uint64_t period_ns,
                       timer_object_t **out);
kstatus_t timer_cancel(timer_object_t *timer);
void timer_poll(uint64_t now_tsc);
bool timer_self_test(void);
