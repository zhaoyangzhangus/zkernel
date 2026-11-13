/*
 * 官方 NVIDIA 595.84 NVKMS 核心的 System OS 接口。
 *
 * 此文件是移植边界。它负责 System 的内存分配、同步、时间、工作线程和 KAPI 打开机制；
 * 不实现 RM 操作、显存分配、GPU 虚拟地址管理、GMMU 编程或 GSP RPC。这些功能仍由不可变的
 * 595.84 RM/NVKMS 对象集负责。
 */

#include <nvidia-modeset-os-interface.h>
#include <nvkms.h>
#include <nvkms-api.h>
#include <nvkms-kapi.h>
#include <nvUnixVersion.h>

#include <arch/x86_64/smp.h>
#include <stdarg.h>
#include <stdint.h>
#include <string.h>

#include <kernel/console.h>
#include <kernel/kmem.h>
#include <kernel/display.h>
#include <kernel/nvidia/nvkms.h>
#include <kernel/object.h>
#include <kernel/realtest.h>
#include <kernel/sched.h>
#include <kernel/timer.h>

#include <nv_port.h>
#include <nv_rm_driver.h>
#include <nv_rm_vendor_bridge.h>

#ifndef OS_REALTEST
#define OS_REALTEST 0
#endif

extern NvS32 os_vsnprintf(char *destination, NvU32 capacity,
                          const char *format, va_list arguments);

/* 内核映像导出了字节复制原语，但特意没有提供宿主环境的 libc 字符串 ABI。将这些小型
 * 辅助函数保留在 NVKMS 边界内，不要只为厂商移植层新增全局符号。 */
static size_t nvkms_local_strlen(const char *s)
{
    const char *p = s;

    if (s == NULL)
        return 0U;
    while (*p != '\0')
        ++p;
    return (size_t)(p - s);
}

static int nvkms_local_strcmp(const char *s1, const char *s2)
{
    if (s1 == NULL || s2 == NULL)
        return s1 == s2 ? 0 : (s1 == NULL ? -1 : 1);
    while (*s1 != '\0' && *s1 == *s2) {
        ++s1;
        ++s2;
    }
    return (int)(unsigned char)*s1 - (int)(unsigned char)*s2;
}

static char *nvkms_local_strncpy(char *dest, const char *src, size_t n)
{
    size_t i;

    if (dest == NULL || n == 0U)
        return dest;
    if (src == NULL) {
        for (i = 0U; i < n; ++i)
            dest[i] = '\0';
        return dest;
    }
    for (i = 0U; i < n && src[i] != '\0'; ++i)
        dest[i] = src[i];
    for (; i < n; ++i)
        dest[i] = '\0';
    return dest;
}

static nv_port_mutex_t g_nvkms_lock;
static bool g_nvkms_lock_initialized;
static nv_port_rwlock_t g_nvkms_pm_lock;
static bool g_nvkms_pm_lock_initialized;
static atomic_bool g_nvkms_loaded;
static void *g_nvkms_vendor_context;
static struct NvKmsKapiFunctionsTable g_nvkms_functions;

struct nvkms_ref_ptr {
    nv_port_refcount_t refs;
    _Atomic(void *) ptr;
};

struct nvkms_timer_t {
    timer_object_t *timer;
    nv_port_thread_t *thread;
    atomic_bool canceled;
    atomic_bool complete;
    atomic_bool free_requested;
    atomic_bool reap_queued;
    bool is_ref_ptr;
    nvkms_timer_proc_t *proc;
    void *data_ptr;
    NvU32 data_u32;
    struct nvkms_timer_t *reap_next;
};

struct nvkms_per_open {
    void *data;
    struct NvKmsKapiDevice *device;
    atomic_bool closed;
    atomic_bool events_available;
    nv_port_work_t event_work;
};

struct nvkms_sema_t {
    nv_port_semaphore_t semaphore;
};

static nv_port_mutex_t g_timer_reap_lock;
static bool g_timer_reap_initialized;
static nv_port_semaphore_t g_timer_reap_semaphore;
static nv_port_thread_t *g_timer_reap_thread;
static struct nvkms_timer_t *g_timer_reap_head;

static void nvkms_log_status(const char *tag, kstatus_t status)
{
    kernel_serial_printf_serial_only("OS_NVKMS_%s status=%d\r\n",
                                    tag, (int)status);
}

typedef struct nvkms_bootstrap_context {
    atomic_bool done;
    atomic_int status;
} nvkms_bootstrap_context_t;

/*
 * nvKmsModuleLoad() 和初始 KAPI 模式设置会使用可休眠互斥锁。运行时入口仍在 BSP 的逻辑
 * 空闲引导续执行路径上，因此应使用现有的真实 Ring0 线程原语执行完整操作，而不是放宽
 * 互斥锁约定。
 */
