#define _POSIX_C_SOURCE 200809L

#include <kernel/object.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

typedef struct host_wait_job host_wait_job_t;
bool host_wait_locks_held(void);
host_wait_job_t *host_completion_job_start(completion_t *completion,
                                           uint64_t timeout_ns);
host_wait_job_t *host_uninterruptible_job_start(completion_t *completion,
                                                uint64_t timeout_ns,
                                                unsigned signal_on_block);
bool host_wait_job_termination_pending(const host_wait_job_t *job,
                                       unsigned reason);

host_wait_job_t *host_wait_job_start(void *const *objects, size_t count,
                                     bool wait_all, uint64_t timeout_ns);
host_wait_job_t *host_wait_job_start_with_sigkill_on_block(
    void *const *objects, size_t count, uint64_t timeout_ns);
host_wait_job_t *host_wait_job_start_with_terminate_on_block(
    void *const *objects, size_t count, bool wait_all, uint64_t timeout_ns);
bool host_wait_until_sleeping(host_wait_job_t *job, uint64_t timeout_ms);
bool host_wait_until_done(host_wait_job_t *job, uint64_t timeout_ms);
kstatus_t host_wait_job_status(const host_wait_job_t *job);
uint32_t host_wait_job_index(const host_wait_job_t *job);
int64_t host_wait_job_value(const host_wait_job_t *job);
bool host_cancel_waiter(host_wait_job_t *job);
bool host_wait_job_is_unblocked(const host_wait_job_t *job);
void host_fail_next_allocation(void);
void host_clear_allocation_failure(void);
void host_fail_allocation_after(unsigned successful_attempts);
unsigned host_allocation_calls(void);
void host_wait_job_join(host_wait_job_t *job);
void host_hold_next_wake(host_wait_job_t *job);
bool host_wait_until_wake_held(host_wait_job_t *job, uint64_t timeout_ms);
void host_release_held_wake(host_wait_job_t *job);
void host_suppress_running_restore(host_wait_job_t *job);
bool host_wait_job_reap_must_defer(const host_wait_job_t *job);
void host_wake_without_wait_notification(host_wait_job_t *job);
bool host_wait_until_schedule_entries(host_wait_job_t *job, uint32_t entries,
                                     uint64_t timeout_ms);

typedef struct fake_wait_object {
    object_header_t header;
    atomic_bool signaled;
    atomic_uint signal_checks;
    atomic_bool gate_predicate;
    atomic_bool predicate_entered;
    atomic_bool notification_started;
    atomic_bool notify_on_check;
    int64_t wait_value;
} fake_wait_object_t;

static unsigned g_failures;
static atomic_uint g_fake_destroys;
static atomic_uint g_locked_predicate_calls;

#define TEST_CHECK(condition, message) do { \
    if (!(condition)) { \
        fprintf(stderr, "FAIL: %s\n", message); \
        ++g_failures; \
    } \
} while (0)

static bool fake_is_signaled(const void *opaque) {
    if (host_wait_locks_held())
        atomic_fetch_add_explicit(&g_locked_predicate_calls, 1U,
                                  memory_order_relaxed);
    fake_wait_object_t *object = (fake_wait_object_t *)opaque;
    atomic_fetch_add_explicit(&object->signal_checks, 1U, memory_order_relaxed);
    bool ready = atomic_load_explicit(&object->signaled, memory_order_acquire);
    /* Simulate a local IRQ posting a notification inside a subsystem probe.
     * Return the old state so sequence validation must prevent lost wakeup. */
    if (atomic_exchange_explicit(&object->notify_on_check, false,
                                  memory_order_acq_rel)) {
        atomic_store_explicit(&object->signaled, true, memory_order_release);
        object_notify_changed(object);
    }
    if (atomic_exchange_explicit(&object->gate_predicate, false, memory_order_acq_rel)) {
        atomic_store_explicit(&object->predicate_entered, true, memory_order_release);
        while (!atomic_load_explicit(&object->notification_started, memory_order_acquire))
            sched_yield();
    }
    return ready;
}

static void fake_destroy(void *opaque) {
    (void)opaque;
    atomic_fetch_add_explicit(&g_fake_destroys, 1U, memory_order_relaxed);
}

static int64_t fake_wait_value(const void *opaque) {
    const fake_wait_object_t *object = (const fake_wait_object_t *)opaque;
    return object->wait_value;
}

static const object_ops_t g_fake_wait_ops = {
    .destroy = fake_destroy,
    .handle_close = 0,
    .type_name = "wait-subscription-test",
    .is_signaled = fake_is_signaled,
    .wait_value = fake_wait_value,
};

static void fake_wait_object_init(fake_wait_object_t *object) {
    object_header_init(&object->header, 0x7fffU, &g_fake_wait_ops);
    atomic_init(&object->signaled, false);
    atomic_init(&object->signal_checks, 0U);
    atomic_init(&object->gate_predicate, false);
    atomic_init(&object->predicate_entered, false);
    atomic_init(&object->notification_started, false);
    atomic_init(&object->notify_on_check, false);
    object->wait_value = 0;
}

