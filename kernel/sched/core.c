#include <kernel/sched.h>
#include <arch/x86_64/cpu.h>
#include <arch/x86_64/paging.h>
#include <arch/x86_64/smp.h>
#include <kernel/process.h>
#include <kernel/perf.h>
#include <kernel/telemetry.h>
#include <kernel/wait.h>
#include <kernel/deferred.h>
#include <kernel/mm.h>
#include "internal.h"
#include <arch/x86_64/syscall_internal.h>

/* 通过私有调度器头文件与 clock.c 共用的定义。 */
scheduler_cpu_t g_cpus[MAX_CPUS];
static thread_t g_idle_threads[MAX_CPUS];
uint32_t g_cpu_count;
static paddr_t g_kernel_root;
static arch_switch_context_t *g_context_test_from;
static arch_switch_context_t *g_context_test_to;
static volatile uint32_t g_context_test_ran;
static volatile uint32_t g_context_test_rounds;
typedef struct sched_wake_trace {
    uint64_t tid, epoch, notified, ready, number, argument0;
    uint64_t current_tid, current_vruntime, vruntime, floor, queued, active;
    uint64_t published, source_cpu, target_cpu;
    uint64_t interrupt, interrupt_tid, interrupt_active, interrupt_preempt;
    uint64_t interrupt_preempt_caller;
    uint64_t requested, request_send;
} sched_wake_trace_t;
typedef struct sched_wake_delay {
    uint64_t tid, number, argument0, total_ns, notify_to_ready_ns, ready_to_run_ns;
    uint64_t current_tid, current_vruntime, vruntime, floor, queued, active;
    uint64_t notify_to_publish_ns, publish_to_ready_ns, source_cpu, target_cpu;
    uint64_t publish_to_interrupt_ns, interrupt_to_ready_ns;
    uint64_t interrupt_tid, interrupt_active, interrupt_preempt;
    uint64_t interrupt_preempt_caller;
    uint64_t publish_to_request_ns, request_to_interrupt_ns, request_send;
} sched_wake_delay_t;
static volatile uint64_t g_sched_wake_trace_tid;
/* 可选诊断阈值，不是调度策略参数。默认保留原有的 10ms 门限；
 * 有界的精细采样可定位帧间隔以下的等待。 */
static volatile uint64_t g_sched_wake_trace_threshold_ns = 10000000ULL;
static sched_wake_trace_t g_sched_wake_trace;
static volatile sched_wake_delay_t g_sched_wake_delays[16];
static atomic_flag g_sched_wake_trace_guard = ATOMIC_FLAG_INIT;
static volatile uint64_t g_sched_preempt_callers[MAX_CPUS];
/* READY 状态下采样到的最长间隔：持续时间、所属 tid/pid、系统调用、抢占状态、
 * 外层保护区调用方、TLB 保护区、类别、阻塞代数、CPU、选中 tid、
 * 当前系统调用号、argument0 和用户 RIP。 */
static volatile uint64_t g_sched_wake_stall[14];
/* 字段与 READY 停滞记录中的所有者信息相同，后接被中断的 RIP/RFLAGS 和起始 TSC。
 * 与 READY 停滞不同，此记录测量已发布邮箱命令的等待时间。 */
static volatile uint64_t g_sched_inbox_stall[17];
/* 有界试验期间实际发送且未合并的最慢重新调度请求：持续时间（ns）、源 CPU、
 * 目标 CPU、起止 TSC、是否接受、尝试次数。此为全局最大值，不一定对应
 * 唤醒延迟记录中的同一事件。 */
static volatile uint64_t g_sched_ipi_send[7];

/* 内核工作线程没有用户返回帧。统一验证所属进程和栈边界，避免把内核栈
 * 内容误记成系统调用参数；诊断禁用时不会进入此路径。 */
static arch_trap_frame_t *scheduler_trace_user_frame(const thread_t *thread) {
    if (thread == 0 || thread->process == 0 ||
        thread->kernel_stack_top < sizeof(arch_trap_frame_t)) return 0;
    return (arch_trap_frame_t *)(uintptr_t)
        (thread->kernel_stack_top - sizeof(arch_trap_frame_t));
}

static uint64_t scheduler_trace_threshold_ns(void) {
    uint64_t value = g_sched_wake_trace_threshold_ns;
    if (value == 0U) return 10000000ULL;
    if (value < 100000ULL) return 100000ULL;
    return value > 1000000000ULL ? 1000000000ULL : value;
}

void sched_trace_inbox_stall(uint64_t now_tsc, uint64_t rip, uint64_t rflags) {
    if (g_sched_wake_trace_tid == 0U) return;
    uint32_t cpu_id = x86_current_cpu_index();
    if (cpu_id >= g_cpu_count ||
        atomic_flag_test_and_set_explicit(&g_sched_wake_trace_guard,
                                          memory_order_acquire)) return;
    sched_wake_trace_t *trace = &g_sched_wake_trace;
    if (trace->tid == g_sched_wake_trace_tid && trace->published &&
        !trace->ready && trace->target_cpu == cpu_id &&
        trace->source_cpu != cpu_id && now_tsc >= trace->published) {
        uint64_t elapsed = x86_tsc_to_ns(now_tsc-trace->published);
        thread_t *current = g_cpus[cpu_id].queue.current;
        if (elapsed >= scheduler_trace_threshold_ns() && elapsed > g_sched_inbox_stall[0] &&
            (current == 0 || current->tid != trace->tid)) {
            bool active = x86_syscall_active_fast();
            bool preempt = sched_preempt_disabled();
            arch_trap_frame_t *frame = active ? scheduler_trace_user_frame(current) : 0;
            uint64_t values[17] = {
                elapsed, current != 0 ? current->tid : 0U,
                current != 0 && current->process != 0 ? current->process->pid : 0U,
                active, preempt, preempt ? g_sched_preempt_callers[cpu_id] : 0U,
                x86_tlb_shootdown_active(), current != 0 ? current->sched_class : 0U,
                trace->epoch, cpu_id, trace->tid,
                frame != 0 ? frame->rax : 0U, frame != 0 ? frame->rdi : 0U,
                frame != 0 ? frame->rip : 0U, rip, rflags, trace->published};
            for (size_t index = 0U; index < 17U; ++index)
                g_sched_inbox_stall[index] = values[index];
        }
    }
    atomic_flag_clear_explicit(&g_sched_wake_trace_guard, memory_order_release);
}

uint64_t scheduler_trace_reschedule_begin(void) {
    return g_sched_wake_trace_tid != 0U ? x86_read_tsc() : 0U;
}

void scheduler_trace_reschedule_send(uint32_t cpu_id, uint64_t started,
                                      bool accepted, uint32_t attempts) {
    /* 若探测在发送过程中启用，此操作没有有效的起始时间。 */
    if (started == 0U || g_sched_wake_trace_tid == 0U || cpu_id >= g_cpu_count) return;
    uint64_t finished = x86_read_tsc();
    if (finished < started ||
        atomic_flag_test_and_set_explicit(&g_sched_wake_trace_guard,
                                          memory_order_acquire)) return;
    uint64_t elapsed = x86_tsc_to_ns(finished-started);
    if (elapsed > g_sched_ipi_send[0]) {
        uint64_t values[7] = {elapsed, x86_current_cpu_index(), cpu_id,
                              started, finished, accepted, attempts};
        for (size_t index = 0U; index < 7U; ++index)
            g_sched_ipi_send[index] = values[index];
    }
    atomic_flag_clear_explicit(&g_sched_wake_trace_guard, memory_order_release);
}

void sched_trace_wake_stall(uint64_t now_tsc) {
    if (g_sched_wake_trace_tid == 0U) return;
    uint32_t cpu_id = x86_current_cpu_index();
    if (cpu_id >= g_cpu_count ||
        atomic_flag_test_and_set_explicit(&g_sched_wake_trace_guard,
                                          memory_order_acquire)) return;
    if (g_sched_wake_trace.tid == g_sched_wake_trace_tid &&
        g_sched_wake_trace.ready &&
        g_sched_wake_trace.target_cpu == cpu_id &&
        now_tsc >= g_sched_wake_trace.ready) {
        uint64_t elapsed = x86_tsc_to_ns(now_tsc-g_sched_wake_trace.ready);
        thread_t *current = g_cpus[cpu_id].queue.current;
        if (elapsed >= scheduler_trace_threshold_ns() && elapsed > g_sched_wake_stall[0] &&
            (current == 0 || current->tid != g_sched_wake_trace_tid)) {
            bool preempt = sched_preempt_disabled();
            g_sched_wake_stall[0] = elapsed;
            g_sched_wake_stall[1] = current != 0 ? current->tid : 0U;
            g_sched_wake_stall[2] = current != 0 && current->process != 0 ?
                                   current->process->pid : 0U;
            g_sched_wake_stall[3] = x86_syscall_active_fast();
            g_sched_wake_stall[4] = preempt;
            g_sched_wake_stall[5] = preempt ? g_sched_preempt_callers[cpu_id] : 0U;
            g_sched_wake_stall[6] = x86_tlb_shootdown_active();
            g_sched_wake_stall[7] = current != 0 ? current->sched_class : 0U;
            g_sched_wake_stall[8] = g_sched_wake_trace.epoch;
            g_sched_wake_stall[9] = cpu_id;
            g_sched_wake_stall[10] = g_sched_wake_trace_tid;
            arch_trap_frame_t *frame = g_sched_wake_stall[3] ?
                scheduler_trace_user_frame(current) : 0;
            g_sched_wake_stall[11] = frame != 0 ? frame->rax : 0U;
            g_sched_wake_stall[12] = frame != 0 ? frame->rdi : 0U;
            g_sched_wake_stall[13] = frame != 0 ? frame->rip : 0U;
        }
    }
    atomic_flag_clear_explicit(&g_sched_wake_trace_guard, memory_order_release);
}

static void scheduler_trace_notify(thread_t *thread) {
    if (g_sched_wake_trace_tid == 0U || thread->tid != g_sched_wake_trace_tid ||
        atomic_load_explicit(&thread->state, memory_order_acquire) != THREAD_BLOCKED ||
        atomic_flag_test_and_set_explicit(&g_sched_wake_trace_guard,
                                          memory_order_acquire)) return;
    uint64_t epoch = atomic_load_explicit(&thread->block_epoch, memory_order_relaxed);
    if (g_sched_wake_trace.tid != thread->tid || g_sched_wake_trace.epoch != epoch) {
        arch_trap_frame_t *frame = scheduler_trace_user_frame(thread);
        g_sched_wake_trace = (sched_wake_trace_t){
            .tid=thread->tid, .epoch=epoch, .notified=x86_read_tsc(),
            .number=frame != 0 ? frame->rax : 0U,
            .argument0=frame != 0 ? frame->rdi : 0U,
            .source_cpu=x86_current_cpu_index(), .target_cpu=thread->owner_cpu};
    }
    atomic_flag_clear_explicit(&g_sched_wake_trace_guard, memory_order_release);
}

static void scheduler_trace_publish(thread_t *thread) {
    if (g_sched_wake_trace_tid == 0U || thread->tid != g_sched_wake_trace_tid ||
        atomic_flag_test_and_set_explicit(&g_sched_wake_trace_guard,
                                          memory_order_acquire)) return;
    if (g_sched_wake_trace.tid == thread->tid &&
        g_sched_wake_trace.epoch == atomic_load_explicit(&thread->block_epoch,
                                                        memory_order_relaxed) &&
        !g_sched_wake_trace.published) {
        g_sched_wake_trace.published=x86_read_tsc();
    }
    atomic_flag_clear_explicit(&g_sched_wake_trace_guard, memory_order_release);
}

void scheduler_trace_reschedule_request(uint32_t cpu_id, bool send) {
    if (g_sched_wake_trace_tid == 0U || cpu_id >= g_cpu_count ||
        atomic_flag_test_and_set_explicit(&g_sched_wake_trace_guard,
                                          memory_order_acquire)) return;
    if (g_sched_wake_trace.tid == g_sched_wake_trace_tid &&
        g_sched_wake_trace.published && !g_sched_wake_trace.ready &&
        !g_sched_wake_trace.requested &&
        g_sched_wake_trace.source_cpu == x86_current_cpu_index() &&
        g_sched_wake_trace.source_cpu != cpu_id &&
        g_sched_wake_trace.target_cpu == cpu_id) {
        g_sched_wake_trace.requested = x86_read_tsc();
        /* 1 = 合并到已有待处理请求；2 = 已尝试发送。
         * 这并不表示硬件已完成投递，也不表示发送成功。 */
        g_sched_wake_trace.request_send = send ? 2U : 1U;
    }
    atomic_flag_clear_explicit(&g_sched_wake_trace_guard, memory_order_release);
}

void scheduler_trace_reschedule_interrupt(uint32_t cpu_id) {
    /* 除非有界浏览器探测明确选中某线程，否则禁用。只观察目标 CPU 收到的首个 IPI；
     * 后续合并或无关的 IPI 不能覆盖此前系统调用/临界区延迟的证据。 */
    if (g_sched_wake_trace_tid == 0U || cpu_id >= g_cpu_count ||
        atomic_flag_test_and_set_explicit(&g_sched_wake_trace_guard,
                                          memory_order_acquire)) return;
    if (g_sched_wake_trace.tid == g_sched_wake_trace_tid &&
        g_sched_wake_trace.notified && !g_sched_wake_trace.ready &&
        g_sched_wake_trace.source_cpu != g_sched_wake_trace.target_cpu &&
        g_sched_wake_trace.target_cpu == cpu_id &&
        !g_sched_wake_trace.interrupt) {
        thread_t *current = g_cpus[cpu_id].queue.current;
        g_sched_wake_trace.interrupt = x86_read_tsc();
        g_sched_wake_trace.interrupt_tid = current ? current->tid : 0U;
        g_sched_wake_trace.interrupt_active = x86_syscall_active_fast();
        g_sched_wake_trace.interrupt_preempt = sched_preempt_disabled();
        g_sched_wake_trace.interrupt_preempt_caller =
            g_sched_wake_trace.interrupt_preempt ? g_sched_preempt_callers[cpu_id] : 0U;
    }
    atomic_flag_clear_explicit(&g_sched_wake_trace_guard, memory_order_release);
}

static void scheduler_trace_ready(scheduler_cpu_t *cpu, thread_t *thread) {
    if (g_sched_wake_trace_tid == 0U || thread->tid != g_sched_wake_trace_tid ||
        atomic_flag_test_and_set_explicit(&g_sched_wake_trace_guard,
                                          memory_order_acquire)) return;
    if (g_sched_wake_trace.tid == thread->tid && g_sched_wake_trace.notified &&
        g_sched_wake_trace.epoch == atomic_load_explicit(&thread->block_epoch,
                                                        memory_order_relaxed)) {
        thread_t *current=cpu->queue.current;
        g_sched_wake_trace.ready=x86_read_tsc();
        g_sched_wake_trace.current_tid=current ? current->tid : 0U;
        g_sched_wake_trace.current_vruntime=current ? current->sched.vruntime : 0U;
        g_sched_wake_trace.vruntime=thread->sched.vruntime;
        g_sched_wake_trace.floor=cpu->queue.min_vruntime;
        g_sched_wake_trace.queued=cpu->queue.nr_running;
        g_sched_wake_trace.active=x86_syscall_active_fast();
    }
    atomic_flag_clear_explicit(&g_sched_wake_trace_guard, memory_order_release);
}