static void nvkms_bootstrap_worker(void *argument)
{
    nvkms_bootstrap_context_t *context =
        (nvkms_bootstrap_context_t *)argument;
    kstatus_t status;

    if (context == NULL)
        return;

    kernel_serial_write_serial_only(
        "OS_NVKMS_BOOTSTRAP_WORKER_ENTRY\r\n");
    kernel_serial_write_serial_only("OS_NVKMS_MODULE_BEGIN\r\n");
    status = nv_nvkms_init();
    nvkms_log_status("MODULE_DONE", status);
    if (status == K_OK) {
        kernel_serial_write_serial_only("OS_NVKMS_DISPLAY_BEGIN\r\n");
        status = nv_nvkms_display_init();
        if (status == K_OK) {
            kernel_serial_write_serial_only(
                "OS_NVKMS_NATIVE_SCANOUT_READY\r\n");
            if (!display_core_native_self_test()) {
                realtest_checkpoint(
                    "OS_NVKMS_NATIVE_SCANOUT_FAIL");
                status = K_EIO;
            } else {
                realtest_checkpoint(
                    "OS_NVKMS_NATIVE_SCANOUT_FLIP_OK");
            }
        }
        nvkms_log_status("DISPLAY_DONE", status);
    }
    atomic_store_explicit(&context->status, (int)status,
                          memory_order_release);
    atomic_store_explicit(&context->done, true, memory_order_release);
    kernel_serial_write_serial_only("OS_NVKMS_BOOTSTRAP_WORKER_DONE\r\n");
}

static void nvkms_bootstrap_poll(void)
{
    __asm__ volatile ("sti; hlt" : : : "memory");
    (void)x86_smp_take_reschedule_request();
    if (sched_runnable_count() != 0U)
        (void)sched_try_run_ready();
    /* 仅串口标记会被真实硬件测试缓冲区捕获。在引导续执行路径中刷新日志，即使厂商工作线程
     * 阻塞或自旋，FAT 日志中也会留下最后一个已完成的边界。 */
    if (realtest_enabled())
        (void)realtest_flush();
}

static bool nvkms_lock(uint64_t timeout_ns)
{
    /* 如果引导操作卡住且厂商核心持有串行化互斥锁，真实硬件探测不能让宿主机无限等待。
     * 生产环境和非真实硬件测试的行为保持不变。 */
#if OS_REALTEST
    if (timeout_ns == UINT64_MAX)
        timeout_ns = 5000000000ULL;
#endif
    return g_nvkms_lock_initialized &&
           nv_port_mutex_lock(&g_nvkms_lock, timeout_ns) == K_OK;
}

static void nvkms_unlock(void)
{
    if (g_nvkms_lock_initialized)
        (void)nv_port_mutex_unlock(&g_nvkms_lock);
}

/*
 * 官方 Linux 移植层在此处使用两把不同的锁：
 *   - nvkms_pm_lock 用于在挂起/恢复期间保护客户端；
 *   - nvkms_lock 用于串行化进入 NVKMS 核心的操作。
 *
 * KAPI 的 try_pmlock 辅助函数只能尝试获取前一把锁。若将核心互斥锁当作电源管理锁，
 * 只要另一项 NVKMS 操作持有核心互斥锁，就会拒绝有效的模式设置；真实硬件跟踪正好暴露了
 * 这个问题。
 */
static bool nvkms_pm_read_lock(void)
{
    if (!g_nvkms_pm_lock_initialized)
        return false;
    nv_port_rwlock_read_lock(&g_nvkms_pm_lock);
    return true;
}

static bool nvkms_pm_read_try_lock(void)
{
    return g_nvkms_pm_lock_initialized &&
           nv_port_rwlock_read_try_lock(&g_nvkms_pm_lock);
}

static void nvkms_pm_read_unlock(void)
{
    if (g_nvkms_pm_lock_initialized)
        nv_port_rwlock_read_unlock(&g_nvkms_pm_lock);
}

static const nv_gpu_device_t *nvkms_current_gpu(void)
{
    const nv_gpu_device_t *gpu = nv_rm_driver_current();
    if (gpu == NULL || gpu->state != NV_GPU_GSP_READY ||
        gpu->vendor_context == NULL)
        return NULL;
    return gpu;
}

static void *nvkms_alloc_raw(size_t size, NvBool zero)
{
    void *memory;

    if (size == 0U)
        return NULL;

    if (size <= PAGE_SIZE)
        memory = zero != NV_FALSE ? kzalloc(size, 0U) : kmalloc(size, 0U);
    else {
        memory = vmalloc(size);
        if (memory != NULL && zero != NV_FALSE)
            (void)memset(memory, 0, size);
    }
    return memory;
}

void *nvkms_alloc(size_t size, NvBool zero)
{
    return nvkms_alloc_raw(size, zero);
}

void nvkms_free(void *ptr, size_t size)
{
    if (ptr == NULL)
        return;
    if (size <= PAGE_SIZE)
        kfree(ptr);
    else
        vfree(ptr);
}

void *nvkms_memset(void *ptr, NvU8 c, size_t size)
{
    return memset(ptr, (int)c, size);
}

void *nvkms_memcpy(void *dest, const void *src, size_t n)
{
    return memcpy(dest, src, n);
}

void *nvkms_memmove(void *dest, const void *src, size_t n)
{
    return memmove(dest, src, n);
}

int nvkms_memcmp(const void *s1, const void *s2, size_t n)
{
    return memcmp(s1, s2, n);
}

size_t nvkms_strlen(const char *s)
{
    return nvkms_local_strlen(s);
}

int nvkms_strcmp(const char *s1, const char *s2)
{
    return nvkms_local_strcmp(s1, s2);
}

