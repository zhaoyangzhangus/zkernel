#include <arch/x86_64/context.h>
#include <arch/x86_64/cpu.h>
#include <arch/x86_64/smp.h>
#include <kernel/deferred.h>
#include <kernel/process.h>
#include <kernel/rcu.h>
#include <kernel/sched.h>
#include <kernel/spinlock.h>
#include <kernel/string.h>

#define DEFERRED_QUEUE_CAPACITY 128U
#define DEFERRED_CRITICAL_BURST_LIMIT 4U
#define DEFERRED_WORKER_STACK_SIZE (64U * 1024U)

typedef struct deferred_item {
    deferred_work_fn_t function;
    void *argument;
} deferred_item_t;

#define DEFERRED_SLOW_WORK_SLOTS 8U
typedef struct deferred_slow_work {
    deferred_work_fn_t function;
    uint64_t count;
    uint64_t total_ns;
    uint64_t max_ns;
    uint64_t last_start_tsc;
    uint64_t last_end_tsc;
} deferred_slow_work_t;
/* 仅执行时间至少为 1 ms 的回调会访问此表或其队列锁。诊断计数会记录慢任务，不进行同步 UART 输出。 */
static deferred_slow_work_t g_deferred_slow_work[DEFERRED_SLOW_WORK_SLOTS];
static uint64_t g_deferred_slow_work_untracked;

static struct {
    spinlock_t lock;
    deferred_item_t items[DEFERRED_QUEUE_CAPACITY];
    uint32_t head;
    uint32_t tail;
    uint32_t count;
    /* 由锁保护；在多个短批次和所有消费者之间保持有效。 */
    uint32_t critical_burst;
    deferred_item_t critical_item;
    bool critical_ready; /* 由锁保护，包括休眠握手期间 */
    atomic_uint init_state;

    /*
    * 一个全局工作线程与当前单全局队列架构相匹配。它是常驻 Ring0 调度线程，不属于任何进程，也不
    * 使用用户 CR3。
     */
    thread_t worker;
    atomic_bool worker_started;
    atomic_bool worker_starting;
} g_deferred;

static volatile uint32_t g_deferred_progress[MAX_CPUS];
static volatile uint32_t g_deferred_waiting[MAX_CPUS];
static volatile uint32_t g_deferred_lock_owner = UINT32_MAX;

static void deferred_debug_set(uint32_t value) {
    uint32_t cpu_index = x86_current_cpu_index();
    if (cpu_index < MAX_CPUS) {
        __atomic_store_n(&g_deferred_progress[cpu_index], value,
                         __ATOMIC_RELEASE);
    }
}

uint32_t deferred_debug_progress(uint32_t cpu_index) {
    if (cpu_index >= MAX_CPUS) return 0U;
    return __atomic_load_n(&g_deferred_progress[cpu_index], __ATOMIC_ACQUIRE);
}

uint32_t deferred_debug_waiting(uint32_t cpu_index) {
    if (cpu_index >= MAX_CPUS) return 0U;
    return __atomic_load_n(&g_deferred_waiting[cpu_index], __ATOMIC_ACQUIRE);
}

uint32_t deferred_debug_lock_owner(void) {
    return __atomic_load_n(&g_deferred_lock_owner, __ATOMIC_ACQUIRE);
}

uint32_t deferred_debug_lock_state(void) {
    return atomic_load_explicit(&g_deferred.lock.state, memory_order_acquire);
}

uint32_t deferred_debug_worker_state(void) {
    return atomic_load_explicit(&g_deferred.worker.state, memory_order_acquire);
}

uint32_t deferred_debug_queue_count(void) {
    return __atomic_load_n(&g_deferred.count, __ATOMIC_ACQUIRE);
}

static uint8_t g_deferred_worker_stack[DEFERRED_WORKER_STACK_SIZE]
    __attribute__((aligned(16)));

