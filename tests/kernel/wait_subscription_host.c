#define _POSIX_C_SOURCE 200809L

#include <kernel/mm.h>
#include <kernel/process.h>
#include <kernel/sched.h>
#include <kernel/wait.h>
#include <arch/x86_64/cpu.h>

#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <time.h>

typedef struct host_wait_job host_wait_job_t;

struct host_wait_job {
    pthread_t pthread;
    pthread_mutex_t lock;
    pthread_cond_t condition;
    thread_t thread;
    void *const *objects;
    completion_t *completion;
    bool uninterruptible;
    size_t count;
    bool wait_all;
    uint64_t timeout_ns;
    unsigned signal_on_block;
    bool sleeping;
    bool wake_pending;
    bool hold_next_wake;
    bool wake_held;
    bool release_wake;
    bool suppress_running_restore;
    uint32_t schedule_entries;
    bool done;
    kstatus_t status;
    uint32_t signaled_index;
    int64_t wait_value;
};

static _Thread_local struct host_wait_job *g_current_job;
static _Thread_local unsigned g_preempt_depth;
static pthread_once_t g_timer_once = PTHREAD_ONCE_INIT;
static atomic_bool g_fail_next_allocation;
static atomic_uint g_fail_allocation_countdown;
static atomic_uint g_allocation_calls;

static uint64_t monotonic_nanoseconds(void) {
    struct timespec now;
    (void)clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * 1000000000ULL + (uint64_t)now.tv_nsec;
}

static void deadline_after_ms(struct timespec *deadline, uint64_t timeout_ms) {
    (void)clock_gettime(CLOCK_REALTIME, deadline);
    deadline->tv_sec += (time_t)(timeout_ms / 1000U);
    deadline->tv_nsec += (long)((timeout_ms % 1000U) * 1000000U);
    if (deadline->tv_nsec >= 1000000000L) {
        ++deadline->tv_sec;
        deadline->tv_nsec -= 1000000000L;
    }
}

static bool host_completion_consume(void *opaque) {
    completion_t *completion = opaque;
    unsigned count = atomic_load_explicit(&completion->count, memory_order_acquire);
    while (count != 0U) {
        if (atomic_compare_exchange_weak_explicit(&completion->count, &count,
                                                  count - 1U,
                                                  memory_order_acq_rel,
                                                  memory_order_acquire)) return true;
    }
    return false;
}

static void *host_wait_thread_main(void *opaque) {
    struct host_wait_job *job = (struct host_wait_job *)opaque;
    g_current_job = job;
    job->status = job->uninterruptible ?
        wait_on_queue_uninterruptible(&job->completion->waitq,
                                       host_completion_consume, job->completion,
                                       job->timeout_ns) : job->completion != 0 ?
        completion_wait(job->completion, job->timeout_ns) :
        object_wait_many(job->objects, job->count, job->wait_all,
                                   job->timeout_ns, &job->signaled_index,
                                   &job->wait_value);
    pthread_mutex_lock(&job->lock);
    job->done = true;
    job->sleeping = false;
    pthread_cond_broadcast(&job->condition);
    pthread_mutex_unlock(&job->lock);
    g_current_job = 0;
    return 0;
}

static void *host_timeout_thread_main(void *unused) {
    (void)unused;
    const struct timespec interval = { .tv_sec = 0, .tv_nsec = 1000000L };
    for (;;) {
        (void)nanosleep(&interval, 0);
        wait_poll_timeouts(monotonic_nanoseconds());
    }
    return 0;
}

static void host_start_timeout_thread(void) {
    pthread_t timer;
    if (pthread_create(&timer, 0, host_timeout_thread_main, 0) == 0) {
        (void)pthread_detach(timer);
    }
}