char *nvkms_strncpy(char *dest, const char *src, size_t n)
{
    return nvkms_local_strncpy(dest, src, n);
}

void nvkms_usleep(NvU64 usec)
{
    uint64_t nanoseconds;

    if (usec == 0U)
        return;
    if (usec <= UINT64_MAX / 1000ULL) {
        nanoseconds = usec * 1000ULL;
        if (usec < 1000ULL) {
            nv_port_udelay(usec);
            return;
        }
        (void)nv_port_sleep_ns(nanoseconds);
        return;
    }
    (void)nv_port_sleep_ns(UINT64_MAX);
}

NvU64 nvkms_get_usec(void)
{
    return nv_port_time_now_ns() / 1000ULL;
}

int nvkms_copyin(void *kptr, NvU64 uaddr, size_t n)
{
    return nv_port_copy_from_user(kptr,
                                  (const void __user *)(uintptr_t)uaddr,
                                  n) == K_OK ? 0 : -1;
}

int nvkms_copyout(NvU64 uaddr, const void *kptr, size_t n)
{
    return nv_port_copy_to_user((void __user *)(uintptr_t)uaddr, kptr, n) ==
                   K_OK ?
               0 :
               -1;
}

void nvkms_yield(void)
{
    schedule();
}

void nvkms_dump_stack(void)
{
    /* System 没有兼容 Linux 的栈解码器；保留仅供串口读取的诊断标记，不要声称已转储栈信息。 */
    kernel_serial_write_serial_only("OS_NVKMS_STACK_DUMP_UNAVAILABLE\r\n");
}

NvBool nvkms_syncpt_op(enum NvKmsSyncPtOp op, NvKmsSyncPtOpParams *params)
{
    (void)op;
    (void)params;
    return NV_FALSE;
}

int nvkms_vsnprintf(char *str, size_t size, const char *format, va_list ap)
{
    if (str == NULL && size != 0U)
        return -1;
    return (int)os_vsnprintf(str, size > UINT32_MAX ? UINT32_MAX :
                                      (NvU32)size,
                             format, ap);
}

int nvkms_snprintf(char *str, size_t size, const char *format, ...)
{
    va_list ap;
    int result;

    va_start(ap, format);
    result = nvkms_vsnprintf(str, size, format, ap);
    va_end(ap);
    return result;
}

void nvkms_log(const int level, const char *gpuPrefix, const char *msg)
{
    const char *prefix = gpuPrefix == NULL ? "" : gpuPrefix;
    const char *text = msg == NULL ? "" : msg;
    kernel_serial_printf_serial_only("NVKMS[%d] %s%s\r\n", level, prefix,
                                    text);
}

struct nvkms_ref_ptr *nvkms_alloc_ref_ptr(void *ptr)
{
    struct nvkms_ref_ptr *ref_ptr =
        (struct nvkms_ref_ptr *)nvkms_alloc(sizeof(*ref_ptr), NV_TRUE);
    if (ref_ptr == NULL)
        return NULL;

    nv_port_ref_init(&ref_ptr->refs, 1U);
    atomic_init(&ref_ptr->ptr, ptr);
    return ref_ptr;
}

void nvkms_free_ref_ptr(struct nvkms_ref_ptr *ref_ptr)
{
    if (ref_ptr == NULL)
        return;

    (void)atomic_exchange_explicit(&ref_ptr->ptr, NULL, memory_order_acq_rel);
    if (nv_port_ref_put(&ref_ptr->refs))
        nvkms_free(ref_ptr, sizeof(*ref_ptr));
}

void nvkms_inc_ref(struct nvkms_ref_ptr *ref_ptr)
{
    if (ref_ptr != NULL && !nv_port_ref_get(&ref_ptr->refs))
        kernel_serial_write_serial_only("OS_NVKMS_REF_GET_FAIL\r\n");
}

void *nvkms_dec_ref(struct nvkms_ref_ptr *ref_ptr)
{
    void *ptr;

    if (ref_ptr == NULL)
        return NULL;
    ptr = atomic_load_explicit(&ref_ptr->ptr, memory_order_acquire);
    if (nv_port_ref_put(&ref_ptr->refs))
        nvkms_free(ref_ptr, sizeof(*ref_ptr));
    return ptr;
}

static void nvkms_timer_reap_push(struct nvkms_timer_t *timer)
{
    if (timer == NULL)
        return;
    (void)nv_port_mutex_lock(&g_timer_reap_lock, UINT64_MAX);
    timer->reap_next = g_timer_reap_head;
    g_timer_reap_head = timer;
    (void)nv_port_mutex_unlock(&g_timer_reap_lock);
}

