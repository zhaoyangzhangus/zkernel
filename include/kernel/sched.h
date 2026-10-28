#pragma once
#pragma once
#include "base.h"
#include "object.h"
#include "list.h"
#include "rbtree.h"
#include "spinlock.h"
#include "cpumask.h"
#include <arch/x86_64/context.h>

struct process;
struct waiter;

enum thread_state {
    THREAD_NEW = 0,
    THREAD_READY,
    THREAD_RUNNING,
    THREAD_BLOCKED,
    THREAD_STOPPED,
    THREAD_DEAD,
};

enum sched_class_id {
    SCHED_CLASS_RT = 0,
    SCHED_CLASS_FAIR,
    SCHED_CLASS_IDLE,
};

typedef struct sched_entity {
    uint64_t runtime_ns;
    uint64_t vruntime;
    uint64_t exec_start_ns;
    uint64_t slice_runtime_ns;
    uint32_t weight;
    int8_t nice;
    int8_t latency_hint;
    uint16_t flags;
    rb_node_t fair_node;
    list_head_t rt_node;
} sched_entity_t;

typedef struct thread {
    /* 由进程管理的对象标识和生命周期关联。 */
    object_header_t object;
    uint64_t tid;
    struct process *process;
    /* 由调度器发布可运行状态；进程管理器只读取该状态。 */
    atomic_uint state;
    /*
    * 由调度器管理的阻塞代数。
    * 所属 CPU 在发布 THREAD_BLOCKED 前递增该值。
    * 远端 SPSC 唤醒命令携带此值，以拒绝过期唤醒。
     */
     atomic_uint block_epoch;
    /* 非所属 CPU 的启动请求独立于线程状态进行串行化。 */
    atomic_bool start_pending;
    /* 由进程管理器维护的执行引用和回收生命周期标志。 */
    uint32_t flags;

    /* 所属线程的嵌套计数：内部 I/O 可以休眠或被抢占，但在保护区退出前，
     * 即使收到外部终止请求，其内核续执行路径也必须保持有效。 */
    atomic_uint exit_defer_count;

    /* 调度器热路径字段，放在架构状态和用户栈状态之前。 */
    uint16_t owner_cpu;
    uint16_t current_cpu;
    uint16_t migration_target_cpu;
    bool migration_pending;
    uint8_t sched_class;
    uint8_t rt_priority;
    uint8_t base_sched_class;
    uint8_t base_rt_priority;
    sched_entity_t sched;

    /* 由架构层管理的执行栈和已保存 CPU 状态。 */
    void *kernel_stack_base;
    vaddr_t kernel_stack_top;
    size_t kernel_stack_size;
    arch_thread_state_t arch;

    /* 由 pthread 风格创建方管理的可选用户栈。最后一次上下文切换后，
     * 调度器回收器会清除此映射。 */
    vaddr_t user_stack_base;
    size_t user_stack_size;
    bool user_stack_owned;

    /* 由调度器管理的 CPU 亲和性，是迁移路径使用的字段。 */
    cpumask_t affinity;

    /* 由等待/同步子系统管理的优先级继承状态。 */
    list_head_t owned_mutexes;
    void *pi_blocked_on;

    /* 由进程管理器维护的线程成员关系。 */
    list_head_t process_node;

    /* 调度器管理的上下文切换后回收 FIFO 链接。 */
    struct thread *reap_next;
    /* 多个命令消费者共享此字段；remote_wake_lock 保护该节点及其计数。
     * 所属 CPU 的 FIFO 仍由本地中断上下文独占。 */
    struct thread *command_release_next;
    uint32_t command_release_count;
    /* 远端状态变更/移除命令可能需要等待所属 CPU 消费。 */
    atomic_uint command_ack;

    /* 由等待子系统管理的阻塞关联。 */
    struct waiter * _Atomic blocked_waiter;

    /* 由进程管理器管理的退出和 exec 发布状态。 */
    int64_t exit_code;
    bool exec_pending;
    vaddr_t exec_entry;
    vaddr_t exec_stack;

    uint64_t signal_pending;
    uint64_t signal_mask;
    vaddr_t signal_frame;
    uint32_t signal_depth;

    /* Linux CLONE_CHILD_CLEARTID / set_tid_address 状态。 */
    vaddr_t linux_clear_tid;
    bool linux_clear_tid_pending;
    vaddr_t linux_robust_list;
    uint64_t linux_robust_length;
    /* 内嵌式远端唤醒邮箱；入队发布时持有一个对象引用。 */
    atomic_bool remote_wake_queued;
    /* 串行化唤醒邮箱的复用以及合并命令释放的所有权。 */
    atomic_uint remote_wake_lock;
    struct thread *remote_wake_next;
    uint32_t remote_wake_epoch;
    /* 优先级更新独立于有界命令环；邮箱锁保护负载、链表及单个排队引用。 */
    atomic_bool priority_queued;
    atomic_uint priority_request;
    struct thread *priority_next;
    /* 由监控器维护的成员关系；回收线程或名称前必须先移除。 */
    struct thread *monitor_next;
    const char *monitor_name;
    uint64_t monitor_created_ns;
    uint64_t monitor_retired_ns;
} thread_t;

struct os_process_snapshot;
struct os_thread_snapshot;
struct os_task_info;
struct os_cpu_usage;
kstatus_t sched_usage_query(struct os_cpu_usage *info);
/* 注销函数返回前，名称和线程存储必须保持有效。查询会在专用成员锁下复制数据；
 * 持有运行队列锁时绝不能调用。 */