static void scheduler_trace_run(thread_t *thread) {
    if (g_sched_wake_trace_tid == 0U || thread->tid != g_sched_wake_trace_tid ||
        atomic_flag_test_and_set_explicit(&g_sched_wake_trace_guard,
                                          memory_order_acquire)) return;
    sched_wake_trace_t trace=g_sched_wake_trace;
    if (trace.tid == thread->tid && trace.ready &&
        trace.epoch == atomic_load_explicit(&thread->block_epoch, memory_order_relaxed)) {
        uint64_t now=x86_read_tsc();
        uint64_t total=x86_tsc_to_ns(now-trace.notified);
        if (total >= scheduler_trace_threshold_ns()) {
            for (size_t index=0;index<16;++index) {
                if (g_sched_wake_delays[index].tid &&
                    (g_sched_wake_delays[index].number != trace.number ||
                     g_sched_wake_delays[index].argument0 != trace.argument0)) continue;
                if (total > g_sched_wake_delays[index].total_ns)
                    g_sched_wake_delays[index]=(sched_wake_delay_t){
                        trace.tid, trace.number, trace.argument0, total,
                        x86_tsc_to_ns(trace.ready-trace.notified),
                        x86_tsc_to_ns(now-trace.ready), trace.current_tid,
                        trace.current_vruntime, trace.vruntime, trace.floor,
                        trace.queued, trace.active,
                        trace.published >= trace.notified ?
                            x86_tsc_to_ns(trace.published-trace.notified) : 0U,
                        trace.published && trace.ready >= trace.published ?
                            x86_tsc_to_ns(trace.ready-trace.published) : 0U,
                        trace.source_cpu, trace.target_cpu,
                        trace.interrupt && trace.published &&
                                trace.interrupt >= trace.published ?
                            x86_tsc_to_ns(trace.interrupt-trace.published) : 0U,
                        trace.interrupt && trace.ready >= trace.interrupt ?
                            x86_tsc_to_ns(trace.ready-trace.interrupt) : 0U,
                        trace.interrupt_tid, trace.interrupt_active,
                        trace.interrupt_preempt, trace.interrupt_preempt_caller,
                        trace.requested >= trace.published && trace.published ?
                            x86_tsc_to_ns(trace.requested-trace.published) : 0U,
                        trace.interrupt >= trace.requested && trace.requested ?
                            x86_tsc_to_ns(trace.interrupt-trace.requested) : 0U,
                        trace.request_send};
                break;
            }
        }
        g_sched_wake_trace=(sched_wake_trace_t){0};
    }
    atomic_flag_clear_explicit(&g_sched_wake_trace_guard, memory_order_release);
}
static bool scheduler_command_drain_local(uint32_t cpu_id,
                                         scheduler_cpu_t *cpu,
                                         bool publish_snapshot);
static void scheduler_command_release_enqueue_local(scheduler_cpu_t *cpu,
                                                     thread_t *thread);
static bool scheduler_command_ack_step(thread_t *thread,
                                       uint32_t source_limit,
                                       uint32_t ack_mask,
                                       uint32_t ack_value,
                                       bool drain_releases);

void sched_mark_initial_placement(thread_t *thread) {
    if (thread != 0) {
        thread->sched.flags |= SCHED_ENTITY_INITIAL_PLACEMENT;
    }
}

/*
 * Idle 是真实的调度目标，但它不是通过常规线程创建路径建立的，因此启动时没有已保存
 * 的上下文。预置一个合成上下文，使阻塞线程始终能切换到可中断的 idle 循环，即使
 * AP/BSP 启动循环尚未经历首次实际的 idle->thread 切换并保存上下文。
 */
static __attribute__((noreturn)) void scheduler_idle_main(void *argument) {
    (void)argument;
    for (;;) {
        /*
         * 请求可能在两次 HLT 之间短暂关闭中断的窗口内发布。休眠前先轮询，
         * 避免 IPI 丢失后 READY 线程滞留在此空闲 CPU 上。
         */
        if (x86_smp_take_reschedule_request() ||
            sched_runnable_count() != 0U) {
            (void)sched_try_run_ready();
        }
        __asm__ volatile ("sti; hlt" : : : "memory");
        sched_finish_switch();
        if (x86_smp_take_reschedule_request() ||
            sched_runnable_count() != 0U) {
            (void)sched_try_run_ready();
        }
        __asm__ volatile ("cli" : : : "memory");
    }
}

static void scheduler_init_idle_context(thread_t *idle,
                                        vaddr_t stack_top) {
    if (idle == 0 || stack_top < sizeof(uint64_t) ||
        (stack_top & 0x0FU) != 0U) return;
    x86_prepare_kernel_thread(&idle->arch, stack_top, scheduler_idle_main, 0);
}

/*
 * 运行队列中的 current 指针必须和当前真正使用的内核栈同步提交。
 * 本地中断若落在“已选择 next、尚未切栈”的窗口，会把旧栈误认为 next 的栈，
 * 所以调度全过程都保持关中断；线程以后恢复到这里时再恢复原来的 IF。
 */
static uint64_t scheduler_irq_save(void) {
    uint64_t flags;
    __asm__ volatile ("pushfq; popq %0; cli" : "=r"(flags) : : "memory");
    return flags;
}

static void scheduler_irq_restore(uint64_t flags) {
    if ((flags & (1ULL << 9)) != 0) {
        __asm__ volatile ("sti" : : : "memory");
    } else {
        __asm__ volatile ("cli" : : : "memory");
    }
}

static void scheduler_zero(void *memory, size_t size) {
    uint8_t *bytes = (uint8_t *)memory;
    while (size-- != 0) *bytes++ = 0;
}

bool scheduler_current_cpu(uint32_t *cpu_id) {
    uint32_t current = x86_current_cpu_index();
    if (cpu_id == 0 || current >= g_cpu_count || !x86_smp_cpu_online(current)) {
        return false;
    }
    *cpu_id = current;
    return true;
}

static void scheduler_spin_lock_raw(spinlock_t *lock) {
    for (;;) {
        if (atomic_exchange_explicit(&lock->state, 1U,
                                     memory_order_acquire) == 0U) {
            return;
        }

        while (atomic_load_explicit(&lock->state,
                                    memory_order_relaxed) != 0U) {
            __asm__ volatile ("pause");
        }
    }
}
uint64_t scheduler_lock(spinlock_t *lock) {
    uint64_t flags = scheduler_irq_save();
    scheduler_spin_lock_raw(lock);
    return flags;
}

/* 延迟上下文会处理回收入口，因此这些入口可以使用普通锁。 */
void scheduler_unlock(spinlock_t *lock, uint64_t flags) {
    atomic_store_explicit(&lock->state, 0U, memory_order_release);
    scheduler_irq_restore(flags);
}

/*
 * exec_start_ns 是运行时间记账基准，不是生命周期时间戳。
 * 仅在线程占用 CPU 期间保持其有效。所属 CPU 的中断边界也会将此操作与状态及
 * current-thread 转换串行化。
 */
static void scheduler_accounting_transition(run_queue_t *queue,
                                            thread_t *current, thread_t *next) {
    if (queue == 0) return;
    if (current != 0 && current != queue->idle) {
        current->sched.exec_start_ns = 0;
    }
    if (next != 0 && next != queue->idle) {
        next->sched.exec_start_ns = queue->clock_ns;
        if (next->sched_class == SCHED_CLASS_RT) {
            next->sched.slice_runtime_ns = 0U;
        }
    }
}

bool sched_accounting_self_test(void) {
    run_queue_t queue;
    thread_t idle;
    thread_t first;
    thread_t second;

    scheduler_zero(&queue, sizeof(queue));
    scheduler_zero(&idle, sizeof(idle));
    scheduler_zero(&first, sizeof(first));
    scheduler_zero(&second, sizeof(second));
    queue.idle = &idle;
    queue.clock_ns = 100U;
    idle.sched.exec_start_ns = 33U;
    first.sched.exec_start_ns = 10U;
    second.sched.exec_start_ns = 20U;

    scheduler_accounting_transition(&queue, &first, &second);
    if (first.sched.exec_start_ns != 0U || second.sched.exec_start_ns != 100U ||
        idle.sched.exec_start_ns != 33U) {
        return false;
    }

    queue.clock_ns = 200U;
    scheduler_accounting_transition(&queue, &second, &second);
    if (second.sched.exec_start_ns != 200U) return false;

    scheduler_accounting_transition(&queue, &second, &idle);
    return second.sched.exec_start_ns == 0U && idle.sched.exec_start_ns == 33U;
}

void sched_cpu_init(uint32_t cpu_id) {
    if (cpu_id >= MAX_CPUS) return;
    scheduler_cpu_t *cpu = &g_cpus[cpu_id];
    scheduler_zero(cpu, sizeof(*cpu));
    atomic_init(&cpu->reap_lock.state, 0U);
    atomic_init(&cpu->queue_snapshot, 0U);
    atomic_init(&cpu->command_pending, false);
    atomic_init(&cpu->remote_wake_head, 0);
    atomic_init(&cpu->priority_head, 0);
    atomic_init(&cpu->need_resched, false);
    atomic_init(&cpu->command_release_pending, false);
    atomic_init(&cpu->reap_deferred_queued, false);
    cpu->command_inbox =
        (sched_cmd_channel_t *)kzalloc(
            sizeof(sched_cmd_channel_t) * MAX_CPUS, 0U);
    if (cpu->command_inbox != 0) {
        for (uint32_t source = 0U; source < MAX_CPUS; ++source) {
            sched_cmd_channel_t *channel = &cpu->command_inbox[source];
            atomic_init(&channel->primary.head, 0U);
            atomic_init(&channel->primary.tail, 0U);
            atomic_init(&channel->overflow.head, 0U);
            atomic_init(&channel->overflow.tail, 0U);
            channel->overflow_active = false;
        }
    }
    cpu->queue.fair_root.root = 0;
    for (uint32_t priority = 0; priority < RT_PRIORITY_LEVELS; ++priority) {
        list_init(&cpu->queue.rt_queues[priority]);
    }
    initialize_thread(&g_idle_threads[cpu_id], (uint64_t)cpu_id, SCHED_CLASS_IDLE, 0);
    atomic_store_explicit(&g_idle_threads[cpu_id].state, THREAD_RUNNING, memory_order_relaxed);
    g_idle_threads[cpu_id].owner_cpu = (uint16_t)cpu_id;
    g_idle_threads[cpu_id].current_cpu = (uint16_t)cpu_id;
    cpu->queue.idle = &g_idle_threads[cpu_id];
    cpu->queue.current = cpu->queue.idle;
    scheduler_publish_queue_snapshot(cpu);
    cpu->idle_stack_top = x86_cpu_kernel_stack(cpu_id);
    scheduler_init_idle_context(&g_idle_threads[cpu_id], cpu->idle_stack_top);
    if (cpu_id + 1U > g_cpu_count) g_cpu_count = cpu_id + 1U;
}

void sched_init(void) {
    g_cpu_count = 0;
    g_kernel_root = x86_current_root_table();
    uint32_t discovered = x86_smp_discovered_count();
    for (uint32_t cpu_id = 0; cpu_id < discovered && cpu_id < MAX_CPUS; ++cpu_id) {
        if (x86_smp_cpu_started(cpu_id)) sched_cpu_init(cpu_id);
    }
}

bool sched_set_boot_kernel_stack(vaddr_t stack_top) {
    if (stack_top == 0 || (stack_top & 15U) != 0 || !x86_is_canonical(stack_top)) {
        return false;
    }
    /* 仅在 BSP 引导路径、关中断状态下调用，因此不需要获取运行队列锁。 */
    uint32_t cpu_id;
    if (!scheduler_current_cpu(&cpu_id)) return false;
    g_cpus[cpu_id].idle_stack_top = stack_top;
    return true;
}

static bool scheduler_command_channel_try_push(sched_cmd_channel_t *channel,
    thread_t *thread, uint32_t op, uint32_t generation, bool *overflowed);

static bool scheduler_remote_command_try_push_once(
    uint32_t source_cpu, uint32_t target_cpu, thread_t *thread,
    uint32_t op, uint32_t generation) {
    if (source_cpu >= g_cpu_count || target_cpu >= g_cpu_count ||
        source_cpu == target_cpu || thread == 0) return false;
    scheduler_cpu_t *target = &g_cpus[target_cpu];
    if (target->command_inbox == 0 || !object_try_get(thread)) return false;
    bool overflowed = false;
    bool queued = scheduler_command_channel_try_push(
        &target->command_inbox[source_cpu], thread, op, generation, &overflowed);
    if (overflowed) ++g_cpus[source_cpu].command_ring_full_count;
    if (queued) {
        atomic_store_explicit(&target->command_pending, true, memory_order_release);
        return true;
    }
    /* 此引用从未转移。延迟执行 put：调用方已屏蔽中断，而且并发的最后一个所有者
     * 可能已经退出。 */
    scheduler_command_release_enqueue_local(&g_cpus[source_cpu], thread);
    (void)x86_smp_request_reschedule(target_cpu);
    return false;
}

static bool scheduler_remote_command_try_push(
    uint32_t source_cpu,
    uint32_t target_cpu,
    thread_t *thread,
    uint32_t op,
    uint32_t generation);

static bool scheduler_current_thread_owner(
    thread_t *thread,
    uint32_t *cpu_id) {
    uint32_t current_cpu;
    if (thread == 0 || !scheduler_current_cpu(&current_cpu) ||
        !scheduler_cpu_available(thread->owner_cpu) ||
        thread->owner_cpu != current_cpu) {
        return false;
    }
    if (cpu_id != 0) *cpu_id = current_cpu;
    return true;
}

static inline bool scheduler_publish_blocked_local(
    uint32_t cpu_id,
    scheduler_cpu_t *cpu,
    thread_t *thread) {
    if (cpu == 0 || thread == 0 || thread->owner_cpu != cpu_id ||
        atomic_load_explicit(&thread->state, memory_order_relaxed) !=
            THREAD_RUNNING) {
        return false;
    }

    uint32_t epoch =
        atomic_load_explicit(&thread->block_epoch, memory_order_relaxed) + 1U;
    atomic_store_explicit(&thread->block_epoch, epoch,
                          memory_order_relaxed);
    atomic_store_explicit(&thread->state, THREAD_BLOCKED,
                          memory_order_release);
    return true;
}

static void scheduler_apply_priority_local(
    scheduler_cpu_t *cpu,
    thread_t *thread,
    uint8_t class_id,
    uint8_t rt_priority) {
    if (cpu == 0 || thread == 0 ||
        (class_id != SCHED_CLASS_RT && class_id != SCHED_CLASS_FAIR) ||
        (class_id == SCHED_CLASS_RT && rt_priority >= RT_PRIORITY_LEVELS)) {
        return;
    }

    bool enqueued = (thread->sched.flags & SCHED_ENTITY_ENQUEUED) != 0;
    if (enqueued) dequeue_local_raw(cpu, thread);
    thread->sched_class = class_id;
    thread->rt_priority = class_id == SCHED_CLASS_RT ? rt_priority : 0U;
    if (enqueued) enqueue_local_raw(cpu, thread);
}

