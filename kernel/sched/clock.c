#include <uapi/syscall.h>
#include "internal.h"
#include <arch/x86_64/smp.h>
#include <uapi/cpu_usage.h>

/*
 * LAPIC 时钟记账和时间片到期逻辑位于此编译单元。
 * 定时器仅记录执行时间并发布抢占请求；是否进入 schedule() 由中断边界决定。
 */
static uint64_t scheduler_fair_vruntime_delta(uint64_t runtime_delta,
                                              uint32_t weight) {
    if (runtime_delta == 0U) return 0U;
    /* 几乎所有内核工作线程和普通应用线程都使用默认权重。避免在其时钟和系统调用尾部
     * 记账路径上执行两次变宽除法；缩放后的结果恰好等于 delta。 */
    if (weight == SCHED_NICE_0_WEIGHT) return runtime_delta;
    if (weight == 0U) weight = 1U;

    /* 先做除法，避免常见的大运行时间场景发生溢出。 */
    uint64_t quotient = runtime_delta / weight;
    uint64_t remainder = runtime_delta % weight;
    if (quotient > UINT64_MAX / SCHED_NICE_0_WEIGHT) return UINT64_MAX;
    uint64_t scaled = quotient * SCHED_NICE_0_WEIGHT;
    uint64_t fraction =
        remainder * SCHED_NICE_0_WEIGHT / weight;
    return scaled > UINT64_MAX - fraction ? UINT64_MAX : scaled + fraction;
}

static uint64_t scheduler_vruntime_add(uint64_t vruntime,
                                       uint64_t delta) {
    return vruntime > UINT64_MAX - delta ? UINT64_MAX : vruntime + delta;
}

uint64_t scheduler_account_current(run_queue_t *queue, uint64_t now_ns) {
    if (now_ns < queue->clock_ns) now_ns = queue->clock_ns;
    uint64_t elapsed=queue->clock_ns ? now_ns-queue->clock_ns : 0U;
    queue->clock_ns = now_ns;
    if (queue->current && elapsed) {
        uint64_t *counter=queue->current==queue->idle ? &queue->usage_idle_ns : &queue->usage_busy_ns;
        __atomic_store_n(counter,scheduler_vruntime_add(__atomic_load_n(counter,__ATOMIC_RELAXED),elapsed),__ATOMIC_RELAXED);
    }
    thread_t *current = queue->current;
    if (current == 0 || current == queue->idle) return 0U;
    uint64_t start = current->sched.exec_start_ns;
    uint64_t delta = start == 0U || now_ns < start ? 0U : now_ns - start;
    __atomic_store_n(&current->sched.runtime_ns, scheduler_vruntime_add(
        __atomic_load_n(&current->sched.runtime_ns, __ATOMIC_RELAXED), delta),
        __ATOMIC_RELAXED);
    current->sched.exec_start_ns = now_ns;
    if (current->sched_class == SCHED_CLASS_FAIR) {
        current->sched.vruntime = scheduler_vruntime_add(
            current->sched.vruntime,
            scheduler_fair_vruntime_delta(delta, current->sched.weight));
        scheduler_update_min_vruntime(queue);
    } else if (current->sched_class == SCHED_CLASS_RT) {
        current->sched.slice_runtime_ns = scheduler_vruntime_add(
            current->sched.slice_runtime_ns, delta);
    }
    return delta;
}

void sched_tick(uint64_t now_ns) {
    uint32_t cpu_id;
    if (!scheduler_current_cpu(&cpu_id)) return;
    scheduler_cpu_t *cpu = &g_cpus[cpu_id];
    uint64_t delta = scheduler_account_current(&cpu->queue, now_ns);
    thread_t *current = cpu->queue.current;
    if (current == 0) return;
    if (current == cpu->queue.idle) {
        if (sched_runnable_count() != 0U) {
            atomic_store_explicit(&cpu->need_resched, true,
                                  memory_order_release);
        }
        return;
    }
    bool runnable = sched_runnable_count() != 0U;
    bool timeslice_expired =
        current->sched_class == SCHED_CLASS_RT &&
        current->sched.slice_runtime_ns >= SCHED_RT_TIMESLICE_NS;
    if (runnable &&
        ((current->sched_class == SCHED_CLASS_FAIR && delta != 0U) ||
         timeslice_expired)) {
        atomic_store_explicit(&cpu->need_resched, true,
                              memory_order_release);
    }
}

bool sched_fair_policy_self_test(void) {
    if (scheduler_fair_vruntime_delta(100U, SCHED_NICE_0_WEIGHT) != 100U ||
        scheduler_fair_vruntime_delta(100U, 2048U) != 50U ||
        scheduler_fair_vruntime_delta(100U, 512U) != 200U) {
        return false;
    }
    if (scheduler_fair_vruntime_delta(UINT64_MAX, 1U) != UINT64_MAX ||
        scheduler_vruntime_add(UINT64_MAX - 10U, 20U) != UINT64_MAX) {
        return false;
    }
    return true;
}

bool sched_tick_should_preempt(void) {
    uint32_t cpu_id;
    return scheduler_current_cpu(&cpu_id) &&
           atomic_load_explicit(&g_cpus[cpu_id].need_resched,
                                memory_order_acquire);
}

/* 两个单调计数分别统计忙碌和空闲，不获取远端运行队列锁。 */
kstatus_t sched_usage_query(os_cpu_usage_t *info) {
    if (!info) return K_EINVAL;
    *info=(os_cpu_usage_t){.hdr={sizeof(*info),OS_SYSCALL_ABI_VERSION}};
    for (uint32_t cpu=0;cpu<g_cpu_count && cpu<MAX_CPUS;++cpu) {
        if (!x86_smp_cpu_online(cpu)) continue;
        info->busy_ns=scheduler_vruntime_add(info->busy_ns,__atomic_load_n(&g_cpus[cpu].queue.usage_busy_ns,__ATOMIC_RELAXED));
        info->idle_ns=scheduler_vruntime_add(info->idle_ns,__atomic_load_n(&g_cpus[cpu].queue.usage_idle_ns,__ATOMIC_RELAXED));
        ++info->cpu_count;
    }
    return info->cpu_count ? K_OK : K_EAGAIN;
}