/*
 * 设备 IRQ 通过 deferred_try_schedule() 入队。此处仅使用 try-lock 并不足够：如果 xHCI 完成中断
 * 到达时普通生产者正持有队列锁，那么确认 IRQ 后直接返回会丢失该 HID 传输唯一的唤醒信号。在此事件
 * 被消费前，下一个报告无法入队，因此键盘和鼠标输入会永久停止。
 *
 * 队列临界区必须中断安全且尽可能短。CPU 获得锁后不会被本地中断打断；其他 CPU 只会在下方固定大小
 * 的环形队列记账完成前短暂自旋。这是事件驱动机制，不是定时器轮询或 xHCI 重试循环。
 */
static uint64_t deferred_irq_save(void) {
    uint64_t flags;
    __asm__ volatile ("pushfq; popq %0; cli" : "=r"(flags) : : "memory");
    return flags;
}

static void deferred_irq_restore(uint64_t flags) {
    if ((flags & (1ULL << 9)) != 0U) {
        __asm__ volatile ("sti" : : : "memory");
    } else {
        __asm__ volatile ("cli" : : : "memory");
    }
}

static uint64_t deferred_lock(void) {
    uint64_t flags = deferred_irq_save();
    /*
    * 队列生产者既包括普通系统调用上下文，也包括 IRQ/延迟上下文。屏蔽中断可以保护本地临界区，但在
    * 普通生产者持有全局队列锁时，无法阻止调度器将其迁移到另一 CPU。必须让所有者留在当前 CPU，直到
    * 环形队列记账发布完成。
     */
    uint32_t cpu_index = x86_current_cpu_index();
    sched_preempt_disable();
    if (cpu_index < MAX_CPUS) {
        __atomic_store_n(&g_deferred_waiting[cpu_index], 1U,
                         __ATOMIC_RELEASE);
    }
    spinlock_lock(&g_deferred.lock);
    __atomic_store_n(&g_deferred_lock_owner, cpu_index, __ATOMIC_RELEASE);
    if (cpu_index < MAX_CPUS) {
        __atomic_store_n(&g_deferred_waiting[cpu_index], 0U,
                         __ATOMIC_RELEASE);
    }
    return flags;
}

static void deferred_unlock(uint64_t flags) {
    __atomic_store_n(&g_deferred_lock_owner, UINT32_MAX, __ATOMIC_RELEASE);
    spinlock_unlock(&g_deferred.lock);
    sched_preempt_enable();
    deferred_irq_restore(flags);
}

static void deferred_wake_worker(void) {
    deferred_debug_set(7U);
    if (!atomic_load_explicit(&g_deferred.worker_started,
                              memory_order_acquire)) {
        return;
    }

    /*
    * sched_wake() 可在 IRQ 中安全调用。如果工作线程仍处于 RUNNING 状态，此调用仅是提示；下方的
    * 队列锁握手会确保它在阻塞前观察到新入队的工作项。
    */
    sched_wake(&g_deferred.worker);
    deferred_debug_set(8U);

    /*
    * sched_wake() 有意设计为状态转换 API。如果先前的唤醒已使工作线程处于运行队列中的 READY 状态，
    * 就不会发生新的 BLOCKED -> READY 转换来触发 IPI。此状态下也要重试目标 CPU 请求，以关闭这种
    * pending IPI 竞争：工作线程已入队，但先前的请求已被消费。
     */
    if (atomic_load_explicit(&g_deferred.worker.state,
                             memory_order_acquire) != THREAD_RUNNING) {
        deferred_debug_set(9U);
        (void)x86_smp_request_reschedule(g_deferred.worker.owner_cpu);
    }
    deferred_debug_set(10U);
}

static bool deferred_worker_has_work_locked(void) {
    return g_deferred.count != 0U || g_deferred.critical_ready;
}

static bool deferred_worker_prepare_sleep(void) {
    thread_t *thread = sched_current_thread();
    if (thread != &g_deferred.worker) return false;

    /*
    * 防止丢失唤醒的屏障：
     *
    * 生产者：
     *     deferred_lock -> enqueue/publish -> unlock -> sched_wake(worker)
     *
    * 消费者：
     *     deferred_lock -> verify empty -> RUNNING->BLOCKED -> unlock
     *                   -> schedule()
     *
    * 因此 IRQ 绝不会在检查队列为空与发布 BLOCKED 状态之间发布工作。如果工作在解锁后发布，
    * sched_wake() 会观察到 BLOCKED（或 READY/当前运行）状态并保留唤醒信号。
     */
    uint64_t flags = deferred_lock();

    if (deferred_worker_has_work_locked()) {
        deferred_unlock(flags);
        return false;
    }

    bool blocked = sched_publish_blocked(thread);

    deferred_unlock(flags);
    return blocked;
}