static void release_and_join(host_wait_job_t *job, fake_wait_object_t *object) {
    if (job == 0) return;
    atomic_store_explicit(&object->signaled, true, memory_order_release);
    object_notify_signaled(object);
    (void)host_wait_until_done(job, 2000U);
    host_wait_job_join(job);
}

static void test_unrelated_object_notification_does_not_recheck_waiter(void) {
    fake_wait_object_t object_a;
    fake_wait_object_t object_b;
    fake_wait_object_init(&object_a);
    fake_wait_object_init(&object_b);
    void *objects_a[] = { &object_a };
    void *objects_b[] = { &object_b };

    host_wait_job_t *job_a = host_wait_job_start(objects_a, 1U, false, UINT64_MAX);
    host_wait_job_t *job_b = host_wait_job_start(objects_b, 1U, false, UINT64_MAX);
    TEST_CHECK(job_a != 0 && job_b != 0, "host waiter creation failed");
    if (job_a == 0 || job_b == 0) {
        release_and_join(job_a, &object_a);
        release_and_join(job_b, &object_b);
        return;
    }

    TEST_CHECK(host_wait_until_sleeping(job_a, 2000U), "waiter A did not block");
    TEST_CHECK(host_wait_until_sleeping(job_b, 2000U), "waiter B did not block");
    unsigned b_checks = atomic_load_explicit(&object_b.signal_checks,
                                             memory_order_relaxed);

    atomic_store_explicit(&object_a.signaled, true, memory_order_release);
    object_notify_signaled(&object_a);
    TEST_CHECK(host_wait_until_done(job_a, 2000U), "waiter A did not complete");

    const struct timespec observe_window = { .tv_sec = 0, .tv_nsec = 100000000L };
    (void)nanosleep(&observe_window, 0);
    TEST_CHECK(atomic_load_explicit(&object_b.signal_checks, memory_order_relaxed) ==
                   b_checks,
               "notifying object A caused unrelated waiter B to recheck its predicate");

    atomic_store_explicit(&object_b.signaled, true, memory_order_release);
    object_notify_signaled(&object_b);
    TEST_CHECK(host_wait_until_done(job_b, 2000U), "waiter B did not complete");
    TEST_CHECK(host_wait_job_status(job_a) == K_OK, "waiter A returned an error");
    TEST_CHECK(host_wait_job_status(job_b) == K_OK, "waiter B returned an error");
    host_wait_job_join(job_a);
    host_wait_job_join(job_b);
}

static void test_wait_any_preserves_index_and_value(void) {
    fake_wait_object_t first;
    fake_wait_object_t second;
    fake_wait_object_init(&first);
    fake_wait_object_init(&second);
    second.wait_value = 73;
    void *objects[] = { &first, &second };
    host_wait_job_t *job = host_wait_job_start(objects, 2U, false, UINT64_MAX);
    TEST_CHECK(job != 0, "wait_any host waiter creation failed");
    if (job == 0) return;
    TEST_CHECK(host_wait_until_sleeping(job, 2000U), "wait_any did not block");
    atomic_store_explicit(&second.signaled, true, memory_order_release);
    object_notify_signaled(&second);
    TEST_CHECK(host_wait_until_done(job, 2000U), "wait_any did not wake");
    TEST_CHECK(host_wait_job_status(job) == K_OK, "wait_any returned an error");
    TEST_CHECK(host_wait_job_index(job) == 1U, "wait_any changed its selected index");
    TEST_CHECK(host_wait_job_value(job) == 73, "wait_any changed its wait value");
    if (!host_wait_until_done(job, 1U)) release_and_join(job, &first);
    else host_wait_job_join(job);
}

static void test_wait_all_requires_every_input(void) {
    fake_wait_object_t first;
    fake_wait_object_t second;
    fake_wait_object_init(&first);
    fake_wait_object_init(&second);
    void *objects[] = { &first, &second };
    host_wait_job_t *job = host_wait_job_start(objects, 2U, true, UINT64_MAX);
    TEST_CHECK(job != 0, "wait_all host waiter creation failed");
    if (job == 0) return;
    TEST_CHECK(host_wait_until_sleeping(job, 2000U), "wait_all did not block");
    atomic_store_explicit(&first.signaled, true, memory_order_release);
    object_notify_signaled(&first);
    const struct timespec observation = { .tv_sec = 0, .tv_nsec = 30000000L };
    (void)nanosleep(&observation, 0);
    TEST_CHECK(!host_wait_until_done(job, 1U), "wait_all completed after only one input");
    atomic_store_explicit(&second.signaled, true, memory_order_release);
    object_notify_signaled(&second);
    TEST_CHECK(host_wait_until_done(job, 2000U), "wait_all did not wake after all inputs");
    TEST_CHECK(host_wait_job_status(job) == K_OK, "wait_all returned an error");
    TEST_CHECK(host_wait_job_index(job) == UINT32_MAX, "wait_all changed its index sentinel");
    host_wait_job_join(job);
}

