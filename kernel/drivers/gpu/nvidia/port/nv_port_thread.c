#include "nv_port.h"

#ifndef NV_PORT_HOST_TEST
#include <arch/x86_64/context.h>
#include <arch/x86_64/cpu.h>
#include <kernel/kmem.h>
#include <kernel/sched.h>
#else
#include <stdlib.h>
#include <threads.h>
#endif

#define NV_PORT_THREAD_STACK_SIZE (64U * 1024U)

#ifdef NV_PORT_HOST_TEST
static int nv_port_host_thread_entry(void *argument) {
    nv_port_thread_t *thread = (nv_port_thread_t *)argument;

    if (thread != 0 && thread->function != 0) {
        thread->function(thread->argument);
    }
    if (thread != 0) {
        atomic_store_explicit(&thread->finished, true, memory_order_release);
        nv_port_completion_signal(&thread->finished_event);
    }
    return 0;
}
#else
static __attribute__((noreturn)) void nv_port_kernel_thread_entry(void *argument) {
    nv_port_thread_t *thread = (nv_port_thread_t *)argument;

    if (thread != 0 && thread->function != 0) {
        thread->function(thread->argument);
        atomic_store_explicit(&thread->finished, true, memory_order_release);
        nv_port_completion_signal(&thread->finished_event);
        (void)sched_publish_dead(&thread->thread);
    }
    for (;;) schedule();
}
#endif

kstatus_t nv_port_thread_create(nv_port_thread_fn_t function, void *argument,
                                nv_port_thread_t **out) {
    if (function == 0 || out == 0) return K_EINVAL;
#ifdef NV_PORT_HOST_TEST
    nv_port_thread_t *thread =
        (nv_port_thread_t *)calloc(1U, sizeof(*thread));
#else
    nv_port_thread_t *thread =
        (nv_port_thread_t *)kzalloc(sizeof(*thread), 0U);
#endif
    if (thread == 0) return K_ENOMEM;

    atomic_init(&thread->stop_requested, false);
    atomic_init(&thread->started, false);
    atomic_init(&thread->finished, false);
    thread->function = function;
    thread->argument = argument;
    nv_port_completion_init(&thread->finished_event);
#ifdef NV_PORT_HOST_TEST
    thread->host_thread_valid = false;
#else
    thread->stack_base = vmalloc(NV_PORT_THREAD_STACK_SIZE);
    if (thread->stack_base == 0) {
        kfree(thread);
        return K_ENOMEM;
    }

    refcount_init(&thread->thread.object.refs, 1U);
    thread->thread.object.type = KOBJECT_TYPE_THREAD;
    thread->thread.object.flags = 0U;
    thread->thread.object.ops = 0;
    /* RM lock ownership and per-thread state require a unique nonzero ID;
     * zero-filled kernel workers must not alias one another. */
    thread->thread.tid = process_allocate_task_id();
    thread->thread.process = 0;
    sched_initialize_new_thread(&thread->thread, x86_current_cpu_index(), true);
    list_init(&thread->thread.process_node);
    list_init(&thread->thread.owned_mutexes);
    thread->thread.kernel_stack_base = thread->stack_base;
    thread->thread.kernel_stack_size = NV_PORT_THREAD_STACK_SIZE;
    thread->thread.kernel_stack_top =
        ((vaddr_t)(uintptr_t)thread->stack_base + NV_PORT_THREAD_STACK_SIZE) &
        ~(vaddr_t)0x0FULL;
    x86_prepare_kernel_thread(&thread->thread.arch, thread->thread.kernel_stack_top,
                              nv_port_kernel_thread_entry, thread);
    sched_monitor_register(&thread->thread, "nvidia-worker");
#endif
    *out = thread;
    return K_OK;
}

kstatus_t nv_port_thread_start(nv_port_thread_t *thread) {
    if (thread == 0) return K_EINVAL;
    bool expected = false;
    if (!atomic_compare_exchange_strong_explicit(
            &thread->started, &expected, true, memory_order_acq_rel,
            memory_order_acquire)) return K_EBUSY;
#ifdef NV_PORT_HOST_TEST
    if (thrd_create(&thread->host_thread, nv_port_host_thread_entry, thread) !=
        thrd_success) {
        atomic_store_explicit(&thread->started, false, memory_order_release);
        return K_EAGAIN;
    }
    thread->host_thread_valid = true;
    return K_OK;
#else
    /* A DEAD state precedes the final context switch. Retain an execution
     * reference until the scheduler's post-switch reaper releases it. */
    object_get(&thread->thread);
    __atomic_fetch_or(&thread->thread.flags, THREAD_FLAG_EXECUTION_REF,
                      __ATOMIC_RELEASE);
    kstatus_t status = sched_start_thread(&thread->thread);
    if (status != K_OK) {
        __atomic_fetch_and(&thread->thread.flags, ~THREAD_FLAG_EXECUTION_REF,
                           __ATOMIC_RELEASE);
        object_put(&thread->thread);
        atomic_store_explicit(&thread->started, false, memory_order_release);
    }
    return status;
#endif
}

