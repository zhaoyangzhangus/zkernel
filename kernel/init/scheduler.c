#include <kernel/init_scheduler.h>

#include <arch/x86_64/cpu.h>
#include "arch/x86_64/paging.h"
#include <arch/x86_64/smp.h>
#include <kernel/debug_stage.h>
#include <kernel/handle.h>
#include <kernel/mutex.h>
#include <kernel/object.h>
#include <kernel/perf.h>
#include <kernel/process.h>
#include <kernel/realtest.h>
#include <kernel/sched.h>
#include <kernel/telemetry.h>
#include <kernel/vm.h>
#include <kernel/wait.h>

static BOOLEAN scheduler_fail_at(const kernel_init_scheduler_hooks_t *hooks,
                                 const CHAR8 *message, const char *file,
                                 uint32_t line) {
    kernel_debug_stage_fail_at(OS_DEBUG_PHASE_SCHEDULER,
                               OS_DEBUG_STEP_FAIL, K_EIO, file, line);
    hooks->write(message);
    hooks->halt();
    return 0;
}

#define scheduler_fail(hooks, message) \
    scheduler_fail_at((hooks), (message), __FILE__, __LINE__)

#if OS_REALTEST || OS_DEBUG_SERIAL
#define SCHED_BENCH_SAMPLES 64U
#define SCHED_BENCH_BATCH 256U

static void scheduler_benchmark_sort(uint64_t *samples) {
    for (uint32_t index = 1U; index < SCHED_BENCH_SAMPLES; ++index) {
        uint64_t value = samples[index];
        uint32_t position = index;
        while (position != 0U && samples[position - 1U] > value) {
            samples[position] = samples[position - 1U];
            --position;
        }
        samples[position] = value;
    }
}

static void scheduler_benchmark_emit_stats(
    const char *base_name,
    const char *min_name,
    const char *median_name,
    const char *average_name,
    const char *p95_name,
    uint64_t *samples) {
    uint64_t total = 0U;
    scheduler_benchmark_sort(samples);
    for (uint32_t index = 0U; index < SCHED_BENCH_SAMPLES; ++index) {
        total += samples[index];
    }
    uint32_t p95_index = (SCHED_BENCH_SAMPLES * 95U + 99U) / 100U - 1U;
    uint64_t median = samples[SCHED_BENCH_SAMPLES / 2U];
    uint64_t average = total / SCHED_BENCH_SAMPLES;
    kernel_perf_emit_value(base_name, median);
    kernel_perf_emit_value(min_name, samples[0]);
    kernel_perf_emit_value(median_name, median);
    kernel_perf_emit_value(average_name, average);
    kernel_perf_emit_value(p95_name, samples[p95_index]);
}

static bool scheduler_benchmark_queue_class(uint8_t class_id,
                                            uint8_t priority,
                                            const char *enqueue_base,
                                            const char *enqueue_min,
                                            const char *enqueue_median,
                                            const char *enqueue_average,
                                            const char *enqueue_p95,
                                            const char *dequeue_base,
                                            const char *dequeue_min,
                                            const char *dequeue_median,
                                            const char *dequeue_average,
                                            const char *dequeue_p95) {
    static thread_t threads[SCHED_BENCH_BATCH];
    uint64_t enqueue_samples[SCHED_BENCH_SAMPLES];
    uint64_t dequeue_samples[SCHED_BENCH_SAMPLES];
    uint32_t cpu_id = x86_current_cpu_index();

    if (cpu_id >= MAX_CPUS) return false;
    for (uint32_t index = 0U; index < SCHED_BENCH_BATCH; ++index) {
        sched_initialize_test_thread(&threads[index],
                                     0x100000U + index, class_id, priority);
        threads[index].owner_cpu = (uint16_t)cpu_id;
        threads[index].current_cpu = (uint16_t)cpu_id;
    }

    /* 先预热分配器、分支预测器和队列元数据。 */
    for (uint32_t index = 0U; index < SCHED_BENCH_BATCH; ++index) {
        if (!sched_enqueue_bootstrap(&threads[index])) return false;
    }
    for (uint32_t index = 0U; index < SCHED_BENCH_BATCH; ++index) {
        sched_remove(&threads[index]);
    }

    for (uint32_t sample = 0U; sample < SCHED_BENCH_SAMPLES; ++sample) {
        uint64_t start = telemetry_timestamp();
        for (uint32_t index = 0U; index < SCHED_BENCH_BATCH; ++index) {
            if (!sched_enqueue_bootstrap(&threads[index])) return false;
        }
        enqueue_samples[sample] =
            (telemetry_timestamp() - start) / SCHED_BENCH_BATCH;

        start = telemetry_timestamp();
        for (uint32_t index = 0U; index < SCHED_BENCH_BATCH; ++index) {
            sched_remove(&threads[index]);
        }
        dequeue_samples[sample] =
            (telemetry_timestamp() - start) / SCHED_BENCH_BATCH;
    }

    scheduler_benchmark_emit_stats(
        enqueue_base, enqueue_min, enqueue_median, enqueue_average,
        enqueue_p95, enqueue_samples);
    scheduler_benchmark_emit_stats(
        dequeue_base, dequeue_min, dequeue_median, dequeue_average,
        dequeue_p95, dequeue_samples);
    return sched_runnable_count() == 0U;
}