static void test_wait_timeout_returns_and_cleans_up(void) {
    fake_wait_object_t object;
    fake_wait_object_init(&object);
    void *objects[] = { &object };
    host_wait_job_t *job = host_wait_job_start(objects, 1U, false, 20000000ULL);
    TEST_CHECK(job != 0, "timeout host waiter creation failed");
    if (job == 0) return;
    TEST_CHECK(host_wait_until_done(job, 2000U), "finite wait did not time out");
    TEST_CHECK(host_wait_job_status(job) == K_ETIMEDOUT, "finite wait returned the wrong status");
    host_wait_job_join(job);
    unsigned checks = atomic_load_explicit(&object.signal_checks, memory_order_relaxed);
    atomic_store_explicit(&object.signaled, true, memory_order_release);
    object_notify_signaled(&object);
    TEST_CHECK(atomic_load_explicit(&object.signal_checks, memory_order_relaxed) == checks + 1U,
               "timed-out waiter remained subscribed");
}

static void test_duplicate_object_preserves_first_input_index(void) {
    fake_wait_object_t object;
    fake_wait_object_init(&object);
    object.wait_value = 19;
    atomic_store_explicit(&object.signaled, true, memory_order_release);
    void *objects[] = { &object, &object };
    uint32_t index = UINT32_MAX;
    int64_t value = 0;
    kstatus_t status = object_wait_many(objects, 2U, false, UINT64_MAX,
                                        &index, &value);
    TEST_CHECK(status == K_OK, "duplicate-object wait failed");
    TEST_CHECK(index == 0U, "duplicate-object wait lost first-input semantics");
    TEST_CHECK(value == 19, "duplicate-object wait returned the wrong value");
}

static void test_wait_allocation_failure_is_reported(void) {
    fake_wait_object_t first;
    fake_wait_object_t second;
    fake_wait_object_init(&first);
    fake_wait_object_init(&second);
    void *objects[] = { &first, &second, &first, &second, &first };
    uint32_t index = UINT32_MAX;
    int64_t value = 0;
    host_fail_next_allocation();
    kstatus_t status = object_wait_many(objects, 5U, false, 3000000ULL,
                                        &index, &value);
    host_clear_allocation_failure();
    TEST_CHECK(status == K_ENOMEM, "multi-object allocation failure was not returned as K_ENOMEM");
    TEST_CHECK(atomic_load_explicit(&first.header.refs.value, memory_order_relaxed) == 1U &&
                   atomic_load_explicit(&second.header.refs.value, memory_order_relaxed) == 1U,
               "allocation failure leaked an object reference");
}

static void test_waiter_pins_object_until_unsubscribe(void) {
    fake_wait_object_t object;
    fake_wait_object_init(&object);
    unsigned destroys_before = atomic_load_explicit(&g_fake_destroys, memory_order_relaxed);
    void *objects[] = { &object };
    host_wait_job_t *job = host_wait_job_start(objects, 1U, false, UINT64_MAX);
    TEST_CHECK(job != 0, "lifetime host waiter creation failed");
    if (job == 0) return;
    TEST_CHECK(host_wait_until_sleeping(job, 2000U), "lifetime waiter did not block");
    TEST_CHECK(atomic_load_explicit(&object.header.refs.value, memory_order_acquire) == 2U,
               "waiter did not pin its object");
    object_put(&object);
    atomic_store_explicit(&object.signaled, true, memory_order_release);
    object_notify_signaled(&object);
    TEST_CHECK(host_wait_until_done(job, 2000U), "pinned waiter did not wake");
    TEST_CHECK(host_wait_job_status(job) == K_OK, "pinned waiter returned an error");
    TEST_CHECK(atomic_load_explicit(&g_fake_destroys, memory_order_relaxed) ==
                   destroys_before + 1U,
               "object was not released after the waiter unsubscribed");
    host_wait_job_join(job);
}

static void test_cancelled_waiter_returns(void) {
    fake_wait_object_t first;
    fake_wait_object_t second;
    fake_wait_object_init(&first);
    fake_wait_object_init(&second);
    void *objects[] = { &first, &second };
    host_wait_job_t *job = host_wait_job_start(objects, 2U, false, UINT64_MAX);
    TEST_CHECK(job != 0, "cancel host waiter creation failed");
    if (job == 0) return;
    TEST_CHECK(host_wait_until_sleeping(job, 2000U), "cancel waiter did not block");
    TEST_CHECK(host_cancel_waiter(job), "wait_cancel did not claim the blocked waiter");
    TEST_CHECK(host_wait_until_done(job, 2000U), "cancelled waiter did not return");
    TEST_CHECK(host_wait_job_status(job) == K_ECANCELED, "cancelled waiter returned the wrong status");
    host_wait_job_join(job);
}