bool nv_port_thread_should_stop(const nv_port_thread_t *thread) {
    return thread != 0 &&
           atomic_load_explicit(&thread->stop_requested, memory_order_acquire);
}

kstatus_t nv_port_thread_stop(nv_port_thread_t *thread) {
    if (thread == 0) return K_EINVAL;
    if (!atomic_load_explicit(&thread->started, memory_order_acquire)) {
        return K_EINVAL;
    }
    atomic_store_explicit(&thread->stop_requested, true, memory_order_release);
#ifndef NV_PORT_HOST_TEST
    if (atomic_load_explicit(&thread->thread.state, memory_order_acquire) ==
        THREAD_BLOCKED) sched_wake(&thread->thread);
#endif
    return K_OK;
}

kstatus_t nv_port_thread_join(nv_port_thread_t *thread, uint64_t timeout_ns) {
    if (thread == 0 ||
        !atomic_load_explicit(&thread->started, memory_order_acquire)) {
        return K_EINVAL;
    }
    if (atomic_load_explicit(&thread->finished, memory_order_acquire)) {
        return K_OK;
    }
    kstatus_t status = nv_port_completion_wait(&thread->finished_event,
                                               timeout_ns);
    if (status != K_OK) return status;
    return atomic_load_explicit(&thread->finished, memory_order_acquire) ?
           K_OK : K_EIO;
}

kstatus_t nv_port_thread_destroy(nv_port_thread_t *thread) {
    if (thread == 0) return K_EINVAL;
#ifdef NV_PORT_HOST_TEST
    if (!atomic_load_explicit(&thread->started, memory_order_acquire)) {
        free(thread);
        return K_OK;
    }
#else
    if (!atomic_load_explicit(&thread->started, memory_order_acquire)) {
        if (atomic_load_explicit(&thread->thread.state, memory_order_acquire) !=
            THREAD_NEW) return K_EBUSY;
        sched_monitor_unregister(&thread->thread);
        vfree(thread->stack_base);
        kfree(thread);
        return K_OK;
    }
#endif
    if (!atomic_load_explicit(&thread->finished, memory_order_acquire)) {
        return K_EBUSY;
    }
#ifdef NV_PORT_HOST_TEST
    if (thread->host_thread_valid) {
        if (thrd_join(thread->host_thread, 0) != thrd_success) return K_EBUSY;
        thread->host_thread_valid = false;
    }
    free(thread);
#else
    if (sched_current_thread() == &thread->thread) return K_EBUSY;
    if (atomic_load_explicit(&thread->thread.state, memory_order_acquire) !=
        THREAD_DEAD) return K_EBUSY;
    if (atomic_load_explicit(&thread->thread.object.refs.value,
                             memory_order_acquire) != 1U ||
        (__atomic_load_n(&thread->thread.flags, __ATOMIC_ACQUIRE) &
         (THREAD_FLAG_EXECUTION_REF | THREAD_FLAG_EXECUTION_REAPING)) != 0U)
        return K_EBUSY;
    sched_monitor_unregister(&thread->thread);
    vfree(thread->stack_base);
    kfree(thread);
#endif
    return K_OK;
}

static void nv_port_thread_test_entry(void *argument) {
    atomic_uint *runs = (atomic_uint *)argument;
    if (runs != 0) atomic_fetch_add_explicit(runs, 1U, memory_order_relaxed);
}

bool nv_port_thread_self_test(void) {
    atomic_uint runs;
    nv_port_thread_t *thread = 0;

    atomic_init(&runs, 0U);
    if (nv_port_thread_create(nv_port_thread_test_entry, &runs, &thread) !=
            K_OK || thread == 0 || nv_port_thread_start(thread) != K_OK ||
        nv_port_thread_join(thread, 1000000000ULL) != K_OK ||
        atomic_load_explicit(&runs, memory_order_acquire) != 1U) {
        if (thread != 0) {
            (void)nv_port_thread_stop(thread);
            (void)nv_port_thread_join(thread, 1000000000ULL);
            (void)nv_port_thread_destroy(thread);
        }
        return false;
    }
    return nv_port_thread_destroy(thread) == K_OK;
}