static bool sched_enqueue_internal(thread_t *thread, bool notify_local) {
    if (thread == 0 || thread->sched_class == SCHED_CLASS_IDLE || g_cpu_count == 0) {
        return false;
    }
    uint32_t current_cpu;
    if (!scheduler_current_cpu(&current_cpu)) return false;
    uint32_t cpu_id = scheduler_cpu_available(thread->owner_cpu) ?
                      thread->owner_cpu : current_cpu;
    /* 只有具备真实内核保存栈的线程才参与跨 CPU 分配；纯结构自检固定在指定队列。 */
    /*
     * 首次放置也按全局负载选择 CPU。
     * 远端 idle CPU 的首次运行由 reschedule IPI 唤醒，并在 idle loop
     * 中消费 pending request，因此不再需要强制 creator-local。
     */
    if ((thread->sched.flags & SCHED_ENTITY_INITIAL_PLACEMENT) != 0) {
        cpu_id = scheduler_choose_cpu(thread, current_cpu);
    }
    scheduler_cpu_t *cpu = &g_cpus[cpu_id];

    if (cpu_id != current_cpu) {
        if (cpu->command_inbox == 0) return false;
        uint64_t irq_flags = scheduler_irq_save();
        uint16_t previous_owner = thread->owner_cpu;
        bool initial = (thread->sched.flags & SCHED_ENTITY_INITIAL_PLACEMENT) != 0;
        /* 在目标 CPU 消费命令前发布所有权。 */
        thread->owner_cpu = (uint16_t)cpu_id;
        thread->sched.flags &= (uint16_t)~SCHED_ENTITY_INITIAL_PLACEMENT;
        bool queued = scheduler_remote_command_try_push(
            current_cpu, cpu_id, thread, SCHED_CMD_ENQUEUE, 0U);
        if (!queued) {
            thread->owner_cpu = previous_owner;
            if (initial) thread->sched.flags |= SCHED_ENTITY_INITIAL_PLACEMENT;
        }
        scheduler_irq_restore(irq_flags);
        if (queued) {
            (void)x86_smp_request_reschedule(cpu_id);
            return true;
        }
        return false;
    }

    uint64_t queue_flags = scheduler_irq_save();
    uint32_t before = cpu->queue.nr_running;
    thread->sched.flags &= (uint16_t)~SCHED_ENTITY_INITIAL_PLACEMENT;
    thread->owner_cpu = (uint16_t)cpu_id;
    thread->current_cpu = (uint16_t)cpu_id;
    enqueue_local(cpu, thread);
    bool enqueued = cpu->queue.nr_running != before;
    bool notify = enqueued &&
                  (cpu_id != current_cpu ||
                   (notify_local && cpu->queue.current != cpu->queue.idle));
    scheduler_irq_restore(queue_flags);
    if (notify) {
        /*
         * 远端 CPU 始终需要 IPI。常规本地入队也会发送 self-IPI，使当前实际运行的线程
         * 能及时响应抢占。BSP 仍在未保存上下文的启动续执行路径中时，
         * 仅启动阶段的调用方会显式关闭该本地通知。
         */
        (void)x86_smp_request_reschedule(cpu_id);
    }
    return enqueued;
}

bool sched_enqueue(thread_t *thread) {
    return sched_enqueue_internal(thread, true);
}

bool sched_enqueue_bootstrap(thread_t *thread) {
    return sched_enqueue_internal(thread, false);
}

kstatus_t sched_start_thread(thread_t *thread) {
    if (thread == 0) return K_EINVAL;

    bool start_expected = false;
    if (!atomic_compare_exchange_strong_explicit(
            &thread->start_pending, &start_expected, true,
            memory_order_acq_rel, memory_order_acquire)) {
        return K_EBUSY;
    }

    unsigned state = atomic_load_explicit(&thread->state, memory_order_acquire);
    if (state != THREAD_NEW) {
        atomic_store_explicit(&thread->start_pending, false,
                              memory_order_release);
        return state == THREAD_READY || state == THREAD_RUNNING ?
               K_EBUSY : K_EINVAL;
    }

    uint32_t current_cpu;
    if (!scheduler_current_cpu(&current_cpu)) {
        atomic_store_explicit(&thread->start_pending, false,
                              memory_order_release);
        return K_EBUSY;
    }

    uint32_t previous_owner = thread->owner_cpu;
    /* 首次放置同时遵循亲和性和全局可运行负载快照；后续启动沿用已确定的所属 CPU。 */
    uint32_t target_cpu = scheduler_choose_start_cpu(thread, current_cpu);
    if (!scheduler_cpu_available(target_cpu)) {
        atomic_store_explicit(&thread->start_pending, false,
                              memory_order_release);
        return K_EBUSY;
    }

    if (target_cpu != current_cpu) {
        scheduler_cpu_t *target = &g_cpus[target_cpu];
        if (target->command_inbox == 0) {
            atomic_store_explicit(&thread->start_pending, false,
                                  memory_order_release);
            return K_EBUSY;
        }
        uint64_t irq_flags = scheduler_irq_save();
        thread->owner_cpu = (uint16_t)target_cpu;
        bool initial = (thread->sched.flags & SCHED_ENTITY_INITIAL_PLACEMENT) != 0U;
        thread->sched.flags &= (uint16_t)~SCHED_ENTITY_INITIAL_PLACEMENT;
        bool queued = scheduler_remote_command_try_push_once(
            current_cpu, target_cpu, thread, SCHED_CMD_START, 0U);
        if (queued) {
            /* 目标 CPU 可能已经开始运行此线程。发布所有权后不要再修改其调度标志。 */
            scheduler_irq_restore(irq_flags);
            (void)x86_smp_request_reschedule(target_cpu);
            return K_OK;
        }
        thread->owner_cpu = (uint16_t)previous_owner;
        if (initial) thread->sched.flags |= SCHED_ENTITY_INITIAL_PLACEMENT;
        atomic_store_explicit(&thread->start_pending, false,
                              memory_order_release);
        scheduler_irq_restore(irq_flags);
        return K_EBUSY;
    }

    uint64_t queue_flags = scheduler_irq_save();
    state = atomic_load_explicit(&thread->state, memory_order_relaxed);
    if (state != THREAD_NEW) {
        scheduler_irq_restore(queue_flags);
        atomic_store_explicit(&thread->start_pending, false,
                              memory_order_release);
        return state == THREAD_READY || state == THREAD_RUNNING ?
               K_EBUSY : K_EINVAL;
    }
    thread->owner_cpu = (uint16_t)current_cpu;
    thread->current_cpu = (uint16_t)current_cpu;
    thread->sched.flags &= (uint16_t)~SCHED_ENTITY_INITIAL_PLACEMENT;
    atomic_store_explicit(&thread->state, THREAD_READY, memory_order_release);
    enqueue_local(&g_cpus[current_cpu], thread);
    bool enqueued = (thread->sched.flags & SCHED_ENTITY_ENQUEUED) != 0U;
    atomic_store_explicit(&thread->start_pending, false,
                          memory_order_release);
    bool notify = enqueued && g_cpus[current_cpu].queue.current !=
                  g_cpus[current_cpu].queue.idle;
    scheduler_irq_restore(queue_flags);
    if (notify) (void)x86_smp_request_reschedule(current_cpu);
    return enqueued ? K_OK : K_EIO;
}

bool sched_publish_blocked(thread_t *thread) {
    uint32_t cpu_id;
    return scheduler_current_thread_owner(thread, &cpu_id) &&
           scheduler_publish_blocked_local(cpu_id, &g_cpus[cpu_id], thread);
}

bool sched_publish_running(thread_t *thread) {
    uint32_t cpu_id;
    if (thread == 0 || !scheduler_current_cpu(&cpu_id) ||
        !scheduler_cpu_available(thread->owner_cpu) ||
        thread->owner_cpu != cpu_id) {
        return false;
    }

    uint64_t irq_flags = scheduler_irq_save();
    scheduler_cpu_t *cpu = &g_cpus[cpu_id];
    unsigned state = atomic_load_explicit(&thread->state,
                                          memory_order_relaxed);
    bool published = false;
    /* 从 schedule() 返回即可证明此线程正在运行。在返回用户态前修复过期的
     * BLOCKED/READY 元数据。 */
    if (cpu->queue.current == thread) {
        if (state == THREAD_RUNNING) {
            published = true;
        } else if (state == THREAD_BLOCKED || state == THREAD_READY) {
            atomic_store_explicit(&thread->state, THREAD_RUNNING,
                                  memory_order_release);
            published = true;
        }
    }
    scheduler_irq_restore(irq_flags);
    return published;
}

/* 必须在所属 CPU 的状态发布中断事务内执行。仅在调用方检查会与恢复运行的线程进入
 * I/O 保护区发生竞争。已发布的栈等待者也必须在回收前完成所属方清理。 */
static bool scheduler_exit_can_publish(thread_t *thread) {
    return atomic_load_explicit(&thread->exit_defer_count,
                                memory_order_acquire) == 0U &&
           atomic_load_explicit(&thread->blocked_waiter,
                                memory_order_acquire) == 0;
}

bool sched_publish_dead(thread_t *thread) {
    if (thread == 0) return false;

    uint32_t current_cpu;
    if (!scheduler_current_cpu(&current_cpu) ||
        !scheduler_cpu_available(thread->owner_cpu)) return false;
    uint32_t owner_cpu = thread->owner_cpu;
    if (owner_cpu != current_cpu) {
        scheduler_cpu_t *owner = &g_cpus[owner_cpu];
        if (owner->command_inbox == 0) return false;
        atomic_store_explicit(&thread->command_ack, 0U,
                              memory_order_relaxed);
        uint64_t irq_flags = scheduler_irq_save();
        uint32_t sending_cpu;
        if (!scheduler_current_cpu(&sending_cpu) || sending_cpu != current_cpu) {
            scheduler_irq_restore(irq_flags);
            return false;
        }
        bool queued = scheduler_remote_command_try_push(
            current_cpu, owner_cpu, thread, SCHED_CMD_EXIT,
            SCHED_CMD_STATE_ACK);
        scheduler_irq_restore(irq_flags);
        if (!queued) return false;
        (void)x86_smp_request_reschedule(owner_cpu);
        unsigned acknowledgement;
        for (;;) {
            /* 所属 CPU 也可能正在等待我们的确认。等待期间排空本地邮箱，
             * 不要依赖系统调用退出时处理 IPI，否则会形成跨 CPU 等待环。 */
            bool acknowledged = scheduler_command_ack_step(
                thread, g_cpu_count,
                SCHED_CMD_STATE_ACK, SCHED_CMD_STATE_ACK, true);
            acknowledgement = atomic_load_explicit(&thread->command_ack,
                                                    memory_order_acquire);
            if (acknowledged) break;
            __asm__ volatile ("pause");
        }
        return (acknowledgement & SCHED_CMD_STATE_CHANGED) != 0U &&
               atomic_load_explicit(&thread->state,
                                    memory_order_acquire) == THREAD_DEAD;
    }

    uint64_t queue_flags = scheduler_irq_save();
    uint32_t locked_cpu;
    if (!scheduler_current_cpu(&locked_cpu) ||
        !scheduler_cpu_available(locked_cpu) || thread->owner_cpu != locked_cpu) {
        scheduler_irq_restore(queue_flags);
        return false;
    }
    scheduler_cpu_t *cpu = &g_cpus[locked_cpu];
    unsigned state = atomic_load_explicit(&thread->state, memory_order_relaxed);
    bool current = cpu->queue.current == thread;
    bool published = state != THREAD_DEAD && state != THREAD_BLOCKED &&
                     (state != THREAD_RUNNING || current) &&
                     scheduler_exit_can_publish(thread);
    if (published) {
        if (state == THREAD_READY && !current) dequeue_local(cpu, thread);
        atomic_store_explicit(&thread->state, THREAD_DEAD,
                              memory_order_release);
    }
    scheduler_irq_restore(queue_flags);
    return published;
}

static bool scheduler_complete_wake_local(
    scheduler_cpu_t *cpu,
    thread_t *thread,
    uint32_t block_epoch,
    bool require_epoch_match) {
    if (cpu == 0 || thread == 0) return false;

    if (require_epoch_match &&
        atomic_load_explicit(&thread->block_epoch,
                             memory_order_acquire) != block_epoch) {
        return false;
    }

    if (atomic_load_explicit(&thread->state, memory_order_relaxed) !=
        THREAD_BLOCKED) return false;
    atomic_store_explicit(&thread->state, THREAD_READY, memory_order_release);

    /*
    * 刚进入阻塞状态的当前线程尚未完成上下文切换。
    * schedule() 会观察到 READY，并将其加入同一运行队列。
     */
    if (cpu->queue.current != thread) {
        enqueue_local_raw(cpu, thread);
    }
    scheduler_trace_ready(cpu, thread);

    return true;
}

static bool scheduler_capture_block_epoch(
    thread_t *thread,
    uint32_t *block_epoch) {
    if (thread == 0 || block_epoch == 0) return false;

    /*
    * 获取一致的 {epoch,state} 快照。两次读取 epoch 可避免延迟唤醒被重定向到
    * 后续的阻塞阶段。
     */
    for (;;) {
        uint32_t before =
            atomic_load_explicit(&thread->block_epoch, memory_order_acquire);
        unsigned state =
            atomic_load_explicit(&thread->state, memory_order_acquire);
        uint32_t after =
            atomic_load_explicit(&thread->block_epoch, memory_order_acquire);

        if (before != after) continue;
        if (state != THREAD_BLOCKED) return false;

        *block_epoch = before;
        return true;
    }
}

static inline bool scheduler_command_publish_snapshot(
    atomic_uint *tail_index,
    sched_cmd_t *slots,
    uint32_t slot_mask,
    uint32_t head,
    uint32_t tail,
    thread_t *thread,
    uint32_t op,
    uint32_t generation) {
    if ((uint32_t)(tail - head) > slot_mask) return false;
    sched_cmd_t *slot = &slots[tail & slot_mask];
    slot->object = (uintptr_t)thread;
    slot->op = op;
    slot->generation = generation;
    /* 由源端独占的尾索引负责发布完整消息。获取到的头索引快照可能过期，
     * 但只会保守地将环报告为已满。 */
    atomic_store_explicit(tail_index, tail + 1U, memory_order_release);
    return true;
}

static bool scheduler_command_ring_try_push(
    sched_cmd_ring_t *ring,
    thread_t *thread,
    uint32_t op,
    uint32_t generation) {
    if (ring == 0 || thread == 0) return false;

    uint32_t tail =
        atomic_load_explicit(&ring->tail, memory_order_relaxed);
    uint32_t head =
        atomic_load_explicit(&ring->head, memory_order_acquire);

    return scheduler_command_publish_snapshot(&ring->tail, ring->slots,
        SCHED_CMD_MASK, head, tail, thread, op, generation);
}

static bool scheduler_command_overflow_try_push(
    sched_cmd_overflow_ring_t *ring,
    thread_t *thread,
    uint32_t op,
    uint32_t generation) {
    if (ring == 0 || thread == 0) return false;

    uint32_t tail =
        atomic_load_explicit(&ring->tail, memory_order_relaxed);
    uint32_t head =
        atomic_load_explicit(&ring->head, memory_order_acquire);
    return scheduler_command_publish_snapshot(&ring->tail, ring->slots,
        SCHED_CMD_OVERFLOW_MASK, head, tail, thread, op, generation);
}

static bool scheduler_command_channel_try_push(
    sched_cmd_channel_t *channel,
    thread_t *thread,
    uint32_t op,
    uint32_t generation,
    bool *overflowed) {
    if (channel == 0 || thread == 0) return false;
    if (overflowed != 0) *overflowed = false;

    if (channel->overflow_active) {
        uint32_t head =
            atomic_load_explicit(&channel->overflow.head,
                                 memory_order_acquire);
        uint32_t tail =
            atomic_load_explicit(&channel->overflow.tail,
                                 memory_order_relaxed);
        if (head != tail) {
            if (overflowed != 0) *overflowed = true;
            return scheduler_command_publish_snapshot(
                &channel->overflow.tail, channel->overflow.slots,
                SCHED_CMD_OVERFLOW_MASK, head, tail, thread, op, generation);
        }
        channel->overflow_active = false;
    }

    if (scheduler_command_ring_try_push(
            &channel->primary, thread, op, generation)) {
        return true;
    }

    channel->overflow_active = true;
    if (overflowed != 0) *overflowed = true;
    return scheduler_command_overflow_try_push(
        &channel->overflow, thread, op, generation);
}