static void test_wait_return_repairs_stale_blocked_state(void) {
    fake_wait_object_t object;
    fake_wait_object_init(&object);
    void *objects[] = { &object };
    host_wait_job_t *job = host_wait_job_start(objects, 1U, false, UINT64_MAX);
    TEST_CHECK(job != 0, "state-repair host waiter creation failed");
    if (job == 0) return;
    TEST_CHECK(host_wait_until_sleeping(job, 2000U),
               "state-repair waiter did not block");

    host_suppress_running_restore(job);
    atomic_store_explicit(&object.signaled, true, memory_order_release);
    object_notify_signaled(&object);
    TEST_CHECK(host_wait_until_done(job, 2000U),
               "state-repair waiter did not return");
    TEST_CHECK(host_wait_job_status(job) == K_OK,
               "state-repair wait returned an error");
    TEST_CHECK(host_wait_job_is_unblocked(job),
               "wait returned while thread state was still BLOCKED");
    host_wait_job_join(job);
}

static void test_unrelated_scheduler_wake_retries_wait(void) {
    fake_wait_object_t object;
    fake_wait_object_init(&object);
    void *objects[] = { &object };
    host_wait_job_t *job = host_wait_job_start(objects, 1U, false, UINT64_MAX);
    TEST_CHECK(job != 0, "spurious-wake host waiter creation failed");
    if (job == 0) return;
    TEST_CHECK(host_wait_until_sleeping(job, 2000U),
               "spurious-wake waiter did not block");

    host_wake_without_wait_notification(job);
    TEST_CHECK(host_wait_until_schedule_entries(job, 2U, 2000U),
               "waiter did not retry after unrelated scheduler wake");
    TEST_CHECK(!host_wait_until_done(job, 1U),
               "unrelated scheduler wake incorrectly completed the wait");

    atomic_store_explicit(&object.signaled, true, memory_order_release);
    object_notify_signaled(&object);
    TEST_CHECK(host_wait_until_done(job, 2000U),
               "retried waiter did not complete after object notification");
    TEST_CHECK(host_wait_job_status(job) == K_OK,
               "unrelated scheduler wake was reported as cancellation");
    TEST_CHECK(host_wait_job_is_unblocked(job),
               "retried wait left a blocked association behind");
    host_wait_job_join(job);
}

static void test_ready_waiter_must_finish_stack_cleanup_before_reap(void) {
    fake_wait_object_t first;
    fake_wait_object_t second;
    fake_wait_object_init(&first);
    fake_wait_object_init(&second);
    void *objects[] = { &first, &second };
    host_wait_job_t *job = host_wait_job_start(objects, 2U, false, UINT64_MAX);
    TEST_CHECK(job != 0, "ready-waiter host job creation failed");
    if (job == 0) return;
    TEST_CHECK(host_wait_until_sleeping(job, 2000U),
               "ready-waiter job did not block");

    host_hold_next_wake(job);
    atomic_store_explicit(&second.signaled, true, memory_order_release);
    object_notify_signaled(&second);
    TEST_CHECK(host_wait_until_wake_held(job, 2000U),
               "ready waiter did not pause before unlinking subscriptions");
    TEST_CHECK(host_wait_job_reap_must_defer(job),
               "READY waiter with stack-local subscriptions was considered safe to reap");

    host_release_held_wake(job);
    TEST_CHECK(host_wait_until_done(job, 2000U),
               "released ready waiter did not finish cleanup");
    TEST_CHECK(host_wait_job_status(job) == K_OK,
               "released ready waiter returned an error");
    TEST_CHECK(host_wait_job_is_unblocked(job),
               "released ready waiter retained its blocked association");
    host_wait_job_join(job);
}

static void test_sigkill_during_block_publication_is_not_lost(void) {
    fake_wait_object_t object;
    fake_wait_object_init(&object);
    void *objects[] = { &object };
    host_wait_job_t *job =
        host_wait_job_start_with_sigkill_on_block(objects, 1U, UINT64_MAX);
    TEST_CHECK(job != 0, "sigkill-race host waiter creation failed");
    if (job == 0) return;
    TEST_CHECK(host_wait_until_done(job, 2000U),
               "SIGKILL racing block publication stranded the waiter");
    TEST_CHECK(host_wait_job_status(job) == K_ECANCELED,
               "SIGKILL-raced wait returned the wrong status");
    TEST_CHECK(host_wait_job_is_unblocked(job),
               "SIGKILL-raced wait left a blocked association behind");
    TEST_CHECK(atomic_load_explicit(&object.header.refs.value,
                                    memory_order_acquire) == 1U,
               "SIGKILL-raced wait leaked its pinned object");
    host_wait_job_join(job);
}