static bool scheduler_benchmark_queue_paths(void) {
    return scheduler_benchmark_queue_class(
               SCHED_CLASS_FAIR, 0U,
               "scheduler.enqueue", "scheduler.enqueue.fair.min",
               "scheduler.enqueue.fair.median",
               "scheduler.enqueue.fair.average", "scheduler.enqueue.fair.p95",
               "scheduler.dequeue", "scheduler.dequeue.fair.min",
               "scheduler.dequeue.fair.median",
               "scheduler.dequeue.fair.average", "scheduler.dequeue.fair.p95") &&
           scheduler_benchmark_queue_class(
               SCHED_CLASS_RT, 7U,
               "scheduler.enqueue.rt", "scheduler.enqueue.rt.min",
               "scheduler.enqueue.rt.median",
               "scheduler.enqueue.rt.average", "scheduler.enqueue.rt.p95",
               "scheduler.dequeue.rt", "scheduler.dequeue.rt.min",
               "scheduler.dequeue.rt.median",
               "scheduler.dequeue.rt.average", "scheduler.dequeue.rt.p95");
}

static bool scheduler_benchmark_schedule_single(void) {
    static thread_t thread;
    uint64_t samples[SCHED_BENCH_SAMPLES];
    uint32_t cpu_id = x86_current_cpu_index();

    if (cpu_id >= MAX_CPUS) return false;
    sched_initialize_test_thread(&thread, 0x200000U, SCHED_CLASS_FAIR, 0U);
    thread.owner_cpu = (uint16_t)cpu_id;
    thread.current_cpu = (uint16_t)cpu_id;
    if (!sched_enqueue_bootstrap(&thread)) return false;
    schedule();

    for (uint32_t index = 0U; index < SCHED_BENCH_BATCH; ++index) {
        schedule();
    }
    for (uint32_t sample = 0U; sample < SCHED_BENCH_SAMPLES; ++sample) {
        uint64_t start = telemetry_timestamp();
        for (uint32_t index = 0U; index < SCHED_BENCH_BATCH; ++index) {
            schedule();
        }
        samples[sample] =
            (telemetry_timestamp() - start) / SCHED_BENCH_BATCH;
    }
    scheduler_benchmark_emit_stats(
        "scheduler.schedule", "scheduler.schedule_same_thread.min",
        "scheduler.schedule_same_thread.median",
        "scheduler.schedule_same_thread.average",
        "scheduler.schedule_same_thread.p95", samples);

    if (!sched_publish_blocked(&thread)) return false;
    schedule();
    return sched_current_thread() != &thread;
}

static bool scheduler_benchmark_schedule_pair(uint8_t class_id,
                                              const char *base_name,
                                              const char *min_name,
                                              const char *median_name,
                                              const char *average_name,
                                              const char *p95_name) {
    static thread_t first;
    static thread_t second;
    uint64_t samples[SCHED_BENCH_SAMPLES];
    uint32_t cpu_id = x86_current_cpu_index();

    if (cpu_id >= MAX_CPUS) return false;
    sched_initialize_test_thread(&first, 0x210000U, class_id, 7U);
    sched_initialize_test_thread(&second, 0x210001U, class_id, 7U);
    first.owner_cpu = second.owner_cpu = (uint16_t)cpu_id;
    first.current_cpu = second.current_cpu = (uint16_t)cpu_id;
    if (!sched_enqueue_bootstrap(&first) ||
        !sched_enqueue_bootstrap(&second)) return false;
    schedule();

    for (uint32_t index = 0U; index < SCHED_BENCH_BATCH; ++index) {
        if (class_id == SCHED_CLASS_FAIR) {
            thread_t *current = sched_current_thread();
            if (current == &first || current == &second) {
                ++current->sched.vruntime;
            }
        }
        schedule();
    }
    for (uint32_t sample = 0U; sample < SCHED_BENCH_SAMPLES; ++sample) {
        uint64_t start = telemetry_timestamp();
        for (uint32_t index = 0U; index < SCHED_BENCH_BATCH; ++index) {
            if (class_id == SCHED_CLASS_FAIR) {
                thread_t *current = sched_current_thread();
                if (current == &first || current == &second) {
                    ++current->sched.vruntime;
                }
            }
            schedule();
        }
        samples[sample] =
            (telemetry_timestamp() - start) / SCHED_BENCH_BATCH;
    }
    scheduler_benchmark_emit_stats(
        base_name, min_name, median_name, average_name, p95_name, samples);

    thread_t *current = sched_current_thread();
    if (current == &first || current == &second) {
        if (!sched_publish_blocked(current)) return false;
        schedule();
    }
    current = sched_current_thread();
    if (current == &first || current == &second) {
        if (!sched_publish_blocked(current)) return false;
        schedule();
    }
    sched_remove(&first);
    sched_remove(&second);
    return sched_current_thread() != &first &&
           sched_current_thread() != &second;
}

static bool scheduler_benchmark_schedule_paths(void) {
    return scheduler_benchmark_schedule_single() &&
           scheduler_benchmark_schedule_pair(
               SCHED_CLASS_FAIR, "scheduler.schedule_two_fair_threads",
               "scheduler.schedule_two_fair_threads.min",
               "scheduler.schedule_two_fair_threads.median",
               "scheduler.schedule_two_fair_threads.average",
               "scheduler.schedule_two_fair_threads.p95") &&
           scheduler_benchmark_schedule_pair(
               SCHED_CLASS_RT, "scheduler.schedule_rt",
               "scheduler.schedule_rt.min", "scheduler.schedule_rt.median",
               "scheduler.schedule_rt.average", "scheduler.schedule_rt.p95");
}