static void nvkms_timer_reap_thread(void *argument)
{
    (void)argument;

    for (;;) {
        struct nvkms_timer_t *timer;
        nv_port_thread_t *timer_thread;
        kstatus_t status;

        status = nv_port_semaphore_down(&g_timer_reap_semaphore, UINT64_MAX);
        if (status != K_OK) {
            if (g_timer_reap_thread != NULL &&
                nv_port_thread_should_stop(g_timer_reap_thread))
                return;
            continue;
        }

        for (;;) {
            (void)nv_port_mutex_lock(&g_timer_reap_lock, UINT64_MAX);
            timer = g_timer_reap_head;
            if (timer != NULL)
                g_timer_reap_head = timer->reap_next;
            (void)nv_port_mutex_unlock(&g_timer_reap_lock);
            if (timer == NULL)
                break;

            timer_thread = timer->thread;
            if (timer_thread != NULL) {
                /* 这是专用的回收线程，因此在此等待不会阻塞共享的合成器/延迟工作线程。 */
                status = nv_port_thread_join(timer_thread, UINT64_MAX);
                if (status != K_OK) {
                    nvkms_timer_reap_push(timer);
                    nv_port_semaphore_up(&g_timer_reap_semaphore);
                    break;
                }
                do {
                    status = nv_port_thread_destroy(timer_thread);
                    if (status == K_EBUSY)
                        schedule();
                } while (status == K_EBUSY);
                if (status != K_OK) {
                    nvkms_timer_reap_push(timer);
                    nv_port_semaphore_up(&g_timer_reap_semaphore);
                    break;
                }
                timer->thread = NULL;
            }

            if (timer->timer != NULL) {
                object_put(timer->timer);
                timer->timer = NULL;
            }
            nvkms_free(timer, sizeof(*timer));
        }

        if (g_timer_reap_thread != NULL &&
            nv_port_thread_should_stop(g_timer_reap_thread))
            return;
    }
}

static void nvkms_queue_timer_reap(struct nvkms_timer_t *timer)
{
    bool expected;

    if (timer == NULL || !g_timer_reap_initialized)
        return;

    expected = false;
    if (!atomic_compare_exchange_strong_explicit(
            &timer->reap_queued, &expected, true,
            memory_order_acq_rel, memory_order_acquire))
        return;

    nvkms_timer_reap_push(timer);
    nv_port_semaphore_up(&g_timer_reap_semaphore);
}

static void nvkms_timer_thread(void *argument)
{
    struct nvkms_timer_t *timer = (struct nvkms_timer_t *)argument;
    bool canceled;

    if (timer == NULL)
        return;

    if (timer->timer != NULL) {
        void *objects[1] = {timer->timer};
        uint32_t signaled_index = UINT32_MAX;
        int64_t wait_value = 0;
        (void)object_wait_many(objects, 1U, false, UINT64_MAX,
                               &signaled_index, &wait_value);
    }

    canceled = atomic_load_explicit(&timer->canceled, memory_order_acquire);
    if (!canceled && timer->thread != NULL)
        canceled = nv_port_thread_should_stop(timer->thread);

    if (!canceled && timer->proc != NULL) {
        if (nvkms_lock(UINT64_MAX)) {
            canceled = atomic_load_explicit(&timer->canceled,
                                            memory_order_acquire);
            if (!canceled) {
                void *data = timer->data_ptr;
                if (timer->is_ref_ptr) {
                    data = nvkms_dec_ref(
                        (struct nvkms_ref_ptr *)timer->data_ptr);
                }
                if (data != NULL || !timer->is_ref_ptr)
                    timer->proc(data, timer->data_u32);
                atomic_store_explicit(&timer->complete, true,
                                      memory_order_release);
            }
            nvkms_unlock();
        }
    } else if (timer->is_ref_ptr) {
        /* 已取消的引用定时器仍持有额外引用。 */
        if (nvkms_lock(UINT64_MAX)) {
            (void)nvkms_dec_ref((struct nvkms_ref_ptr *)timer->data_ptr);
            nvkms_unlock();
        }
    }

    if (timer->is_ref_ptr ||
        atomic_load_explicit(&timer->free_requested, memory_order_acquire))
        nvkms_queue_timer_reap(timer);
}

static kstatus_t nvkms_timer_create(struct nvkms_timer_t *timer, NvU64 usec)
{
    uint64_t nanoseconds;

    if (timer == NULL)
        return K_EINVAL;
    if (usec == 0U) {
        timer->timer = NULL;
        return K_OK;
    }
    if (usec > UINT64_MAX / 1000ULL)
        return K_EOVERFLOW;
    nanoseconds = usec * 1000ULL;
    return timer_create(nanoseconds, 0U, &timer->timer);
}

static nvkms_timer_handle_t *nvkms_alloc_timer_internal(
    nvkms_timer_proc_t *proc, void *data_ptr, NvU32 data_u32, NvU64 usec,
    bool is_ref_ptr)
{
    struct nvkms_timer_t *timer;
    kstatus_t status;

    if (proc == NULL || (is_ref_ptr && data_ptr == NULL))
        return NULL;
    timer = (struct nvkms_timer_t *)nvkms_alloc(sizeof(*timer), NV_TRUE);
    if (timer == NULL)
        return NULL;
    atomic_init(&timer->canceled, false);
    atomic_init(&timer->complete, false);
    atomic_init(&timer->free_requested, is_ref_ptr);
    atomic_init(&timer->reap_queued, false);
    timer->is_ref_ptr = is_ref_ptr;
    timer->proc = proc;
    timer->data_ptr = data_ptr;
    timer->data_u32 = data_u32;

    status = nvkms_timer_create(timer, usec);
    if (status != K_OK) {
        nvkms_free(timer, sizeof(*timer));
        return NULL;
    }
    if (nv_port_thread_create(nvkms_timer_thread, timer, &timer->thread) !=
        K_OK) {
        if (timer->timer != NULL)
            object_put(timer->timer);
        nvkms_free(timer, sizeof(*timer));
        return NULL;
    }
    if (nv_port_thread_start(timer->thread) != K_OK) {
        (void)nv_port_thread_destroy(timer->thread);
        if (timer->timer != NULL)
            object_put(timer->timer);
        nvkms_free(timer, sizeof(*timer));
        return NULL;
    }
    return timer;
}