static bool scheduler_remote_command_push(
    uint32_t source_cpu,
    uint32_t target_cpu,
    thread_t *thread,
    uint32_t op,
    uint32_t generation,
    bool hold_reference) {
    if (source_cpu >= MAX_CPUS || target_cpu >= g_cpu_count ||
        thread == 0) return false;

    scheduler_cpu_t *target = &g_cpus[target_cpu];
    if (target->command_inbox == 0) return false;

    if (hold_reference && !object_try_get(thread)) return false;

    bool counted_overflow = false;
    for (;;) {
        bool overflowed = false;
        bool queued = scheduler_command_channel_try_push(
            &target->command_inbox[source_cpu],
            thread,
            op,
            generation,
            &overflowed);
        if (overflowed && !counted_overflow) {
            ++g_cpus[source_cpu].command_ring_full_count;
            counted_overflow = true;
        }
        if (queued) {
            atomic_store_explicit(&target->command_pending, true,
                                  memory_order_release);
            return true;
        }

        /* 目标 CPU 已拥有待处理工作，并会负责排空。 */
        (void)x86_smp_request_reschedule(target_cpu);
        __asm__ volatile ("pause");
    }
}

static bool scheduler_remote_command_try_push(
    uint32_t source_cpu,
    uint32_t target_cpu,
    thread_t *thread,
    uint32_t op,
    uint32_t generation) {
    return scheduler_remote_command_push(
        source_cpu, target_cpu, thread, op, generation, true);
}

static bool scheduler_remote_command_try_push_unowned(
    uint32_t source_cpu,
    uint32_t target_cpu,
    thread_t *thread,
    uint32_t op,
    uint32_t generation) {
    return scheduler_remote_command_push(
        source_cpu, target_cpu, thread, op, generation, false);
}

static bool scheduler_transfer_local(
    uint32_t source_cpu,
    scheduler_cpu_t *source,
    thread_t *thread,
    uint32_t target_cpu) {
    if (source == 0 || thread == 0 || target_cpu >= g_cpu_count ||
        target_cpu == source_cpu) {
        if (source != 0 && thread != 0 && target_cpu == source_cpu) {
            enqueue_local_raw(source, thread);
            return true;
        }
        return false;
    }

    scheduler_cpu_t *target = &g_cpus[target_cpu];
    if (target->command_inbox == 0) return false;
    uint64_t previous_vruntime = thread->sched.vruntime;
    bool was_relative = (thread->sched.flags & SCHED_ENTITY_VRUNTIME_RELATIVE) != 0U;
    scheduler_prepare_fair_migration(&source->queue, thread);
    thread->owner_cpu = (uint16_t)target_cpu;
    if (!scheduler_remote_command_try_push_once(
            source_cpu, target_cpu, thread, SCHED_CMD_ENQUEUE, 0U)) {
        thread->owner_cpu = (uint16_t)source_cpu;
        thread->sched.vruntime = previous_vruntime;
        if (!was_relative)
            thread->sched.flags &= (uint16_t)~SCHED_ENTITY_VRUNTIME_RELATIVE;
        return false;
    }
    (void)x86_smp_request_reschedule(target_cpu);
    return true;
}

static void scheduler_migrate_local(
    uint32_t source_cpu,
    scheduler_cpu_t *source,
    thread_t *thread,
    uint32_t target_cpu) {
    if (source == 0 || thread == 0 || source_cpu >= g_cpu_count ||
        target_cpu >= g_cpu_count || !scheduler_cpu_available(target_cpu) ||
        !scheduler_affinity_allows(thread, target_cpu) ||
        thread->owner_cpu != source_cpu) {
        return;
    }

    unsigned state = atomic_load_explicit(&thread->state, memory_order_acquire);
    if (state == THREAD_DEAD) return;
    if (target_cpu == source_cpu) {
        thread->migration_pending = false;
        return;
    }

    thread->migration_target_cpu = (uint16_t)target_cpu;
    if (source->queue.current == thread || state == THREAD_RUNNING) {
        thread->migration_pending = true;
        return;
    }

    if ((thread->sched.flags & SCHED_ENTITY_ENQUEUED) != 0) {
        dequeue_local_raw(source, thread);
    }
    thread->migration_pending = false;
    if (state == THREAD_READY) {
        if (!scheduler_transfer_local(source_cpu, source, thread, target_cpu)) {
            thread->owner_cpu = (uint16_t)source_cpu;
            thread->migration_pending = true;
            enqueue_local_raw(source, thread);
            atomic_store_explicit(&source->need_resched, true, memory_order_release);
        }
    } else if (g_cpus[target_cpu].command_inbox != 0) {
        /* 阻塞线程可能在唤醒前再次迁移。保留其相对负载，直到最终目标 CPU 将其插入队列。 */
        scheduler_prepare_fair_migration(&source->queue, thread);
        thread->owner_cpu = (uint16_t)target_cpu;
    }
}

static bool scheduler_stage_handoff(scheduler_cpu_t *cpu, thread_t *thread) {
    thread_t *next = pick_next_local(cpu);
    if (thread == 0 || thread != cpu->queue.current ||
        !thread->migration_pending || thread->arch.switch_ctx.rsp == 0U ||
        atomic_load_explicit(&thread->state, memory_order_relaxed) != THREAD_RUNNING ||
        next == 0 || next == thread || next->arch.switch_ctx.rsp == 0U ||
        cpu->switch_handoff != 0) return false;
    atomic_store_explicit(&thread->state, THREAD_READY, memory_order_relaxed);
    thread->migration_pending = false;
    scheduler_prepare_fair_migration(&cpu->queue, thread);
    cpu->switch_handoff = thread;
    return true;
}

/* 在新栈上由上下文切换汇编调用，此时中断已屏蔽。
 * 发布远端入队后不得再访问旧栈。 */
void scheduler_finish_handoff(void) {
    uint32_t cpu_id;
    if (!scheduler_current_cpu(&cpu_id)) return;
    scheduler_cpu_t *cpu = &g_cpus[cpu_id];
    thread_t *thread = cpu->switch_handoff;
    if (thread == 0) return;
    cpu->switch_handoff = 0;
    uint32_t target = thread->migration_target_cpu;
    if (!scheduler_transfer_local(cpu_id, cpu, thread, target)) {
        /* 发布失败时同时保留可运行线程和迁移意图。其已保存的续执行状态现在可在
         * 此 CPU 上安全运行。 */
        thread->migration_pending = true;
        enqueue_local_raw(cpu, thread);
        atomic_store_explicit(&cpu->need_resched, true, memory_order_release);
    }
    if (cpu->queue_snapshot_dirty) scheduler_publish_queue_snapshot(cpu);
}

static inline void scheduler_thread_mailbox_lock(thread_t *thread) {
    while (atomic_exchange_explicit(&thread->remote_wake_lock, 1U,
                                    memory_order_acquire)) {
        while (atomic_load_explicit(&thread->remote_wake_lock,
                                    memory_order_relaxed)) {
            __asm__ volatile ("pause");
        }
    }
}

static inline void scheduler_thread_mailbox_unlock(thread_t *thread) {
    atomic_store_explicit(&thread->remote_wake_lock, 0U, memory_order_release);
}

static inline void scheduler_command_release_enqueue_locked(
    scheduler_cpu_t *cpu,
    thread_t *thread) {
    /* 调用方持有线程邮箱锁，并通过关闭本地中断串行化 CPU FIFO。
     * 仅发布从空到非空的状态转换。 */
    if (thread->command_release_count != 0U) {
        ++thread->command_release_count;
        return;
    }
    bool was_empty = cpu->command_release_head == 0;
    thread->command_release_next = 0;
    if (cpu->command_release_tail != 0) {
        cpu->command_release_tail->command_release_next = thread;
    } else {
        cpu->command_release_head = thread;
    }
    cpu->command_release_tail = thread;
    thread->command_release_count = 1U;
    if (was_empty) {
        atomic_store_explicit(&cpu->command_release_pending, true,
                              memory_order_release);
    }
}

static void scheduler_command_release_enqueue_local(
    scheduler_cpu_t *cpu,
    thread_t *thread) {
    scheduler_thread_mailbox_lock(thread);
    scheduler_command_release_enqueue_locked(cpu, thread);
    scheduler_thread_mailbox_unlock(thread);
}

static void scheduler_command_release_background(void *argument) {
    object_put(argument);
}

static void scheduler_command_release_drain(uint32_t cpu_id) {
    if (cpu_id >= g_cpu_count) return;
    scheduler_cpu_t *cpu = &g_cpus[cpu_id];
    if (SCHED_UNLIKELY(!atomic_load_explicit(
            &cpu->command_release_pending, memory_order_acquire))) {
        return;
    }

    for (;;) {
        uint64_t queue_flags = scheduler_irq_save();
        uint32_t local_cpu;
        /* 弹出操作由中断串行化且严格在本地执行；销毁操作则不是。
         * 最后一个命令引用可能释放 VM 并执行 TLB shootdown。
         * 在所属 CPU 上由未加锁、可中断的续执行路径完成这些工作前，
         * 保持 FIFO 和待处理位不变。每次弹出都重新检查所有权：object_put 可能阻塞或迁移。 */
        if ((queue_flags & (1ULL << 9)) == 0U || sched_preempt_disabled() ||
            x86_tlb_shootdown_active() ||
            !scheduler_current_cpu(&local_cpu) || local_cpu != cpu_id) {
            scheduler_irq_restore(queue_flags);
            return;
        }
        thread_t *thread = cpu->command_release_head;
        uint32_t count = 0U;
        if (thread != 0) {
            scheduler_thread_mailbox_lock(thread);
            cpu->command_release_head = thread->command_release_next;
            if (cpu->command_release_head == 0) {
                cpu->command_release_tail = 0;
            }
            thread->command_release_next = 0;
            count = thread->command_release_count;
            thread->command_release_count = 0U;
            scheduler_thread_mailbox_unlock(thread);
        }
        if (cpu->command_release_head == 0) {
            atomic_store_explicit(&cpu->command_release_pending, false,
                                  memory_order_release);
        }
        scheduler_irq_restore(queue_flags);

        if (thread == 0) return;
        if (count == 0U) continue;
        if (!deferred_background_worker_started()) {
            while (count-- != 0U) object_put(thread);
            continue;
        }
        /* 将此分离批次的一个自有引用保留到最后。即使其他 CPU 上的所有者并发释放
         * 其余所有引用，之前的 put 也不会销毁对象。无需分配内存，也无需为每个引用
         * 单独创建工作项。 */
        while (count > 1U) {
            object_put(thread);
            --count;
        }
        if (!deferred_background_schedule(scheduler_command_release_background,
                                          thread)) {
            /* 队列已满：将最后一个引用留在当前 CPU 的 FIFO 中。两次 put 之间发生抢占
             * 可能已迁移当前调用方。绝不让输入路径承担开销大的最终销毁操作。 */
            queue_flags = scheduler_irq_save();
            uint32_t retry_cpu = x86_current_cpu_index();
            scheduler_command_release_enqueue_local(&g_cpus[retry_cpu], thread);
            scheduler_irq_restore(queue_flags);
            return;
        }
    }
}

static bool scheduler_command_ring_try_pop(
    sched_cmd_ring_t *ring,
    sched_cmd_t *message) {
    if (ring == 0 || message == 0) return false;

    uint32_t head =
        atomic_load_explicit(&ring->head, memory_order_relaxed);
    uint32_t tail =
        atomic_load_explicit(&ring->tail, memory_order_acquire);
    if (head == tail) return false;

    *message = ring->slots[head & SCHED_CMD_MASK];
    atomic_store_explicit(&ring->head, head + 1U, memory_order_release);
    return true;
}

static void scheduler_command_consume_local(
    uint32_t cpu_id,
    scheduler_cpu_t *cpu,
    sched_cmd_t message,
    bool holds_reference) {
    thread_t *thread = (thread_t *)message.object;
    if (thread == 0) return;

    if (thread->owner_cpu == cpu_id) {
        if (message.op == SCHED_CMD_WAKE) {
            (void)scheduler_complete_wake_local(
                cpu, thread, message.generation, true);
        } else if (message.op == SCHED_CMD_START) {
            if (atomic_load_explicit(&thread->state,
                                     memory_order_relaxed) == THREAD_NEW) {
                thread->sched.flags &= (uint16_t)~SCHED_ENTITY_INITIAL_PLACEMENT;
                atomic_store_explicit(&thread->state, THREAD_READY,
                                      memory_order_release);
                enqueue_local_raw(cpu, thread);
            }
            atomic_store_explicit(&thread->start_pending, false,
                                  memory_order_release);
        } else if (message.op == SCHED_CMD_ENQUEUE) {
            enqueue_local_raw(cpu, thread);
        } else if (message.op == SCHED_CMD_SET_PRIORITY) {
            scheduler_apply_priority_local(
                cpu,
                thread,
                (uint8_t)(message.generation >> SCHED_CMD_CLASS_SHIFT),
                (uint8_t)message.generation);
        } else if (message.op == SCHED_CMD_SET_CLASS) {
            scheduler_apply_priority_local(
                cpu, thread, (uint8_t)message.generation,
                thread->rt_priority);
        } else if (message.op == SCHED_CMD_REMOVE ||
                   message.op == SCHED_CMD_STOP ||
                   message.op == SCHED_CMD_EXIT) {
            if (message.op == SCHED_CMD_EXIT) {
                unsigned state = atomic_load_explicit(&thread->state,
                                                      memory_order_relaxed);
                bool state_changed = false;
                if ((state == THREAD_NEW || state == THREAD_READY) &&
                    scheduler_exit_can_publish(thread)) {
                    if (cpu->queue.current != thread)
                        dequeue_local_raw(cpu, thread);
                    atomic_store_explicit(&thread->state, THREAD_DEAD,
                                          memory_order_release);
                    state_changed = true;
                }
                if ((message.generation & SCHED_CMD_STATE_ACK) != 0U) {
                    atomic_store_explicit(&thread->command_ack,
                                          SCHED_CMD_STATE_ACK |
                                              (state_changed ?
                                               SCHED_CMD_STATE_CHANGED : 0U),
                                          memory_order_release);
                }
            } else if (cpu->queue.current != thread) {
                dequeue_local_raw(cpu, thread);
            }
            if (message.op == SCHED_CMD_REMOVE &&
                (message.generation & SCHED_CMD_REMOVE_ACK) != 0U) {
                atomic_store_explicit(&thread->command_ack,
                                      message.generation,
                                      memory_order_release);
            }
        } else if (message.op == SCHED_CMD_MIGRATE) {
            scheduler_migrate_local(
                cpu_id, cpu, thread, message.generation);
        }
    } else if (message.op == SCHED_CMD_START) {
        /* 过期的启动请求不能让后续调用方一直卡在门控处。 */
        atomic_store_explicit(&thread->start_pending, false,
                              memory_order_release);
    } else if (message.op == SCHED_CMD_EXIT &&
               (message.generation & SCHED_CMD_STATE_ACK) != 0U) {
        /* 路由完成后目标已迁移。应通过确认拒绝请求，而不是让发送方无限等待；
         * 绝不触碰新所属 CPU 的队列。 */
        atomic_store_explicit(&thread->command_ack, SCHED_CMD_STATE_ACK,
                              memory_order_release);
    }

    if (holds_reference) {
        scheduler_command_release_enqueue_local(cpu, thread);
    }
}

static inline void scheduler_command_drain_ring_local(
    uint32_t cpu_id,
    scheduler_cpu_t *cpu,
    atomic_uint *head_index,
    atomic_uint *tail_index,
    const sched_cmd_t *slots,
    uint32_t slot_mask) {
    uint32_t head =
        atomic_load_explicit(head_index, memory_order_relaxed);
    uint32_t tail =
        atomic_load_explicit(tail_index, memory_order_acquire);
    uint32_t consumed = head;

    /* 仅消费已捕获的批次。所有回调完成后才释放槽位，以维持生产者/消费者所有权
     * 和回绕协议。 */
    while (consumed != tail) {
        sched_cmd_t message = slots[consumed & slot_mask];
        ++consumed;
        bool holds_reference =
            message.op != SCHED_CMD_REMOVE ||
            (message.generation & SCHED_CMD_REMOVE_NO_REF) == 0U;
        scheduler_command_consume_local(
            cpu_id, cpu, message, holds_reference);
    }
    if (consumed != head) {
        atomic_store_explicit(head_index, consumed, memory_order_release);
    }
}