static bool scheduler_benchmark_local_wake(void) {
    static thread_t threads[SCHED_BENCH_BATCH];
    uint64_t samples[SCHED_BENCH_SAMPLES];
    uint32_t cpu_id = x86_current_cpu_index();

    if (cpu_id >= MAX_CPUS) return false;
    for (uint32_t index = 0U; index < SCHED_BENCH_BATCH; ++index) {
        sched_initialize_test_thread(&threads[index],
                                     0x220000U + index,
                                     SCHED_CLASS_FAIR, 0U);
        threads[index].owner_cpu = (uint16_t)cpu_id;
        threads[index].current_cpu = (uint16_t)cpu_id;
        atomic_store_explicit(&threads[index].state, THREAD_BLOCKED,
                              memory_order_relaxed);
    }

    for (uint32_t index = 0U; index < SCHED_BENCH_BATCH; ++index) {
        sched_wake(&threads[index]);
    }
    for (uint32_t index = 0U; index < SCHED_BENCH_BATCH; ++index) {
        sched_remove(&threads[index]);
        atomic_store_explicit(&threads[index].state, THREAD_BLOCKED,
                              memory_order_relaxed);
    }

    for (uint32_t sample = 0U; sample < SCHED_BENCH_SAMPLES; ++sample) {
        uint64_t start = telemetry_timestamp();
        for (uint32_t index = 0U; index < SCHED_BENCH_BATCH; ++index) {
            if (atomic_load_explicit(&threads[index].state,
                                     memory_order_relaxed) !=
                    THREAD_BLOCKED) {
                return false;
            }
            sched_wake(&threads[index]);
        }
        samples[sample] =
            (telemetry_timestamp() - start) / SCHED_BENCH_BATCH;
        for (uint32_t index = 0U; index < SCHED_BENCH_BATCH; ++index) {
            sched_remove(&threads[index]);
            atomic_store_explicit(&threads[index].state, THREAD_BLOCKED,
                                  memory_order_relaxed);
        }
    }
    scheduler_benchmark_emit_stats(
        "scheduler.wake", "scheduler.wake.local_wake.min",
        "scheduler.wake.local_wake.median",
        "scheduler.wake.local_wake.average",
        "scheduler.wake.local_wake.p95", samples);
    schedule();
    return sched_runnable_count() == 0U;
}

#endif

static BOOLEAN canonical_scheduler_self_test(void) {
    static thread_t fair_threads[300];
    thread_t thread;
    uint64_t benchmark_start;
    for (UINTN i = 0; i < sizeof(thread); ++i) ((UINT8 *)&thread)[i] = 0;
    atomic_init(&thread.state, THREAD_READY);
    atomic_init(&thread.block_epoch, 0U);
    atomic_init(&thread.blocked_waiter, 0);
    atomic_init(&thread.command_ack, 0U);
    thread.sched_class = SCHED_CLASS_FAIR;
    thread.owner_cpu = 0;
    thread.current_cpu = 0;
    list_init(&thread.sched.rt_node);
    benchmark_start = telemetry_timestamp();
    if (!sched_enqueue_bootstrap(&thread)) return 0;
    kernel_perf_emit_scope("scheduler.enqueue", benchmark_start);
    realtest_checkpoint("SCHED_CORE_STAGE_ENQUEUE_OK");
    benchmark_start = telemetry_timestamp();
    schedule();
    kernel_perf_emit_scope("scheduler.schedule", benchmark_start);
    if (atomic_load_explicit(&thread.state, memory_order_relaxed) != THREAD_RUNNING) return 0;
    realtest_checkpoint("SCHED_CORE_STAGE_SCHEDULE_OK");
    benchmark_start = telemetry_timestamp();
    sched_block_current();
    kernel_perf_emit_scope("scheduler.block", benchmark_start);
    if (atomic_load_explicit(&thread.state, memory_order_relaxed) != THREAD_BLOCKED) return 0;
    realtest_checkpoint("SCHED_CORE_STAGE_BLOCK_OK");

    benchmark_start = telemetry_timestamp();
    sched_wake(&thread);
    kernel_perf_emit_scope("scheduler.wake", benchmark_start);
    benchmark_start = telemetry_timestamp();
    schedule();
    kernel_perf_emit_scope("scheduler.wake_to_running", benchmark_start);
    if (atomic_load_explicit(&thread.state, memory_order_relaxed) != THREAD_RUNNING) return 0;
    realtest_checkpoint("SCHED_CORE_STAGE_WAKE_OK");
    /* 不要让启动夹具影响下方的公平调度树计数。 */
    sched_block_current();
    if (atomic_load_explicit(&thread.state, memory_order_relaxed) != THREAD_BLOCKED) return 0;
    realtest_checkpoint("SCHED_CORE_STAGE_REBLOCK_OK");

    /* 瓒呰繃鏃ф暟缁勪笂闄愶紝骞朵互浜掔礌姝ラ暱涔卞簭鍒犻櫎锛岃鐩栫孩榛戞爲鍒犻櫎淇璺緞銆?*/
    for (UINT32 i = 0; i < 300U; ++i) {
        for (UINTN byte = 0; byte < sizeof(thread_t); ++byte) {
            ((UINT8 *)&fair_threads[i])[byte] = 0;
        }
        atomic_init(&fair_threads[i].state, THREAD_READY);
        atomic_init(&fair_threads[i].block_epoch, 0U);
        atomic_init(&fair_threads[i].blocked_waiter, 0);
        atomic_init(&fair_threads[i].command_ack, 0U);
        fair_threads[i].tid = i + 1U;
        fair_threads[i].sched_class = SCHED_CLASS_FAIR;
        fair_threads[i].owner_cpu = 0;
        fair_threads[i].current_cpu = 0;
        fair_threads[i].sched.vruntime = (uint64_t)((i * 197U) % 307U);
        list_init(&fair_threads[i].sched.rt_node);
        sched_enqueue_bootstrap(&fair_threads[i]);
    }
    realtest_checkpoint("SCHED_CORE_STAGE_BULK_ENQUEUE_OK");
    if (sched_runnable_count() != 300U || !sched_validate_current_cpu()) return 0;
    realtest_checkpoint("SCHED_CORE_STAGE_VALIDATE_OK");
    realtest_checkpoint("SCHED_CORE_STAGE_REMOVE_BEGIN");
    for (UINT32 i = 0; i < 300U; ++i) {
        UINT32 index = (i * 73U) % 300U;
        benchmark_start = telemetry_timestamp();
        sched_remove(&fair_threads[index]);
        if (i == 0U) kernel_perf_emit_scope("scheduler.dequeue", benchmark_start);
        if ((i & 15U) == 0 && !sched_validate_current_cpu()) return 0;
        if ((i & 63U) == 63U) {
            realtest_checkpoint("SCHED_CORE_STAGE_REMOVE_PROGRESS");
        }
    }
    realtest_checkpoint("SCHED_CORE_STAGE_REMOVE_DONE");
    return sched_runnable_count() == 0 && sched_validate_current_cpu();
}

