#include <arch/x86_64/context.h>
#include <arch/x86_64/cpu.h>
#include <arch/x86_64/smp.h>
#include <arch/x86_64/paging.h>
#include <kernel/elf_loader.h>
#include <kernel/debug_stage.h>
#include <kernel/process.h>
#include <kernel/resource.h>
#include <kernel/file_lock.h>
#include <kernel/linux_abi.h>
#include <kernel/string.h>
#include <kernel/sched.h>
#include <kernel/wait.h>
#include <kernel/window_server.h>
#include <uapi/process.h>
#include <uapi/syscall.h>
#include <uapi/wait.h>
#include "internal.h"

#define USER_CODE_SELECTOR    0x23ULL
#define USER_DATA_SELECTOR    0x1BULL
#define USER_INITIAL_RFLAGS   0x202ULL
#define THREAD_KERNEL_STACK_SIZE (64U * 1024U)

static atomic_uint_fast64_t g_next_task_id;
static atomic_uint g_process_init_state;
static spinlock_t g_process_registry_lock;
static list_head_t g_process_registry;
/* 仅在 registry_lock 保护下借用；任何成员变更都会使其失效。 */
static list_head_t *g_process_enumeration_cursor;
static uint32_t g_process_enumeration_index;
static list_head_t *g_process_monitor_cursor;
static spinlock_t g_process_teardown_lock;
static process_teardown_callback_t
    g_process_teardown_callbacks[PROCESS_TEARDOWN_CALLBACK_MAX];
/* 注册只追加不删除；此连续前缀由对应锁保护。 */
static uint32_t g_process_teardown_count;

extern void x86_user_thread_start(void);

static void process_destroy(void *object);
static bool process_is_signaled(const void *object);
static int64_t process_wait_value(const void *object);
static void process_global_initialize(void);
static void process_orphan_children(process_t *process);
static void process_detach_from_parent(process_t *process);

static void process_registry_lock(void) {
    process_global_initialize();
    sched_preempt_disable();
    spinlock_lock(&g_process_registry_lock);
}

static void process_registry_unlock(void) {
    spinlock_unlock(&g_process_registry_lock);
    sched_preempt_enable();
}

static void process_registry_invalidate_cursors_locked(void) {
    g_process_enumeration_cursor = 0;
    g_process_enumeration_index = 0U;
    g_process_monitor_cursor = 0;
}

static void process_registry_remove(process_t *process) {
    bool removed = false;
    if (process == 0) return;
    process_registry_lock();
    if (process->global_registered) {
        process_registry_invalidate_cursors_locked();
        list_del(&process->global_node);
        process->global_registered = false;
        removed = true;
    }
    process_registry_unlock();
    if (removed) object_put(process);
}

static void process_parent_lock(process_t *process) {
    sched_preempt_disable();
    spinlock_lock(&process->parent_lock);
}

static void process_parent_unlock(process_t *process) {
    spinlock_unlock(&process->parent_lock);
    sched_preempt_enable();
}

static void process_children_lock(process_t *process) {
    sched_preempt_disable();
    spinlock_lock(&process->children_lock);
}

static void process_children_unlock(process_t *process) {
    spinlock_unlock(&process->children_lock);
    sched_preempt_enable();
}

static void process_teardown_lock(void) {
    sched_preempt_disable();
    spinlock_lock(&g_process_teardown_lock);
}

static void process_teardown_unlock(void) {
    spinlock_unlock(&g_process_teardown_lock);
    sched_preempt_enable();
}

static uint32_t process_snapshot_teardown_callbacks(
    process_teardown_callback_t *callbacks) {
    if (callbacks == 0) return 0U;
    process_teardown_lock();
    uint32_t count = g_process_teardown_count;
    __builtin_memcpy(callbacks, g_process_teardown_callbacks,
                     count * sizeof(*callbacks));
    process_teardown_unlock();
    return count;
}

static const object_ops_t g_process_ops = {
    .destroy = process_destroy,
    .type_name = "Process",
    .is_signaled = process_is_signaled,
    .wait_value = process_wait_value,
};

static process_t *process_parent_reference(process_t *process) {
    process_t *parent;
    if (process == 0) return 0;
    process_parent_lock(process);
    parent = process->parent;
    if (parent != 0) object_get(parent);
    process_parent_unlock(process);
    return parent;
}

static void process_notify_parent(process_t *process) {
    process_t *parent = process_parent_reference(process);
    if (parent == 0) return;
    (void)wake_all(&parent->child_waitq);
    object_put(parent);
}

static void process_orphan_children(process_t *process) {
    if (process == 0) return;
    for (;;) {
        process_t *child;
        bool related;

        process_children_lock(process);
        if (list_empty(&process->children)) {
            process_children_unlock(process);
            return;
        }
        child = list_entry(process->children.next, process_t, child_node);
        list_del(&child->child_node);
        process_parent_lock(child);
        related = child->parent == process;
        if (related) child->parent = 0;
        process_parent_unlock(child);
        process_children_unlock(process);

        /* 链表持有一个子进程引用，父子关系持有一个父进程引用。
         * 持有链表锁时不能释放任一引用。 */
        object_put(child);
        if (related) object_put(process);
    }
}

static void process_detach_from_parent(process_t *process) {
    process_t *parent;
    bool related = false;
    bool detached = false;

    if (process == 0) return;
    parent = process_parent_reference(process);
    if (parent == 0) return;

    /* 获取父进程子进程链表锁后重新检查关系，避免采用与
     * process_orphan_children() 相反的锁顺序。 */
    process_children_lock(parent);
    process_parent_lock(process);
    if (process->parent == parent) {
        related = true;
        if (process->child_node.next != &process->child_node) {
            list_del(&process->child_node);
            detached = true;
        }
        process->parent = 0;
    }
    process_parent_unlock(process);
    process_children_unlock(parent);

    if (detached) object_put(process); /* 父进程子进程链表持有的引用 */
    if (related) object_put(parent);   /* 子进程父子关系持有的引用 */
    /* 等待者可能正在等待一个构造失败或已中止、但仍存活的子进程。
     * 移除该子进程会像回收进程一样将等待条件变为 ECHILD。
     * 通知完成前必须保留查找引用。 */
    if (detached) (void)wake_all(&parent->child_waitq);
    object_put(parent);                /* 临时查找引用 */
}