nvkms_timer_handle_t *nvkms_alloc_timer(nvkms_timer_proc_t *proc,
                                        void *dataPtr, NvU32 dataU32,
                                        NvU64 usec)
{
    return nvkms_alloc_timer_internal(proc, dataPtr, dataU32, usec, false);
}

NvBool nvkms_alloc_timer_with_ref_ptr(nvkms_timer_proc_t *proc,
                                      struct nvkms_ref_ptr *ref_ptr,
                                      NvU32 dataU32, NvU64 usec)
{
    struct nvkms_timer_t *timer;

    if (proc == NULL || ref_ptr == NULL || !g_timer_reap_initialized)
        return NV_FALSE;
    nvkms_inc_ref(ref_ptr);
    timer = nvkms_alloc_timer_internal(proc, ref_ptr, dataU32, usec, true);
    if (timer == NULL) {
        if (nvkms_lock(UINT64_MAX)) {
            (void)nvkms_dec_ref(ref_ptr);
            nvkms_unlock();
        }
        return NV_FALSE;
    }
    return NV_TRUE;
}

void nvkms_free_timer(nvkms_timer_handle_t *handle)
{
    struct nvkms_timer_t *timer = (struct nvkms_timer_t *)handle;
    nv_port_thread_t *thread;

    if (timer == NULL)
        return;
    if (atomic_exchange_explicit(&timer->free_requested, true,
                                 memory_order_acq_rel))
        return;
    atomic_store_explicit(&timer->canceled, true, memory_order_release);
    if (timer->timer != NULL)
        (void)timer_cancel(timer->timer);
    thread = timer->thread;
    if (thread == NULL)
        return;
    (void)nv_port_thread_stop(thread);
    /* 官方实现要求此处不得阻塞。尤其是 proc() 可能在持有全局 NVKMS 锁时，对自己的句柄调用
     * nvkms_free_timer()。应让定时器线程在 proc() 返回且包装函数发布完成状态后，再将回收任务
     * 加入队列。 */
    if (atomic_load_explicit(&thread->finished, memory_order_acquire))
        nvkms_queue_timer_reap(timer);
}

void nvkms_event_queue_changed(nvkms_per_open_handle_t *pOpenKernel,
                               NvBool eventsAvailable)
{
    struct nvkms_per_open *popen = (struct nvkms_per_open *)pOpenKernel;

    if (popen == NULL || !eventsAvailable ||
        atomic_load_explicit(&popen->closed, memory_order_acquire))
        return;
    atomic_store_explicit(&popen->events_available, true,
                          memory_order_release);
    kstatus_t status = nv_port_work_queue_irq(&popen->event_work);
    if (status != K_OK && status != K_EBUSY &&
        atomic_load_explicit(&popen->events_available, memory_order_acquire))
        kernel_serial_write_serial_only("OS_NVKMS_EVENT_QUEUE_FAIL\r\n");
}

static void nvkms_event_work(void *argument)
{
    struct nvkms_per_open *popen = (struct nvkms_per_open *)argument;

    if (popen == NULL ||
        atomic_load_explicit(&popen->closed, memory_order_acquire))
        return;
    if (!atomic_exchange_explicit(&popen->events_available, false,
                                  memory_order_acq_rel))
        return;
    if (popen->device != NULL && popen->data != NULL)
        nvKmsKapiHandleEventQueueChange(popen->device);
}

void *nvkms_get_per_open_data(int fd)
{
    (void)fd;
    return NULL;
}

NvBool nvkms_open_gpu(NvU32 gpuId, NvBool reset_aware)
{
    const nv_gpu_device_t *gpu = nvkms_current_gpu();
    return gpu != NULL &&
           nv_rm_vendor_kms_open_gpu(gpu->vendor_context, gpuId,
                                      reset_aware != NV_FALSE) == K_OK;
}

void nvkms_close_gpu(NvU32 gpuId, NvBool reset_aware)
{
    const nv_gpu_device_t *gpu = nvkms_current_gpu();
    if (gpu != NULL)
        nv_rm_vendor_kms_close_gpu(gpu->vendor_context, gpuId,
                                    reset_aware != NV_FALSE);
}

NvU32 nvkms_enumerate_gpus(nv_gpu_info_t *gpu_info)
{
    const nv_gpu_device_t *gpu = nvkms_current_gpu();
    return gpu == NULL ? 0U :
           nv_rm_vendor_kms_enumerate_gpus(gpu->vendor_context, gpu_info);
}

NvBool nvkms_allow_write_combining(void)
{
    const nv_gpu_device_t *gpu = nvkms_current_gpu();
    return gpu != NULL &&
           nv_rm_vendor_kms_allow_write_combining(gpu->vendor_context) ?
               NV_TRUE :
               NV_FALSE;
}

NvBool nvkms_kernel_supports_syncpts(void)
{
    return NV_FALSE;
}