static BOOLEAN g_canonical_object_destroyed;

static VOID canonical_object_destroy(void *object) {
    (void)object;
    g_canonical_object_destroyed = 1;
}

static const object_ops_t g_canonical_object_ops = {
    .destroy = canonical_object_destroy,
    .type_name = "boot-test-object",
    .is_signaled = 0,
    .wait_value = 0,
};

static bool canonical_true_predicate(void *context) {
    return *(BOOLEAN *)context != 0;
}

static BOOLEAN canonical_object_handle_self_test(void) {
    struct {
        object_header_t header;
        UINT64 value;
    } object = {0};
    handle_table_t table;
    handle_t handle = 0;
    void *lookup = 0;
    BOOLEAN condition = 1;
    wait_queue_t queue;
    object.header.ops = &g_canonical_object_ops;
    object.header.type = 1U;
    refcount_init(&object.header.refs, 1U);
    g_canonical_object_destroyed = 0;
    if (handle_table_init(&table) != K_OK ||
        handle_create(&table, &object, 0x3U, &handle) != K_OK ||
        handle_lookup(&table, handle, 0x1U, &lookup) != K_OK || lookup != &object ||
        handle_close(&table, handle) != K_OK ||
        handle_lookup(&table, handle, 0U, &lookup) != K_ENOENT) return 0;
    object_put(lookup);
    /* 在真实内核中测试跨分块边界的 close-on-exec。 */
    handle_t exec_handles[300];
    for (uint32_t index = 0U; index < 300U; ++index) {
        if (handle_create_with_flags(&table, &object, 0x3U,
                                    (index & 1U) ? HANDLE_FLAG_CLOEXEC : 0U,
                                    &exec_handles[index]) != K_OK) return 0;
    }
    if (handle_table_close_on_exec(&table) != K_OK ||
        handle_table_close_on_exec(&table) != K_OK) return 0;
    for (uint32_t index = 0U; index < 300U; ++index) {
        lookup = 0;
        kstatus_t status = handle_lookup(&table, exec_handles[index], 0x1U, &lookup);
        if ((index & 1U) != 0U) {
            if (status != K_ENOENT || lookup != 0) return 0;
        } else {
            if (status != K_OK || lookup != &object) return 0;
            object_put(lookup);
        }
    }
    handle_table_destroy(&table);
    object_put(&object);
    wait_queue_init(&queue);
    return g_canonical_object_destroyed &&
           wait_on_queue(&queue, canonical_true_predicate, &condition, 1000U) == K_OK;
}