static bool process_child_matches(const process_t *child, int64_t pid) {
    if (child == 0) return false;
    return pid == -1 || (pid > 0 && child->pid == (uint64_t)pid);
}

typedef struct process_wait_context {
    process_t *parent;
    int64_t pid;
    bool has_child;
    process_t *candidate;
} process_wait_context_t;

static bool process_wait_predicate(void *opaque) {
    process_wait_context_t *context = (process_wait_context_t *)opaque;
    context->has_child = false;
    context->candidate = 0;
    process_children_lock(context->parent);
    for (list_head_t *node = context->parent->children.next;
         node != &context->parent->children; node = node->next) {
        process_t *child = list_entry(node, process_t, child_node);
        if (!process_child_matches(child, context->pid)) continue;
        context->has_child = true;
        if (atomic_load_explicit(&child->state, memory_order_acquire) ==
            PROCESS_DEAD) {
            object_get(child);
            context->candidate = child;
            break;
        }
        /* 正 PID 指定唯一子进程。匹配到仍存活的子进程后，等待条件已确定；
         * 后续无关子进程不能改变结果。 */
        if (context->pid > 0) break;
    }
    process_children_unlock(context->parent);
    /* 子进程消失也是此等待操作的终止结果。 */
    return !context->has_child || context->candidate != 0;
}

/* 仅由持锁完成 PROCESS_DEAD 状态转换的所有者调用。
 * 调用方保留进程引用；回调和通知均在解锁后执行。 */
static void process_publish_exit(process_t *process) {
    process_registry_remove(process);
    process_orphan_children(process);
    process_teardown_callback_t callbacks[PROCESS_TEARDOWN_CALLBACK_MAX];
    uint32_t count = process_snapshot_teardown_callbacks(callbacks);
    for (uint32_t index = 0U; index < count; ++index) callbacks[index](process);
    object_notify_signaled(process);
    process_notify_parent(process);
}

bool process_begin_exit(process_t *process, int64_t status, uint32_t signal) {
    if (process == 0) return false;

    bool first_exit = false;
    bool empty = false;
    process_thread_lock(process);
    if (atomic_load_explicit(&process->state, memory_order_acquire) ==
        PROCESS_RUNNING) {
        process->exit_code = status;
        process->exit_signal = signal;
        empty = process->thread_count == 0U;
        atomic_store_explicit(&process->state,
                              empty ? PROCESS_DEAD : PROCESS_EXITING,
                              memory_order_release);
        first_exit = true;
    }
    process_thread_unlock(process);
    if (empty) {
        process_publish_exit(process);
        /* 没有执行回收器负责释放空进程的资源。 */
        process_release_runtime_resources(process);
    }
    return first_exit;
}

void process_detach_thread(thread_t *thread) {
    process_t *process = thread != 0 ? thread->process : 0;
    if (process == 0) return;
    bool notify_process = false;
    process_thread_lock(process);
    if (thread->process_node.next != &thread->process_node) {
        uint64_t runtime = __atomic_load_n(&thread->sched.runtime_ns, __ATOMIC_RELAXED);
        process->retired_cpu_ns = process->retired_cpu_ns > UINT64_MAX - runtime ?
                                 UINT64_MAX : process->retired_cpu_ns + runtime;
        if (process->monitor_thread_cursor == &thread->process_node)
            process->monitor_thread_cursor = 0;
        /* 增删会改变后续线程的枚举索引，且缓存节点可能即将释放。 */
        process->enumeration_thread_cursor = 0;
        list_del(&thread->process_node);
        if (process->thread_count != 0) --process->thread_count;
        if (process->thread_count == 0 &&
            (process->flags & PROCESS_FLAG_EVER_HAD_THREAD) != 0) {
            unsigned state = atomic_load_explicit(&process->state, memory_order_acquire);
            if (state == PROCESS_RUNNING) process->exit_code = thread->exit_code;
            if (state == PROCESS_RUNNING || state == PROCESS_EXITING) {
                atomic_store_explicit(&process->state, PROCESS_DEAD, memory_order_release);
                notify_process = true;
            }
        }
    }
    process_thread_unlock(process);
    if (notify_process) process_publish_exit(process);
}

static bool process_is_signaled(const void *object) {
    const process_t *process = (const process_t *)object;
    return atomic_load_explicit(&process->state, memory_order_acquire) == PROCESS_DEAD;
}

static int64_t process_wait_value(const void *object) {
    return ((const process_t *)object)->exit_code;
}

void process_set_name(process_t *process, const char *path) {
    const char *base;
    size_t length = 0U;
    if (process == 0 || path == 0 || *path == '\0') return;
    base = path;
    for (const char *cursor = path; *cursor != '\0'; ++cursor) {
        if (*cursor == '/') base = cursor + 1U;
    }
    while (base[length] != '\0' && length + 1U < OS_PROCESS_NAME_MAX) ++length;
    if (length == 0U) return;
    sched_preempt_disable();
    spinlock_lock(&process->info_lock);
    for (size_t index = 0U; index < OS_PROCESS_NAME_MAX; ++index) {
        process->name[index] = index < length ? base[index] : '\0';
    }
    process->name[length] = '\0';
    spinlock_unlock(&process->info_lock);
    sched_preempt_enable();
}