static void scheduler_command_drain_primary_local(
    uint32_t cpu_id,
    scheduler_cpu_t *cpu,
    sched_cmd_ring_t *ring) {
    scheduler_command_drain_ring_local(cpu_id, cpu, &ring->head, &ring->tail,
                                       ring->slots, SCHED_CMD_MASK);
}

static void scheduler_command_drain_overflow_local(
    uint32_t cpu_id,
    scheduler_cpu_t *cpu,
    sched_cmd_overflow_ring_t *ring) {
    scheduler_command_drain_ring_local(cpu_id, cpu, &ring->head, &ring->tail,
                                       ring->slots, SCHED_CMD_OVERFLOW_MASK);
}

static bool scheduler_remote_wake_publish(scheduler_cpu_t *cpu,
                                           thread_t *thread, uint32_t epoch) {
    if (!object_try_get(thread)) return false;
    scheduler_thread_mailbox_lock(thread);
    if (atomic_load_explicit(&thread->block_epoch, memory_order_acquire) != epoch ||
        atomic_load_explicit(&thread->state, memory_order_acquire) != THREAD_BLOCKED) {
        scheduler_thread_mailbox_unlock(thread);
        object_put(thread);
        return false;
    }
    thread->remote_wake_epoch = epoch;
    if (atomic_load_explicit(&thread->remote_wake_queued, memory_order_relaxed)) {
        scheduler_thread_mailbox_unlock(thread);
        object_put(thread);
        return true;
    }
    atomic_store_explicit(&thread->remote_wake_queued, true, memory_order_relaxed);
    thread_t *head = atomic_load_explicit(&cpu->remote_wake_head,
                                          memory_order_acquire);
    do {
        thread->remote_wake_next = head;
    } while (!atomic_compare_exchange_weak_explicit(
        &cpu->remote_wake_head, &head, thread,
        memory_order_release, memory_order_acquire));
    scheduler_thread_mailbox_unlock(thread);
    atomic_store_explicit(&cpu->command_pending, true, memory_order_release);
    return true;
}

static void scheduler_remote_wake_drain(uint32_t cpu_id, scheduler_cpu_t *cpu) {
    thread_t *thread = atomic_exchange_explicit(&cpu->remote_wake_head, 0,
                                                memory_order_acquire);
    while (thread != 0) {
        scheduler_thread_mailbox_lock(thread);
        /* 先获取快照并完成处理，再释放节点供后续复用。 */
        thread_t *next = thread->remote_wake_next;
        uint32_t epoch = thread->remote_wake_epoch;
        if (thread->owner_cpu == cpu_id) {
            (void)scheduler_complete_wake_local(cpu, thread, epoch, true);
        }
        atomic_store_explicit(&thread->remote_wake_queued, false,
                              memory_order_relaxed);
        /* 释放现有锁前先转移邮箱引用；此处无需再次获取锁，也无需销毁对象。 */
        scheduler_command_release_enqueue_locked(cpu, thread);
        scheduler_thread_mailbox_unlock(thread);
        thread = next;
    }
}

/* 不等待远端腾出槽位，也不分配内存。每个线程至多占一个节点及一个引用。
 * 调用方持有线程邮箱锁；迁移转发时沿用已有引用。 */
static void scheduler_priority_link(scheduler_cpu_t *cpu, thread_t *thread) {
    thread_t *head = atomic_load_explicit(&cpu->priority_head, memory_order_acquire);
    do {
        thread->priority_next = head;
    } while (!atomic_compare_exchange_weak_explicit(
        &cpu->priority_head, &head, thread, memory_order_release, memory_order_acquire));
    atomic_store_explicit(&cpu->command_pending, true, memory_order_release);
}

static void scheduler_priority_drain(uint32_t cpu_id, scheduler_cpu_t *cpu) {
    thread_t *thread = atomic_exchange_explicit(&cpu->priority_head, 0,
                                                memory_order_acquire);
    while (thread != 0) {
        scheduler_thread_mailbox_lock(thread);
        thread_t *next = thread->priority_next;
        uint32_t target = thread->owner_cpu;
        unsigned state = atomic_load_explicit(&thread->state, memory_order_acquire);
        if (state != THREAD_DEAD && target != cpu_id && scheduler_cpu_available(target)) {
            /* 线程已迁移；不能修改旧 CPU 的运行队列，也不能丢弃最新更新。 */
            scheduler_priority_link(&g_cpus[target], thread);
            scheduler_thread_mailbox_unlock(thread);
            (void)x86_smp_request_reschedule(target);
        } else {
            if (state != THREAD_DEAD && target == cpu_id) {
                uint32_t payload = atomic_load_explicit(&thread->priority_request,
                                                         memory_order_relaxed);
                scheduler_apply_priority_local(cpu, thread,
                    (uint8_t)(payload >> SCHED_CMD_CLASS_SHIFT), (uint8_t)payload);
            }
            thread->priority_next = 0;
            atomic_store_explicit(&thread->priority_queued, false, memory_order_release);
            scheduler_command_release_enqueue_locked(cpu, thread);
            scheduler_thread_mailbox_unlock(thread);
        }
        thread = next;
    }
}

static bool scheduler_command_drain_local_limit(
    uint32_t cpu_id,
    scheduler_cpu_t *cpu,
    uint32_t source_limit,
    bool publish_snapshot) {
    if (cpu == 0 || cpu->command_inbox == 0 ||
        SCHED_UNLIKELY(!atomic_exchange_explicit(
            &cpu->command_pending, false, memory_order_acquire))) {
        return false;
    }

    scheduler_remote_wake_drain(cpu_id, cpu);
    scheduler_priority_drain(cpu_id, cpu);
    if (source_limit > MAX_CPUS) source_limit = MAX_CPUS;
    for (uint32_t source = 0U; source < source_limit; ++source) {
        if (source == cpu_id) continue;
        sched_cmd_channel_t *channel = &cpu->command_inbox[source];
        scheduler_command_drain_primary_local(
            cpu_id, cpu, &channel->primary);
        scheduler_command_drain_overflow_local(
            cpu_id, cpu, &channel->overflow);
    }
    if (publish_snapshot && cpu->queue_snapshot_dirty) {
        scheduler_publish_queue_snapshot(cpu);
    }
    return true;
}

static bool scheduler_should_preempt_local(const scheduler_cpu_t *cpu) {
    if (cpu == 0) return false;
    thread_t *current = cpu->queue.current;
    if (current == 0 || current == cpu->queue.idle) {
        if (cpu->queue.nr_running == 0U) return false;
        thread_t *next = pick_next_local((scheduler_cpu_t *)cpu);
        return next != 0 && next != cpu->queue.idle &&
               next->arch.switch_ctx.rsp != 0U;
    }
    unsigned state = atomic_load_explicit(&current->state, memory_order_relaxed);
    if (state != THREAD_RUNNING || current->migration_pending) return true;
    if (atomic_load_explicit(&cpu->need_resched, memory_order_relaxed)) {
        return true;
    }
    if (cpu->queue.rt_bitmap == 0U) return false;

    thread_t *next = pick_next_local((scheduler_cpu_t *)cpu);
    if (current->sched_class != SCHED_CLASS_RT) return true;
    if (next == 0 || next->sched_class != SCHED_CLASS_RT) return false;
    return next->rt_priority < current->rt_priority ||
           (next->rt_priority == current->rt_priority &&
            current->sched.slice_runtime_ns >= SCHED_RT_TIMESLICE_NS);
}

static void scheduler_schedule_internal(bool commands_already_drained);

static bool scheduler_local_wake_requires_preempt(
    const scheduler_cpu_t *cpu,
    const thread_t *thread) {
    if (cpu == 0 || thread == 0) return false;

    const thread_t *current = cpu->queue.current;
    if (current == 0 || current == cpu->queue.idle) return true;
    unsigned current_state =
        atomic_load_explicit(&current->state, memory_order_relaxed);
    if (current == thread || current_state != THREAD_RUNNING) return true;
    /* 本地 FAIR 唤醒不会发送重新调度 IPI。与时钟处理竞争中的 FAIR 任务相同，
     * 在下一个安全边界请求重新选择。schedule() 会在选择前记入当前系统调用尾段，
     * 此处不要比较尚未记账的当前 vruntime，也不要抢占 RT 任务。 */
    if (thread->sched_class == SCHED_CLASS_FAIR)
        return current->sched_class == SCHED_CLASS_FAIR;
    if (thread->sched_class != SCHED_CLASS_RT) return false;
    if (current->sched_class != SCHED_CLASS_RT) return true;
    return thread->rt_priority < current->rt_priority ||
           (thread->rt_priority == current->rt_priority &&
            current->sched.slice_runtime_ns >= SCHED_RT_TIMESLICE_NS);
}

void sched_handle_reschedule_request(void) {
    uint64_t irq_flags = scheduler_irq_save();
    uint32_t cpu_id;
    if (!scheduler_current_cpu(&cpu_id)) {
        scheduler_irq_restore(irq_flags);
        return;
    }
    scheduler_cpu_t *cpu = &g_cpus[cpu_id];
    scheduler_command_drain_local(cpu_id, cpu, false);
    bool should_preempt = scheduler_should_preempt_local(cpu);
    if (!should_preempt && cpu->queue_snapshot_dirty) {
        scheduler_publish_queue_snapshot(cpu);
    }
    scheduler_irq_restore(irq_flags);
    if (should_preempt) scheduler_schedule_internal(true);
    else sched_finish_switch();
}

void sched_wake(thread_t *thread) {
    if (thread == 0 || g_cpu_count == 0) return;
    scheduler_trace_notify(thread);

    uint32_t caller_cpu = x86_current_cpu_index();
    uint32_t cpu_id = scheduler_cpu_available(thread->owner_cpu) ?
                      thread->owner_cpu : caller_cpu;

    if (!scheduler_cpu_available(cpu_id)) return;

    scheduler_cpu_t *cpu = &g_cpus[cpu_id];

    /*
    * 远端阻塞唤醒：常见路径不获取目标运行队列锁。
     *
    * 仅在访问源端独占的生产者索引时关闭本地中断，避免同一物理 CPU 从中断上下文
    * 重入此 SPSC 生产者。
     */
    if (cpu_id != caller_cpu) {
        if (caller_cpu >= g_cpu_count || caller_cpu >= MAX_CPUS ||
            cpu->command_inbox == 0) return;
        uint32_t block_epoch;
        if (!scheduler_capture_block_epoch(thread, &block_epoch)) {
            return;
        }

        uint64_t irq_flags = scheduler_irq_save();
        bool queued = scheduler_remote_wake_publish(cpu, thread, block_epoch);
        scheduler_irq_restore(irq_flags);

        if (queued) {
            scheduler_trace_publish(thread);
            (void)x86_smp_request_reschedule(cpu_id);
        }
        return;
    }

    uint64_t queue_flags = scheduler_irq_save();
    scheduler_trace_publish(thread);
    bool woke = scheduler_complete_wake_local(
        cpu, thread, 0U, false);
    if (woke && cpu->queue_snapshot_dirty) {
        scheduler_publish_queue_snapshot(cpu);
    }
    bool need_preempt = woke &&
                        scheduler_local_wake_requires_preempt(cpu, thread);
    if (need_preempt) {
        atomic_store_explicit(&cpu->need_resched, true,
                              memory_order_release);
    }
    scheduler_irq_restore(queue_flags);
}

static void scheduler_reap_enqueue_local(scheduler_cpu_t *cpu,
                                           thread_t *thread) {
    if (cpu == 0 || thread == 0) return;

    uint32_t flags =
        __atomic_load_n(&thread->flags, __ATOMIC_ACQUIRE);
    uint16_t sched_flags = thread->sched.flags;

    /*
     * 没有 execution ref 的线程不需要 deferred reap。
     * QUEUED 防止异常的重复 schedule 把同一线程加入两次。
     */
    if ((flags & THREAD_FLAG_EXECUTION_REF) == 0U ||
        (sched_flags & SCHED_ENTITY_REAP_QUEUED) != 0U) {
        return;
    }

    thread->sched.flags |= SCHED_ENTITY_REAP_QUEUED;

    thread->reap_next = 0;

    if (cpu->reap_tail != 0) {
        cpu->reap_tail->reap_next = thread;
    } else {
        cpu->reap_head = thread;
    }

    cpu->reap_tail = thread;
}

static void scheduler_schedule_internal(bool commands_already_drained) {
    uint64_t irq_flags = scheduler_irq_save();
    if (sched_preempt_disabled()) {
        scheduler_irq_restore(irq_flags);
        return;
    }
    uint32_t cpu_id;
    if (!scheduler_current_cpu(&cpu_id)) {
        scheduler_irq_restore(irq_flags);
        return;
    }
    scheduler_cpu_t *cpu = &g_cpus[cpu_id];
    atomic_store_explicit(&cpu->need_resched, false, memory_order_relaxed);
    if (!commands_already_drained ||
        atomic_load_explicit(&cpu->command_pending, memory_order_acquire)) {
        scheduler_command_drain_local(cpu_id, cpu, false);
    }
    thread_t *current = cpu->queue.current;
    bool current_transferred = false;
    /* 入队或迁移当前线程前，先记入上次时钟节拍之后的执行时间。
     * 短暂的阻塞/让出 CPU 不能成为逃避记账的途径。 */
    (void)scheduler_account_current(&cpu->queue,
                                    x86_tsc_to_ns(x86_read_tsc()));
    unsigned current_state = THREAD_DEAD;
    if (current != 0 && current != cpu->queue.idle) {
        current_state = atomic_load_explicit(&current->state,
                                             memory_order_relaxed);
        if (current_state == THREAD_RUNNING &&
            !current->migration_pending &&
            (current->sched.flags & SCHED_ENTITY_ENQUEUED) == 0U &&
            (cpu->queue.nr_running == 0U ||
             scheduler_fair_current_wins(&cpu->queue))) {
            if (cpu->queue_snapshot_dirty) {
                scheduler_publish_queue_snapshot(cpu);
            }
            scheduler_irq_restore(irq_flags);
            sched_finish_switch();
            return;
        }
        if (current_state == THREAD_RUNNING &&
            SCHED_UNLIKELY(current->migration_pending)) {
            uint32_t target_cpu = current->migration_target_cpu;
            if (target_cpu == cpu_id ||
                !scheduler_cpu_available(target_cpu) ||
                !scheduler_affinity_allows(current, target_cpu)) {
                current->migration_pending = false;
            } else {
                current_transferred = scheduler_stage_handoff(cpu, current);
            }
        }
        if (!current_transferred &&
            (current_state == THREAD_RUNNING || current_state == THREAD_READY)) {
            if (current_state == THREAD_RUNNING) {
                atomic_store_explicit(&current->state, THREAD_READY,
                                      memory_order_relaxed);
            }
            enqueue_local_raw(cpu, current);
        }
    }
    thread_t *next = pick_next_local(cpu);
    if (next != 0) {
        scheduler_trace_run(next);
        dequeue_local_raw(cpu, next);
        atomic_store_explicit(&next->state, THREAD_RUNNING, memory_order_relaxed);
        next->owner_cpu = (uint16_t)cpu_id;
        next->current_cpu = (uint16_t)cpu_id;
        cpu->queue.current = next;
    }
    scheduler_publish_queue_snapshot(cpu);
    bool same_current = next == current && current_state == THREAD_RUNNING &&
                        !current_transferred;
    if (!same_current) {
        scheduler_accounting_transition(&cpu->queue, current, next);
    }
    /*
     * DEAD current 的 execution ref 只能在真正离开它的栈/CR3 后释放。
     *
     * 使用 FIFO，不能因为前一个待回收线程尚未处理就丢掉当前线程。
     */
    if (current != 0 &&
        current != cpu->queue.idle &&
        atomic_load_explicit(&current->state,
                             memory_order_relaxed) == THREAD_DEAD &&
        next != 0 &&
        next != current &&
        current->arch.switch_ctx.rsp != 0 &&
        next->arch.switch_ctx.rsp != 0) {

        scheduler_reap_enqueue_local(cpu, current);
    }
    if (same_current) {
        scheduler_irq_restore(irq_flags);
        sched_finish_switch();
        return;
    }

    /*
     * 只有具备真实保存栈的线程才触发架构切换。这样早期纯数据结构自检仍可使用
     * 临时 thread_t，而正式线程会同步 CR3、TSS.RSP0、FS.base 和系统调用栈。
     */
    if (next != 0 && next != current && current != 0 &&
        current->arch.switch_ctx.rsp != 0 &&
        next->arch.switch_ctx.rsp != 0) {
        x86_cpu_local_t *local = x86_cpu_local_current();
        if (local != 0) {
            /*
             * 系统调用可能显式阻塞并调用 schedule()。在线程上保留此信息，
             * 避免此 CPU 上的下一个线程继承调用方仅适用于系统调用的中断策略。
             */
            current->arch.syscall_active = local->SyscallActive;
            local->SyscallActive = next == cpu->queue.idle ? 0U :
                                   next->arch.syscall_active;
        }
        paddr_t root = g_kernel_root;
        vaddr_t kernel_stack = cpu->idle_stack_top;
        if (next->process != 0 && next->process->vm != 0) {
            root = next->process->vm->root_table;
            kernel_stack = next->kernel_stack_top;
            /* vmalloc 等晚期内核映射在切换前同步到目标 PML4。 */
            x86_sync_kernel_half(root, g_kernel_root);
        }
        x86_tss_set_rsp0(kernel_stack);
        x86_syscall_set_kernel_stack(kernel_stack);
        x86_set_user_fs_base(next->arch.fs_base);
        x86_fp_switch(&current->arch, &next->arch);
        paddr_t cr3 = paddr_make(x86_cr3_value(
            root, next->process != 0 && next->process->vm != 0 ?
                next->process->vm->pcid : 0U));
        x86_switch_context_root(&current->arch.switch_ctx, &next->arch.switch_ctx,
                                cr3, current_transferred);
    }
    scheduler_irq_restore(irq_flags);
    /* 回收可能发起跨 CPU TLB shootdown，必须在 IF=1 时执行。 */
    sched_finish_switch();
}