static BOOLEAN canonical_vm_self_test(void) {
    vm_space_t *parent = 0;
    vm_space_t *child = 0;
    vm_space_t *sparse_space = 0;
    vm_object_t *object = 0;
    vm_object_t *shared_object = 0;
    vm_object_t *sparse_object = 0;
    vm_object_t *index_object = 0;
    vaddr_t address = 0x0000000040000000ULL;
    vaddr_t shared_parent_address = 0x0000000050000000ULL;
    vaddr_t shared_child_address = 0x0000000060000000ULL;
    vaddr_t sparse_address = 0x0000200000000000ULL;
    const uint64_t sparse_size = 1ULL << 40;
    const uint32_t index_test_pages = 257U;
    paddr_t parent_physical = paddr_make(0);
    paddr_t child_physical = paddr_make(0);
    paddr_t shared_parent_physical = paddr_make(0);
    paddr_t shared_child_physical = paddr_make(0);
    uint64_t pte_flags = 0;
    uint64_t benchmark_start;
    kstatus_t benchmark_status;
    BOOLEAN success = 0;

    if (vm_space_create(&parent) != K_OK ||
        vm_object_create_anon(PAGE_SIZE * 3U, &object) != K_OK) goto cleanup;

    benchmark_start = telemetry_timestamp();
    benchmark_status = vm_map_object(
        parent, object, &address, 0, PAGE_SIZE * 3U,
        VM_PROT_READ | VM_PROT_WRITE | VM_PROT_USER,
        VM_MAP_PRIVATE | VM_MAP_FIXED);
    kernel_perf_emit_scope("mm.vma_insert", benchmark_start);
    if (benchmark_status != K_OK) goto cleanup;

    benchmark_start = telemetry_timestamp();
    benchmark_status = vm_handle_fault(
        parent, &(vm_fault_info_t){address, VM_PROT_WRITE, 0});
    kernel_perf_emit_scope("mm.page_fault", benchmark_start);
    if (benchmark_status != K_OK ||
        x86_translate_page(parent->root_table, address, &parent_physical, 0) != K_OK) goto cleanup;

    /* 页面现在已常驻；此测试将 VMA 树查找与首次缺页分配路径隔离。 */
    benchmark_start = telemetry_timestamp();
    benchmark_status = vm_handle_fault(
        parent, &(vm_fault_info_t){address, VM_PROT_READ, 0});
    kernel_perf_emit_scope("mm.vma_lookup", benchmark_start);
    if (benchmark_status != K_OK) goto cleanup;

    UINT8 *parent_bytes = (UINT8 *)phys_to_direct(parent_physical);
    if (parent_bytes == 0) goto cleanup;
    parent_bytes[0] = 0x5AU;

    if (vm_space_clone_cow(parent, &child) != K_OK ||
        vm_handle_fault(child, &(vm_fault_info_t){address, VM_PROT_READ, 0}) != K_OK ||
        x86_translate_page(child->root_table, address, &child_physical, 0) != K_OK ||
        child_physical.value != parent_physical.value ||
        vm_handle_fault(child, &(vm_fault_info_t){address, VM_PROT_WRITE, 3U}) != K_OK ||
        x86_translate_page(child->root_table, address, &child_physical, 0) != K_OK ||
        child_physical.value == parent_physical.value) goto cleanup;

    UINT8 *child_bytes = (UINT8 *)phys_to_direct(child_physical);
    if (child_bytes == 0 || child_bytes[0] != 0x5AU) goto cleanup;
    child_bytes[0] = 0xA5U;
    if (parent_bytes[0] != 0x5AU) goto cleanup;

    vaddr_t middle = address + PAGE_SIZE;
    if (vm_handle_fault(parent, &(vm_fault_info_t){middle, VM_PROT_WRITE, 0}) != K_OK ||
        vm_protect(parent, middle, PAGE_SIZE, VM_PROT_READ | VM_PROT_USER) != K_OK ||
        x86_translate_page(parent->root_table, middle, &parent_physical, &pte_flags) != K_OK ||
        (pte_flags & 2U) != 0 ||
        vm_protect(parent, middle, PAGE_SIZE,
                   VM_PROT_READ | VM_PROT_WRITE | VM_PROT_USER) != K_OK ||
        vm_handle_fault(parent, &(vm_fault_info_t){middle, VM_PROT_WRITE, 3U}) != K_OK ||
        x86_translate_page(parent->root_table, middle, &parent_physical, &pte_flags) != K_OK ||
        (pte_flags & 2U) == 0) goto cleanup;

    benchmark_start = telemetry_timestamp();
    benchmark_status = vm_unmap(parent, middle, PAGE_SIZE);
    kernel_perf_emit_scope("mm.vma_remove", benchmark_start);
    if (benchmark_status != K_OK ||
        x86_translate_page(parent->root_table, middle, &parent_physical, 0) != K_ENOENT) goto cleanup;

    /* 鍏变韩瀵硅薄蹇呴』鍦ㄤ袱涓湴鍧€绌洪棿涓В鏋愬埌鍚屼竴鍚庡椤碉紝涓斿啓鍏ョ珛鍗冲彲瑙併€?*/
    if (vm_object_create_shared(PAGE_SIZE, &shared_object) != K_OK ||
        vm_map_object(parent, shared_object, &shared_parent_address, 0, PAGE_SIZE,
                      VM_PROT_READ | VM_PROT_WRITE | VM_PROT_USER,
                      VM_MAP_PRIVATE | VM_MAP_FIXED) != K_EINVAL ||
        vm_map_object(parent, shared_object, &shared_parent_address, 0, PAGE_SIZE,
                      VM_PROT_READ | VM_PROT_WRITE | VM_PROT_USER,
                      VM_MAP_SHARED | VM_MAP_FIXED) != K_OK ||
        vm_map_object(child, shared_object, &shared_child_address, 0, PAGE_SIZE,
                      VM_PROT_READ | VM_PROT_WRITE | VM_PROT_USER,
                      VM_MAP_SHARED | VM_MAP_FIXED) != K_OK ||
        vm_handle_fault(parent, &(vm_fault_info_t){shared_parent_address,
                                                   VM_PROT_WRITE, 0}) != K_OK ||
        vm_handle_fault(child, &(vm_fault_info_t){shared_child_address,
                                                  VM_PROT_READ, 0}) != K_OK ||
        x86_translate_page(parent->root_table, shared_parent_address,
                           &shared_parent_physical, 0) != K_OK ||
        x86_translate_page(child->root_table, shared_child_address,
                           &shared_child_physical, 0) != K_OK ||
        shared_parent_physical.value != shared_child_physical.value) goto cleanup;
    UINT8 *shared_bytes = (UINT8 *)phys_to_direct(shared_parent_physical);
    UINT8 *shared_child_bytes = (UINT8 *)phys_to_direct(shared_child_physical);
    if (shared_bytes == 0 || shared_child_bytes == 0) goto cleanup;
    shared_bytes[0] = 0xC3U;
    if (shared_child_bytes[0] != 0xC3U) goto cleanup;

    /* 回归测试：Chrome/PartitionAlloc 会预留 TB 级稀疏区域，并在页面缺页前修改其保护属性。 */
    if (vm_space_create(&sparse_space) != K_OK ||
        vm_object_create_anon(sparse_size, &sparse_object) != K_OK ||
        vm_map_object(sparse_space, sparse_object, &sparse_address,
                      0, sparse_size,
                      VM_PROT_READ | VM_PROT_WRITE | VM_PROT_USER,
                      VM_MAP_PRIVATE | VM_MAP_FIXED) != K_OK ||
        x86_page_range_has_present_pages(
            sparse_space->root_table, sparse_address,
            sparse_address + sparse_size) ||
        vm_protect(sparse_space, sparse_address, sparse_size,
                   VM_PROT_READ | VM_PROT_USER) != K_OK ||
        vm_unmap(sparse_space, sparse_address, sparse_size) != K_OK) {
        goto cleanup;
    }

    /* 随机稀疏页面访问通过共享后备 API 测试匿名页索引插入和命中。 */
    if (vm_object_create_shared(PAGE_SIZE * index_test_pages,
                                &index_object) != K_OK) {
        goto cleanup;
    }
    for (uint32_t iteration = 0; iteration < index_test_pages; ++iteration) {
        uint64_t page_index = ((uint64_t)iteration * 73U) % index_test_pages;
        uint8_t *bytes = 0;
        if (vm_object_shared_page_direct(index_object,
                                         page_index * PAGE_SIZE,
                                         true, &bytes) != K_OK ||
            bytes == 0) {
            goto cleanup;
        }
        bytes[0] = (uint8_t)page_index;
        bytes[1] = (uint8_t)(page_index >> 8U);
    }
    if (vm_object_populated_pages(index_object) != index_test_pages) {
        goto cleanup;
    }
    for (uint32_t iteration = 0; iteration < index_test_pages; ++iteration) {
        uint64_t page_index = ((uint64_t)iteration * 73U) % index_test_pages;
        uint8_t *bytes = 0;
        if (vm_object_shared_page_direct(index_object,
                                         page_index * PAGE_SIZE,
                                         true, &bytes) != K_OK ||
            bytes == 0 || bytes[0] != (uint8_t)page_index ||
            bytes[1] != (uint8_t)(page_index >> 8U)) {
            goto cleanup;
        }
    }
    success = 1;

cleanup:
    if (sparse_space != 0) vm_space_put(sparse_space);
    if (child != 0) vm_space_put(child);
    if (parent != 0) vm_space_put(parent);
    if (object != 0) vm_object_put(object);
    if (shared_object != 0) vm_object_put(shared_object);
    if (sparse_object != 0) vm_object_put(sparse_object);
    if (index_object != 0) vm_object_put(index_object);
    return success;
}