static void test_thread_terminate_during_block_publication_is_not_lost(void) {
    const unsigned counts[] = {1U, 2U, 32U};
    for (unsigned all = 0U; all < 2U; ++all) {
        for (unsigned c = 0U; c < 3U; ++c) {
            fake_wait_object_t values[32];
            void *objects[32];
            for (unsigned i = 0U; i < counts[c]; ++i) {
                fake_wait_object_init(&values[i]);
                objects[i] = &values[i];
            }
            host_wait_job_t *job = host_wait_job_start_with_terminate_on_block(
                objects, counts[c], all != 0U, UINT64_MAX);
            TEST_CHECK(job != 0, "thread-terminate-race waiter creation failed");
            if (job == 0) continue;
            bool done = host_wait_until_done(job, 200U);
            TEST_CHECK(done, "thread termination racing BLOCKED publication stranded the waiter");
            /* Unstrand the negative-control fixture before joining it. */
            if (!done) {
                TEST_CHECK(host_cancel_waiter(job), "could not rescue stranded test waiter");
                TEST_CHECK(host_wait_until_done(job, 2000U), "rescued waiter did not finish");
            }
            TEST_CHECK(host_wait_job_status(job) == K_ECANCELED,
                       "thread-terminate-raced wait returned wrong status");
            TEST_CHECK(host_wait_job_is_unblocked(job),
                       "thread-terminate-raced wait retained stack association");
            for (unsigned i = 0U; i < counts[c]; ++i) {
                TEST_CHECK(list_empty(&values[i].header.signal_waiters.waiters),
                           "thread-terminate-raced wait retained subscription");
                TEST_CHECK(atomic_load_explicit(&values[i].header.refs.value,
                                                memory_order_acquire) == 1U,
                           "thread-terminate-raced wait leaked object reference");
            }
            host_wait_job_join(job);
        }
    }
}

static void test_small_wait_sets_and_large_allocation_stages(void) {
    fake_wait_object_t values[5];
    void *objects[5];
    for (unsigned i = 0U; i < 5U; ++i) {
        fake_wait_object_init(&values[i]);
        objects[i] = &values[i];
    }
    for (size_t count = 2U; count <= WAIT_INLINE_QUEUE_COUNT; ++count) {
        for (unsigned mode = 0U; mode < 4U; ++mode) {
            for (size_t i = 0U; i < count; ++i) atomic_store(&values[i].signaled, false);
            unsigned calls = host_allocation_calls();
            host_fail_next_allocation();
            host_wait_job_t *job = host_wait_job_start(objects, count,
                mode == 1U, mode == 3U ? 100000000ULL : UINT64_MAX);
            TEST_CHECK(job != 0, "inline wait job creation failed");
            if (job == 0) { host_clear_allocation_failure(); continue; }
            TEST_CHECK(host_wait_until_sleeping(job, 2000U), "inline wait did not block");
            if (mode == 0U) {
                atomic_store(&values[count - 1U].signaled, true);
                object_notify_signaled(&values[count - 1U]);
            } else if (mode == 1U) {
                for (size_t i = 0U; i + 1U < count; ++i) {
                    atomic_store(&values[i].signaled, true);
                    object_notify_signaled(&values[i]);
                }
                TEST_CHECK(!host_wait_until_done(job, 10U),
                           "inline wait_all completed without its final input");
                atomic_store(&values[count - 1U].signaled, true);
                object_notify_signaled(&values[count - 1U]);
            } else if (mode == 2U) {
                TEST_CHECK(host_cancel_waiter(job), "inline cancellation failed");
            }
            bool done = host_wait_until_done(job, 2000U);
            TEST_CHECK(done, "inline wait wake/timeout/cancel did not complete");
            if (!done) {
                for (size_t i = 0U; i < count; ++i) {
                    atomic_store(&values[i].signaled, true);
                    object_notify_signaled(&values[i]);
                }
                (void)host_wait_until_done(job, 2000U);
            }
            TEST_CHECK(host_wait_job_status(job) == (mode < 2U ? K_OK :
                       mode == 2U ? K_ECANCELED : K_ETIMEDOUT),
                       "inline wait returned wrong terminal status");
            if (mode < 2U) {
                TEST_CHECK(host_wait_job_index(job) == (mode == 1U ?
                           UINT32_MAX : (uint32_t)count - 1U),
                           "inline wait changed index semantics");
            }
            TEST_CHECK(host_wait_job_is_unblocked(job), "inline wait retained blocked state");
            host_wait_job_join(job);
            TEST_CHECK(host_allocation_calls() == calls,
                       "small wait set attempted a heap allocation");
            host_clear_allocation_failure();
            for (size_t i = 0U; i < count; ++i) {
                TEST_CHECK(atomic_load(&values[i].header.refs.value) == 1U &&
                           list_empty(&values[i].header.signal_waiters.waiters),
                           "inline wait leaked refs/subscriptions");
            }
        }
    }
    // Five distinct queues allocate the shared scratch array and then the
    // subscription array. Reject each remaining stage independently.
    for (unsigned stage = 0U; stage < 2U; ++stage) {
        for (unsigned i = 0U; i < 5U; ++i) atomic_store(&values[i].signaled, false);
        unsigned calls = host_allocation_calls();
        host_fail_allocation_after(stage);
        host_wait_job_t *job = host_wait_job_start(objects, 5U, false, UINT64_MAX);
        TEST_CHECK(job != 0, "large wait allocation-stage job creation failed");
        if (job == 0) { host_clear_allocation_failure(); continue; }
        bool done = host_wait_until_done(job, 2000U);
        TEST_CHECK(done, "large wait failed to report allocation failure");
        if (!done) {
            for (unsigned i = 0U; i < 5U; ++i) {
                atomic_store(&values[i].signaled, true);
                object_notify_signaled(&values[i]);
            }
            (void)host_wait_until_done(job, 2000U);
        }
        TEST_CHECK(host_wait_job_status(job) == K_ENOMEM,
                   "large wait returned wrong allocation failure");
        host_wait_job_join(job);
        host_clear_allocation_failure();
        TEST_CHECK(host_allocation_calls() == calls + stage + 1U,
                   "large wait failed at the wrong allocation stage");
        for (unsigned i = 0U; i < 5U; ++i) {
            TEST_CHECK(atomic_load(&values[i].header.refs.value) == 1U &&
                       list_empty(&values[i].header.signal_waiters.waiters),
                       "large wait allocation failure leaked refs/subscriptions");
        }
    }
}