bool sched_try_run_ready(void) {
    uint64_t irq_flags = scheduler_irq_save();
    uint32_t cpu_id;
    if (!scheduler_current_cpu(&cpu_id)) {
        scheduler_irq_restore(irq_flags);
        return false;
    }
    scheduler_cpu_t *cpu = &g_cpus[cpu_id];
    scheduler_command_drain_local(cpu_id, cpu, false);
    thread_t *next = pick_next_local(cpu);
    /* 丢弃没有真实上下文的早期自检线程，避免空闲 CPU 被假线程锁死。 */
    while (next != 0 && next != cpu->queue.idle &&
           next->arch.switch_ctx.rsp == 0) {
        dequeue_local_raw(cpu, next);
        atomic_store_explicit(&next->state, THREAD_READY, memory_order_release);
        next = pick_next_local(cpu);
    }
    bool runnable = next != 0 && next != cpu->queue.idle;
    if (!runnable && cpu->queue_snapshot_dirty) {
        scheduler_publish_queue_snapshot(cpu);
    }
    scheduler_irq_restore(irq_flags);
    if (runnable) scheduler_schedule_internal(true);
    else sched_finish_switch();
    return runnable;
}

void schedule(void) {
    scheduler_schedule_internal(false);
}

void sched_preempt_disable(void) {
    /* GS 是 CPU 私有的：一次本地读改写即可，无需 locked atomic。 */
    if (g_sched_wake_trace_tid != 0U && !x86_preempt_disabled_fast()) {
        uint32_t cpu_id = x86_current_cpu_index();
        if (cpu_id < MAX_CPUS)
            g_sched_preempt_callers[cpu_id] =
                (uint64_t)(uintptr_t)__builtin_return_address(0);
    }
    x86_preempt_disable_fast();
}

void sched_preempt_enable(void) {
    if (!x86_preempt_disabled_fast()) return;
    x86_preempt_enable_fast();
    if (g_sched_wake_trace_tid != 0U && !x86_preempt_disabled_fast()) {
        uint32_t cpu_id = x86_current_cpu_index();
        if (cpu_id < MAX_CPUS) g_sched_preempt_callers[cpu_id] = 0U;
    }
}

bool sched_preempt_disabled(void) {
    return x86_preempt_disabled_fast();
}

/*
 * 弹出一个线程；其 CPU 已经切离旧栈/CR3。
 *
 * 此辅助函数刻意接收显式 CPU ID：一旦源 CPU 到达切换后的续执行路径，
 * 实际对象/VM 清理即可安全地交由全局延迟工作线程执行。
 */
static thread_t *scheduler_reap_pop_cpu(uint32_t cpu_id) {
    if (cpu_id >= g_cpu_count) return 0;

    scheduler_cpu_t *cpu = &g_cpus[cpu_id];

    uint64_t queue_flags =
        scheduler_lock(&cpu->reap_lock);

    thread_t *thread = cpu->reap_head;

    if (thread != 0) {
        cpu->reap_head = thread->reap_next;

        if (cpu->reap_head == 0) {
            cpu->reap_tail = 0;
        }

        thread->reap_next = 0;

        /*
         * 从此处开始，FIFO 不再拥有该条目。
         * 在 thread_release_execution_ref() 完成前，EXECUTION_REF 本身仍保护线程生命周期。
         */
        thread->sched.flags &= (uint16_t)~SCHED_ENTITY_REAP_QUEUED;
    }

    scheduler_unlock(&cpu->reap_lock,
                     queue_flags);

    return thread;
}

static bool scheduler_reap_cpu_pending(uint32_t cpu_id) {
    if (cpu_id >= g_cpu_count) return false;

    scheduler_cpu_t *cpu = &g_cpus[cpu_id];

    uint64_t queue_flags =
        scheduler_lock(&cpu->reap_lock);

    bool pending = cpu->reap_head != 0;

    scheduler_unlock(&cpu->reap_lock,
                     queue_flags);

    return pending;
}

static void scheduler_reap_drain_cpu_budget(uint32_t cpu_id, uint32_t budget) {
    while (budget-- != 0U) {
        thread_t *thread =
            scheduler_reap_pop_cpu(cpu_id);

        if (thread == 0) {
            break;
        }

        /*
         * 此处未持有调度器锁；回收锁仅用于保护此延迟 FIFO。
         *
         * 此操作可能销毁进程 VM 对象并发起 TLB shootdown。
         */
        thread_release_execution_ref(thread);
    }
}

static void scheduler_reap_drain_cpu(uint32_t cpu_id) {
    scheduler_reap_drain_cpu_budget(cpu_id, UINT32_MAX);
}

static void scheduler_reap_deferred_work(void *argument);

static void scheduler_reap_schedule_deferred(uint32_t cpu_id) {
    if (cpu_id >= g_cpu_count) return;

    scheduler_cpu_t *cpu = &g_cpus[cpu_id];

    if (!scheduler_reap_cpu_pending(cpu_id)) {
        return;
    }

    bool expected = false;

    if (!atomic_compare_exchange_strong_explicit(
            &cpu->reap_deferred_queued,
            &expected,
            true,
            memory_order_acq_rel,
            memory_order_acquire)) {
        return;
    }

    /*
    * 此函数可能从 LAPIC 硬中断上下文调用，因此此处只能使用中断安全的延迟工作生产者。
     */
    bool queued = deferred_background_schedule(scheduler_reap_deferred_work,
                                               (void *)(uintptr_t)cpu_id);
    /* 可选后台工作线程尚未创建时，引导线程可能会执行回收。一旦该线程存在，清理队列满时不得
     * 将工作退回输入路径。 */
    if (!queued && !deferred_background_worker_started())
        queued = deferred_try_schedule(scheduler_reap_deferred_work,
                                      (void *)(uintptr_t)cpu_id);
    if (!queued) {

        /*
         * 队列已满：保留回收器条目，并允许在下一个定时器/调度边界重试。
         */
        atomic_store_explicit(
            &cpu->reap_deferred_queued,
            false,
            memory_order_release);
    }
}

static void scheduler_reap_deferred_work(void *argument) {
    uint32_t cpu_id =
        (uint32_t)(uintptr_t)argument;

    if (cpu_id >= g_cpu_count) return;

    scheduler_cpu_t *cpu =
        &g_cpus[cpu_id];

    /*
    * 此时已处于普通 Ring0 工作线程上下文，且中断已开启。在发布此工作项前，源 CPU 已完成
    * 实际上下文切换。
     */
    /* 销毁过程可能释放进程 VM 并等待远端 TLB。每个工作项释放一个引用，以保证后台任务之间的
     * 公平性（启动早期回退路径中也包括设备任务）。 */
    scheduler_reap_drain_cpu_budget(cpu_id, 1U);

    /*
    * 当前队列排空后，才释放合并请求的所有权。
     */
    atomic_store_explicit(
        &cpu->reap_deferred_queued,
        false,
        memory_order_release);

    /*
    * 关闭流程：
     *
    * 排空操作发现队列为空
     *       ↓
    * queued==true 时 CPU 又将一个 DEAD 线程入队
     *       ↓
    * 工作线程清除 queued
     *
    * 由此产生竞争。
     */
    if (scheduler_reap_cpu_pending(cpu_id)) {
        scheduler_reap_schedule_deferred(cpu_id);
    }
}

void sched_finish_switch(void) {
    uint64_t flags;

    __asm__ volatile (
        "pushfq; popq %0"
        : "=r"(flags)
        :
        : "memory");

    uint32_t cpu_id;

    if (!scheduler_current_cpu(&cpu_id)) {
        return;
    }

    if ((flags & (1ULL << 9)) == 0 || sched_preempt_disabled() ||
        x86_tlb_shootdown_active()) {
        /*
         * 典型情况：
         *
         * 定时器 IRQ
         *   -> sched_tick()
         *   -> schedule()
         *   -> 上下文切换
         *
         * 旧栈/CR3 已不再活动，但当前仍处于恢复后的中断调用链中。因此 VM/TLB 拆除必须延迟，
         * 不能直接丢弃。
         */
        scheduler_reap_schedule_deferred(cpu_id);
        return;
    }

    /* 允许执行拆除的上下文不一定能容忍延迟：输入/合成器线程也会经过此边界返回。 */
    scheduler_command_release_drain(cpu_id);
    if (deferred_background_worker_started())
        scheduler_reap_schedule_deferred(cpu_id);
    else scheduler_reap_drain_cpu(cpu_id);
}

void sched_block_current(void) {
    uint32_t cpu_id;
    if (!scheduler_current_cpu(&cpu_id)) return;
    thread_t *current = g_cpus[cpu_id].queue.current;
    scheduler_cpu_t *cpu = &g_cpus[cpu_id];
    if (current == 0 || current == cpu->queue.idle) return;
    if (scheduler_publish_blocked_local(cpu_id, cpu, current)) schedule();
}

bool sched_state_transition_self_test(void) {
    thread_t thread = {0};
    thread_t ready = {0};
    atomic_init(&thread.state, THREAD_RUNNING);
    atomic_init(&thread.block_epoch, 0U);
    atomic_init(&thread.blocked_waiter, 0);
    atomic_init(&thread.command_ack, 0U);
    thread.owner_cpu = (uint16_t)x86_current_cpu_index();

    if (!sched_publish_blocked(&thread) ||
        atomic_load_explicit(&thread.state, memory_order_acquire) !=
            THREAD_BLOCKED ||
        sched_publish_dead(&thread)) {
        return false;
    }

    uint32_t current_cpu = x86_current_cpu_index();
    if (!scheduler_cpu_available(current_cpu)) return false;
    scheduler_cpu_t *current = &g_cpus[current_cpu];
    thread_t *saved_current = current->queue.current;
    uint64_t irq_flags = scheduler_irq_save();
    current->queue.current = &thread;
    bool resumed_blocked = sched_publish_running(&thread) &&
        atomic_load_explicit(&thread.state, memory_order_acquire) ==
            THREAD_RUNNING;
    atomic_store_explicit(&thread.state, THREAD_READY, memory_order_relaxed);
    bool resumed_ready = sched_publish_running(&thread) &&
        atomic_load_explicit(&thread.state, memory_order_acquire) ==
            THREAD_RUNNING;
    current->queue.current = saved_current;
    scheduler_irq_restore(irq_flags);
    if (!resumed_blocked || !resumed_ready) return false;

    initialize_thread(&ready, 2U, SCHED_CLASS_FAIR, 0U);
    ready.owner_cpu = thread.owner_cpu;
    if (!sched_publish_dead(&ready) ||
        atomic_load_explicit(&ready.state, memory_order_acquire) != THREAD_DEAD ||
        sched_publish_dead(&ready)) {
        return false;
    }

    /* 远端 EXIT 确认用于区分“已发布”与“已终止”线程，因此只有成功完成终止的线程才能提交退出状态。 */
    thread_t remote_exit = {0};
    scheduler_cpu_t remote_cpu = {0};
    atomic_init(&remote_exit.state, THREAD_READY);
    atomic_init(&remote_exit.command_ack, 0U);
    remote_exit.owner_cpu = (uint16_t)x86_current_cpu_index();
    remote_cpu.queue.current = &remote_exit;
    sched_cmd_t exit_message = {
        .object = (uintptr_t)&remote_exit,
        .op = SCHED_CMD_EXIT,
        .generation = SCHED_CMD_STATE_ACK,
    };
    scheduler_command_consume_local(remote_exit.owner_cpu, &remote_cpu,
                                    exit_message, false);
    if (atomic_load_explicit(&remote_exit.state, memory_order_acquire) !=
            THREAD_DEAD ||
        atomic_load_explicit(&remote_exit.command_ack, memory_order_acquire) !=
            (SCHED_CMD_STATE_ACK | SCHED_CMD_STATE_CHANGED)) {
        return false;
    }
    atomic_store_explicit(&remote_exit.command_ack, 0U,
                          memory_order_relaxed);
    scheduler_command_consume_local(remote_exit.owner_cpu, &remote_cpu,
                                    exit_message, false);
    if (atomic_load_explicit(&remote_exit.command_ack, memory_order_acquire) !=
        SCHED_CMD_STATE_ACK) {
        return false;
    }
    return true;
}

static bool scheduler_command_drain_local(
    uint32_t cpu_id,
    scheduler_cpu_t *cpu,
    bool publish_snapshot) {
    return scheduler_command_drain_local_limit(
        cpu_id, cpu, g_cpu_count, publish_snapshot);
}

static void scheduler_command_ack_drain_locked(uint32_t cpu_id,
                                               scheduler_cpu_t *cpu,
                                               uint32_t source_limit) {
    (void)scheduler_command_drain_local_limit(
        cpu_id, cpu, source_limit, false);
    if (cpu->queue_snapshot_dirty) scheduler_publish_queue_snapshot(cpu);
}

/*
 * 通过处理此 CPU 的收件箱，推进远端确认等待。重新调度 IPI 可能会将命令排空延迟到系统调用退出时，
 * 但调用方本身可能正卡在该系统调用中等待远端所有者。此路径与常规本地排空保持一致，也能保留每 CPU
 * SPSC 消费者所有权及中断串行化。
 */
static bool scheduler_command_ack_step(thread_t *thread,
                                       uint32_t source_limit,
                                       uint32_t ack_mask,
                                       uint32_t ack_value,
                                       bool drain_releases) {
    if (thread == 0) return false;

    uint64_t irq_flags = scheduler_irq_save();
    uint32_t cpu_id;
    /* 等待中的发送方可能在循环迭代之间迁移。绝不能在另一 CPU 上排空此前采样 CPU 的 SPSC 收件箱。 */
    bool current = scheduler_current_cpu(&cpu_id) &&
                   scheduler_cpu_available(cpu_id);
    if (current)
        scheduler_command_ack_drain_locked(cpu_id, &g_cpus[cpu_id], source_limit);
    scheduler_irq_restore(irq_flags);
    if (current && drain_releases) scheduler_command_release_drain(cpu_id);

    return (atomic_load_explicit(&thread->command_ack,
                                 memory_order_acquire) & ack_mask) == ack_value;
}