static void __attribute__((noreturn))
deferred_worker_main(void *argument) {
    (void)argument;

    for (;;) {
        /*
         * 有界批次可避免持续繁忙的设备让此 FAIR 内核工作线程永久占用 CPU。
         */
        uint32_t completed = deferred_run(64U);

        if (completed == 64U) {
            schedule();
            continue;
        }

        /*
         * deferred_run() 可能因队列变空而停止。在生产者锁下重新检查，并与观察到队列为空这一操作
         * 原子地发布 BLOCKED 状态。
         */
        if (deferred_worker_prepare_sleep()) {
            schedule();
        } else {
            __asm__ volatile ("pause");
        }
    }
}

bool deferred_init(void) {
    /* 初始状态发布后，队列操作会使用各自的锁。读取就绪状态时不重写共享的初始化缓存行。 */
    if (atomic_load_explicit(&g_deferred.init_state, memory_order_acquire) == 2U) return true;
    unsigned expected = 0U;
    if (atomic_compare_exchange_strong_explicit(&g_deferred.init_state, &expected, 1U,
                                                memory_order_acq_rel,
                                                memory_order_acquire)) {
        atomic_init(&g_deferred.lock.state, 0U);
        g_deferred.head = 0;
        g_deferred.tail = 0;
        g_deferred.count = 0;
        g_deferred.critical_burst = 0;
        g_deferred.critical_item.function = 0;
        g_deferred.critical_item.argument = 0;
        g_deferred.critical_ready = false;
        atomic_init(&g_deferred.worker_started, false);
        atomic_init(&g_deferred.worker_starting, false);
        for (uint32_t cpu_index = 0U; cpu_index < MAX_CPUS; ++cpu_index) {
            __atomic_store_n(&g_deferred_progress[cpu_index], 0U,
                             __ATOMIC_RELAXED);
            __atomic_store_n(&g_deferred_waiting[cpu_index], 0U,
                             __ATOMIC_RELAXED);
        }
        __atomic_store_n(&g_deferred_lock_owner, UINT32_MAX,
                         __ATOMIC_RELAXED);
        atomic_store_explicit(&g_deferred.init_state, 2U, memory_order_release);
        return true;
    }
    while (atomic_load_explicit(&g_deferred.init_state, memory_order_acquire) != 2U) {
        __asm__ volatile ("pause");
    }
    return true;
}

