#include "internal.h"
#include <arch/x86_64/cpu.h>
#include <arch/x86_64/smp.h>
#include <uapi/process.h>
#include <uapi/syscall.h>

/* 此锁保护成员关系和生命周期，不保护运行队列。读取方在锁内复制快照；
 * 不会将借用指针带入用户态复制操作。注册必须显式进行：idle 和临时调度测试线程
 * 不会显示出来。 */
static spinlock_t g_monitor_lock;
static thread_t *g_monitor_threads;
static uint64_t g_monitor_retired_ns;

static uint64_t monitor_add(uint64_t a, uint64_t b) {
    return a > UINT64_MAX - b ? UINT64_MAX : a + b;
}

static void monitor_name(char *out, size_t size, const char *name) {
    size_t i = 0U;
    for (; i + 1U < size && name[i] != '\0'; ++i) out[i] = name[i];
    if (size != 0U) out[i] = '\0';
}

void sched_monitor_register(thread_t *thread, const char *name) {
    if (thread == 0 || thread->tid == 0U || thread->process != 0 || name == 0) return;
    uint64_t flags = scheduler_lock(&g_monitor_lock);
    if (thread->monitor_name == 0) {
        thread->monitor_name = name;
        thread->monitor_created_ns = x86_tsc_to_ns(x86_read_tsc());
        thread->monitor_next = g_monitor_threads;
        g_monitor_threads = thread;
    }
    scheduler_unlock(&g_monitor_lock, flags);
}

void sched_monitor_unregister(thread_t *thread) {
    if (thread == 0) return;
    uint64_t flags = scheduler_lock(&g_monitor_lock);
    for (thread_t **link = &g_monitor_threads; *link != 0; link = &(*link)->monitor_next) {
        if (*link != thread) continue;
        *link = thread->monitor_next;
        uint64_t runtime = __atomic_load_n(&thread->sched.runtime_ns, __ATOMIC_RELAXED);
        g_monitor_retired_ns = monitor_add(g_monitor_retired_ns,
            runtime >= thread->monitor_retired_ns ? runtime - thread->monitor_retired_ns : 0U);
        thread->monitor_retired_ns = runtime;
        thread->monitor_next = 0;
        thread->monitor_name = 0;
        break;
    }
    scheduler_unlock(&g_monitor_lock, flags);
}

kstatus_t sched_monitor_process(os_process_snapshot_t *snapshot) {
    if (snapshot == 0) return K_EINVAL;
    uint64_t flags = scheduler_lock(&g_monitor_lock);
    if (g_monitor_threads == 0 && g_monitor_retired_ns == 0U) {
        scheduler_unlock(&g_monitor_lock, flags);
        return K_ENOENT;
    }
    *snapshot = (os_process_snapshot_t){0};
    snapshot->hdr.size = sizeof(*snapshot);
    snapshot->hdr.version = OS_SYSCALL_ABI_VERSION;
    snapshot->pid = OS_KERNEL_WORKERS_PID;
    snapshot->state = OS_PROCESS_RUNNING;
    for (thread_t *thread = g_monitor_threads; thread != 0; thread = thread->monitor_next)
        ++snapshot->thread_count;
    monitor_name(snapshot->name, sizeof(snapshot->name), "Kernel workers");
    scheduler_unlock(&g_monitor_lock, flags);
    return K_OK;
}

kstatus_t sched_monitor_thread(uint32_t index, os_thread_snapshot_t *snapshot) {
    if (snapshot == 0) return K_EINVAL;
    uint64_t flags = scheduler_lock(&g_monitor_lock);
    kstatus_t status = K_ENOENT;
    for (thread_t *thread = g_monitor_threads; thread != 0; thread = thread->monitor_next) {
        if (index-- != 0U) continue;
        *snapshot = (os_thread_snapshot_t){0};
        snapshot->hdr.size = sizeof(*snapshot);
        snapshot->hdr.version = OS_SYSCALL_ABI_VERSION;
        snapshot->tid = thread->tid;
        snapshot->process_pid = OS_KERNEL_WORKERS_PID;
        snapshot->state = atomic_load_explicit(&thread->state, memory_order_acquire);
        snapshot->current_cpu = thread->current_cpu;
        monitor_name(snapshot->name, sizeof(snapshot->name), thread->monitor_name);
        status = K_OK;
        break;
    }
    scheduler_unlock(&g_monitor_lock, flags);
    return status;
}

kstatus_t sched_monitor_query(uint64_t tid, os_task_info_t *info) {
    if (info == 0) return K_EINVAL;
    uint64_t flags = scheduler_lock(&g_monitor_lock);
    *info = (os_task_info_t){0};
    info->hdr.size = sizeof(*info);
    info->hdr.version = OS_SYSCALL_ABI_VERSION;
    info->pid = OS_KERNEL_WORKERS_PID;
    info->tid = tid;
    info->flags = OS_TASK_PROTECTED | OS_TASK_KERNEL;
    info->state = OS_PROCESS_RUNNING;
    info->cpu_count = x86_smp_started_count();
    if (info->cpu_count == 0U) info->cpu_count = 1U;
    info->runtime_ns = tid == 0U ? g_monitor_retired_ns : 0U;
    kstatus_t status = tid == 0U ? K_OK : K_ENOENT;
    for (thread_t *thread = g_monitor_threads; thread != 0; thread = thread->monitor_next) {
        if (tid != 0U && thread->tid != tid) continue;
        uint64_t runtime = __atomic_load_n(&thread->sched.runtime_ns, __ATOMIC_RELAXED);
        if (tid == 0U) runtime = runtime >= thread->monitor_retired_ns ?
            runtime - thread->monitor_retired_ns : 0U;
        info->runtime_ns = monitor_add(info->runtime_ns, runtime);
        ++info->thread_count;
        if (tid != 0U) {
            info->create_time_ns = thread->monitor_created_ns;
            info->state = atomic_load_explicit(&thread->state, memory_order_acquire);
            status = K_OK;
            break;
        }
    }
    scheduler_unlock(&g_monitor_lock, flags);
    return status;
}