static void test_large_reversed_wait_set(void) {
    fake_wait_object_t values[32];
    void *objects[32];
    for (unsigned mode = 0U; mode < 2U; ++mode) {
        for (unsigned i = 0U; i < 32U; ++i) {
            fake_wait_object_init(&values[i]);
            objects[i] = &values[31U - i];
        }
        host_wait_job_t *job = host_wait_job_start(objects, 32U, mode != 0U,
                                                  UINT64_MAX);
        TEST_CHECK(job != 0, "reversed large wait job creation failed");
        if (job == 0) continue;
        TEST_CHECK(host_wait_until_sleeping(job, 2000U), "reversed large wait did not block");
        if (mode == 0U) {
            fake_wait_object_t *chosen = objects[7];
            chosen->wait_value = 1234;
            atomic_store(&chosen->signaled, true);
            object_notify_signaled(chosen);
        } else {
            for (unsigned i = 0U; i < 32U; ++i) {
                atomic_store(&values[i].signaled, true);
                object_notify_signaled(&values[i]);
            }
        }
        bool done = host_wait_until_done(job, 2000U);
        TEST_CHECK(done, "reversed large wait did not wake");
        if (!done) {
            for (unsigned i = 0U; i < 32U; ++i) {
                atomic_store(&values[i].signaled, true);
                object_notify_signaled(&values[i]);
            }
            (void)host_wait_until_done(job, 2000U);
        }
        TEST_CHECK(host_wait_job_status(job) == K_OK, "reversed wait status changed");
        TEST_CHECK(host_wait_job_index(job) == (mode == 0U ? 7U : UINT32_MAX),
                   "sorting changed the original input index");
        TEST_CHECK(host_wait_job_value(job) == (mode == 0U ? 1234 : 0),
                   "sorting changed the original input value");
        host_wait_job_join(job);
        for (unsigned i = 0U; i < 32U; ++i) {
            TEST_CHECK(atomic_load(&values[i].header.refs.value) == 1U &&
                       list_empty(&values[i].header.signal_waiters.waiters),
                       "reversed wait leaked references/subscriptions");
        }
    }
}

static void test_completion_token_and_blocking_lifecycle(void) {
    completion_t completion;
    completion_init(&completion, 3U);
    for (unsigned i = 0U; i < 3U; ++i) {
        TEST_CHECK(completion_wait(&completion, 0U) == K_OK,
                   "ready completion did not consume its token");
    }
    TEST_CHECK(completion_wait(&completion, 0U) == K_ETIMEDOUT &&
               atomic_load(&completion.count) == 0U,
               "completion consumed more tokens than published");
    for (unsigned mode = 0U; mode < 3U; ++mode) {
        host_wait_job_t *job = host_completion_job_start(&completion,
            mode == 1U ? 20000000ULL : UINT64_MAX);
        TEST_CHECK(job != 0, "completion host job creation failed");
        if (job == 0) continue;
        TEST_CHECK(host_wait_until_sleeping(job, 2000U),
                   "empty completion did not subscribe/block");
        if (mode == 0U) complete(&completion);
        if (mode == 2U) {
            TEST_CHECK(host_cancel_waiter(job), "completion cancel claim failed");
        }
        bool done = host_wait_until_done(job, 2000U);
        TEST_CHECK(done, "completion did not finish after wake/timeout/cancel");
        if (!done) {
            complete(&completion);
            (void)host_wait_until_done(job, 2000U);
        }
        TEST_CHECK(host_wait_job_status(job) ==
                   (mode == 0U ? K_OK : mode == 1U ? K_ETIMEDOUT : K_ECANCELED),
                   "completion returned wrong terminal status");
        TEST_CHECK(host_wait_job_is_unblocked(job),
                   "completion retained blocked thread association");
        TEST_CHECK(list_empty(&completion.waitq.waiters),
                   "completion retained queue subscriptions");
        host_wait_job_join(job);
    }
    complete(&completion);
    TEST_CHECK(completion_wait(&completion, 0U) == K_OK &&
               atomic_load(&completion.count) == 0U,
               "completion could not be reused after timeout/cancel");
}