static host_wait_job_t *host_wait_job_start_internal(
    void *const *objects, size_t count, bool wait_all, uint64_t timeout_ns,
    unsigned signal_on_block, completion_t *completion, bool uninterruptible) {
    (void)pthread_once(&g_timer_once, host_start_timeout_thread);
    struct host_wait_job *job = (struct host_wait_job *)calloc(1U, sizeof(*job));
    if (job == 0) return 0;
    job->objects = objects;
    job->completion = completion;
    job->uninterruptible = uninterruptible;
    job->count = count;
    job->wait_all = wait_all;
    job->timeout_ns = timeout_ns;
    job->signal_on_block = signal_on_block;
    job->thread.process = (struct process *)job;
    job->thread.object.type = KOBJECT_TYPE_THREAD;
    job->thread.sched_class = SCHED_CLASS_FAIR;
    atomic_init(&job->thread.state, THREAD_RUNNING);
    atomic_init(&job->thread.blocked_waiter, 0);
    if (pthread_mutex_init(&job->lock, 0) != 0 ||
        pthread_cond_init(&job->condition, 0) != 0 ||
        pthread_create(&job->pthread, 0, host_wait_thread_main, job) != 0) {
        free(job);
        return 0;
    }
    return job;
}

host_wait_job_t *host_wait_job_start(void *const *objects, size_t count,
                                     bool wait_all, uint64_t timeout_ns) {
    return host_wait_job_start_internal(objects, count, wait_all, timeout_ns,
                                        false, 0, false);
}

host_wait_job_t *host_wait_job_start_with_sigkill_on_block(
    void *const *objects, size_t count, uint64_t timeout_ns) {
    return host_wait_job_start_internal(objects, count, false, timeout_ns,
                                        true, 0, false);
}

host_wait_job_t *host_wait_job_start_with_terminate_on_block(
    void *const *objects, size_t count, bool wait_all, uint64_t timeout_ns) {
    return host_wait_job_start_internal(objects, count, wait_all, timeout_ns,
                                        2U, 0, false);
}

host_wait_job_t *host_completion_job_start(completion_t *completion,
                                           uint64_t timeout_ns) {
    return host_wait_job_start_internal(0, 0U, false, timeout_ns, false,
                                        completion, false);
}

host_wait_job_t *host_uninterruptible_job_start(completion_t *completion,
                                                uint64_t timeout_ns,
                                                unsigned signal_on_block) {
    return host_wait_job_start_internal(0, 0U, false, timeout_ns,
                                        signal_on_block, completion, true);
}

bool host_wait_job_termination_pending(const host_wait_job_t *job,
                                       unsigned reason) {
    if (reason == 1U)
        return (__atomic_load_n(&job->thread.signal_pending, __ATOMIC_ACQUIRE) &
                (1ULL << (OS_SIGKILL - 1U))) != 0U;
    return (__atomic_load_n(&job->thread.flags, __ATOMIC_ACQUIRE) &
            THREAD_FLAG_TERMINATE_REQUEST) != 0U;
}

bool host_wait_job_is_unblocked(const host_wait_job_t *opaque) {
    const struct host_wait_job *job = (const struct host_wait_job *)opaque;
    return atomic_load_explicit(&job->thread.state, memory_order_acquire) ==
               THREAD_RUNNING &&
           atomic_load_explicit(&job->thread.blocked_waiter,
                                memory_order_acquire) == 0;
}

bool host_wait_until_sleeping(host_wait_job_t *opaque, uint64_t timeout_ms) {
    struct host_wait_job *job = (struct host_wait_job *)opaque;
    struct timespec deadline;
    deadline_after_ms(&deadline, timeout_ms);
    pthread_mutex_lock(&job->lock);
    while (!job->sleeping && !job->done) {
        if (pthread_cond_timedwait(&job->condition, &job->lock, &deadline) == ETIMEDOUT) {
            break;
        }
    }
    bool sleeping = job->sleeping;
    pthread_mutex_unlock(&job->lock);
    return sleeping;
}

bool host_wait_until_done(host_wait_job_t *opaque, uint64_t timeout_ms) {
    struct host_wait_job *job = (struct host_wait_job *)opaque;
    struct timespec deadline;
    deadline_after_ms(&deadline, timeout_ms);
    pthread_mutex_lock(&job->lock);
    while (!job->done) {
        if (pthread_cond_timedwait(&job->condition, &job->lock, &deadline) == ETIMEDOUT) {
            break;
        }
    }
    bool done = job->done;
    pthread_mutex_unlock(&job->lock);
    return done;
}

