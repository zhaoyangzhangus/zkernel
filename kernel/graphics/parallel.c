#include <arch/x86_64/context.h>
#include <arch/x86_64/cpu.h>
#include <arch/x86_64/smp.h>
#include <kernel/sched.h>
#include "internal.h"

#define COMPOSITOR_RASTER_WORKERS 2U
#define COMPOSITOR_RASTER_STACK (32U * 1024U)
#define COMPOSITOR_PARALLEL_MIN_ROWS 64U

typedef struct compositor_raster_worker {
    thread_t thread;
    wait_queue_t waitq;
    atomic_bool pending;
    compositor_rows_fn draw;
    void *context;
    uint32_t first, end;
} __attribute__((aligned(64))) compositor_raster_worker_t;

static compositor_raster_worker_t g_raster_workers[COMPOSITOR_RASTER_WORKERS];
static uint8_t g_raster_stacks[COMPOSITOR_RASTER_WORKERS][COMPOSITOR_RASTER_STACK]
    __attribute__((aligned(16)));
static uint32_t g_raster_worker_count;
static wait_queue_t g_raster_join;
static atomic_flag g_raster_busy = ATOMIC_FLAG_INIT;
static bool g_raster_started;
atomic_uint_fast64_t g_compositor_parallel_batches;
atomic_uint_fast64_t g_compositor_parallel_worker_rows;

static bool compositor_raster_ready(void *context) {
    compositor_raster_worker_t *worker = context;
    return atomic_load_explicit(&worker->pending, memory_order_acquire);
}

static bool compositor_raster_finished(void *context) {
    uint32_t count = *(const uint32_t *)context;
    for (uint32_t i = 0U; i < count; ++i)
        if (atomic_load_explicit(&g_raster_workers[i].pending,
                                 memory_order_acquire)) return false;
    return true;
}

static void __attribute__((noreturn)) compositor_raster_worker_main(void *context) {
    compositor_raster_worker_t *worker = context;
    for (;;) {
        if (wait_on_queue_uninterruptible(&worker->waitq,
                compositor_raster_ready, worker, UINT64_MAX) != K_OK) continue;
        worker->draw(worker->context, worker->first, worker->end);
        atomic_fetch_add_explicit(&g_compositor_parallel_worker_rows,
            worker->end - worker->first, memory_order_relaxed);
        /* 完成发布之后不得再读取任务：所有者可能立即提交下一批。 */
        atomic_store_explicit(&worker->pending, false, memory_order_release);
        (void)wake_one(&g_raster_join);
    }
}

/* 仅合成器所有者启动一次；保留 CPU0 给输入及设备下半部。 */
void compositor_parallel_start(void) {
    if (g_raster_started) return;
    g_raster_started = true;
    wait_queue_init(&g_raster_join);
    uint32_t owner = x86_current_cpu_index();
    for (uint32_t cpu = 1U; cpu < x86_smp_discovered_count() && cpu < MAX_CPUS &&
         g_raster_worker_count < COMPOSITOR_RASTER_WORKERS; ++cpu) {
        if (cpu == owner || !x86_smp_cpu_online(cpu)) continue;
        uint32_t i = g_raster_worker_count;
        compositor_raster_worker_t *worker = &g_raster_workers[i];
        thread_t *thread = &worker->thread;
        wait_queue_init(&worker->waitq);
        refcount_init(&thread->object.refs, 1U);
        thread->object.type = KOBJECT_TYPE_THREAD;
        thread->tid = UINT64_MAX - 64U - i;
        sched_initialize_new_thread(thread, cpu, true);
        atomic_store_explicit(&thread->state, THREAD_READY, memory_order_relaxed);
        thread->kernel_stack_base = g_raster_stacks[i];
        thread->kernel_stack_size = COMPOSITOR_RASTER_STACK;
        thread->kernel_stack_top = (vaddr_t)(uintptr_t)
            (g_raster_stacks[i] + COMPOSITOR_RASTER_STACK);
        list_init(&thread->process_node);
        list_init(&thread->owned_mutexes);
        x86_prepare_kernel_thread(&thread->arch, thread->kernel_stack_top,
                                  compositor_raster_worker_main, worker);
        if (!sched_enqueue_bootstrap(thread)) continue;
        ++g_raster_worker_count;
        sched_monitor_register(thread, i == 0U ? "raster-0" : "raster-1");
    }
}

/* 每个任务独占扫描线；调用返回前全部完成，栈上的描述符及表面快照始终有效。
 * 小区域和单核直接绘制，不为每条扫描线排队，也不在等待时忙轮询。 */
void compositor_parallel_rows(uint32_t first, uint32_t end, uint32_t width,
                              compositor_rows_fn draw, void *context) {
    if (draw == 0 || end <= first) return;
    uint32_t rows = end - first;
    uint32_t helpers = g_raster_worker_count;
    if (rows / 8U < helpers + 1U) helpers = rows / 8U ? rows / 8U - 1U : 0U;
    /* 输入服务预算内的条带直接绘制；跨核唤醒和等待成本会抵消小批复制收益。
     * 原生输出、拖动等不分片的大任务仍可使用工作线程。 */
    if (helpers == 0U || rows < COMPOSITOR_PARALLEL_MIN_ROWS ||
        (uint64_t)rows * width <= WINDOW_COMPOSITOR_ATOMIC_PIXELS ||
        atomic_flag_test_and_set_explicit(&g_raster_busy, memory_order_acquire)) {
        draw(context, first, end);
        return;
    }
    uint32_t pieces = helpers + 1U;
    for (uint32_t i = 0U; i < helpers; ++i) {
        compositor_raster_worker_t *worker = &g_raster_workers[i];
        worker->draw = draw;
        worker->context = context;
        worker->first = first + (uint64_t)rows * i / pieces;
        worker->end = first + (uint64_t)rows * (i + 1U) / pieces;
        atomic_store_explicit(&worker->pending, true, memory_order_release);
        (void)wake_one(&worker->waitq);
    }
    draw(context, first + (uint64_t)rows * helpers / pieces, end);
    while (!compositor_raster_finished(&helpers))
        (void)wait_on_queue_uninterruptible(&g_raster_join,
            compositor_raster_finished, &helpers, UINT64_MAX);
    atomic_fetch_add_explicit(&g_compositor_parallel_batches, 1U, memory_order_relaxed);
    atomic_flag_clear_explicit(&g_raster_busy, memory_order_release);
}