static void *notify_at_subscription_boundary(void *opaque) {
    fake_wait_object_t *object = opaque;
    /* Bounded publisher wait also releases the predicate on setup failure. */
    for (unsigned i = 0U; i < 2000U &&
         !atomic_load_explicit(&object->predicate_entered, memory_order_acquire); ++i) {
        const struct timespec pause = {0, 1000000L};
        nanosleep(&pause, 0);
    }
    atomic_store_explicit(&object->signaled, true, memory_order_release);
    atomic_store_explicit(&object->notification_started, true, memory_order_release);
    object_notify_signaled(object);
    return 0;
}

static void test_notification_during_subscription_publication(void) {
    _Static_assert(sizeof(wait_subscription_t) == 32U, "subscription retained legacy snapshot");
    _Static_assert(sizeof(waiter_t) == 80U, "waiter retained legacy nodes/storage");
    const size_t counts[] = {1U, 4U, 32U};
    for (size_t c = 0U; c < 3U; ++c) for (unsigned all = 0U; all < 2U; ++all)
    for (unsigned round = 0U; round < 16U; ++round) {
        fake_wait_object_t objects[32];void *inputs[32];
        for (size_t i = 0U; i < counts[c]; ++i) {
            fake_wait_object_init(&objects[i]);inputs[i] = &objects[i];
            if (all != 0U && i != 0U) atomic_store(&objects[i].signaled, true);
        }
        objects[0].wait_value = 1234;
        atomic_store(&objects[0].gate_predicate, true);
        pthread_t publisher;
        int started = pthread_create(&publisher, 0, notify_at_subscription_boundary, &objects[0]);
        TEST_CHECK(started == 0, "boundary publisher creation failed");
        if (started != 0) return;
        host_wait_job_t *job = host_wait_job_start(inputs, counts[c], all != 0U, UINT64_MAX);
        TEST_CHECK(job != 0, "boundary waiter creation failed");
        if (job == 0) { pthread_join(publisher, 0); return; }
        bool done = host_wait_until_done(job, 3000U);
        TEST_CHECK(done, "notification at subscription boundary was lost");
        if (!done) { host_cancel_waiter(job); host_wait_until_done(job, 2000U); }
        pthread_join(publisher, 0);
        TEST_CHECK(atomic_load(&objects[0].predicate_entered), "boundary predicate was not exercised");
        TEST_CHECK(host_wait_job_status(job) == K_OK, "boundary wait did not succeed");
        TEST_CHECK(host_wait_job_is_unblocked(job), "boundary wait retained blocked association");
        if (!all) TEST_CHECK(host_wait_job_index(job) == 0U && host_wait_job_value(job) == 1234,
                             "boundary wait lost input index/value");
        for (size_t i = 0U; i < counts[c]; ++i) {
            TEST_CHECK(list_empty(&objects[i].header.signal_waiters.waiters),
                       "boundary wait retained queue subscription");
            TEST_CHECK(atomic_load(&objects[i].header.refs.value) == 1U,
                       "boundary wait leaked object reference");
        }
        host_wait_job_join(job);
    }
}

static void test_large_ready_and_poll_wait_skip_subscription_allocation(void) {
    const size_t counts[] = { 5U, 8U, 16U };
    for (size_t c = 0U; c < sizeof(counts)/sizeof(counts[0]); ++c) {
        for (unsigned mode = 0U; mode < 3U; ++mode) {
            fake_wait_object_t objects[16];
            void *inputs[16];
            for (size_t i = 0U; i < counts[c]; ++i) {
                fake_wait_object_init(&objects[i]);
                objects[i].wait_value = (int64_t)i + 100;
                atomic_store(&objects[i].signaled, mode == 1U || (mode == 0U && i == counts[c]-1U));
                inputs[i] = &objects[i];
            }
            unsigned calls = host_allocation_calls();
            /* The scratch array allocates once; subscription storage must
             * not allocate when ready, or when a poll has already expired. */
            host_fail_allocation_after(1U);
            host_wait_job_t *job = host_wait_job_start(inputs, counts[c], mode != 0U,
                                                       mode == 2U ? 0U : UINT64_MAX);
            TEST_CHECK(job != 0, "ready/poll job creation failed");
            if (job == 0) { host_clear_allocation_failure(); continue; }
            bool done = host_wait_until_done(job, 2000U);
            TEST_CHECK(done, "ready/poll wait did not return");
            if (!done) { host_cancel_waiter(job); host_wait_until_done(job, 2000U); }
            TEST_CHECK(host_wait_job_status(job) == (mode == 2U ? K_ETIMEDOUT : K_OK),
                       "ready/poll wait attempted subscription allocation");
            if (mode == 0U) {
                TEST_CHECK(host_wait_job_index(job) == counts[c]-1U &&
                           host_wait_job_value(job) == (int64_t)counts[c]+99,
                           "ready wait lost index/value");
            }
            host_wait_job_join(job);
            host_clear_allocation_failure();
            TEST_CHECK(host_allocation_calls() == calls+1U, "ready/poll allocated subscriptions");
            for (size_t i = 0U; i < counts[c]; ++i) {
                TEST_CHECK(atomic_load(&objects[i].header.refs.value) == 1U &&
                           list_empty(&objects[i].header.signal_waiters.waiters),
                           "ready/poll leaked refs or subscriptions");
            }
        }
    }
}