bool deferred_start_worker(void) {
    if (!deferred_init()) return false;

    if (atomic_load_explicit(&g_deferred.worker_started,
                             memory_order_acquire)) {
        return true;
    }

    /*
    * 这是显式的调度器就绪边界。启动早期只调用 deferred_init()/deferred_run()，无需可调度线程。
     */
    thread_t *current = sched_current_thread();
    uint32_t cpu_id = x86_current_cpu_index();
    if (current == 0 || cpu_id >= MAX_CPUS) return false;

    /* 创建期间不允许其他调用方重置同一个线程；失败后释放创建权，允许稍后重试。 */
    bool expected = false;
    if (!atomic_compare_exchange_strong_explicit(&g_deferred.worker_starting,
            &expected, true, memory_order_acq_rel, memory_order_acquire)) return false;
    if (atomic_load_explicit(&g_deferred.worker_started, memory_order_acquire)) {
        atomic_store_explicit(&g_deferred.worker_starting, false, memory_order_release);
        return true;
    }

    thread_t *worker = &g_deferred.worker;
    memset(worker, 0, sizeof(*worker));

    refcount_init(&worker->object.refs, 1U);
    worker->object.type = KOBJECT_TYPE_THREAD;
    worker->object.flags = 0U;
    worker->object.ops = 0;

    /* Vendor 回调使用此工作线程的身份表示锁所有权。调度平局判定不得为不同内核任务分配相同 ID。 */
    worker->tid = process_allocate_task_id();
    worker->process = 0;
    sched_initialize_new_thread(worker, cpu_id, true);
    atomic_store_explicit(&worker->state, THREAD_READY, memory_order_relaxed);

    worker->kernel_stack_base = g_deferred_worker_stack;
    worker->kernel_stack_size = sizeof(g_deferred_worker_stack);
    worker->kernel_stack_top =
        ((vaddr_t)(uintptr_t)g_deferred_worker_stack +
         sizeof(g_deferred_worker_stack)) &
        ~(vaddr_t)0x0FULL;

    list_init(&worker->process_node);
    list_init(&worker->owned_mutexes);

    x86_prepare_kernel_thread(&worker->arch, worker->kernel_stack_top,
                              deferred_worker_main, 0);

    /*
    * 使线程可运行前先发布唤醒目标。并发 IRQ 可能在状态为 READY 时调用 sched_wake()；这不会造成问题，
    * 因为下方 sched_enqueue() 已会保留该可运行实例。
     */
    atomic_store_explicit(&g_deferred.worker_started, true,
                          memory_order_release);

    bool enqueued = sched_enqueue_bootstrap(worker);
    if (!enqueued) {
        atomic_store_explicit(&g_deferred.worker_started, false,
                              memory_order_release);
        atomic_store_explicit(&g_deferred.worker_starting, false,
                              memory_order_release);
        return false;
    }
    sched_monitor_register(worker, "irq-deferred");
    atomic_store_explicit(&g_deferred.worker_starting, false, memory_order_release);

    /*
    * 不要从用户初始化引导续接路径同步切换。工作线程已处于 READY 并入队；重新调度请求和下一个常规
    * 调度边界会运行该线程，不会使引导线程困在首次运行的上下文交接中。
     */

    return true;
}

bool deferred_worker_started(void) {
    return atomic_load_explicit(&g_deferred.worker_started,
                                memory_order_acquire);
}

/* 调用方持有中断安全的队列锁。两个生产者入口共用一条环形队列发布路径，同时保留各自的诊断和唤醒。 */
static bool deferred_enqueue_locked(deferred_work_fn_t function, void *argument) {
    if (g_deferred.count == DEFERRED_QUEUE_CAPACITY) return false;
    g_deferred.items[g_deferred.tail].function = function;
    g_deferred.items[g_deferred.tail].argument = argument;
    g_deferred.tail = (g_deferred.tail + 1U) % DEFERRED_QUEUE_CAPACITY;
    ++g_deferred.count;
    return true;
}

bool deferred_schedule(deferred_work_fn_t function, void *argument) {
    uint64_t flags;
    deferred_debug_set(1U);
    if (function == 0 || !deferred_init()) return false;
    deferred_debug_set(2U);
    flags = deferred_lock();
    deferred_debug_set(4U);
    if (!deferred_enqueue_locked(function, argument)) {
        deferred_unlock(flags);
        return false;
    }
    deferred_debug_set(5U);
    deferred_unlock(flags);
    deferred_debug_set(6U);
    deferred_wake_worker();
    deferred_debug_set(11U);
    return true;
}

bool deferred_try_schedule(deferred_work_fn_t function, void *argument) {
    uint64_t flags;
    if (function == 0 || !deferred_init()) return false;
    flags = deferred_lock();
    if (!deferred_enqueue_locked(function, argument)) {
        deferred_unlock(flags);
        return false;
    }
    deferred_unlock(flags);
    deferred_wake_worker();
    return true;
}