kstatus_t host_wait_job_status(const host_wait_job_t *opaque) {
    const struct host_wait_job *job = (const struct host_wait_job *)opaque;
    return job->status;
}

uint32_t host_wait_job_index(const host_wait_job_t *opaque) {
    const struct host_wait_job *job = (const struct host_wait_job *)opaque;
    return job->signaled_index;
}

int64_t host_wait_job_value(const host_wait_job_t *opaque) {
    const struct host_wait_job *job = (const struct host_wait_job *)opaque;
    return job->wait_value;
}

bool host_cancel_waiter(host_wait_job_t *opaque) {
    struct host_wait_job *job = (struct host_wait_job *)opaque;
    return wait_cancel_thread(&job->thread);
}

void host_fail_next_allocation(void) {
    atomic_store_explicit(&g_fail_next_allocation, true, memory_order_release);
}

void host_clear_allocation_failure(void) {
    atomic_store_explicit(&g_fail_next_allocation, false, memory_order_release);
    atomic_store_explicit(&g_fail_allocation_countdown, 0U, memory_order_release);
}

void host_fail_allocation_after(unsigned successful_attempts) {
    atomic_store(&g_fail_allocation_countdown, successful_attempts + 1U);
}

unsigned host_allocation_calls(void) { return atomic_load(&g_allocation_calls); }

void host_wait_job_join(host_wait_job_t *opaque) {
    struct host_wait_job *job = (struct host_wait_job *)opaque;
    if (job == 0) return;
    (void)pthread_join(job->pthread, 0);
    pthread_cond_destroy(&job->condition);
    pthread_mutex_destroy(&job->lock);
    free(job);
}

void host_hold_next_wake(host_wait_job_t *opaque) {
    struct host_wait_job *job = (struct host_wait_job *)opaque;
    pthread_mutex_lock(&job->lock);
    job->hold_next_wake = true;
    job->wake_held = false;
    job->release_wake = false;
    pthread_mutex_unlock(&job->lock);
}

bool host_wait_until_wake_held(host_wait_job_t *opaque, uint64_t timeout_ms) {
    struct host_wait_job *job = (struct host_wait_job *)opaque;
    struct timespec deadline;
    deadline_after_ms(&deadline, timeout_ms);
    pthread_mutex_lock(&job->lock);
    while (!job->wake_held && !job->done) {
        if (pthread_cond_timedwait(&job->condition, &job->lock, &deadline) ==
            ETIMEDOUT) {
            break;
        }
    }
    bool held = job->wake_held;
    pthread_mutex_unlock(&job->lock);
    return held;
}

void host_release_held_wake(host_wait_job_t *opaque) {
    struct host_wait_job *job = (struct host_wait_job *)opaque;
    pthread_mutex_lock(&job->lock);
    job->release_wake = true;
    pthread_cond_broadcast(&job->condition);
    pthread_mutex_unlock(&job->lock);
}

void host_suppress_running_restore(host_wait_job_t *opaque) {
    struct host_wait_job *job = (struct host_wait_job *)opaque;
    pthread_mutex_lock(&job->lock);
    job->suppress_running_restore = true;
    pthread_mutex_unlock(&job->lock);
}

bool host_wait_job_reap_must_defer(const host_wait_job_t *opaque) {
    const struct host_wait_job *job = (const struct host_wait_job *)opaque;
    return atomic_load_explicit(&job->thread.state, memory_order_acquire) ==
               THREAD_READY &&
           sched_wait_cleanup_pending(&job->thread);
}

void host_wake_without_wait_notification(host_wait_job_t *opaque) {
    struct host_wait_job *job = (struct host_wait_job *)opaque;
    pthread_mutex_lock(&job->lock);
    job->wake_pending = true;
    pthread_cond_broadcast(&job->condition);
    pthread_mutex_unlock(&job->lock);
}

bool host_wait_until_schedule_entries(host_wait_job_t *opaque,
                                      uint32_t entries,
                                      uint64_t timeout_ms) {
    struct host_wait_job *job = (struct host_wait_job *)opaque;
    struct timespec deadline;
    deadline_after_ms(&deadline, timeout_ms);
    pthread_mutex_lock(&job->lock);
    while (job->schedule_entries < entries && !job->done) {
        if (pthread_cond_timedwait(&job->condition, &job->lock, &deadline) ==
            ETIMEDOUT) break;
    }
    bool reached = job->schedule_entries >= entries;
    pthread_mutex_unlock(&job->lock);
    return reached;
}