/* 路径快照和替换只复制固定大小的数据，不在自旋锁下解析路径或读取磁盘。 */
kstatus_t process_cwd_copy(process_t *process, char output[PROCESS_CWD_CAPACITY]) {
    if (process == 0 || output == 0) return K_EINVAL;
    sched_preempt_disable();
    spinlock_lock(&process->info_lock);
    memcpy(output, process->cwd, PROCESS_CWD_CAPACITY);
    spinlock_unlock(&process->info_lock);
    sched_preempt_enable();
    return K_OK;
}

kstatus_t process_cwd_set(process_t *process, const char *path) {
    if (process == 0 || path == 0 || path[0] != '/') return K_EINVAL;
    size_t length = 0U;
    while (length < PROCESS_CWD_CAPACITY && path[length] != '\0') ++length;
    if (length >= PROCESS_CWD_CAPACITY) return K_ENAMETOOLONG;
    sched_preempt_disable();
    spinlock_lock(&process->info_lock);
    memcpy(process->cwd, path, length + 1U);
    spinlock_unlock(&process->info_lock);
    sched_preempt_enable();
    return K_OK;
}

/* 调用方拥有或保留 process；读取名称无需注册表锁或线程锁。 */
static void process_snapshot_name(process_t *process, char *name, size_t capacity) {
    size_t copied = capacity < OS_PROCESS_NAME_MAX ? capacity : OS_PROCESS_NAME_MAX;
    sched_preempt_disable();
    spinlock_lock(&process->info_lock);
    memcpy(name, process->name, copied);
    spinlock_unlock(&process->info_lock);
    sched_preempt_enable();
    if (capacity > copied) memset(name + copied, 0, capacity - copied);
}

static void process_copy_name(process_t *destination,
                              process_t *source) {
    char name[OS_PROCESS_NAME_MAX];
    if (destination == 0 || source == 0) return;
    process_snapshot_name(source, name, sizeof(name));
    process_set_name(destination, name);
}

kstatus_t process_enumerate(uint32_t index, os_process_snapshot_t *snapshot) {
    uint32_t current = 0U;
    if (snapshot == 0) return K_EINVAL;
    process_registry_lock();
    list_head_t *start = g_process_registry.next;
    if (g_process_enumeration_cursor != 0 &&
        index >= g_process_enumeration_index) {
        start = g_process_enumeration_cursor;
        current = g_process_enumeration_index;
        if (index > current) {
            start = start->next;
            ++current;
        }
    }
    for (list_head_t *node = start;
         node != &g_process_registry; node = node->next) {
        process_t *process = list_entry(node, process_t, global_node);
        if (current++ != index) continue;
        g_process_enumeration_cursor = node;
        g_process_enumeration_index = index;
        object_get(process);
        process_registry_unlock();
        snapshot->hdr.size = sizeof(*snapshot);
        snapshot->hdr.version = OS_SYSCALL_ABI_VERSION;
        snapshot->hdr.flags = 0U;
        snapshot->pid = process->pid;
        process_parent_lock(process);
        snapshot->parent_pid = process->parent != 0 ? process->parent->pid : 0U;
        process_parent_unlock(process);
        process_thread_lock(process);
        snapshot->state = atomic_load_explicit(&process->state, memory_order_acquire);
        snapshot->thread_count = process->thread_count;
        snapshot->exit_code = process->exit_code;
        process_thread_unlock(process);
        snapshot->create_time_ns = process->create_time_ns;
        process_snapshot_name(process, snapshot->name, sizeof(snapshot->name));
        object_put(process);
        return K_OK;
    }
    process_registry_unlock();
    return current == index ? sched_monitor_process(snapshot) : K_ENOENT;
}

kstatus_t process_enumerate_thread(uint32_t index,
                                   os_thread_snapshot_t *snapshot) {
    uint32_t remaining = index;
    if (snapshot == 0) return K_EINVAL;
    process_registry_lock();
    for (list_head_t *process_node = g_process_registry.next;
         process_node != &g_process_registry; process_node = process_node->next) {
        process_t *process = list_entry(process_node, process_t, global_node);
        process_thread_lock(process);
        /* 成员关系和数量共用 thread_lock。按索引查询时跳过前面的整个列表，避免逐个遍历线程。
         * 使用减法也可避免累加全局数量时溢出。 */
        if (remaining >= process->thread_count) {
            remaining -= process->thread_count;
            process_thread_unlock(process);
            continue;
        }
        /* 首、尾和上次命中的节点都在同一线程锁内；选择距离最近的起点。
         * 顺序采样时只前进一个节点，任意索引仍保持原来的正向枚举顺序。 */
        uint32_t target = remaining, distance = target;
        list_head_t *start = process->threads.next;
        bool reverse = false;
        uint32_t tail_distance = process->thread_count - 1U - target;
        if (tail_distance < distance) {
            start = process->threads.prev;
            distance = tail_distance;
            reverse = true;
        }
        if (process->enumeration_thread_cursor != 0) {
            uint32_t cached = process->enumeration_thread_index;
            uint32_t cached_distance = target >= cached ? target - cached : cached - target;
            if (cached < process->thread_count && cached_distance < distance) {
                start = process->enumeration_thread_cursor;
                distance = cached_distance;
                reverse = target < cached;
            }
        }
        remaining = distance;
        for (list_head_t *thread_node = start;
             thread_node != &process->threads;
             thread_node = reverse ? thread_node->prev : thread_node->next) {
            if (remaining-- != 0U) continue;
            thread_t *thread = list_entry(thread_node, thread_t, process_node);
            process->enumeration_thread_cursor = thread_node;
            process->enumeration_thread_index = target;
            process->monitor_thread_cursor = thread_node;
            snapshot->hdr.size = sizeof(*snapshot);
            snapshot->hdr.version = OS_SYSCALL_ABI_VERSION;
            snapshot->hdr.flags = 0U;
            snapshot->tid = thread->tid;
            snapshot->process_pid = process->pid;
            snapshot->state = atomic_load_explicit(&thread->state,
                                                   memory_order_acquire);
            snapshot->current_cpu = thread->current_cpu;
            /* 快照不再需要线程本身。读取名称时只保留其所属进程，且不持有全局列表锁。 */
            object_get(process);
            /* 下一次监控查询通常会直接使用此快照。在注册表成员锁仍持有时设置 PID 提示；移除
             * 成员时会先使提示失效，再释放注册表所有权。 */
            g_process_monitor_cursor = &process->global_node;
            process_thread_unlock(process);
            process_registry_unlock();
            process_snapshot_name(process, snapshot->name, sizeof(snapshot->name));
            object_put(process);
            return K_OK;
        }
        process_thread_unlock(process);
    }
    process_registry_unlock();
    return sched_monitor_thread(remaining, snapshot);
}