bool deferred_schedule_critical(deferred_work_fn_t function, void *argument) {
    uint64_t flags;

    /*
    * 此单槽队列专门保留给 xHCI MSI-X 工作线程。调用方拥有合并式排队标志，涵盖待处理和正在执行的设备
    * 工作。若未增加逐来源所有权，不得将其改成共享回退队列。
     *
    * 发布操作现在与工作线程休眠握手共用 deferred_lock，因此可同时关闭紧急槽位和普通环形队列中
    * EMPTY->BLOCKED 之间的丢失唤醒窗口。
     */
    if (function == 0 || !deferred_init()) return false;

    flags = deferred_lock();

    if (g_deferred.critical_ready) {
        deferred_unlock(flags);
        return false;
    }

    g_deferred.critical_item.function = function;
    g_deferred.critical_item.argument = argument;

    g_deferred.critical_ready = true;

    deferred_unlock(flags);
    deferred_wake_worker();
    return true;
}

/* 调用方持有队列锁。执行回调前清空槽位，以保留回调中继续追加后续工作的能力。 */
static bool deferred_take_critical(deferred_item_t *item) {
    if (item == 0 || !g_deferred.critical_ready) {
        return false;
    }
    *item = g_deferred.critical_item;
    g_deferred.critical_ready = false;
    return item->function != 0;
}

static void deferred_record_work(deferred_work_fn_t function, uint64_t start) {
    uint64_t end = x86_read_tsc();
    uint64_t nanoseconds = x86_tsc_to_ns(end - start);
    if (nanoseconds < 1000000ULL) return;
    uint64_t flags = deferred_lock();
    deferred_slow_work_t *record = 0;
    for (uint32_t i = 0; i < DEFERRED_SLOW_WORK_SLOTS; ++i) {
        if (g_deferred_slow_work[i].function == function) {
            record = &g_deferred_slow_work[i];
            break;
        }
        if (record == 0 && g_deferred_slow_work[i].function == 0)
            record = &g_deferred_slow_work[i];
    }
    if (record != 0) {
        record->function = function;
        ++record->count;
        record->total_ns += nanoseconds;
        if (nanoseconds > record->max_ns) record->max_ns = nanoseconds;
        record->last_start_tsc = start;
        record->last_end_tsc = end;
    } else {
        ++g_deferred_slow_work_untracked;
    }
    deferred_unlock(flags);
}

uint32_t deferred_run(uint32_t budget) {
    uint32_t completed = 0;
    if (budget == 0U || !deferred_init()) return 0;
    while (completed < budget) {
        deferred_item_t item = {0};
        /* 仅串行化选择过程，不串行化执行过程。共享的突发计数可避免短系统调用批次（以及并发消费者）
         * 在等待中的网络任务获得执行机会前重置 HID 优先级。 */
        uint64_t flags = deferred_lock();
        bool have_item = false;
        if (g_deferred.critical_burst < DEFERRED_CRITICAL_BURST_LIMIT ||
            g_deferred.count == 0U) {
            have_item = deferred_take_critical(&item);
        }
        if (have_item) {
            if (g_deferred.critical_burst < DEFERRED_CRITICAL_BURST_LIMIT)
                ++g_deferred.critical_burst;
        } else if (g_deferred.count != 0U) {
            item = g_deferred.items[g_deferred.head];
            g_deferred.head = (g_deferred.head + 1U) % DEFERRED_QUEUE_CAPACITY;
            --g_deferred.count;
            have_item = item.function != 0;
            g_deferred.critical_burst = 0;
        } else {
            g_deferred.critical_burst = 0;
        }
        deferred_unlock(flags);
        if (!have_item) break;
        uint64_t start_tsc = x86_read_tsc();
        item.function(item.argument);
        deferred_record_work(item.function, start_tsc);
        ++completed;
    }
    /* RCU 回调在可抢占的普通内核上下文中执行，不能放进硬中断。 */
    if (completed < budget) (void)rcu_poll(budget - completed);
    return completed;
}

static void deferred_self_test_work(void *argument) {
    atomic_uint *counter = (atomic_uint *)argument;
    if (counter != 0) atomic_fetch_add_explicit(counter, 1U, memory_order_relaxed);
}

bool deferred_self_test(void) {
    atomic_uint counter;
    atomic_init(&counter, 0U);
    if (!deferred_schedule(deferred_self_test_work, &counter) ||
        !deferred_schedule(deferred_self_test_work, &counter) ||
        deferred_run(2U) != 2U) return false;
    return atomic_load_explicit(&counter, memory_order_relaxed) == 2U;
}