bool sched_remote_remove_ack_self_test(void) {
    scheduler_cpu_t cpu0 = {0};
    scheduler_cpu_t cpu1 = {0};
    sched_cmd_channel_t cpu0_channels[2] = {0};
    sched_cmd_channel_t cpu1_channels[2] = {0};
    thread_t thread0 = {0};
    thread_t thread1 = {0};
    const uint32_t generation =
        SCHED_CMD_REMOVE_NO_REF | SCHED_CMD_REMOVE_ACK;

    for (uint32_t source = 0U; source < 2U; ++source) {
        atomic_init(&cpu0_channels[source].primary.head, 0U);
        atomic_init(&cpu0_channels[source].primary.tail, 0U);
        atomic_init(&cpu0_channels[source].overflow.head, 0U);
        atomic_init(&cpu0_channels[source].overflow.tail, 0U);
        atomic_init(&cpu1_channels[source].primary.head, 0U);
        atomic_init(&cpu1_channels[source].primary.tail, 0U);
        atomic_init(&cpu1_channels[source].overflow.head, 0U);
        atomic_init(&cpu1_channels[source].overflow.tail, 0U);
    }
    atomic_init(&cpu0.command_pending, false);
    atomic_init(&cpu1.command_pending, false);
    atomic_init(&thread0.command_ack, 0U);
    atomic_init(&thread1.command_ack, 0U);
    thread0.owner_cpu = 0U;
    thread1.owner_cpu = 1U;
    cpu0.command_inbox = cpu0_channels;
    cpu1.command_inbox = cpu1_channels;
    cpu0.queue.current = &thread0;
    cpu1.queue.current = &thread1;

    /* 每个 CPU 都等待另一方的 REMOVE 确认。通过实际本地排空路径测试两种环形队列容量和索引回绕。 */
    atomic_store_explicit(&cpu0_channels[1].primary.head, UINT32_MAX,
                          memory_order_relaxed);
    atomic_store_explicit(&cpu0_channels[1].primary.tail, UINT32_MAX,
                          memory_order_relaxed);
    atomic_store_explicit(&cpu1_channels[0].overflow.head, UINT32_MAX,
                          memory_order_relaxed);
    atomic_store_explicit(&cpu1_channels[0].overflow.tail, UINT32_MAX,
                          memory_order_relaxed);
    if (!scheduler_command_ring_try_push(
            &cpu0_channels[1].primary, &thread0, SCHED_CMD_REMOVE,
            generation) ||
        !scheduler_command_overflow_try_push(
            &cpu1_channels[0].overflow, &thread1, SCHED_CMD_REMOVE,
            generation)) {
        return false;
    }
    atomic_store_explicit(&cpu0.command_pending, true, memory_order_release);
    atomic_store_explicit(&cpu1.command_pending, true, memory_order_release);

    /* 依次模拟两个所有者，但不将任一合成队列伪装成当前硬件 CPU 的收件箱。测试共享的加锁排空路径。 */
    uint64_t flags = scheduler_irq_save();
    scheduler_command_ack_drain_locked(0U, &cpu0, 2U);
    bool cpu0_still_waiting = atomic_load_explicit(
        &thread1.command_ack, memory_order_acquire) != generation;
    scheduler_command_ack_drain_locked(1U, &cpu1, 2U);
    bool cpu1_unblocked = atomic_load_explicit(
        &thread0.command_ack, memory_order_acquire) == generation;
    scheduler_command_ack_drain_locked(0U, &cpu0, 2U);
    bool cpu0_unblocked = atomic_load_explicit(
        &thread1.command_ack, memory_order_acquire) == generation;
    scheduler_irq_restore(flags);
    return cpu0_still_waiting && cpu1_unblocked && cpu0_unblocked &&
           atomic_load_explicit(&cpu0_channels[1].primary.head,
                                memory_order_acquire) == 0U &&
           atomic_load_explicit(&cpu1_channels[0].overflow.head,
                                memory_order_acquire) == 0U &&
           atomic_load_explicit(&thread0.command_ack, memory_order_acquire) ==
               generation &&
           atomic_load_explicit(&thread1.command_ack, memory_order_acquire) ==
               generation;
}

static bool scheduler_remote_wake_benchmark(void);

static bool scheduler_command_channel_self_test(thread_t *thread) {
    sched_cmd_channel_t channel = {0};
    uint32_t primary_start = UINT32_MAX - 3U;
    uint32_t overflow_start = UINT32_MAX - 31U;
    atomic_init(&channel.primary.head, primary_start);
    atomic_init(&channel.primary.tail, primary_start);
    atomic_init(&channel.overflow.head, overflow_start);
    atomic_init(&channel.overflow.tail, overflow_start);
    for (uint32_t index = 0U;
         index < SCHED_CMD_SLOTS + SCHED_CMD_OVERFLOW_SLOTS; ++index) {
        bool overflowed = false;
        if (!scheduler_command_channel_try_push(&channel, thread,
                SCHED_CMD_WAKE, index, &overflowed) ||
            overflowed != (index >= SCHED_CMD_SLOTS)) return false;
    }
    for (uint32_t index = 0U; index < SCHED_CMD_SLOTS; ++index) {
        sched_cmd_t entry = channel.primary.slots[
            (primary_start + index) & SCHED_CMD_MASK];
        if (entry.object != (uintptr_t)thread || entry.op != SCHED_CMD_WAKE ||
            entry.generation != index) return false;
    }
    for (uint32_t index = 0U; index < SCHED_CMD_OVERFLOW_SLOTS; ++index) {
        sched_cmd_t entry = channel.overflow.slots[
            (overflow_start + index) & SCHED_CMD_OVERFLOW_MASK];
        if (entry.object != (uintptr_t)thread || entry.op != SCHED_CMD_WAKE ||
            entry.generation != SCHED_CMD_SLOTS + index) return false;
    }
    bool overflowed = false;
    if (scheduler_command_channel_try_push(&channel, thread, SCHED_CMD_WAKE,
            99U, &overflowed) || !overflowed) return false;
    /* 主队列不能越过仍在溢出队列中的较早消息。 */
    atomic_store_explicit(&channel.primary.head, primary_start + SCHED_CMD_SLOTS,
                          memory_order_release);
    if (scheduler_command_channel_try_push(&channel, thread, SCHED_CMD_WAKE,
            99U, &overflowed)) return false;
    atomic_store_explicit(&channel.overflow.head, overflow_start + 1U,
                          memory_order_release);
    if (!scheduler_command_channel_try_push(&channel, thread, SCHED_CMD_WAKE,
            100U, &overflowed) || !overflowed) return false;
    atomic_store_explicit(&channel.overflow.head,
                          overflow_start + SCHED_CMD_OVERFLOW_SLOTS + 1U,
                          memory_order_release);
    return scheduler_command_channel_try_push(&channel, thread, SCHED_CMD_WAKE,
               101U, &overflowed) && !overflowed && !channel.overflow_active;
}

bool sched_remote_wake_self_test(void) {
    sched_cmd_ring_t ring;
    sched_cmd_t message;
    thread_t first = {0};
    thread_t second = {0};

    if (!scheduler_command_channel_self_test(&first)) return false;

    scheduler_zero(&ring, sizeof(ring));
    atomic_init(&ring.head, 0U);
    atomic_init(&ring.tail, 0U);

    for (uint32_t index = 0U; index < SCHED_CMD_SLOTS; ++index) {
        if (!scheduler_command_ring_try_push(
                &ring, &first, SCHED_CMD_WAKE, index + 1U)) return false;
    }
    if (scheduler_command_ring_try_push(
            &ring, &second, SCHED_CMD_ENQUEUE, 0U)) return false;

    for (uint32_t index = 0U; index < SCHED_CMD_SLOTS; ++index) {
        if (!scheduler_command_ring_try_pop(&ring, &message) ||
            message.object != (uintptr_t)&first ||
            message.op != SCHED_CMD_WAKE ||
            message.generation != index + 1U) {
            return false;
        }
    }
    if (scheduler_command_ring_try_pop(&ring, &message)) return false;

    /* 测试生产者/消费者 32 位计数回绕时的槽位复用。 */
    atomic_store_explicit(&ring.head, UINT32_MAX - 3U, memory_order_relaxed);
    atomic_store_explicit(&ring.tail, UINT32_MAX - 3U, memory_order_relaxed);
    for (uint32_t index = 0U; index < SCHED_CMD_SLOTS * 2U; ++index) {
        if (!scheduler_command_ring_try_push(
                &ring, &second, SCHED_CMD_ENQUEUE, index + 100U) ||
            !scheduler_command_ring_try_pop(&ring, &message) ||
            message.object != (uintptr_t)&second ||
            message.op != SCHED_CMD_ENQUEUE ||
            message.generation != index + 100U) {
            return false;
        }
    }

    thread_t blocked = {0};
    scheduler_cpu_t cpu = {0};
    atomic_init(&blocked.state, THREAD_BLOCKED);
    atomic_init(&blocked.block_epoch, 7U);
    atomic_init(&blocked.command_ack, 0U);
    cpu.queue.current = &blocked;
    if (scheduler_complete_wake_local(&cpu, &blocked, 6U, true) ||
        atomic_load_explicit(&blocked.state, memory_order_acquire) !=
            THREAD_BLOCKED ||
        !scheduler_complete_wake_local(&cpu, &blocked, 7U, true) ||
        atomic_load_explicit(&blocked.state, memory_order_acquire) !=
            THREAD_READY) {
        return false;
    }

    return scheduler_remote_wake_benchmark();
}

static void sched_context_test_entry(void) {
    g_context_test_ran = 1U;
    for (uint32_t round = 0U; round < g_context_test_rounds; ++round) {
        x86_switch_context(g_context_test_to, g_context_test_from);
    }
    for (;;) __asm__ volatile ("cli; hlt" : : : "memory");
}

#define SCHED_REMOTE_BENCH_SAMPLES 32U

#if OS_REALTEST
static atomic_bool g_remote_benchmark_stop;
static atomic_bool g_remote_benchmark_waiting;
static atomic_uint g_remote_benchmark_runs;

static void scheduler_remote_benchmark_worker(void *argument) {
    (void)argument;
    for (;;) {
        thread_t *thread = sched_current_thread();
        if (thread == 0) {
            for (;;) __asm__ volatile ("cli; hlt" : : : "memory");
        }

        atomic_store_explicit(&g_remote_benchmark_waiting, true,
                              memory_order_release);
        if (!sched_publish_blocked(thread)) {
            schedule();
            continue;
        }
        schedule();
        atomic_store_explicit(&g_remote_benchmark_waiting, false,
                              memory_order_release);
        if (atomic_load_explicit(&g_remote_benchmark_stop,
                                 memory_order_acquire)) {
            (void)sched_publish_dead(thread);
            schedule();
            for (;;) __asm__ volatile ("cli; hlt" : : : "memory");
        }
        atomic_fetch_add_explicit(&g_remote_benchmark_runs, 1U,
                                  memory_order_release);
    }
}

static bool scheduler_remote_benchmark_wait_blocked(
    thread_t *thread,
    uint64_t deadline) {
    for (;;) {
        bool waiting = atomic_load_explicit(&g_remote_benchmark_waiting,
                                            memory_order_acquire);
        unsigned state = atomic_load_explicit(&thread->state,
                                              memory_order_acquire);
        if (waiting && state == THREAD_BLOCKED) return true;
        if ((int64_t)(x86_read_tsc() - deadline) >= 0) return false;
        __asm__ volatile ("pause");
    }
}

static void scheduler_remote_benchmark_emit_stats(
    const char *base_name,
    const char *min_name,
    const char *median_name,
    const char *average_name,
    const char *p95_name,
    uint64_t *samples) {
    uint64_t total = 0U;
    for (uint32_t index = 1U; index < SCHED_REMOTE_BENCH_SAMPLES; ++index) {
        uint64_t value = samples[index];
        uint32_t position = index;
        while (position != 0U && samples[position - 1U] > value) {
            samples[position] = samples[position - 1U];
            --position;
        }
        samples[position] = value;
    }
    for (uint32_t index = 0U; index < SCHED_REMOTE_BENCH_SAMPLES; ++index) {
        total += samples[index];
    }
    uint32_t p95_index =
        (SCHED_REMOTE_BENCH_SAMPLES * 95U + 99U) / 100U - 1U;
    uint64_t median = samples[SCHED_REMOTE_BENCH_SAMPLES / 2U];
    kernel_perf_emit_value(base_name, median);
    kernel_perf_emit_value(min_name, samples[0]);
    kernel_perf_emit_value(median_name, median);
    kernel_perf_emit_value(average_name,
                           total / SCHED_REMOTE_BENCH_SAMPLES);
    kernel_perf_emit_value(p95_name, samples[p95_index]);
}
#endif