static process_t *process_monitor_find(uint64_t pid) {
    process_t *found = 0;
    process_registry_lock();
    /* 资源查询通常紧跟枚举操作，或针对线程重复查询同一父 PID。两个借用提示仅在持有此锁时有效。 */
    list_head_t *hint = g_process_monitor_cursor;
    if (hint == 0 || list_entry(hint, process_t, global_node)->pid != pid) {
        hint = g_process_enumeration_cursor;
    }
    if (hint != 0 && list_entry(hint, process_t, global_node)->pid == pid) {
        found = list_entry(hint, process_t, global_node);
    } else {
        for (list_head_t *node = g_process_registry.next; node != &g_process_registry; node = node->next) {
            process_t *candidate = list_entry(node, process_t, global_node);
            if (candidate->pid == pid) { found = candidate; break; }
        }
    }
    if (found != 0) {
        object_get(found);
        g_process_monitor_cursor = &found->global_node;
    }
    process_registry_unlock();
    return found;
}

/* 调用方持有 thread_lock，包括 VM 生命周期保护。 */
static bool process_monitor_protected(const process_t *process) {
    return process->pid <= 1U || process->vm == 0 ||
        (__atomic_load_n(&process->flags, __ATOMIC_RELAXED) & PROCESS_FLAG_INIT_CPU_PINNED) != 0U;
}

static bool process_monitor_terminable(const process_t *process, uint64_t create_time_ns) {
    return !process_monitor_protected(process) &&
        process->create_time_ns == create_time_ns &&
        atomic_load_explicit(&process->state, memory_order_acquire) == PROCESS_RUNNING;
}

/* 返回的线程指针为借用指针，只能在持有 thread_lock 时使用。 */
static thread_t *process_monitor_thread_locked(process_t *process, uint64_t tid) {
    list_head_t *hint = process->monitor_thread_cursor;
    if (hint != 0) {
        thread_t *thread = list_entry(hint, thread_t, process_node);
        if (thread->tid == tid) return thread;
    }
    for (list_head_t *node = process->threads.next; node != &process->threads; node = node->next) {
        thread_t *thread = list_entry(node, thread_t, process_node);
        if (thread->tid == tid) {
            process->monitor_thread_cursor = node;
            return thread;
        }
    }
    return 0;
}

uint64_t linux_thread_visible_tid(const thread_t *thread) {
    process_t *process = thread->process;
    return process != 0 && linux_process_is(process) &&
           thread->tid == process->linux_thread_group_leader_tid ?
           process->pid : thread->tid;
}

static thread_t *process_linux_thread_reference(process_t *process, uint64_t tid) {
    thread_t *found = 0;
    process_thread_lock(process);
    for (list_head_t *node = process->threads.next;
         node != &process->threads; node = node->next) {
        thread_t *thread = list_entry(node, thread_t, process_node);
        if (linux_thread_visible_tid(thread) == tid &&
            atomic_load_explicit(&thread->state, memory_order_acquire) != THREAD_DEAD) {
            object_get(thread);
            found = thread;
            break;
        }
    }
    process_thread_unlock(process);
    return found;
}

thread_t *linux_thread_reference_by_tid(uint64_t tid) {
    thread_t *current = sched_current_thread();
    if (tid == 0U || (current != 0 && linux_thread_visible_tid(current) == tid)) {
        if (current != 0) object_get(current);
        return current;
    }
    /* 常见的 pthread 查询仅涉及当前进程。查询外部任务时，锁顺序与枚举/移除一致：先获取注册表锁，
     * 再获取 thread_lock。 */
    thread_t *found = current != 0 && current->process != 0 ?
        process_linux_thread_reference(current->process, tid) : 0;
    if (found != 0) return found;
    process_registry_lock();
    for (list_head_t *node = g_process_registry.next;
         node != &g_process_registry; node = node->next) {
        process_t *process = list_entry(node, process_t, global_node);
        if (current != 0 && process == current->process) continue;
        found = process_linux_thread_reference(process, tid);
        if (found != 0) break;
    }
    process_registry_unlock();
    return found;
}

