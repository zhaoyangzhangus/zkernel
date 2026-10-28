#pragma once
#pragma once
#include "base.h"
#include "object.h"
#include "list.h"
#include "refcount.h"
#include "wait.h"

struct device;
struct process;
struct vm_space;

#define KOBJECT_TYPE_GPU_ALLOCATION 0x0110U
#define KOBJECT_TYPE_GPU_FENCE      0x0111U
#define KOBJECT_TYPE_GPU_CONTEXT    0x0112U

#define GPU_CONTEXT_RIGHT_SUBMIT    (1U << 0)
#define GPU_CONTEXT_RIGHT_QUERY     (1U << 1)
#define GPU_CONTEXT_RIGHT_ALL       (GPU_CONTEXT_RIGHT_SUBMIT | GPU_CONTEXT_RIGHT_QUERY)
#define GPU_ALLOCATION_RIGHT_READ   (1U << 0)
#define GPU_ALLOCATION_RIGHT_WRITE  (1U << 1)
#define GPU_ALLOCATION_RIGHT_ALL    (GPU_ALLOCATION_RIGHT_READ | GPU_ALLOCATION_RIGHT_WRITE)
#define GPU_FENCE_RIGHT_WAIT        (1U << 31)
#define GPU_FENCE_RIGHT_SIGNAL      (1U << 0)
#define GPU_SUBMIT_BATCH_MAX        32U
#define GPU_FENCE_RIGHT_ALL         (GPU_FENCE_RIGHT_WAIT | GPU_FENCE_RIGHT_SIGNAL)

enum gpu_context_state {
    GPU_CONTEXT_RUNNING = 0,
    GPU_CONTEXT_LOST = 1,
    GPU_CONTEXT_RESETTING = 2,
};

/*
 * Window 仅持有 gpu_allocation_t 引用。后端私有的 RM/KMS 句柄由此枚举和回调表封装；
 * 通用图形代码不会解释 backend_private。
 */
typedef enum gpu_allocation_backend {
    GPU_ALLOCATION_BACKEND_CPU_WB = 0,
    GPU_ALLOCATION_BACKEND_CPU_WC = 1,
    GPU_ALLOCATION_BACKEND_NVIDIA_GPU = 2,
} gpu_allocation_backend_t;

typedef struct gpu_allocation gpu_allocation_t;

typedef struct gpu_allocation_backend_ops {
    /* 释放后端所有权；回调返回后由 GPU 核心释放分配对象本身。 */
    void (*release)(gpu_allocation_t *allocation, void *backend_private);
    kstatus_t (*pin)(gpu_allocation_t *allocation, void *backend_private);
    void (*unpin)(gpu_allocation_t *allocation, void *backend_private);
    kstatus_t (*map_cpu)(gpu_allocation_t *allocation, void *backend_private,
                         uint64_t offset, uint64_t length, void **address);
    void (*unmap_cpu)(gpu_allocation_t *allocation, void *backend_private,
                      void *address, uint64_t length);
    kstatus_t (*get_physical)(gpu_allocation_t *allocation,
                              void *backend_private, uint64_t offset,
                              uint64_t length, paddr_t *physical);
} gpu_allocation_backend_ops_t;

struct gpu_allocation {
    object_header_t object;
    uint64_t size;
    uint32_t domain;
    uint32_t flags;
    gpu_va_t gpu_va;
    gpu_allocation_backend_t backend_kind;
    const gpu_allocation_backend_ops_t *backend_ops;
    void *backend_private;

    /* 仅用于 CPU_WB/CPU_WC 后备存储。NVIDIA 后端状态绝不保存在这些字段中，
     * 也不会暴露给用户态。 */
    void *backing;
    paddr_t backing_phys;
    uint64_t backing_size;
    void *backing_page;
    uint8_t backing_order;
    atomic_uint pin_count;
    atomic_uint residency_state;
};

typedef struct gpu_fence {
    object_header_t object;
    atomic_uint_fast64_t completed_value;
    atomic_int status;
    /* signal/fail 的唯一状态转换锁；等待路径只读取原子状态。 */
    spinlock_t lock;
    wait_queue_t waitq;
    void *backend;
} gpu_fence_t;

typedef struct gpu_context {
    object_header_t object;
    struct device *device;
    struct process *process;
    void *gpu_vm;
    void *backend;
    spinlock_t lock;
    list_head_t submissions;
    atomic_uint state;
} gpu_context_t;

/* 一批提交共享同一个上下文锁，后端可据此一次性敲响门铃。 */
typedef struct gpu_submission {
    gpu_allocation_t *allocation;
    uint64_t command_offset;
    uint64_t command_length;
    gpu_fence_t *fence;
    uint64_t signal_value;
} gpu_submission_t;

kstatus_t gpu_allocation_create(uint64_t size, gpu_allocation_t **out);
kstatus_t gpu_allocation_create_with_backend(
    uint64_t size, uint32_t domain, uint32_t flags,
    gpu_allocation_backend_t backend_kind, gpu_va_t gpu_va,
    const gpu_allocation_backend_ops_t *backend_ops,
    void *backend_private, gpu_allocation_t **out);
gpu_allocation_backend_t gpu_allocation_backend(
    const gpu_allocation_t *allocation);
kstatus_t gpu_allocation_pin(gpu_allocation_t *allocation);
kstatus_t gpu_allocation_unpin(gpu_allocation_t *allocation);
kstatus_t gpu_allocation_cpu_map(gpu_allocation_t *allocation,
                                 uint64_t offset, uint64_t length,
                                 void **address);
void gpu_allocation_cpu_unmap(gpu_allocation_t *allocation, void *address,
                              uint64_t length);
kstatus_t gpu_allocation_get_physical(gpu_allocation_t *allocation,
                                      uint64_t offset, uint64_t length,
                                      paddr_t *physical);
kstatus_t gpu_context_create(struct device *device, struct process *process,
                             gpu_context_t **out);
kstatus_t gpu_fence_create(gpu_fence_t **out);
kstatus_t gpu_submit(gpu_context_t *context, gpu_allocation_t *allocation,
                     uint64_t command_offset, uint64_t command_length,
                     gpu_fence_t *fence, uint64_t signal_value);
kstatus_t gpu_submit_batch(gpu_context_t *context, const gpu_submission_t *submissions,
                           uint32_t count, uint32_t *submitted);
kstatus_t gpu_fence_signal(gpu_fence_t *fence, uint64_t value);
kstatus_t gpu_fence_fail(gpu_fence_t *fence, kstatus_t status);
kstatus_t gpu_fence_wait(gpu_fence_t *fence, uint64_t value,
                         uint64_t timeout_ns);
kstatus_t gpu_context_mark_lost(gpu_context_t *context);
kstatus_t gpu_context_reset(gpu_context_t *context);
/* Failure-only runtime self-test diagnostics. */
uint32_t syscall_gpu_submit_debug_progress(uint32_t cpu_index);
uint32_t gpu_context_debug_progress(uint32_t cpu_index);
uint64_t gpu_context_debug_address(uint32_t cpu_index);
bool gpu_core_self_test(void);