void sched_monitor_register(thread_t *thread, const char *name);
void sched_monitor_unregister(thread_t *thread);
kstatus_t sched_monitor_process(struct os_process_snapshot *snapshot);
kstatus_t sched_monitor_thread(uint32_t index, struct os_thread_snapshot *snapshot);
kstatus_t sched_monitor_query(uint64_t tid, struct os_task_info *info);

/*
 * 被唤醒的等待者会将队列节点保留在内核栈上，直到恢复执行并将其摘链。
 * 仍关联这些节点的 READY 线程暂时不能安全回收。
 */
static inline bool sched_wait_cleanup_pending(const thread_t *thread) {
    return thread != 0 &&
           atomic_load_explicit(&thread->blocked_waiter,
                                memory_order_acquire) != 0;
}

#define RT_PRIORITY_LEVELS 32u

typedef struct run_queue {
    thread_t *current;
    thread_t *idle;

    rb_root_t fair_root;
    thread_t *fair_leftmost;
    uint64_t min_vruntime;
    uint64_t clock_ns;
    uint32_t fair_count;
    uint32_t rt_bitmap;
    uint32_t nr_running;
    uint32_t hot_padding;

    list_head_t rt_queues[RT_PRIORITY_LEVELS];
    /* 仅所属 CPU 更新；监视器无锁读取，不受进程枚举上限和线程退出影响。 */
    uint64_t usage_busy_ns,usage_idle_ns;
} run_queue_t;

void sched_init(void);
void sched_cpu_init(uint32_t cpu_id);
/* 由调度器初始化 Process 创建的尚不可运行线程。 */
void sched_initialize_new_thread(thread_t *thread, uint32_t cpu_id,
                                 bool pin_to_cpu);
/* 由调度器为基于栈的同步自测样例执行初始化。 */
void sched_initialize_test_thread(thread_t *thread, uint64_t tid,
                                  uint8_t class_id, uint8_t priority);
/* 在线程启动前发布由调度器管理的首次放置元数据。 */
void sched_mark_initial_placement(thread_t *thread);
/* 成功发布新的可运行队列项时返回 true。 */
bool sched_enqueue(thread_t *thread);
/*
 * 仅用于启动阶段，将 BSP 引导续接任务加入队列。此操作仍会发送远端重新调度请求，
 * 但不会向逻辑空闲项发送自中断，因为其栈是正在运行的引导续接栈，而非已保存的
 * 调度器上下文。
 */
bool sched_enqueue_bootstrap(thread_t *thread);
kstatus_t sched_start_thread(thread_t *thread);
bool sched_publish_blocked(thread_t *thread);
bool sched_publish_running(thread_t *thread);
/* 仅由调度器发布可运行状态的终止转换。 */
bool sched_publish_dead(thread_t *thread);
void sched_wake(thread_t *thread);
void sched_block_current(void);
void schedule(void);
/* 接收 IPI 门铃；仅当本地策略要求时才执行调度。 */
void sched_handle_reschedule_request(void);
void sched_tick(uint64_t now_ns);
/* 有界、可选的唤醒延迟诊断；此钩子绝不触发调度。 */
void sched_trace_wake_stall(uint64_t now_tsc);
void sched_trace_inbox_stall(uint64_t now_tsc, uint64_t rip, uint64_t rflags);
bool sched_tick_should_preempt(void);
kstatus_t sched_set_affinity(thread_t *thread, const cpumask_t *mask);
void sched_set_effective_priority(thread_t *thread, uint8_t class_id,
                                  uint8_t rt_priority);

thread_t *sched_current_thread(void);
/* 调用方是当前执行线程；这些函数不会禁用抢占或清除待处理信号。
 * 发布 DEAD 状态时，会在所属 CPU 关闭中断后重新检查。 */
static inline void sched_exit_defer_begin(thread_t *thread) {
    if (atomic_fetch_add_explicit(&thread->exit_defer_count, 1U,
                                  memory_order_acq_rel) == UINT32_MAX)
        __builtin_trap();
}

static inline void sched_exit_defer_end(thread_t *thread) {
    if (atomic_fetch_sub_explicit(&thread->exit_defer_count, 1U,
                                  memory_order_acq_rel) == 0U)
        __builtin_trap();
}
void sched_remove(thread_t *thread);
void sched_finish_switch(void);
uint32_t sched_runnable_count(void);
bool sched_try_run_ready(void);
void sched_preempt_disable(void);
void sched_preempt_enable(void);
bool sched_preempt_disabled(void);
bool sched_validate_current_cpu(void);
bool sched_accounting_self_test(void);
bool sched_fair_policy_self_test(void);
bool sched_rt_policy_self_test(void);
bool sched_balance_self_test(void);
bool sched_state_transition_self_test(void);
bool sched_remote_remove_ack_self_test(void);
bool sched_remote_wake_self_test(void);
bool sched_context_switch_self_test(void);
bool sched_debug_cpu(uint32_t cpu_id, uint32_t *current_state,
                     uint64_t *current_tid, uint32_t *runnable_count);
bool sched_cpu_uses_root(uint32_t cpu_id, paddr_t root);
/* 引导栈切换到高地址别名后，同步空闲线程所使用的 Ring0 栈顶。 */
bool sched_set_boot_kernel_stack(vaddr_t stack_top);

/* 启动阶段诊断：返回最近一次入队的线程，正式版本由 trace 设施替代。 */