kstatus_t process_task_query(uint64_t pid, uint64_t tid, os_task_info_t *info) {
    if (info == 0 || pid == 0U) return K_EINVAL;
    if (pid == OS_KERNEL_WORKERS_PID) return sched_monitor_query(tid, info);
    process_t *process = process_monitor_find(pid);
    if (process == 0) return K_ENOENT;
    *info = (os_task_info_t){0};
    info->hdr.size = sizeof(*info); info->hdr.version = OS_SYSCALL_ABI_VERSION;
    info->pid = pid; info->tid = tid; info->create_time_ns = process->create_time_ns;
    info->cpu_count = x86_smp_started_count();
    if (info->cpu_count == 0U) info->cpu_count = 1U;
    process_thread_lock(process);
    info->state = atomic_load_explicit(&process->state, memory_order_acquire);
    info->thread_count = process->thread_count;
    info->flags = process_monitor_protected(process) ? OS_TASK_PROTECTED : 0U;
    if (process->vm != 0) {
        info->rss_bytes = atomic_load_explicit(&process->vm->rss_pages, memory_order_relaxed) * PAGE_SIZE;
        info->committed_bytes = atomic_load_explicit(&process->vm->commit_pages, memory_order_relaxed) * PAGE_SIZE;
    }
    info->runtime_ns = tid == 0U ? process->retired_cpu_ns : 0U;
    bool found = true;
    if (tid != 0U) {
        thread_t *thread = process_monitor_thread_locked(process, tid);
        found = thread != 0;
        if (found) {
            info->runtime_ns = __atomic_load_n(&thread->sched.runtime_ns, __ATOMIC_RELAXED);
            info->state = atomic_load_explicit(&thread->state, memory_order_acquire);
        }
    } else {
        for (list_head_t *node = process->threads.next;
             node != &process->threads && info->runtime_ns != UINT64_MAX; node = node->next) {
            thread_t *thread = list_entry(node, thread_t, process_node);
            uint64_t runtime = __atomic_load_n(&thread->sched.runtime_ns, __ATOMIC_RELAXED);
            info->runtime_ns = info->runtime_ns > UINT64_MAX - runtime ? UINT64_MAX : info->runtime_ns + runtime;
        }
    }
    process_thread_unlock(process);
    object_put(process);
    return found ? K_OK : K_ENOENT;
}

/* 内存不足时使用无需分配的回退路径；解锁后，任何活动线程指针都必须有对象引用保护。 */
static void process_terminate_threads_fallback(process_t *process, thread_t *exclude, int64_t status) {
    uint64_t after_tid = 0U;
    for (;;) {
        thread_t *targets[16];
        uint32_t count = 0U;
        process_thread_lock(process);
        for (list_head_t *node = process->threads.next; node != &process->threads; node = node->next) {
            thread_t *candidate = list_entry(node, thread_t, process_node);
            if (candidate == exclude || atomic_load_explicit(&candidate->state, memory_order_acquire) == THREAD_DEAD) continue;
            if (candidate->tid <= after_tid) continue;
            /* 列表不必按 TID 排序。保留其中最小的后续 ID；解锁后不保留列表游标或无引用保护的指针。 */
            uint32_t position = count;
            while (position != 0U && targets[position - 1U]->tid > candidate->tid) --position;
            if (position == 16U) continue;
            if (count < 16U) ++count;
            for (uint32_t index = count - 1U; index > position; --index) {
                targets[index] = targets[index - 1U];
            }
            targets[position] = candidate;
        }
        for (uint32_t index = 0U; index < count; ++index) {
            object_get(targets[index]);
        }
        if (count != 0U) after_tid = targets[count - 1U]->tid;
        process_thread_unlock(process);
        for (uint32_t index = 0U; index < count; ++index) {
            (void)thread_terminate(targets[index], status);
            object_put(targets[index]);
        }
        if (count < 16U) break;
    }
}

void process_terminate_siblings(process_t *process, thread_t *exclude, int64_t status) {
    /* EXITING 状态禁止新增关联。在线程锁外分配快照存储期间，线程数只会减少。 */
    thread_t *local_targets[16];
    thread_t **targets = local_targets;
    process_thread_lock(process);
    uint32_t capacity = process->thread_count;
    process_thread_unlock(process);
    if (capacity > sizeof(local_targets) / sizeof(local_targets[0])) {
        targets = kmalloc((size_t)capacity * sizeof(*targets), 0U);
    }
    uint32_t count = 0U;
    process_thread_lock(process);
    for (list_head_t *node = process->threads.next; node != &process->threads; node = node->next) {
        thread_t *candidate = list_entry(node, thread_t, process_node);
        if (candidate == exclude || atomic_load_explicit(&candidate->state, memory_order_acquire) == THREAD_DEAD) continue;
        __atomic_fetch_or(&candidate->signal_pending, 1ULL << (OS_SIGKILL - 1U), __ATOMIC_RELEASE);
        if (targets != 0) {
            object_get(candidate);
            targets[count++] = candidate;
        }
    }
    process_thread_unlock(process);
    if (targets == 0) {
        process_terminate_threads_fallback(process, exclude, status);
    } else {
        for (uint32_t index = 0U; index < count; ++index) {
            (void)thread_terminate(targets[index], status);
            object_put(targets[index]);
        }
        if (targets != local_targets) kfree(targets);
    }
}

kstatus_t process_terminate_pid(process_t *caller, uint64_t pid, uint64_t create_time_ns) {
    if (caller == 0 || pid <= 1U || pid == caller->pid) return K_EPERM;
    process_t *process = process_monitor_find(pid);
    if (process == 0) return K_ENOENT;
    process_thread_lock(process);
    bool allowed = process_monitor_terminable(process, create_time_ns);
    process_thread_unlock(process);
    if (!allowed) { object_put(process); return K_EPERM; }
    if (process_begin_exit(process, -(int64_t)OS_SIGKILL, OS_SIGKILL)) {
        process_terminate_siblings(process, 0, -(int64_t)OS_SIGKILL);
    }
    object_put(process);
    return K_OK;
}

kstatus_t process_terminate_tid(process_t *caller, uint64_t pid, uint64_t tid, uint64_t create_time_ns) {
    if (caller == 0 || pid <= 1U || pid == caller->pid || tid == 0U) return K_EPERM;
    process_t *process = process_monitor_find(pid);
    if (process == 0) return K_ENOENT;
    thread_t *target = 0;
    process_thread_lock(process);
    bool allowed = process_monitor_terminable(process, create_time_ns);
    if (allowed) {
        thread_t *candidate = process_monitor_thread_locked(process, tid);
        if (candidate != 0) {
            object_get(candidate);
            __atomic_fetch_or(&candidate->flags, THREAD_FLAG_TERMINATE_REQUEST, __ATOMIC_RELEASE);
            target = candidate;
        }
    }
    process_thread_unlock(process);
    if (target != 0) {
        /* 运行中的线程只能在安全的用户态边界退出，绝不能通过回收任意活动内核续接路径来退出。 */
        (void)thread_terminate(target, -(int64_t)OS_SIGKILL);
        object_put(target);
    }
    object_put(process);
    return !allowed ? K_EPERM : target == 0 ? K_ENOENT : K_OK;
}