static bool scheduler_remote_wake_benchmark(void) {
#if !OS_REALTEST
    return true;
#else
    static thread_t thread;
    static uint8_t stack[8192] __attribute__((aligned(16)));
    uint64_t submit_samples[SCHED_REMOTE_BENCH_SAMPLES];
    uint64_t ipi_samples[SCHED_REMOTE_BENCH_SAMPLES];
    uint64_t ready_samples[SCHED_REMOTE_BENCH_SAMPLES];
    uint64_t running_samples[SCHED_REMOTE_BENCH_SAMPLES];
    uint32_t source_cpu;
    uint32_t target_cpu = UINT32_MAX;

    if (!scheduler_current_cpu(&source_cpu) || g_cpu_count < 2U) return true;
    for (uint32_t cpu_id = 0U; cpu_id < g_cpu_count; ++cpu_id) {
        if (cpu_id != source_cpu && scheduler_cpu_available(cpu_id)) {
            target_cpu = cpu_id;
            break;
        }
    }
    if (target_cpu == UINT32_MAX) return true;

    scheduler_zero(&thread, sizeof(thread));
    refcount_init(&thread.object.refs, 1U);
    sched_initialize_new_thread(&thread, target_cpu, true);
    uintptr_t stack_top = ((uintptr_t)stack + sizeof(stack)) &
                          ~(uintptr_t)15U;
    thread.kernel_stack_base = stack;
    thread.kernel_stack_top = stack_top;
    thread.kernel_stack_size = sizeof(stack);
    x86_prepare_kernel_thread(&thread.arch, stack_top,
                              scheduler_remote_benchmark_worker, 0);
    atomic_init(&g_remote_benchmark_stop, false);
    atomic_init(&g_remote_benchmark_waiting, false);
    atomic_init(&g_remote_benchmark_runs, 0U);

    bool started = sched_start_thread(&thread) == K_OK;
    uint64_t timeout = x86_timeout_ns_to_tsc(1000000000ULL);
    uint64_t deadline = x86_read_tsc() + timeout;
    if (!started || !scheduler_remote_benchmark_wait_blocked(&thread,
                                                              deadline)) {
        if (started) {
            atomic_store_explicit(&g_remote_benchmark_stop, true,
                                  memory_order_release);
            sched_wake(&thread);
        }
        return false;
    }

    for (uint32_t sample = 0U;
         sample < SCHED_REMOTE_BENCH_SAMPLES; ++sample) {
        uint32_t block_epoch =
            atomic_load_explicit(&thread.block_epoch, memory_order_relaxed);
        uint32_t run_before =
            atomic_load_explicit(&g_remote_benchmark_runs,
                                 memory_order_acquire);

        uint64_t start = telemetry_timestamp();
        if (!scheduler_remote_command_try_push(
                source_cpu, target_cpu, &thread, SCHED_CMD_WAKE,
                block_epoch)) {
            started = false;
            break;
        }
        submit_samples[sample] = telemetry_timestamp() - start;

        start = telemetry_timestamp();
        if (!x86_smp_request_reschedule(target_cpu)) {
            started = false;
            break;
        }
        ipi_samples[sample] = telemetry_timestamp() - start;

        start = telemetry_timestamp();
        timeout = x86_timeout_ns_to_tsc(1000000000ULL);
        deadline = x86_read_tsc() + timeout;
        while (atomic_load_explicit(&thread.state, memory_order_acquire) ==
               THREAD_BLOCKED) {
            if ((int64_t)(x86_read_tsc() - deadline) >= 0) {
                started = false;
                break;
            }
            __asm__ volatile ("pause");
        }
        if (!started) break;
        ready_samples[sample] = telemetry_timestamp() - start;

        start = telemetry_timestamp();
        timeout = x86_timeout_ns_to_tsc(1000000000ULL);
        deadline = x86_read_tsc() + timeout;
        while (atomic_load_explicit(&g_remote_benchmark_runs,
                                    memory_order_acquire) == run_before) {
            if ((int64_t)(x86_read_tsc() - deadline) >= 0) {
                started = false;
                break;
            }
            __asm__ volatile ("pause");
        }
        if (!started) break;
        running_samples[sample] = telemetry_timestamp() - start;

        timeout = x86_timeout_ns_to_tsc(1000000000ULL);
        if (!scheduler_remote_benchmark_wait_blocked(
                &thread, x86_read_tsc() + timeout)) {
            started = false;
            break;
        }
    }

    atomic_store_explicit(&g_remote_benchmark_stop, true,
                          memory_order_release);
    if (started) {
        sched_wake(&thread);
        timeout = x86_timeout_ns_to_tsc(1000000000ULL);
        deadline = x86_read_tsc() + timeout;
        while (atomic_load_explicit(&thread.state, memory_order_acquire) !=
               THREAD_DEAD) {
            if ((int64_t)(x86_read_tsc() - deadline) >= 0) {
                started = false;
                break;
            }
            __asm__ volatile ("pause");
        }
    }
    if (!started) return false;

    scheduler_remote_benchmark_emit_stats(
        "scheduler.wake.remote_wake_submit",
        "scheduler.wake.remote_wake_submit.min",
        "scheduler.wake.remote_wake_submit.median",
        "scheduler.wake.remote_wake_submit.average",
        "scheduler.wake.remote_wake_submit.p95", submit_samples);
    scheduler_remote_benchmark_emit_stats(
        "scheduler.wake.remote_ipi_submit",
        "scheduler.wake.remote_ipi_submit.min",
        "scheduler.wake.remote_ipi_submit.median",
        "scheduler.wake.remote_ipi_submit.average",
        "scheduler.wake.remote_ipi_submit.p95", ipi_samples);
    scheduler_remote_benchmark_emit_stats(
        "scheduler.wake.remote_wake_to_ready",
        "scheduler.wake.remote_wake_to_ready.min",
        "scheduler.wake.remote_wake_to_ready.median",
        "scheduler.wake.remote_wake_to_ready.average",
        "scheduler.wake.remote_wake_to_ready.p95", ready_samples);
    scheduler_remote_benchmark_emit_stats(
        "scheduler.wake.remote_wake_to_running",
        "scheduler.wake.remote_wake_to_running.min",
        "scheduler.wake.remote_wake_to_running.median",
        "scheduler.wake.remote_wake_to_running.average",
        "scheduler.wake.remote_wake_to_running.p95", running_samples);
    scheduler_remote_benchmark_emit_stats(
        "scheduler.schedule_with_remote_command",
        "scheduler.schedule_with_remote_command.min",
        "scheduler.schedule_with_remote_command.median",
        "scheduler.schedule_with_remote_command.average",
        "scheduler.schedule_with_remote_command.p95", running_samples);
    return true;
#endif
}

bool sched_context_switch_self_test(void) {
    static uint8_t stack[4096] __attribute__((aligned(16)));
    uintptr_t stack_top = ((uintptr_t)stack + sizeof(stack)) & ~(uintptr_t)15U;

    const uint32_t rounds = 10000U;
    uint64_t benchmark_start = telemetry_timestamp();
    uint64_t elapsed;
    arch_switch_context_t from = {0};
    arch_switch_context_t to = {0};
    stack_top -= sizeof(uint64_t);
    *(uint64_t *)stack_top = (uint64_t)(uintptr_t)&sched_context_test_entry;
    to.rsp = stack_top;
    g_context_test_from = &from;
    g_context_test_to = &to;
    g_context_test_rounds = rounds;
    g_context_test_ran = 0U;
    for (uint32_t round = 0U; round < rounds; ++round) {
        x86_switch_context(&from, &to);
    }
    elapsed = telemetry_timestamp() - benchmark_start;
    kernel_perf_emit_value("scheduler.context_switch", elapsed / rounds);
    kernel_perf_emit_value("scheduler.context_switch.average",
                           elapsed / rounds);
    g_context_test_from = 0;
    g_context_test_to = 0;
    g_context_test_rounds = 0U;
    return g_context_test_ran == 1U;
}

kstatus_t sched_set_affinity(thread_t *thread, const cpumask_t *mask) {
    if (thread == 0 || mask == 0 || g_cpu_count == 0) return K_EINVAL;
    cpumask_t requested = *mask;
    bool any = false;
    for (uint32_t word = 0; word < MAX_CPUS / 64U; ++word) {
        if (requested.bits[word] != 0) any = true;
    }
    if (!any) return K_EINVAL;

    uint32_t current_cpu;
    if (!scheduler_current_cpu(&current_cpu)) return K_EINVAL;
    uint32_t owner_cpu = scheduler_cpu_available(thread->owner_cpu) ?
                         thread->owner_cpu : current_cpu;
    if (scheduler_affinity_mask_allows(&requested, owner_cpu)) {
        thread->affinity = requested;
        return K_OK;
    }

    uint32_t target_cpu;
    if (!scheduler_select_cpu_mask(&requested, current_cpu, &target_cpu) ||
        !scheduler_cpu_available(target_cpu) ||
        !scheduler_affinity_mask_allows(&requested, target_cpu)) return K_EINVAL;
    /* 预检查失败时不得替换当前有效亲和性。普通选择调用方仍回退到当前 CPU；亲和性更新则必须显式
     * 选择请求掩码中可用的成员。 */
    thread->affinity = requested;

    unsigned state = atomic_load_explicit(&thread->state, memory_order_acquire);
    if (state == THREAD_NEW || state == THREAD_DEAD) {
        thread->owner_cpu = (uint16_t)target_cpu;
        thread->migration_pending = false;
        return K_OK;
    }

    scheduler_cpu_t *owner = &g_cpus[owner_cpu];
    if (owner_cpu != current_cpu) {
        if (owner->command_inbox == 0) return K_EBUSY;
        uint64_t irq_flags = scheduler_irq_save();
        bool queued = scheduler_remote_command_try_push(
            current_cpu, owner_cpu, thread, SCHED_CMD_MIGRATE, target_cpu);
        scheduler_irq_restore(irq_flags);
        if (queued) (void)x86_smp_request_reschedule(owner_cpu);
        return queued ? K_OK : K_EBUSY;
    }

    uint64_t queue_flags = scheduler_irq_save();
    scheduler_migrate_local(owner_cpu, owner, thread, target_cpu);
    bool pending = thread->migration_pending;
    scheduler_irq_restore(queue_flags);
    if (pending) (void)x86_smp_request_reschedule(owner_cpu);
    return K_OK;
}

void sched_set_effective_priority(thread_t *thread, uint8_t class_id,
                                  uint8_t rt_priority) {
    if (thread == 0 || g_cpu_count == 0 ||
        (class_id != SCHED_CLASS_RT && class_id != SCHED_CLASS_FAIR) ||
        (class_id == SCHED_CLASS_RT && rt_priority >= RT_PRIORITY_LEVELS)) return;
    uint64_t queue_flags = scheduler_irq_save();
    uint32_t current_cpu;
    if (!scheduler_current_cpu(&current_cpu)) {
        scheduler_irq_restore(queue_flags);
        return;
    }
    scheduler_thread_mailbox_lock(thread);
    uint32_t cpu_id = thread->owner_cpu;
    if (!scheduler_cpu_available(cpu_id) ||
        atomic_load_explicit(&thread->state, memory_order_acquire) == THREAD_DEAD) {
        scheduler_thread_mailbox_unlock(thread);
        scheduler_irq_restore(queue_flags);
        return;
    }
    uint32_t payload = ((uint32_t)class_id << SCHED_CMD_CLASS_SHIFT) | rt_priority;
    bool notify = false;
    if (cpu_id == current_cpu) {
        /* 旧邮箱可能仍位于迁移前的 CPU；覆盖其负载，避免以后重放旧捐赠。 */
        atomic_store_explicit(&thread->priority_request, payload, memory_order_release);
        scheduler_apply_priority_local(&g_cpus[cpu_id], thread, class_id, rt_priority);
    } else if (atomic_load_explicit(&thread->priority_queued, memory_order_relaxed)) {
        atomic_store_explicit(&thread->priority_request, payload, memory_order_release);
        /* 已发布的节点或正在消费的批次负责前进，不为重复更新发送 IPI。 */
    } else if (object_try_get(thread)) {
        atomic_store_explicit(&thread->priority_request, payload, memory_order_release);
        atomic_store_explicit(&thread->priority_queued, true, memory_order_release);
        scheduler_priority_link(&g_cpus[cpu_id], thread);
        notify = true;
    }
    scheduler_thread_mailbox_unlock(thread);
    scheduler_irq_restore(queue_flags);
    if (notify) (void)x86_smp_request_reschedule(cpu_id);
}

thread_t *sched_current_thread(void) {
    uint32_t cpu_id;
    if (!scheduler_current_cpu(&cpu_id)) return 0;
    return g_cpus[cpu_id].queue.current;
}

void sched_remove(thread_t *thread) {
    if (thread == 0 || g_cpu_count == 0) return;
    uint32_t current_cpu;
    if (!scheduler_current_cpu(&current_cpu)) return;
    uint32_t cpu_id = scheduler_cpu_available(thread->owner_cpu) ?
                      thread->owner_cpu : current_cpu;
    scheduler_cpu_t *cpu = &g_cpus[cpu_id];

    if (cpu_id != current_cpu) {
        if (cpu->command_inbox == 0) return;
        uint64_t irq_flags = scheduler_irq_save();
        bool unowned = atomic_load_explicit(&thread->object.refs.value,
                                            memory_order_acquire) == 0U;
        uint32_t generation = unowned ?
            (SCHED_CMD_REMOVE_NO_REF | SCHED_CMD_REMOVE_ACK) : 0U;
        if (unowned) {
            atomic_store_explicit(&thread->command_ack, 0U,
                                  memory_order_relaxed);
        }
        bool queued = unowned ?
            scheduler_remote_command_try_push_unowned(
                current_cpu, cpu_id, thread, SCHED_CMD_REMOVE, generation) :
            scheduler_remote_command_try_push(
                current_cpu, cpu_id, thread, SCHED_CMD_REMOVE, generation);
        scheduler_irq_restore(irq_flags);
        if (!queued) return;
        (void)x86_smp_request_reschedule(cpu_id);
        if (unowned) {
            while (!scheduler_command_ack_step(
                thread, g_cpu_count,
                UINT32_MAX, generation, true)) {
                __asm__ volatile ("pause");
            }
        }
        return;
    }

    uint64_t queue_flags = scheduler_irq_save();
    if (cpu->queue.current != thread) dequeue_local(cpu, thread);
    scheduler_irq_restore(queue_flags);
}

uint32_t sched_runnable_count(void) {
    uint32_t cpu_id;
    if (!scheduler_current_cpu(&cpu_id)) return 0U;

    scheduler_cpu_t *cpu = &g_cpus[cpu_id];
    uint32_t runnable = scheduler_snapshot_runnable(cpu);

    /*
    * 即使所有者 CPU 尚未将 BLOCKED 转为 READY 并将线程加入本地运行队列，远端唤醒仍属于调度工作。
    * 如果 IPI 延迟，这也能保留现有的定时器/空闲回退机制。
     */
    if (runnable == 0U &&
        atomic_load_explicit(&cpu->command_pending, memory_order_acquire)) {
        return 1U;
    }

    return runnable;
}

bool sched_validate_current_cpu(void) {
    uint32_t cpu_id;
    if (!scheduler_current_cpu(&cpu_id)) return false;
    scheduler_cpu_t *cpu = &g_cpus[cpu_id];
    uint64_t queue_flags = scheduler_irq_save();
    bool valid = cpu->queue.fair_root.root == 0 ||
                 rb_tree_color(cpu->queue.fair_root.root) == RB_TREE_BLACK;
    thread_t *previous = 0;
    uint32_t black_depth = UINT32_MAX;
    uint32_t fair_count = 0;
    if (valid) {
        valid = validate_fair_node(cpu->queue.fair_root.root, 0, &previous, 0,
                                   &black_depth, &fair_count) &&
                fair_count == cpu->queue.fair_count;
    }
    uint32_t rt_count = 0;
    for (uint32_t priority = 0; valid && priority < RT_PRIORITY_LEVELS; ++priority) {
        bool empty = list_empty(&cpu->queue.rt_queues[priority]);
        if (empty != ((cpu->queue.rt_bitmap & (1U << priority)) == 0)) valid = false;
        for (list_head_t *item = cpu->queue.rt_queues[priority].next;
             item != &cpu->queue.rt_queues[priority]; item = item->next) {
            if (++rt_count > cpu->queue.nr_running) {
                valid = false;
                break;
            }
        }
    }
    valid = valid && fair_count + rt_count == cpu->queue.nr_running;
    scheduler_irq_restore(queue_flags);
    return valid;
}

bool sched_debug_cpu(uint32_t cpu_id, uint32_t *current_state,
                     uint64_t *current_tid, uint32_t *runnable_count) {
    if (cpu_id >= g_cpu_count || g_cpus[cpu_id].queue.idle == 0) return false;
    scheduler_cpu_t *cpu = &g_cpus[cpu_id];
    uint64_t queue_flags = scheduler_irq_save();
    thread_t *current = cpu->queue.current;
    if (current_state != 0) {
        *current_state = current != 0 ?
            atomic_load_explicit(&current->state, memory_order_acquire) : UINT32_MAX;
    }
    if (current_tid != 0) *current_tid = current != 0 ? current->tid : 0U;
    if (runnable_count != 0) *runnable_count = cpu->queue.nr_running;
    scheduler_irq_restore(queue_flags);
    return true;
}

/*
 * 用户地址 TLB shootdown 的目标筛选。
 *
 * queue.current 在 schedule() 中先于 CR3 切换发布。切入目标地址空间时最多
 * 多发一次 IPI；切出时 CPU 已只执行内核切换代码，不再访问旧用户映射。
 * x86_switch_context_root() 的 MOV CR3 当前没有设置 PCID no-flush(bit 63)，
 * 因此已经切出的 CPU 下次重新进入该 PCID 时会刷新旧 translation。
 */
bool sched_cpu_uses_root(uint32_t cpu_id, paddr_t root) {
    if (root.value == 0 || cpu_id >= g_cpu_count ||
        g_cpus[cpu_id].queue.idle == 0) return false;

    scheduler_cpu_t *cpu = &g_cpus[cpu_id];
    uint64_t queue_flags = scheduler_irq_save();
    thread_t *current = cpu->queue.current;
    paddr_t current_root = g_kernel_root;
    if (current != 0 && current->process != 0 && current->process->vm != 0) {
        current_root = current->process->vm->root_table;
    }
    bool uses_root = current_root.value == root.value;
    scheduler_irq_restore(queue_flags);
    return uses_root;
}
