#pragma once
#pragma once
#include "base.h"
#include "list.h"
#include "spinlock.h"

struct thread;
struct wait_queue;
struct waiter;

/* 限制常见小型等待集合使用的临时栈空间。较大的集合仍通过检查后的动态分配处理；
 * 订阅生命周期保持不变。 */
#define WAIT_INLINE_QUEUE_COUNT 4U

/* 栈等待者发布期间，取消策略不可变。 */
#define WAIT_FLAG_UNINTERRUPTIBLE 1U

typedef struct wait_subscription {
    list_head_t queue_node;
    struct waiter *waiter;
    struct wait_queue *queue;
} wait_subscription_t;

enum waiter_state {
    WAITER_WAITING = 0,
    WAITER_WOKEN,
    WAITER_TIMED_OUT,
    WAITER_CANCELLED,
};

typedef struct waiter {
    list_head_t timeout_node;
    struct thread *thread;
    struct wait_queue *queue;
    wait_subscription_t *subscriptions;
    size_t subscription_count;
    atomic_uint state;
    uint32_t flags;
    uint64_t deadline_tsc;
    uint64_t diagnostic_started_tsc;
    uint64_t diagnostic_claimed_tsc;
} waiter_t;

typedef struct wait_queue {
    spinlock_t lock;
    list_head_t waiters;
    atomic_uint sequence;
} wait_queue_t;

typedef struct completion {
    atomic_uint count;
    wait_queue_t waitq;
} completion_t;

void wait_queue_init(wait_queue_t *q);
kstatus_t wait_on_queue(wait_queue_t *q, bool (*predicate)(void *), void *ctx, uint64_t timeout_ns);
/* 仅供内部关键操作使用：延迟取消并保留待处理终止标记。就绪或超时仍会正常结束等待。 */
kstatus_t wait_on_queue_uninterruptible(wait_queue_t *q,
                                       bool (*predicate)(void *), void *ctx,
                                       uint64_t timeout_ns);
kstatus_t wait_on_queues(wait_queue_t *const *queues, size_t count,
                         bool (*predicate)(void *), void *ctx,
                         uint64_t timeout_ns);
/* 临时数组可能被排序/压缩；整个过程中均由调用方管理其存储。 */
kstatus_t wait_on_queues_in_place(wait_queue_t **queues, size_t count,
                                 bool (*predicate)(void *), void *ctx,
                                 uint64_t timeout_ns);
uint32_t wake_one(wait_queue_t *q);
uint32_t wake_all(wait_queue_t *q);
/* 调用方持有线程引用；栈等待者在等待锁保护下读取。 */
bool wait_cancel_thread(struct thread *thread);
void wait_poll_timeouts(uint64_t now_tsc);

void completion_init(completion_t *completion, uint32_t initial_count);
void complete(completion_t *completion);
kstatus_t completion_wait(completion_t *completion, uint64_t timeout_ns);