static uint64_t read_tsc(void) {
    uint32_t low;
    uint32_t high;
    __asm__ volatile ("rdtsc" : "=a"(low), "=d"(high));
    return ((uint64_t)high << 32) | low;
}

static void process_global_initialize(void) {
    unsigned expected = 0U;
    if (atomic_compare_exchange_strong_explicit(&g_process_init_state, &expected, 1U,
                                                 memory_order_acq_rel,
                                                 memory_order_acquire)) {
        atomic_init(&g_next_task_id, 1U);
        spinlock_init(&g_process_registry_lock);
        list_init(&g_process_registry);
        spinlock_init(&g_process_teardown_lock);
        atomic_store_explicit(&g_process_init_state, 2U, memory_order_release);
        return;
    }
    while (atomic_load_explicit(&g_process_init_state, memory_order_acquire) != 2U) {
        __asm__ volatile ("pause");
    }
}

uint64_t process_allocate_task_id(void) {
    process_global_initialize();
    return atomic_fetch_add_explicit(&g_next_task_id, 1U,
                                     memory_order_relaxed);
}

kstatus_t process_register_teardown_callback(
    process_teardown_callback_t callback) {
    if (callback == 0) return K_EINVAL;
    process_global_initialize();
    process_teardown_lock();
    for (uint32_t index = 0U;
         index < g_process_teardown_count; ++index) {
        if (g_process_teardown_callbacks[index] == callback) {
            process_teardown_unlock();
            return K_OK;
        }
    }
    if (g_process_teardown_count < PROCESS_TEARDOWN_CALLBACK_MAX) {
        g_process_teardown_callbacks[g_process_teardown_count++] = callback;
        process_teardown_unlock();
        return K_OK;
    }
    process_teardown_unlock();
    return K_ENOSPC;
}

void process_initialize_object(object_header_t *header, object_type_id_t type,
                               const object_ops_t *ops) {
    object_header_init(header, type, ops);
}

static kstatus_t process_create_vm(process_t *parent, vm_space_t **out) {
    if (parent == 0) return vm_space_create(out);
    /* 分配子进程期间，父进程退出可能解除其 VM 关联。在所有者锁下固定父进程快照，但绝不在该锁下
     * 执行克隆或释放操作。 */
    process_thread_lock(parent);
    vm_space_t *source = parent->vm;
    if (source != 0) vm_space_get(source);
    process_thread_unlock(parent);
    if (source == 0) return K_EINVAL;
    kstatus_t status = vm_space_clone_cow(source, out);
    vm_space_put(source);
    return status;
}

kstatus_t process_create_named(process_t *parent, const char *name,
                               process_t **out) {
    if (out == 0) return K_EINVAL;
    process_global_initialize();
    process_t *process = (process_t *)kzalloc(sizeof(process_t), 0);
    if (process == 0) return K_ENOMEM;
    process_initialize_object(&process->object, KOBJECT_TYPE_PROCESS,
                              &g_process_ops);
    process->pid = process_allocate_task_id();
    atomic_init(&process->state, PROCESS_NEW);
    /* CPU 固定仅适用于引导进程本身。若继承该限制，所有后代（包括多线程应用）都会被固定到引导
     * CPU，从而破坏 SMP 调度。 */
    atomic_init(&process->parent_lock.state, 0U);
    atomic_init(&process->children_lock.state, 0U);
    wait_queue_init(&process->child_waitq);
    list_init(&process->children);
    list_init(&process->child_node);
    atomic_init(&process->thread_lock.state, 0U);
    atomic_init(&process->signal_lock.state, 0U);
    for (uint32_t signal = 0U; signal <= OS_SIGNAL_COUNT; ++signal) {
        process->signal_actions[signal].handler = OS_SIG_DFL;
    }
    list_init(&process->threads);
    list_init(&process->job_node);
    list_init(&process->global_node);
    spinlock_init(&process->info_lock);
    process->global_registered = false;
    if (name != 0 && *name != '\0') process_set_name(process, name);
    else if (parent != 0) process_copy_name(process, parent);
    else process_set_name(process, "system");
    process->create_time_ns = read_tsc();
    process->personality = parent != 0 ? parent->personality :
                          OS_PROCESS_PERSONALITY_NATIVE;
    process->linux_loader_pending = false;
    process->linux_root[0] = '/';
    process->linux_root[1] = '\0';
    process->cwd[0] = '/';
    process->cwd[1] = '\0';
    process->linux_exe[0] = '\0';
    process->linux_brk_base = 0U;
    process->linux_brk = 0U;
    if (parent != 0) {
        __builtin_memcpy(process->linux_root, parent->linux_root,
                         sizeof(process->linux_root));
        (void)process_cwd_copy(parent, process->cwd);
        __builtin_memcpy(process->linux_exe, parent->linux_exe,
                         sizeof(process->linux_exe));
        process->linux_brk_base = parent->linux_brk_base;
        process->linux_brk = parent->linux_brk;
    }
    linux_fd_table_init(&process->linux_fds);

    kstatus_t status = process_create_vm(parent, &process->vm);
    if (status != K_OK) {
        kfree(process);
        return status;
    }
    status = handle_table_init(&process->handles);
    if (status != K_OK) {
        vm_space_put(process->vm);
        linux_fd_table_destroy(&process->linux_fds);
        kfree(process);
        return status;
    }
    process->parent = 0;
    if (parent != 0) {
        process_parent_lock(process);
        process->parent = parent;
        object_get(parent);
        process_parent_unlock(process);
        status = parent->job != 0 ? job_attach_process(parent->job, process) : K_OK;
        if (status != K_OK) {
            process_detach_from_parent(process);
            handle_table_destroy(&process->handles);
            vm_space_put(process->vm);
            linux_fd_table_destroy(&process->linux_fds);
            kfree(process);
            return status;
        }
        process_children_lock(parent);
        if (atomic_load_explicit(&parent->state, memory_order_acquire) !=
            PROCESS_RUNNING) {
            process_children_unlock(parent);
            (void)job_detach_process(process);
            process_detach_from_parent(process);
            handle_table_destroy(&process->handles);
            vm_space_put(process->vm);
            linux_fd_table_destroy(&process->linux_fds);
            kfree(process);
            return K_EBUSY;
        }
        object_get(process);
        list_add_tail(&parent->children, &process->child_node);
        process_children_unlock(parent);
    } else {
        job_t *root_job = 0;
        status = job_create(0, 0, &root_job);
        if (status == K_OK) status = job_attach_process(root_job, process);
        if (root_job != 0) object_put(root_job);
        if (status != K_OK) {
            handle_table_destroy(&process->handles);
            linux_fd_table_destroy(&process->linux_fds);
            vm_space_put(process->vm);
            kfree(process);
            return status;
        }
    }
    atomic_store_explicit(&process->state, PROCESS_RUNNING, memory_order_release);
    process_registry_lock();
    object_get(process);
    process_registry_invalidate_cursors_locked();
    list_add_tail(&g_process_registry, &process->global_node);
    process->global_registered = true;
    process_registry_unlock();
    *out = process;
    return K_OK;
}