NvBool nvkms_fd_is_nvidia_chardev(int fd)
{
    (void)fd;
    return NV_FALSE;
}

struct nvkms_per_open *nvkms_open_from_kapi(struct NvKmsKapiDevice *device)
{
    struct nvkms_per_open *popen;

    if (device == NULL || !g_nvkms_lock_initialized)
        return NULL;
    popen = (struct nvkms_per_open *)nvkms_alloc(sizeof(*popen), NV_TRUE);
    if (popen == NULL)
        return NULL;
    popen->device = device;
    atomic_init(&popen->closed, false);
    atomic_init(&popen->events_available, false);
    nv_port_work_init(&popen->event_work, nvkms_event_work, popen);

    if (!nvkms_pm_read_lock() || !nvkms_lock(UINT64_MAX)) {
        if (g_nvkms_pm_lock_initialized)
            nvkms_pm_read_unlock();
        nvkms_free(popen, sizeof(*popen));
        return NULL;
    }
    popen->data = nvKmsOpen(0U, NVKMS_CLIENT_KERNEL_SPACE,
                            (nvkms_per_open_handle_t *)popen);
    nvkms_unlock();
    nvkms_pm_read_unlock();
    if (popen->data == NULL) {
        nvkms_free(popen, sizeof(*popen));
        return NULL;
    }
    return popen;
}

void nvkms_close_from_kapi(struct nvkms_per_open *popen)
{
    if (popen == NULL)
        return;
    atomic_store_explicit(&popen->closed, true, memory_order_release);
    if (!nvkms_pm_read_lock() || !nvkms_lock(UINT64_MAX)) {
        /* nvKmsClose() 仍可能处于等待状态时，不要释放仍在使用的厂商逐打开对象。全局锁在公开
         * KAPI 前已完成初始化，因此此路径会安全失败并记录诊断信息。 */
        kernel_serial_write_serial_only("OS_NVKMS_CLOSE_LOCK_FAIL\r\n");
        if (g_nvkms_pm_lock_initialized)
            nvkms_pm_read_unlock();
        return;
    }
    if (popen->data != NULL) {
        nvKmsClose(popen->data);
        popen->data = NULL;
    }
    nvkms_unlock();
    nvkms_pm_read_unlock();
    (void)nv_port_work_cancel(&popen->event_work);
    (void)nv_port_work_drain(&popen->event_work, UINT64_MAX);
    nvkms_free(popen, sizeof(*popen));
}

NvBool nvkms_ioctl_from_kapi(struct nvkms_per_open *popen, NvU32 cmd,
                             void *params_address, const size_t params_size)
{
    NvBool status = NV_FALSE;

    if (popen == NULL || params_address == NULL ||
        atomic_load_explicit(&popen->closed, memory_order_acquire) ||
        popen->data == NULL || !nvkms_pm_read_lock())
        return NV_FALSE;
    if (!nvkms_lock(UINT64_MAX)) {
        nvkms_pm_read_unlock();
        return NV_FALSE;
    }
    status = nvKmsIoctl(popen->data, cmd,
                        (NvU64)(uintptr_t)params_address, params_size);
    nvkms_unlock();
    nvkms_pm_read_unlock();
    return status;
}

NvBool nvkms_ioctl_from_kapi_try_pmlock(struct nvkms_per_open *popen,
                                        NvU32 cmd, void *params_address,
                                        const size_t params_size)
{
    NvBool status;
    kstatus_t lock_status;

    if (popen == NULL || params_address == NULL ||
        atomic_load_explicit(&popen->closed, memory_order_acquire) ||
        popen->data == NULL) {
        if (cmd == NVKMS_IOCTL_SET_MODE) {
            kernel_serial_write_serial_only(
                "OS_NVKMS_SET_MODE_PLOCK_INPUT_FAIL\r\n");
            (void)realtest_flush();
        }
        return NV_FALSE;
    }
    if (!g_nvkms_pm_lock_initialized) {
        if (cmd == NVKMS_IOCTL_SET_MODE) {
            kernel_serial_write_serial_only(
                "OS_NVKMS_SET_MODE_PMLOCK_UNINITIALIZED\r\n");
            (void)realtest_flush();
        }
        return NV_FALSE;
    }
    if (!nvkms_pm_read_try_lock()) {
        if (cmd == NVKMS_IOCTL_SET_MODE) {
            kernel_serial_write_serial_only(
                "OS_NVKMS_SET_MODE_PMLOCK_BUSY\r\n");
            (void)realtest_flush();
        }
        return NV_FALSE;
    }
    if (cmd == NVKMS_IOCTL_SET_MODE) {
        kernel_serial_write_serial_only(
            "OS_NVKMS_SET_MODE_CORE_LOCK_BEGIN\r\n");
        (void)realtest_flush();
    }
    lock_status = nv_port_mutex_lock(&g_nvkms_lock,
#if OS_REALTEST
                                     5000000000ULL
#else
                                     UINT64_MAX
#endif
    );
    if (lock_status != K_OK) {
        if (cmd == NVKMS_IOCTL_SET_MODE) {
            kernel_serial_printf_serial_only(
                "OS_NVKMS_SET_MODE_CORE_LOCK_FAIL status=%d\r\n",
                (int)lock_status);
            (void)realtest_flush();
        }
        nvkms_pm_read_unlock();
        return NV_FALSE;
    }
    if (cmd == NVKMS_IOCTL_SET_MODE) {
        kernel_serial_write_serial_only(
            "OS_NVKMS_SET_MODE_CORE_LOCK_OK\r\n");
        kernel_serial_write_serial_only(
            "OS_NVKMS_SET_MODE_IOCTL_BEGIN\r\n");
    }
    status = nvKmsIoctl(popen->data, cmd,
                        (NvU64)(uintptr_t)params_address, params_size);
    nvkms_unlock();
    nvkms_pm_read_unlock();
    if (cmd == NVKMS_IOCTL_SET_MODE) {
        kernel_serial_printf_serial_only(
            "OS_NVKMS_SET_MODE_IOCTL_DONE status=%u\r\n",
            status ? 1U : 0U);
        (void)realtest_flush();
    }
    return status;
}

