#pragma once

#include <kernel/sched.h>

#define SCHED_ENTITY_ENQUEUED             (1U << 0)
#define SCHED_ENTITY_INITIAL_PLACEMENT   (1U << 1)
#define SCHED_ENTITY_REAP_QUEUED         (1U << 2)
#define SCHED_ENTITY_VRUNTIME_RELATIVE   (1U << 3)

#define SCHED_CMD_SLOTS 8U
#define SCHED_CMD_MASK  (SCHED_CMD_SLOTS - 1U)
#define SCHED_CMD_OVERFLOW_SLOTS 64U
#define SCHED_CMD_OVERFLOW_MASK  (SCHED_CMD_OVERFLOW_SLOTS - 1U)

#define SCHED_CMD_WAKE    1U
#define SCHED_CMD_ENQUEUE 2U
#define SCHED_CMD_SET_PRIORITY 3U
#define SCHED_CMD_SET_CLASS    4U
#define SCHED_CMD_REMOVE       5U
#define SCHED_CMD_STOP         6U
#define SCHED_CMD_EXIT         7U
#define SCHED_CMD_MIGRATE      8U
#define SCHED_CMD_START        9U

#define SCHED_CMD_CLASS_SHIFT 8U
#define SCHED_CMD_REMOVE_ACK   1U
#define SCHED_CMD_STATE_ACK    2U
#define SCHED_CMD_STATE_CHANGED (1U << 2)
#define SCHED_CMD_REMOVE_NO_REF (1U << 31)
#define SCHED_NICE_0_WEIGHT 1024U
#define SCHED_RT_TIMESLICE_NS 4000000ULL
#define SCHED_LIKELY(value) __builtin_expect(!!(value), 1)
#define SCHED_UNLIKELY(value) __builtin_expect(!!(value), 0)

void scheduler_update_min_vruntime(run_queue_t *queue);
bool scheduler_fair_current_wins(const run_queue_t *queue);
void scheduler_prepare_fair_migration(const run_queue_t *source, thread_t *thread);
void scheduler_restore_fair_migration(run_queue_t *target, thread_t *thread);
uint64_t scheduler_account_current(run_queue_t *queue, uint64_t now_ns);
void scheduler_trace_reschedule_interrupt(uint32_t cpu_id);
void scheduler_trace_reschedule_request(uint32_t cpu_id, bool send);
uint64_t scheduler_trace_reschedule_begin(void);
void scheduler_trace_reschedule_send(uint32_t cpu_id, uint64_t started,
                                      bool accepted, uint32_t attempts);

_Static_assert((SCHED_CMD_SLOTS & (SCHED_CMD_SLOTS - 1U)) == 0U,
               "scheduler command ring size must be a power of two");
_Static_assert((SCHED_CMD_OVERFLOW_SLOTS &
                (SCHED_CMD_OVERFLOW_SLOTS - 1U)) == 0U,
               "scheduler overflow ring size must be a power of two");
_Static_assert(CACHELINE_SIZE > sizeof(atomic_uint),
               "cache line must be larger than atomic index");

typedef struct sched_cmd {
    uintptr_t object;
    uint32_t op;
    uint32_t generation;
} sched_cmd_t;

_Static_assert(sizeof(sched_cmd_t) == 16U,
               "scheduler command entry must remain compact");

typedef struct sched_cmd_ring {
    /* 由消费者持有；生产者只读取。 */
    atomic_uint head;
    uint8_t head_pad[CACHELINE_SIZE - sizeof(atomic_uint)];

    /* 由生产者持有；消费者只读取。 */
    atomic_uint tail;
    uint8_t tail_pad[CACHELINE_SIZE - sizeof(atomic_uint)];

    sched_cmd_t slots[SCHED_CMD_SLOTS];
} __aligned(CACHELINE_SIZE) sched_cmd_ring_t;

typedef struct sched_cmd_overflow_ring {
    atomic_uint head;
    uint8_t head_pad[CACHELINE_SIZE - sizeof(atomic_uint)];
    atomic_uint tail;
    uint8_t tail_pad[CACHELINE_SIZE - sizeof(atomic_uint)];
    sched_cmd_t slots[SCHED_CMD_OVERFLOW_SLOTS];
} __aligned(CACHELINE_SIZE) sched_cmd_overflow_ring_t;

_Static_assert(__builtin_offsetof(sched_cmd_ring_t, tail) >= CACHELINE_SIZE,
               "command producer index must have its own cache line");
_Static_assert(__builtin_offsetof(sched_cmd_overflow_ring_t, tail) >=
                   CACHELINE_SIZE,
               "overflow producer index must have its own cache line");

typedef struct sched_cmd_channel {
    sched_cmd_ring_t primary;
    sched_cmd_overflow_ring_t overflow;
    /* 由生产者持有的模式位；消费者仅观察环形队列索引。 */
    bool overflow_active;
    uint8_t producer_pad[CACHELINE_SIZE - sizeof(bool)];
} __aligned(CACHELINE_SIZE) sched_cmd_channel_t;