kstatus_t process_create(process_t *parent, process_t **out) {
    return process_create_named(parent, 0, out);
}

static void process_destroy(void *object) {
    process_t *process = (process_t *)object;
    process_registry_remove(process);
    atomic_store_explicit(&process->state, PROCESS_DEAD, memory_order_release);
    process_orphan_children(process);
    process_detach_from_parent(process);
    (void)job_detach_process(process);
    process_release_runtime_resources(process);
    kfree(process);
}

kstatus_t process_abort(process_t *process) {
    if (process == 0) return K_EINVAL;
    atomic_store_explicit(&process->state, PROCESS_DEAD, memory_order_release);
    process_registry_remove(process);
    process_orphan_children(process);
    process_detach_from_parent(process);
    (void)job_detach_process(process);
    process_release_runtime_resources(process);
    return K_OK;
}

kstatus_t process_fork(process_t *parent, process_t **out) {
    if (parent == 0 || out == 0) return K_EINVAL;
    process_t *child = 0;
    kstatus_t status = process_create(parent, &child);
    if (status != K_OK) return status;

    handle_table_t cloned = {0};
    /* 窗口是绑定所有者的资源。fork 出来的命令不得继承 shell 的窗口句柄，否则退出时会关闭父进程窗口。 */
    status = handle_table_clone_without_type(&parent->handles, &cloned,
                                             KOBJECT_TYPE_WINDOW);
    if (status != K_OK) {
        (void)process_abort(child);
        object_put(child);
        return status;
    }
    handle_table_destroy(&child->handles);
    child->handles = cloned;
    status = linux_fd_table_clone(&parent->linux_fds, &child->linux_fds);
    if (status != K_OK) {
        (void)process_abort(child);
        object_put(child);
        return status;
    }
    spinlock_lock(&parent->signal_lock);
    spinlock_lock(&child->signal_lock);
    for (uint32_t signal = 0U; signal <= OS_SIGNAL_COUNT; ++signal) {
        child->signal_actions[signal] = parent->signal_actions[signal];
    }
    spinlock_unlock(&child->signal_lock);
    spinlock_unlock(&parent->signal_lock);
    *out = child;
    return K_OK;
}

uint64_t process_parent_pid(const process_t *process) {
    uint64_t pid = 0;
    if (process == 0) return 0;
    process_parent_lock((process_t *)process);
    if (process->parent != 0) pid = process->parent->pid;
    process_parent_unlock((process_t *)process);
    return pid;
}

kstatus_t process_wait_child(process_t *parent, int64_t pid, uint32_t options,
                             int64_t *child_pid, int64_t *exit_code,
                             uint32_t *exit_signal) {
    if (parent == 0 || child_pid == 0 || exit_code == 0 || exit_signal == 0 ||
        (options & ~OS_PROCESS_WAIT_NOHANG) != 0U ||
        (pid != -1 && pid <= 0)) return K_EINVAL;

    process_wait_context_t context = {
        .parent = parent,
        .pid = pid,
        .has_child = false,
        .candidate = 0,
    };
    uint64_t timeout = (options & OS_PROCESS_WAIT_NOHANG) != 0U ? 0U :
                       OS_WAIT_INFINITE;
    for (;;) {
        kstatus_t status = wait_on_queue(&parent->child_waitq,
                                          process_wait_predicate, &context,
                                          timeout);
        if (status == K_ETIMEDOUT) {
            if (!context.has_child) return K_ECHILD;
            *child_pid = 0;
            *exit_code = 0;
            *exit_signal = 0U;
            return K_OK;
        }
        if (status != K_OK) return status;
        if (!context.has_child) return K_ECHILD;

        process_t *child = context.candidate;
        bool reaped = false;
        int64_t status_value = 0;
        uint32_t signal_value = 0U;
        uint64_t result_pid = 0;
        process_children_lock(parent);
        process_parent_lock(child);
        if (child->parent == parent &&
            child->child_node.next != &child->child_node &&
            atomic_load_explicit(&child->state, memory_order_acquire) ==
                PROCESS_DEAD) {
            list_del(&child->child_node);
            child->parent = 0;
            result_pid = child->pid;
            status_value = child->exit_code;
            signal_value = child->exit_signal;
            reaped = true;
        }
        process_parent_unlock(child);
        process_children_unlock(parent);
        if (!reaped) {
            object_put(child);
            timeout = (options & OS_PROCESS_WAIT_NOHANG) != 0U ? 0U :
                      OS_WAIT_INFINITE;
            continue;
        }
        object_put(parent); /* 子进程父子关系引用 */
        object_put(child);  /* 子进程列表引用 */
        object_put(child);  /* 条件判断引用 */
        (void)wake_all(&parent->child_waitq);
        *child_pid = (int64_t)result_pid;
        *exit_code = status_value;
        *exit_signal = signal_value;
        return K_OK;
    }
}