thread_t *sched_current_thread(void) {
    return g_current_job != 0 ? &g_current_job->thread : 0;
}

void sched_preempt_disable(void) { ++g_preempt_depth; }
void sched_preempt_enable(void) {
    if (g_preempt_depth == 0U) abort();
    --g_preempt_depth;
}
bool sched_preempt_disabled(void) { return g_preempt_depth != 0U; }
bool host_wait_locks_held(void) { return g_preempt_depth != 0U; }

bool sched_publish_blocked(thread_t *thread) {
    struct host_wait_job *job = g_current_job;
    if (job != 0 && job->signal_on_block) {
        const uint64_t kill_bit = 1ULL << (OS_SIGKILL - 1U);
        unsigned reason = job->signal_on_block;
        job->signal_on_block = false;
        if (reason == 2U) {
            (void)__atomic_fetch_or(&thread->flags,
                                    THREAD_FLAG_TERMINATE_REQUEST,
                                    __ATOMIC_RELEASE);
        } else {
            (void)__atomic_fetch_or(&thread->signal_pending, kill_bit,
                                    __ATOMIC_RELEASE);
        }
    }
    atomic_store_explicit(&thread->state, THREAD_BLOCKED, memory_order_release);
    return true;
}

bool sched_publish_running(thread_t *thread) {
    atomic_store_explicit(&thread->state, THREAD_RUNNING, memory_order_release);
    return true;
}

void schedule(void) {
    struct host_wait_job *job = g_current_job;
    pthread_mutex_lock(&job->lock);
    ++job->schedule_entries;
    pthread_cond_broadcast(&job->condition);
    if (!job->wake_pending) {
        job->sleeping = true;
        pthread_cond_broadcast(&job->condition);
        while (!job->wake_pending) pthread_cond_wait(&job->condition, &job->lock);
    }
    job->wake_pending = false;
    job->sleeping = false;
    if (job->hold_next_wake) {
        job->hold_next_wake = false;
        job->wake_held = true;
        pthread_cond_broadcast(&job->condition);
        while (!job->release_wake) {
            pthread_cond_wait(&job->condition, &job->lock);
        }
        job->wake_held = false;
        job->release_wake = false;
    }
    bool suppress_running_restore = job->suppress_running_restore;
    job->suppress_running_restore = false;
    if (!suppress_running_restore) {
        atomic_store_explicit(&job->thread.state, THREAD_RUNNING,
                              memory_order_release);
    }
    pthread_cond_broadcast(&job->condition);
    pthread_mutex_unlock(&job->lock);
}

void sched_wake(thread_t *thread) {
    struct host_wait_job *job = (struct host_wait_job *)thread->process;
    atomic_store_explicit(&thread->state, THREAD_READY, memory_order_release);
    pthread_mutex_lock(&job->lock);
    job->wake_pending = true;
    pthread_cond_broadcast(&job->condition);
    pthread_mutex_unlock(&job->lock);
}

uint64_t x86_read_tsc(void) { return monotonic_nanoseconds(); }
uint64_t x86_tsc_to_ns(uint64_t ticks) { return ticks; }
uint64_t x86_timeout_ns_to_tsc(uint64_t timeout_ns) { return timeout_ns; }

void *kmalloc(size_t size, uint32_t flags) {
    (void)flags;
    atomic_fetch_add(&g_allocation_calls, 1U);
    unsigned remaining = atomic_load(&g_fail_allocation_countdown);
    while (remaining != 0U) {
        if (atomic_compare_exchange_weak(&g_fail_allocation_countdown,
                                         &remaining, remaining - 1U)) {
            if (remaining == 1U) return 0;
            break;
        }
    }
    bool expected = true;
    if (atomic_compare_exchange_strong_explicit(&g_fail_next_allocation, &expected,
                                                 false, memory_order_acq_rel,
                                                 memory_order_acquire)) return 0;
    return malloc(size);
}

void kfree(void *pointer) { free(pointer); }
