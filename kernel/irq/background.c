#include <arch/x86_64/context.h>
#include <arch/x86_64/cpu.h>
#include <arch/x86_64/smp.h>
#include <kernel/deferred.h>
#include <kernel/process.h>
#include <kernel/sched.h>
#include <kernel/spinlock.h>
#include <kernel/string.h>

#define BACKGROUND_CAPACITY 128U
#define BACKGROUND_STACK_SIZE (64U * 1024U)
typedef struct background_item {
    deferred_work_fn_t function;
    void *argument;
} background_item_t;

static struct {
    spinlock_t lock;
    background_item_t items[BACKGROUND_CAPACITY];
    uint32_t head, tail, count;
    atomic_uint started; /* 0: unavailable, 1: constructing, 2: runnable */
    thread_t worker;
} g_background;
static uint8_t g_background_stack[BACKGROUND_STACK_SIZE] __attribute__((aligned(16)));

static uint64_t background_lock(void) {
    uint64_t flags;
    __asm__ volatile ("pushfq; popq %0; cli" : "=r"(flags) : : "memory");
    spinlock_lock(&g_background.lock);
    return flags;
}

static void background_unlock(uint64_t flags) {
    spinlock_unlock(&g_background.lock);
    if ((flags & (1ULL << 9)) != 0U) __asm__ volatile ("sti" : : : "memory");
}

bool deferred_background_worker_started(void) {
    return atomic_load_explicit(&g_background.started, memory_order_acquire) == 2U;
}

bool deferred_background_schedule(deferred_work_fn_t function, void *argument) {
    if (function == 0 || !deferred_background_worker_started()) return false;
    uint64_t flags = background_lock();
    if (g_background.count == BACKGROUND_CAPACITY) {
        background_unlock(flags);
        return false;
    }
    bool notify = g_background.count == 0U;
    g_background.items[g_background.tail] = (background_item_t){function, argument};
    g_background.tail = (g_background.tail + 1U) % BACKGROUND_CAPACITY;
    ++g_background.count;
    background_unlock(flags);
    if (notify) sched_wake(&g_background.worker);
    return true;
}

static bool background_take(background_item_t *item) {
    uint64_t flags = background_lock();
    bool available = g_background.count != 0U;
    if (available) {
        *item = g_background.items[g_background.head];
        g_background.head = (g_background.head + 1U) % BACKGROUND_CAPACITY;
        --g_background.count;
    }
    background_unlock(flags);
    return available;
}

static bool background_prepare_sleep(void) {
    /* Publish BLOCKED under the producer's lock, closing empty -> sleep.
     * A producer after unlock sees BLOCKED and owns the corresponding wake. */
    uint64_t flags = background_lock();
    bool blocked = g_background.count == 0U &&
        sched_publish_blocked(&g_background.worker);
    background_unlock(flags);
    return blocked;
}

static __attribute__((noreturn)) void background_worker_main(void *argument) {
    (void)argument;
    for (;;) {
        background_item_t item;
        if (background_take(&item)) {
            item.function(item.argument);
            schedule();
        } else if (background_prepare_sleep()) {
            schedule();
        }
    }
}

bool deferred_background_start_worker(void) {
    if (deferred_background_worker_started()) return true;
    if (sched_current_thread() == 0) return false;
    uint32_t cpu = x86_current_cpu_index();
    if (cpu >= MAX_CPUS) return false;
    unsigned expected = 0U;
    if (!atomic_compare_exchange_strong_explicit(&g_background.started,
            &expected, 1U, memory_order_acq_rel, memory_order_acquire)) return false;
    /* Prefer the last AP: input normally owns CPU0, composition the first AP.
     * These are still separate schedulable FAIR threads on a one-CPU system. */
    for (uint32_t i = 0; i < x86_smp_discovered_count() && i < MAX_CPUS; ++i)
        if (i != x86_current_cpu_index() && x86_smp_cpu_online(i)) cpu = i;
    thread_t *worker = &g_background.worker;
    memset(worker, 0, sizeof(*worker));
    refcount_init(&worker->object.refs, 1U);
    worker->object.type = KOBJECT_TYPE_THREAD;
    worker->tid = process_allocate_task_id();
    sched_initialize_new_thread(worker, cpu, true);
    list_init(&worker->process_node);
    list_init(&worker->owned_mutexes);
    worker->kernel_stack_base = g_background_stack;
    worker->kernel_stack_size = sizeof(g_background_stack);
    worker->kernel_stack_top = ((uintptr_t)g_background_stack +
        sizeof(g_background_stack)) & ~(uintptr_t)15U;
    x86_prepare_kernel_thread(&worker->arch, worker->kernel_stack_top,
                              background_worker_main, 0);
    atomic_store_explicit(&worker->state, THREAD_READY, memory_order_release);
    if (!sched_enqueue_bootstrap(worker)) {
        atomic_store_explicit(&g_background.started, 0U, memory_order_release);
        return false;
    }
    sched_monitor_register(worker, "background-cleanup");
    atomic_store_explicit(&g_background.started, 2U, memory_order_release);
    return true;
}