void process_release_runtime_resources(process_t *process) {
    if (process == 0) return;
    /* 此标志表示单调递增的所有权声明，不是完成屏障。重复执行回收操作时，无需再次写入已声明的缓存行。 */
    if ((__atomic_load_n(&process->flags, __ATOMIC_ACQUIRE) &
         PROCESS_FLAG_RESOURCES_RELEASED) != 0U) return;
    uint32_t old_flags = __atomic_fetch_or(&process->flags,
                                           PROCESS_FLAG_RESOURCES_RELEASED,
                                           __ATOMIC_ACQ_REL);
    if ((old_flags & PROCESS_FLAG_RESOURCES_RELEASED) != 0) return;
    handle_table_destroy(&process->handles);
    file_lock_release_owner(process->pid);
    linux_fd_table_destroy(&process->linux_fds);
    process_thread_lock(process);
    vm_space_t *space = process->vm;
    process->vm = 0;
    process_thread_unlock(process);
    if (space != 0) vm_space_put(space);
}


static bool process_destruction_self_test_round(void) {
    process_t *parent = 0;
    process_t *child = 0;
    thread_t *thread = 0;
    handle_t child_handle = 0;
    void *lookup = 0;
    bool success = false;
    if (process_create(0, &parent) != K_OK) goto cleanup;
    /* 子进程不得继承仅供引导阶段使用的 CPU 限制。 */
    parent->flags |= PROCESS_FLAG_INIT_CPU_PINNED;
    if (thread_create_user(parent, 0x400000ULL, 0x800000ULL, 0, &thread) != K_OK ||
        parent->thread_count != 1U || thread->process != parent ||
         process_create(parent, &child) != K_OK || child->parent != parent ||
         (child->flags & PROCESS_FLAG_INIT_CPU_PINNED) != 0U ||
         parent->pid == child->pid || parent->pid == thread->tid ||
         child->pid == thread->tid ||
         handle_create(&parent->handles, child, 1U, &child_handle) != K_OK ||
        handle_lookup(&parent->handles, child_handle, 1U, &lookup) != K_OK ||
        lookup != child || handle_close(&parent->handles, child_handle) != K_OK ||
        handle_close(&parent->handles, child_handle) != K_ENOENT) goto cleanup;
    child_handle = 0;
    object_put(lookup);
    lookup = 0;
    if (thread_terminate(thread, K_ECANCELED) != K_OK) goto cleanup;
    uint32_t signaled_index = UINT32_MAX;
    int64_t wait_value = 0;
    void *wait_objects[2] = {thread, parent};
    if (parent->thread_count != 0U || !object_is_signaled(thread) ||
        !object_is_signaled(parent) || parent->exit_code != K_ECANCELED ||
        object_wait_many(wait_objects, 2U, true, 0, &signaled_index, &wait_value) != K_OK ||
        signaled_index != UINT32_MAX) goto cleanup;
    object_put(thread);
    thread = 0;
    success = true;

cleanup:
    if (lookup != 0) object_put(lookup);
    if (thread != 0) {
        (void)thread_terminate(thread, K_ECANCELED);
        object_put(thread);
    }
    if (child_handle != 0) (void)handle_close(&parent->handles, child_handle);
    if (child != 0) {
        (void)process_abort(child);
        object_put(child);
    }
    if (parent != 0) {
        if (thread == 0) (void)process_abort(parent);
        object_put(parent);
    }
    return success;
}

bool process_core_self_test(void) {
    process_t exit_probe = {0};
    spinlock_init(&exit_probe.thread_lock);
    atomic_init(&exit_probe.state, PROCESS_RUNNING);
    exit_probe.thread_count = 1U;
    if (!process_begin_exit(&exit_probe, -9, 9U) ||
        process_begin_exit(&exit_probe, -15, 15U) ||
        atomic_load_explicit(&exit_probe.state, memory_order_acquire) !=
            PROCESS_EXITING ||
        exit_probe.exit_code != -9 || exit_probe.exit_signal != 9U) {
        return false;
    }

    /* 覆盖真实的注册表/资源路径：新建空进程终止后，不会有线程回收器再访问它。 */
    process_t *empty = 0;
    if (process_create(0, &empty) != K_OK) return false;
    bool empty_exited = process_begin_exit(empty, -9, 9U) &&
        !process_begin_exit(empty, -15, 15U) &&
        atomic_load_explicit(&empty->state, memory_order_acquire) == PROCESS_DEAD &&
        empty->exit_code == -9 && empty->exit_signal == 9U &&
        empty->vm == 0 && !empty->global_registered &&
        (empty->flags & PROCESS_FLAG_RESOURCES_RELEASED) != 0U &&
        object_is_signaled(empty);
    if (!empty_exited) (void)process_abort(empty);
    object_put(empty);
    if (!empty_exited) return false;

    /* 多轮验证进程、线程、句柄的销毁以及引用计数不会累积泄漏。 */
    for (uint32_t round = 0; round < 32U; ++round) {
        if (!process_destruction_self_test_round()) return false;
    }
    return true;
}