/* 仅供调度器实现文件共用的私有状态。 */
typedef struct scheduler_cpu {
    run_queue_t queue;
    bool queue_snapshot_dirty;

    /* 任一入站命令变为可见时由生产者发布。 */
    atomic_bool command_pending;
    thread_t * _Atomic remote_wake_head;
    thread_t * _Atomic priority_head;
    /* 定时器和本地唤醒路径发布本地重新调度请求。 */
    atomic_bool need_resched;
    /*
    * 以读取为主的调度器负载快照。
    * 低 32 位：排队中的可运行线程数。
    * 高 32 位：排队中加上当前非空闲可运行线程数。
     */
    atomic_uint_fast64_t queue_snapshot;
    /*
    * 入站 SPSC 命令通道数组，长度为 MAX_CPUS。
    * 第 N 个环由 CPU N 生产、当前调度器 CPU 消费。
    * 主环已满时，溢出环仍保持命令顺序。
     */
    sched_cmd_channel_t *command_inbox;
    /* 在本地生产者处统计进入溢出环的命令数。 */
    uint64_t command_ring_full_count;

    /* 已离开执行栈、等待最终回收的线程。 */
    thread_t *reap_head;
    thread_t *reap_tail;
    /* 在所属 CPU 中断边界之外释放的远端命令引用。 */
    thread_t *command_release_head;
    thread_t *command_release_tail;
    /* 释放队列为空时不能进入中断状态保存/恢复路径。 */
    atomic_bool command_release_pending;
    /* 所属 CPU 切离后，回收器可在延迟上下文中运行。 */
    spinlock_t reap_lock;

    /* 每个 CPU 最多排队一个延迟回收任务。 */
    atomic_bool reap_deferred_queued;

    /* 所属 CPU 的槽位：仅在汇编保存旧续执行状态并安装新栈/地址空间后发布迁移。 */
    thread_t *switch_handoff;

    vaddr_t idle_stack_top;
} scheduler_cpu_t;

extern scheduler_cpu_t g_cpus[MAX_CPUS];
extern uint32_t g_cpu_count;

static inline bool scheduler_affinity_mask_allows(const cpumask_t *mask,
                                                 uint32_t cpu_id) {
    return mask != 0 && cpu_id < MAX_CPUS &&
           (mask->bits[cpu_id >> 6] &
            (1ULL << (cpu_id & 63U))) != 0;
}

static inline bool scheduler_affinity_allows(const thread_t *thread,
                                            uint32_t cpu_id) {
    return scheduler_affinity_mask_allows(thread != 0 ? &thread->affinity : 0,
                                         cpu_id);
}

static inline uint64_t scheduler_make_queue_snapshot(
    const scheduler_cpu_t *cpu) {
    uint32_t runnable = cpu->queue.nr_running;
    uint32_t load = runnable;
    thread_t *current = cpu->queue.current;

    if (current != 0 &&
        current != cpu->queue.idle &&
        (current->sched.flags & SCHED_ENTITY_ENQUEUED) == 0U) {
        unsigned state =
            atomic_load_explicit(&current->state, memory_order_relaxed);
        if (state == THREAD_RUNNING || state == THREAD_READY) {
            ++load;
        }
    }

    return ((uint64_t)load << 32) | (uint64_t)runnable;
}

static inline void scheduler_publish_queue_snapshot(scheduler_cpu_t *cpu) {
    atomic_store_explicit(&cpu->queue_snapshot,
                          scheduler_make_queue_snapshot(cpu),
                          memory_order_release);
    cpu->queue_snapshot_dirty = false;
}

static inline uint32_t scheduler_snapshot_runnable(
    const scheduler_cpu_t *cpu) {
    return (uint32_t)atomic_load_explicit(&cpu->queue_snapshot,
                                          memory_order_acquire);
}

static inline uint32_t scheduler_snapshot_load(
    const scheduler_cpu_t *cpu) {
    return (uint32_t)(atomic_load_explicit(&cpu->queue_snapshot,
                                           memory_order_acquire) >> 32);
}

bool scheduler_current_cpu(uint32_t *cpu_id);
uint64_t scheduler_lock(spinlock_t *lock);
void scheduler_unlock(spinlock_t *lock, uint64_t flags);
bool scheduler_cpu_available(uint32_t cpu_id);
uint32_t scheduler_choose_cpu(thread_t *thread, uint32_t current_cpu);
bool scheduler_select_cpu_mask(const cpumask_t *mask, uint32_t current_cpu,
                               uint32_t *selected_cpu);
uint32_t scheduler_choose_start_cpu(thread_t *thread, uint32_t current_cpu);

void initialize_thread(thread_t *thread, uint64_t tid, uint8_t class_id,
                       uint8_t priority);
void enqueue_local_raw(scheduler_cpu_t *cpu, thread_t *thread);
void dequeue_local_raw(scheduler_cpu_t *cpu, thread_t *thread);
void enqueue_local(scheduler_cpu_t *cpu, thread_t *thread);
void dequeue_local(scheduler_cpu_t *cpu, thread_t *thread);
thread_t *pick_next_local(scheduler_cpu_t *cpu);
bool validate_fair_node(rb_node_t *node, rb_node_t *parent,
                        thread_t **previous, uint32_t black_depth,
                        uint32_t *expected_black_depth, uint32_t *count);