nvkms_sema_handle_t *nvkms_sema_alloc(void)
{
    struct nvkms_sema_t *sema =
        (struct nvkms_sema_t *)nvkms_alloc(sizeof(*sema), NV_TRUE);
    if (sema != NULL)
        nv_port_semaphore_init(&sema->semaphore, 1U);
    return sema;
}

void nvkms_sema_free(nvkms_sema_handle_t *sema)
{
    if (sema != NULL)
        nvkms_free(sema, sizeof(struct nvkms_sema_t));
}

void nvkms_sema_down(nvkms_sema_handle_t *sema)
{
    if (sema != NULL)
        (void)nv_port_semaphore_down(&sema->semaphore, UINT64_MAX);
}

void nvkms_sema_up(nvkms_sema_handle_t *sema)
{
    if (sema != NULL)
        nv_port_semaphore_up(&sema->semaphore);
}

NvBool nvkms_test_fail_alloc_core_channel(enum FailAllocCoreChannelMethod method)
{
    (void)method;
    return NV_FALSE;
}

NvBool nvkms_conceal_vrr_caps(void)
{
    return NV_FALSE;
}

NvBool nvkms_output_rounding_fix(void)
{
    return NV_FALSE;
}

NvBool nvkms_disable_hdmi_frl(void)
{
    return NV_FALSE;
}

NvBool nvkms_disable_vrr_memclk_switch(void)
{
    return NV_FALSE;
}

NvBool nvkms_hdmi_deepcolor(void)
{
    return NV_FALSE;
}

NvBool nvkms_opportunistic_display_sync(void)
{
    return NV_FALSE;
}

enum NvKmsDebugForceColorSpace nvkms_debug_force_color_space(void)
{
    return NVKMS_DEBUG_FORCE_COLOR_SPACE_NONE;
}

NvBool nvkms_enable_overlay_layers(void)
{
    return NV_FALSE;
}

NvBool nvkms_debug_logging(void)
{
    return NV_FALSE;
}

struct nvkms_backlight_device *nvkms_register_backlight(
    NvU32 gpu_id, NvU32 display_id, void *drv_priv, NvU32 current_brightness)
{
    (void)gpu_id;
    (void)display_id;
    (void)drv_priv;
    (void)current_brightness;
    return NULL;
}

void nvkms_unregister_backlight(struct nvkms_backlight_device *nvkms_bd)
{
    (void)nvkms_bd;
}

void nvkms_call_rm(void *ops)
{
    const nv_gpu_device_t *gpu = nvkms_current_gpu();
    kstatus_t status;

    if (gpu == NULL) {
        kernel_serial_write_serial_only("OS_NVKMS_RM_CALL_NO_GPU\r\n");
        return;
    }
    status = nv_rm_vendor_kms_call_rm(gpu->vendor_context, ops);
    if (status != K_OK)
        nvkms_log_status("RM_CALL_FAIL", status);
}

static bool nvkms_init_runtime_objects(void)
{
    if (!g_nvkms_lock_initialized) {
        nv_port_mutex_init(&g_nvkms_lock);
        g_nvkms_lock_initialized = true;
        atomic_init(&g_nvkms_loaded, false);
    }
    if (!g_nvkms_pm_lock_initialized) {
        nv_port_rwlock_init(&g_nvkms_pm_lock);
        g_nvkms_pm_lock_initialized = true;
    }
    if (!g_timer_reap_initialized) {
        nv_port_mutex_init(&g_timer_reap_lock);
        nv_port_semaphore_init(&g_timer_reap_semaphore, 0U);
        g_timer_reap_head = NULL;
        g_timer_reap_thread = NULL;
        g_timer_reap_initialized = true;
        if (nv_port_thread_create(nvkms_timer_reap_thread, NULL,
                                  &g_timer_reap_thread) != K_OK ||
            g_timer_reap_thread == NULL ||
            nv_port_thread_start(g_timer_reap_thread) != K_OK) {
            if (g_timer_reap_thread != NULL) {
                (void)nv_port_thread_destroy(g_timer_reap_thread);
                g_timer_reap_thread = NULL;
            }
            g_timer_reap_initialized = false;
            return false;
        }
    }
    return true;
}