static void test_local_notification_inside_readiness_callback(void) {
    fake_wait_object_t object;
    fake_wait_object_init(&object);
    atomic_store(&object.notify_on_check, true);
    void *inputs[] = { &object };
    host_wait_job_t *job = host_wait_job_start(inputs, 1U, false, UINT64_MAX);
    TEST_CHECK(job != 0, "local notification job creation failed");
    if (job == 0) return;
    bool done = host_wait_until_done(job, 2000U);
    TEST_CHECK(done, "local IRQ notification deadlocked or was lost");
    if (!done) abort(); /* A deadlocked worker still owns the stack object. */
    TEST_CHECK(host_wait_job_status(job) == K_OK,
               "local notification did not retry readiness");
    TEST_CHECK(list_empty(&object.header.signal_waiters.waiters),
               "local notification retained subscription");
    host_wait_job_join(job);
}

static void test_internal_wait_defers_termination(void) {
    for (unsigned reason = 1U; reason <= 2U; ++reason) {
        for (unsigned timeout = 0U; timeout <= 1U; ++timeout) {
            completion_t completion;
            completion_init(&completion, 0U);
            host_wait_job_t *job = host_uninterruptible_job_start(
                &completion, timeout ? 200000000ULL : UINT64_MAX, reason);
            TEST_CHECK(job != 0, "internal wait job creation failed");
            if (job == 0) continue;
            TEST_CHECK(host_wait_until_sleeping(job, 2000U),
                       "pending termination prevented internal wait sleeping");
            TEST_CHECK(!host_cancel_waiter(job), "internal wait was cancelled");
            TEST_CHECK(!host_wait_until_done(job, 5U),
                       "internal wait returned without readiness or timeout");
            if (!timeout) {
                host_wake_without_wait_notification(job);
                TEST_CHECK(host_wait_until_schedule_entries(job, 2U, 2000U),
                           "internal wait did not retry after unrelated wake");
                TEST_CHECK(host_wait_until_sleeping(job, 2000U),
                           "internal wait did not reblock");
                TEST_CHECK(!host_cancel_waiter(job),
                           "retried internal wait lost cancellation policy");
                complete(&completion);
            }
            bool done = host_wait_until_done(job, 2000U);
            TEST_CHECK(done, "internal wait failed to wake");
            if (!done) abort();
            TEST_CHECK(host_wait_job_status(job) == (timeout ? K_ETIMEDOUT : K_OK),
                       "internal wait lost readiness/timeout result");
            TEST_CHECK(host_wait_job_termination_pending(job, reason),
                       "internal wait discarded pending termination");
            TEST_CHECK(host_wait_job_is_unblocked(job) &&
                       list_empty(&completion.waitq.waiters),
                       "internal wait retained stack association/subscription");
            TEST_CHECK(atomic_load(&completion.count) == 0U,
                       "internal wait did not consume completion exactly once");
            host_wait_job_join(job);
        }
    }
    if (g_failures == 0U) puts("WAIT_INTERNAL_DEFERRED_TERMINATION_PASS");
}

int main(void) {
    test_internal_wait_defers_termination();
    test_local_notification_inside_readiness_callback();
    test_large_ready_and_poll_wait_skip_subscription_allocation();
    test_notification_during_subscription_publication();
    if (g_failures == 0U) puts("WAIT_PUBLICATION_BOUNDARY_96_CASES_PASS");
    test_small_wait_sets_and_large_allocation_stages();
    test_large_reversed_wait_set();
    test_completion_token_and_blocking_lifecycle();
    test_unrelated_object_notification_does_not_recheck_waiter();
    test_wait_any_preserves_index_and_value();
    test_wait_all_requires_every_input();
    test_wait_timeout_returns_and_cleans_up();
    test_duplicate_object_preserves_first_input_index();
    test_wait_allocation_failure_is_reported();
    test_waiter_pins_object_until_unsubscribe();
    test_cancelled_waiter_returns();
    test_wait_return_repairs_stale_blocked_state();
    test_unrelated_scheduler_wake_retries_wait();
    test_ready_waiter_must_finish_stack_cleanup_before_reap();
    test_sigkill_during_block_publication_is_not_lost();
    test_thread_terminate_during_block_publication_is_not_lost();
    TEST_CHECK(atomic_load(&g_locked_predicate_calls) == 0U,
               "readiness callback ran under wait locks (IRQ lock inversion)");
    if (g_failures != 0U) {
        fprintf(stderr, "WAIT_SUBSCRIPTION_TESTS_FAIL count=%u\n", g_failures);
        return 1;
    }
    puts("WAIT_SUBSCRIPTION_TESTS_PASS");
    return 0;
}