BOOLEAN kernel_init_scheduler(const OS_BOOT_INFO *boot_info,
                              const kernel_init_scheduler_hooks_t *hooks) {
    uint64_t benchmark_start;
    if (boot_info == 0 || hooks == 0 || hooks->write == 0 ||
        hooks->write_u32 == 0 || hooks->halt == 0) return 0;

    kernel_debug_stage(OS_DEBUG_PHASE_REFACTOR_4,
                       OS_DEBUG_STEP_ENTER, 0U);
    realtest_mark("SMP_CALL_BEGIN");
    if (!x86_smp_start_aps(boot_info) ||
        x86_smp_started_count() != x86_smp_discovered_count()) {
        return scheduler_fail(hooks, "OS_SMP_START_FAIL\r\n");
    }
    realtest_mark("SMP_CALL_OK");
    realtest_mark("SMP_REPORT_BEGIN");
    hooks->write("OS_SMP_STARTED_COUNT=");
    hooks->write_u32(x86_smp_started_count());
    hooks->write("\r\nOS_SMP_TRAMPOLINE_OK\r\n");
    realtest_mark("SMP_REPORT_OK");
    realtest_mark("SCHED_INIT_BEGIN");
    sched_init();
    realtest_mark("SCHED_INIT_OK");
    realtest_mark("SMP_RELEASE_BEGIN");
    if (!x86_smp_release_aps()) {
        return scheduler_fail(hooks, "OS_SMP_RELEASE_FAIL\r\n");
    }
    realtest_mark("SMP_RELEASE_OK");
    realtest_mark("SMP_POST_RELEASE_REPORT_BEGIN");
    hooks->write("OS_SMP_CPU_LOCAL_OK\r\n");
    realtest_mark("SMP_POST_RELEASE_REPORT_OK");
    realtest_mark("SMP_IPI_TEST_BEGIN");
    if (!x86_smp_ipi_self_test()) {
        return scheduler_fail(hooks, "OS_SMP_IPI_FAIL\r\n");
    }
    realtest_mark("SMP_IPI_TEST_OK");
    hooks->write("OS_SMP_IPI_OK\r\n");
    realtest_mark("SMP_TLB_TEST_BEGIN");
    benchmark_start = telemetry_timestamp();
    bool tlb_test_ok = x86_tlb_shootdown_self_test();
    kernel_perf_emit_scope("mm.tlb_shootdown", benchmark_start);
    if (!tlb_test_ok) {
        return scheduler_fail(hooks, "OS_TLB_SHOOTDOWN_FAIL\r\n");
    }
    realtest_checkpoint("SMP_TLB_TEST_OK");
    hooks->write("OS_TLB_SHOOTDOWN_OK\r\n");
    realtest_checkpoint("SCHED_SPEC4_READY_BEGIN");
    kernel_debug_stage_ready(OS_DEBUG_PHASE_SPEC_4);
    realtest_checkpoint("SCHED_SPEC4_READY_OK");
    realtest_checkpoint("SCHED_SPEC5_ENTER_BEGIN");
    kernel_debug_stage_enter(OS_DEBUG_PHASE_SPEC_5);
    realtest_checkpoint("SCHED_SPEC5_ENTER_OK");
    realtest_checkpoint("SCHED_CORE_TEST_BEGIN");
    benchmark_start = telemetry_timestamp();
    if (!canonical_scheduler_self_test()) {
        return scheduler_fail(hooks, "OS_SCHED_CORE_FAIL\r\n");
    }
    realtest_checkpoint("SCHED_CORE_TEST_OK");
    realtest_checkpoint("SCHED_CORE_BENCH_BEGIN");
    kernel_perf_emit_scope("scheduler.core", benchmark_start);
#if OS_REALTEST || OS_DEBUG_SERIAL
    /* 合成计时循环属于诊断启动，不应进入桌面关键路径。功能性调度器测试在每个构建中保留。 */
    if (!scheduler_benchmark_queue_paths()) {
        return scheduler_fail(hooks, "OS_SCHED_BENCH_FAIL\r\n");
    }
    if (!scheduler_benchmark_schedule_paths()) {
        return scheduler_fail(hooks, "OS_SCHED_PATH_BENCH_FAIL\r\n");
    }
    if (!scheduler_benchmark_local_wake()) {
        return scheduler_fail(hooks, "OS_SCHED_WAKE_BENCH_FAIL\r\n");
    }
    hooks->write("OS_SCHED_BENCH_OK\r\n");
#endif
    realtest_checkpoint("SCHED_CORE_BENCH_OK");
    realtest_checkpoint("SCHED_CORE_REPORT_BEGIN");
    hooks->write("OS_SCHED_CORE_OK\r\n");
    realtest_checkpoint("SCHED_CORE_REPORT_OK");
    benchmark_start = telemetry_timestamp();
    if (!sched_accounting_self_test()) {
        return scheduler_fail(hooks, "OS_SCHED_ACCOUNTING_FAIL\r\n");
    }
    kernel_perf_emit_scope("scheduler.accounting", benchmark_start);
    hooks->write("OS_SCHED_ACCOUNTING_OK\r\n");
    if (!sched_fair_policy_self_test()) {
        return scheduler_fail(hooks, "OS_SCHED_FAIR_FAIL\r\n");
    }
    hooks->write("OS_SCHED_FAIR_OK\r\n");
    if (!sched_rt_policy_self_test()) {
        return scheduler_fail(hooks, "OS_SCHED_RT_FAIL\r\n");
    }
    hooks->write("OS_SCHED_RT_OK\r\n");
    benchmark_start = telemetry_timestamp();
    if (!sched_balance_self_test()) {
        return scheduler_fail(hooks, "OS_SCHED_BALANCE_FAIL\r\n");
    }
    kernel_perf_emit_scope("scheduler.balance", benchmark_start);
    hooks->write("OS_SCHED_BALANCE_OK\r\n");
    kernel_debug_stage(OS_DEBUG_PHASE_REFACTOR_4,
                       OS_DEBUG_STEP_PROGRESS, 3U);
    if (!sched_state_transition_self_test()) {
        return scheduler_fail(hooks, "OS_SCHED_STATE_TRANSITION_FAIL\r\n");
    }
    hooks->write("OS_SCHED_STATE_TRANSITION_OK\r\n");
    if (!sched_remote_remove_ack_self_test()) {
        return scheduler_fail(hooks, "OS_SCHED_REMOTE_REMOVE_FAIL\r\n");
    }
    hooks->write("OS_SCHED_REMOTE_REMOVE_OK\r\n");
    if (!sched_remote_wake_self_test()) {
        return scheduler_fail(hooks, "OS_SCHED_REMOTE_WAKE_FAIL\r\n");
    }
    hooks->write("OS_SCHED_REMOTE_WAKE_OK\r\n");
    kernel_debug_stage(OS_DEBUG_PHASE_REFACTOR_4,
                       OS_DEBUG_STEP_PROGRESS, 4U);
    if (!sched_context_switch_self_test()) {
        return scheduler_fail(hooks, "OS_CONTEXT_SWITCH_TEST_FAIL\r\n");
    }
    hooks->write("OS_CONTEXT_SWITCH_OK\r\n");
    /* 空闲 CPU 在执行 HLT 前轮询待处理唤醒，以关闭 IPI 丢失窗口。 */
    kernel_debug_stage(OS_DEBUG_PHASE_REFACTOR_4,
                       OS_DEBUG_STEP_PROGRESS, 6U);
    kernel_debug_stage(OS_DEBUG_PHASE_SCHEDULER,
                       OS_DEBUG_STEP_READY, 1U);
    kernel_debug_stage(OS_DEBUG_PHASE_REFACTOR_4,
                       OS_DEBUG_STEP_PROGRESS, 2U);
    kernel_debug_stage(OS_DEBUG_PHASE_REFACTOR_4,
                       OS_DEBUG_STEP_PROGRESS, 5U);
    if (!kmutex_pi_self_test()) {
        return scheduler_fail(hooks, "OS_MUTEX_PI_FAIL\r\n");
    }
    hooks->write("OS_MUTEX_PI_OK\r\n");
    kernel_debug_stage_ready(OS_DEBUG_PHASE_SPEC_5);
    kernel_debug_stage_enter(OS_DEBUG_PHASE_SPEC_6);
    kernel_debug_stage_enter(OS_DEBUG_PHASE_SPEC_8);
    kernel_debug_stage(OS_DEBUG_PHASE_REFACTOR_6,
                       OS_DEBUG_STEP_ENTER, 0U);
    if (!canonical_object_handle_self_test()) {
        return scheduler_fail(hooks, "OS_OBJECT_CORE_FAIL\r\n");
    }
    hooks->write("OS_OBJECT_CORE_OK\r\n");
    benchmark_start = telemetry_timestamp();
    if (!canonical_vm_self_test()) {
        return scheduler_fail(hooks, "OS_VM_CORE_FAIL\r\n");
    }
    kernel_perf_emit_scope("mm.vm", benchmark_start);
    hooks->write("OS_VM_CORE_OK\r\nOS_VM_SHARED_OK\r\n");
    kernel_debug_stage(OS_DEBUG_PHASE_REFACTOR_6,
                       OS_DEBUG_STEP_PROGRESS, 7U);
    kernel_debug_stage(OS_DEBUG_PHASE_REFACTOR_6,
                       OS_DEBUG_STEP_PROGRESS, 1U);
    kernel_debug_stage(OS_DEBUG_PHASE_REFACTOR_6,
                       OS_DEBUG_STEP_PROGRESS, 2U);
    kernel_debug_stage(OS_DEBUG_PHASE_REFACTOR_6,
                       OS_DEBUG_STEP_PROGRESS, 3U);
    kernel_debug_stage(OS_DEBUG_PHASE_REFACTOR_6,
                       OS_DEBUG_STEP_PROGRESS, 4U);
    kernel_debug_stage(OS_DEBUG_PHASE_REFACTOR_6,
                       OS_DEBUG_STEP_PROGRESS, 5U);
    /* 缺页入口现在将 CR2/错误信息交给 MM 策略所有者。 */
    kernel_debug_stage(OS_DEBUG_PHASE_REFACTOR_6,
                       OS_DEBUG_STEP_PROGRESS, 6U);
    kernel_debug_stage(OS_DEBUG_PHASE_REFACTOR_2,
                       OS_DEBUG_STEP_READY, 1U);
    benchmark_start = telemetry_timestamp();
    if (!process_core_self_test()) {
        return scheduler_fail(hooks, "OS_PROCESS_CORE_FAIL\r\n");
    }
    kernel_perf_emit_scope("process.core", benchmark_start);
    hooks->write("OS_PROCESS_CORE_OK\r\n");
    kernel_debug_stage(OS_DEBUG_PHASE_REFACTOR_5,
                       OS_DEBUG_STEP_PROGRESS, 3U);
    kernel_debug_stage(OS_DEBUG_PHASE_REFACTOR_5,
                       OS_DEBUG_STEP_PROGRESS, 4U);
    kernel_debug_stage(OS_DEBUG_PHASE_REFACTOR_5,
                       OS_DEBUG_STEP_PROGRESS, 5U);
    kernel_debug_stage_ready(OS_DEBUG_PHASE_SPEC_6);
    kernel_debug_stage(OS_DEBUG_PHASE_REFACTOR_3,
                       OS_DEBUG_STEP_PROGRESS, 6U);
    kernel_debug_stage(OS_DEBUG_PHASE_REFACTOR_4,
                       OS_DEBUG_STEP_PROGRESS, 1U);
    realtest_checkpoint("SMP_AP_TIMER_ENABLE_BEGIN");
    if (!x86_smp_enable_ap_timers()) {
        return scheduler_fail(hooks, "OS_SMP_TIMER_ENABLE_FAIL\r\n");
    }
    realtest_mark("SMP_AP_TIMER_ENABLE_OK");
    return 1;
}