kstatus_t nv_nvkms_init(void)
{
    const nv_gpu_device_t *gpu = nvkms_current_gpu();
    NvBool table_ok;

    if (!nvkms_init_runtime_objects())
        return K_ENOMEM;
    if (gpu == NULL)
        return K_EIO;
    if (atomic_load_explicit(&g_nvkms_loaded, memory_order_acquire))
        return K_OK;
    g_nvkms_vendor_context = gpu->vendor_context;

    if (!nvkms_lock(UINT64_MAX))
        return K_EBUSY;
    if (!nvKmsModuleLoad()) {
        nvkms_unlock();
        g_nvkms_vendor_context = NULL;
        nvkms_log_status("MODULE_LOAD_FAIL", K_EIO);
        return K_EIO;
    }

    memset(&g_nvkms_functions, 0, sizeof(g_nvkms_functions));
    g_nvkms_functions.versionString = NV_VERSION_STRING;
    table_ok = nvKmsKapiGetFunctionsTableInternal(&g_nvkms_functions);
    if (!table_ok) {
        nvKmsModuleUnload();
        nvkms_unlock();
        g_nvkms_vendor_context = NULL;
        nvkms_log_status("KAPI_TABLE_FAIL", K_EIO);
        return K_EIO;
    }
    atomic_store_explicit(&g_nvkms_loaded, true, memory_order_release);
    nvkms_unlock();
    kernel_serial_write_serial_only("OS_NVKMS_READY version=595.84\r\n");
    return K_OK;
}

kstatus_t nv_nvkms_bootstrap_init(void)
{
    nvkms_bootstrap_context_t context;
    nv_port_thread_t *thread = NULL;
    kstatus_t status;
    kstatus_t destroy_status;

    atomic_init(&context.done, false);
    atomic_init(&context.status, (int)K_EIO);
    kernel_serial_write_serial_only(
        "OS_NVKMS_BOOTSTRAP_THREAD_START\r\n");
    kernel_serial_write_serial_only(
        "OS_NVKMS_BOOTSTRAP_CREATE_BEGIN\r\n");

    status = nv_port_thread_create(nvkms_bootstrap_worker, &context, &thread);
    if (status != K_OK || thread == NULL) {
        if (status == K_OK)
            status = K_EIO;
        nvkms_log_status("BOOTSTRAP_THREAD_CREATE_FAIL", status);
        return status;
    }
    kernel_serial_write_serial_only(
        "OS_NVKMS_BOOTSTRAP_CREATE_OK\r\n");

    kernel_serial_write_serial_only(
        "OS_NVKMS_BOOTSTRAP_START_BEGIN\r\n");
    status = nv_port_thread_start(thread);
    if (status != K_OK) {
        nvkms_log_status("BOOTSTRAP_THREAD_START_FAIL", status);
        destroy_status = nv_port_thread_destroy(thread);
        if (destroy_status != K_OK)
            nvkms_log_status("BOOTSTRAP_THREAD_DESTROY_FAIL", destroy_status);
        return status;
    }
    kernel_serial_write_serial_only(
        "OS_NVKMS_BOOTSTRAP_START_OK\r\n");

    while (!atomic_load_explicit(&context.done, memory_order_acquire))
        nvkms_bootstrap_poll();

    status = (kstatus_t)atomic_load_explicit(&context.status,
                                             memory_order_acquire);
    do {
        destroy_status = nv_port_thread_destroy(thread);
        if (destroy_status == K_EBUSY)
            nvkms_bootstrap_poll();
    } while (destroy_status == K_EBUSY);
    if (destroy_status != K_OK) {
        nvkms_log_status("BOOTSTRAP_THREAD_DESTROY_FAIL", destroy_status);
        return destroy_status;
    }

    kernel_serial_write_serial_only(
        "OS_NVKMS_BOOTSTRAP_DESTROY_OK\r\n");
    nvkms_log_status("BOOTSTRAP_THREAD_DONE", status);
    return status;
}

static void nvkms_timer_reap_shutdown(void)
{
    nv_port_thread_t *reaper = g_timer_reap_thread;

    if (reaper == NULL) {
        g_timer_reap_initialized = false;
        return;
    }

    (void)nv_port_thread_stop(reaper);
    nv_port_semaphore_up(&g_timer_reap_semaphore);
    while (!atomic_load_explicit(&reaper->finished, memory_order_acquire))
        schedule();
    while (nv_port_thread_destroy(reaper) == K_EBUSY)
        schedule();
    g_timer_reap_thread = NULL;
    g_timer_reap_initialized = false;
}

void nv_nvkms_shutdown(void)
{
    if (!g_nvkms_lock_initialized ||
        !atomic_load_explicit(&g_nvkms_loaded, memory_order_acquire))
        return;
    if (!nvkms_lock(UINT64_MAX))
        return;
    nvKmsModuleUnload();
    atomic_store_explicit(&g_nvkms_loaded, false, memory_order_release);
    nvkms_unlock();
    nvkms_timer_reap_shutdown();
    g_nvkms_vendor_context = NULL;
    kernel_serial_write_serial_only("OS_NVKMS_STOPPED\r\n");
}

bool nv_nvkms_ready(void)
{
    return g_nvkms_lock_initialized &&
           atomic_load_explicit(&g_nvkms_loaded, memory_order_acquire);
}

const void *nv_nvkms_functions(void)
{
    return nv_nvkms_ready() ? (const void *)&g_nvkms_functions : NULL;
}
