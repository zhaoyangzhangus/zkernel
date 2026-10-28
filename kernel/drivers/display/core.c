#include <kernel/display.h>
#include <arch/x86_64/cpu.h>
#include <kernel/console.h>
#include <kernel/deferred.h>
#include <kernel/kmem.h>
#include <kernel/mm.h>
#include <kernel/realtest.h>
#include <kernel/sched.h>
#include <kernel/telemetry.h>

#define DISPLAY_COPY_ROW_BUDGET 32U
#define DISPLAY_NATIVE_SELF_TEST_TIMEOUT_NS 5000000000ULL

typedef struct display_state {
    spinlock_t lock;
    uint64_t framebuffer;
    uint64_t framebuffer_size;
    uint32_t width;
    uint32_t height;
    uint32_t pixels_per_scanline;
    uint32_t format;
    uint32_t masks[4];

    /*
    * 可选的原生扫描输出后端。
     *
    * 显示引擎仍可能从该分配读取数据期间，current_scanout 恰好拥有一个对象引用和一个固定引用。
     */
    display_scanout_backend_t scanout_backend;
    gpu_allocation_t *current_scanout;
    bool scanout_backend_registered;

    /* 同一输出只允许一个未完成的提交，GOP copy 与 native flip 共用此门。 */
    bool initialized;
    bool pending_commit;
} display_state_t;

typedef struct display_pending_commit {
    gpu_allocation_t *buffer;
    uint64_t offset;
    uint64_t stride;
    uint32_t width;
    uint32_t height;
    uint32_t next_row;
    gpu_fence_t *wait_fence;
    uint64_t wait_value;
    gpu_fence_t *signal_fence;
    uint64_t signal_value;
    uint64_t start_tsc;
} display_pending_commit_t;

static display_state_t g_display;
static atomic_uint g_display_init_state;


static bool display_initialize(void) {
    if (atomic_load_explicit(&g_display_init_state, memory_order_acquire) == 2U) return true;
    unsigned expected = 0U;
    if (atomic_compare_exchange_strong_explicit(&g_display_init_state, &expected, 1U,
                                                memory_order_acq_rel,
                                                memory_order_acquire)) {
        spinlock_init(&g_display.lock);
        atomic_store_explicit(&g_display_init_state, 2U, memory_order_release);
        return true;
    }
    while (atomic_load_explicit(&g_display_init_state, memory_order_acquire) != 2U) {
        __asm__ volatile ("pause");
    }
    return true;
}

static void display_lock(void) {
    /* 此锁保护耗时的帧缓冲区复制。单 vCPU 环境下，如果持有者被抢占，用户系统调用可能在此永久自旋，
     * 使持有者无法恢复运行。将其视为调度器锁。 */
    sched_preempt_disable();
    spinlock_lock(&g_display.lock);
}

static void display_unlock(void) {
    spinlock_unlock(&g_display.lock);
    sched_preempt_enable();
}

static void display_clear_pending(void) {
    display_lock();
    g_display.pending_commit = false;
    display_unlock();
}

static uint32_t display_component(uint8_t value, uint32_t mask) {
    uint32_t shift = 0U;
    uint32_t width = 0U;
    uint32_t maximum;
    if (mask == 0U) return 0U;
    while (shift < 32U && ((mask >> shift) & 1U) == 0U) ++shift;
    while (shift + width < 32U && ((mask >> (shift + width)) & 1U) != 0U) ++width;
    if (width == 0U || width >= 32U) return mask;
    maximum = (1U << width) - 1U;
    return ((((uint32_t)value * maximum + 127U) / 255U) << shift) & mask;
}

static uint32_t display_convert_pixel(uint32_t pixel, uint32_t format,
                                      const uint32_t masks[4]) {
    uint8_t red = (uint8_t)(pixel >> 16);
    uint8_t green = (uint8_t)(pixel >> 8);
    uint8_t blue = (uint8_t)pixel;
    if (format == 1U) return ((uint32_t)blue) | ((uint32_t)green << 8) |
                                   ((uint32_t)red << 16);
    if (format == 2U) return display_component(red, masks[0]) |
                              display_component(green, masks[1]) |
                              display_component(blue, masks[2]);
    return pixel & 0x00FFFFFFU;
}


/*
 * 所有内核显示生产者共用的 GOP WB -> WC 发布原语。
 *
 * 合成器过去曾维护此代码的私有副本。将实际设备内存写入逻辑保留在显示核心有两个优点：
 *
 *   1. window_server 和 DISPLAY_COMMIT 使用完全相同的优化路径；
 *   2. 将来替换原生扫描输出/翻页后端时，只需修改显示层一个位置，无需改动嵌入合成器中的架构专用存储操作。
 *
 * GOP 帧缓冲区格式为 1 时，XRGB8888 源像素无需转换。其他 GOP 布局仍沿用现有转换路径。
 */
/* GOP 路径通过 REP MOVSB 使用普通存储。缓存策略和帧边界栅栏由映射/发布层负责；此辅助函数既不选择
 * 扫描输出后端，也不启用非临时事务。 */
static void display_copy_native_xrgb8888_span(
    volatile uint32_t *__restrict destination,
    const uint32_t *__restrict source,
    uint32_t pixels) {
    if (destination == 0 || source == 0 || pixels == 0U) return;

    /* 按字节计数的字符串复制可处理任意扫描线对齐，且不会触及 SIMD 状态。与此前的 volatile 像素循环
     * 一样，发布栅栏仍由调用方放在帧边界处。 */
    size_t bytes = (size_t)pixels * sizeof(uint32_t);
    __asm__ volatile ("cld; rep movsb"
                      : "+D"(destination), "+S"(source), "+c"(bytes)
                      :
                      : "memory", "cc");
}

/* 光标等即时输出直接写最终像素，不需要构造临时行缓冲；仍统一处理 GOP 色彩布局。 */
void display_core_publish_xrgb8888_pixel(volatile uint32_t *destination, uint32_t pixel) {
    if (destination == 0) return;
    *destination = g_display.format == 1U ? pixel :
        display_convert_pixel(pixel, g_display.format, g_display.masks);
}

void display_core_publish_xrgb8888_span(
    volatile uint32_t *destination,
    const uint32_t *source,
    uint32_t pixels) {

    if (destination == 0 || source == 0 || pixels == 0U) {
        return;
    }

    /*
    * 当前启动模型中，display_core_init() 完成后帧缓冲区几何信息/格式保持不变。此处不获取显示锁：
    * 合成器复制工作线程会并行发布互不重叠的跨度。
     */
    if (g_display.format == 1U) {
        display_copy_native_xrgb8888_span(
            destination,
            source,
            pixels);
        return;
    }

    for (uint32_t x = 0U; x < pixels; ++x) {
        destination[x] =
            display_convert_pixel(
                source[x],
                g_display.format,
                g_display.masks);
    }
}

static bool display_commit_shape_valid(const gpu_allocation_t *buffer,
                                       uint64_t offset, uint64_t stride,
                                       uint32_t width, uint32_t height) {
    uint64_t row_bytes;
    uint64_t required;
    if (buffer == 0 || buffer->object.type != KOBJECT_TYPE_GPU_ALLOCATION ||
        width == 0U || height == 0U ||
        stride < (uint64_t)width * sizeof(uint32_t) || offset > buffer->size) {
        return false;
    }
    row_bytes = (uint64_t)width * sizeof(uint32_t);
    if ((uint64_t)(height - 1U) > UINT64_MAX / stride) return false;
    required = (uint64_t)(height - 1U) * stride;
    if (required > UINT64_MAX - row_bytes) return false;
    required += row_bytes;
    return required <= buffer->size - offset;
}

static kstatus_t display_wait_status(const display_pending_commit_t *pending) {
    uint64_t completed;
    kstatus_t status;
    if (pending->wait_fence == 0) return K_OK;
    status = (kstatus_t)atomic_load_explicit(&pending->wait_fence->status,
                                             memory_order_acquire);
    if (status != K_OK) return status;
    completed = atomic_load_explicit(&pending->wait_fence->completed_value,
                                     memory_order_acquire);
    return completed >= pending->wait_value ? K_OK : K_ETIMEDOUT;
}

static void display_release_pending(display_pending_commit_t *pending) {
    if (pending == 0) return;
    if (pending->signal_fence != 0) object_put(pending->signal_fence);
    if (pending->wait_fence != 0) object_put(pending->wait_fence);
    if (pending->buffer != 0) {
        (void)gpu_allocation_unpin(pending->buffer);
        object_put(pending->buffer);
    }
    kfree(pending);
}


/*
 * 原生原子扫描输出所有权
 * -------------------------------
 *
 * 从 display_commit_submit() 开始，pending->buffer 持有一个对象引用和一个固定引用。
 *
 * GOP 回退路径：
 *     复制/栅栏完成时释放这两个引用。
 *
 * 原生路径成功：
 *     将这组引用和固定状态转移到 g_display.current_scanout。
 *     只有在翻页完成/垂直消隐后，才释放先前的 current_scanout。
 *
 * 原生路径失败：
 *     保留旧 current_scanout，并按常规流程释放 pending。
 *
 * 这样释放栅栏表示真正的扫描输出生命周期完成，而非仅表示“翻页命令已入队”。
 */
static void display_release_scanout_buffer(gpu_allocation_t *buffer) {
    if (buffer == 0) return;

    (void)gpu_allocation_unpin(buffer);

    object_put(buffer);
}


static bool display_scanout_backend_snapshot(
    display_scanout_backend_t *backend) {

    bool registered;

    if (backend == 0) {
        return false;
    }

    display_lock();

    registered =
        g_display.scanout_backend_registered;

    if (registered) {
        *backend =
            g_display.scanout_backend;
    }

    display_unlock();
    return registered;
}


static void display_native_scanout_complete(
    void *completion_context,
    kstatus_t status) {

    display_pending_commit_t *pending =
        (display_pending_commit_t *)completion_context;

    gpu_allocation_t *old_scanout = 0;
    uint64_t gpu_va = 0U;

    if (pending == 0) {
        return;
    }

    if (pending->buffer != 0) {
        gpu_va =
            pending->buffer->gpu_va.value;
    }

    if (status == K_OK) {
        /*
         * K_OK 表示新的扫描输出已锁存，显示引擎不再读取旧缓冲区。
         *
         * 将 pending 的引用和固定状态转移到 current_scanout，而非复制。
         */
        display_lock();

        old_scanout =
            g_display.current_scanout;

        g_display.current_scanout =
            pending->buffer;

        pending->buffer = 0;
        g_display.pending_commit = false;

        display_unlock();

        display_release_scanout_buffer(
            old_scanout);

        (void)gpu_fence_signal(
            pending->signal_fence,
            pending->signal_value);

        (void)telemetry_record_latency(
            TELEMETRY_CATEGORY_GPU_SUBMIT,
            gpu_va,
            pending->start_tsc);
    } else {
        /*
         * 原子翻页失败：保持当前扫描输出不变。新缓冲区仍由 pending 所有，并在下方释放。
         */
        display_clear_pending();

        (void)gpu_fence_fail(
            pending->signal_fence,
            status);
    }

    display_release_pending(pending);
}


void display_core_poll_scanout(void) {
    display_scanout_backend_t backend;
    display_lock();
    backend = g_display.scanout_backend;
    display_unlock();
    if (backend.poll != 0) backend.poll(backend.context);
}

static void display_submit_native_scanout(
    display_pending_commit_t *pending,
    const display_scanout_backend_t *backend) {

    display_scanout_request_t request;
    kstatus_t status;

    if (pending == 0 ||
        backend == 0 ||
        backend->atomic_commit == 0) {

        if (pending != 0) {
            display_clear_pending();

            (void)gpu_fence_fail(
                pending->signal_fence,
                K_EIO);

            display_release_pending(pending);
        }

        return;
    }

    request.output = 0U;
    request.buffer = pending->buffer;
    request.offset = pending->offset;
    request.stride = pending->stride;
    request.width = pending->width;
    request.height = pending->height;
    request.format = 1U;

    /*
    * 调用 GPU 驱动期间绝不能持有 display_lock。允许同步完成，且回调可能在调用返回前已释放 pending；
    * 因此 K_OK 路径不得再次访问 pending。
     */
    status =
        backend->atomic_commit(
            backend->context,
            &request,
            display_native_scanout_complete,
            pending);

    if (status == K_OK) {
        return;
    }

    /*
    * 返回值不是 K_OK 表示后端没有接管异步所有权，也不得调用完成回调。
     */
    display_clear_pending();

    (void)gpu_fence_fail(
        pending->signal_fence,
        status);

    display_release_pending(pending);
}

static void display_complete_commit(void *argument) {
    display_pending_commit_t *pending = (display_pending_commit_t *)argument;
    bool copied = false;
    bool complete = false;
    bool mapped = false;
    uint32_t first_row;
    uint32_t last_row;
    uint64_t map_length;
    void *source_mapping = 0;
    if (pending == 0) return;
    kstatus_t wait_status = display_wait_status(pending);
    if (wait_status == K_ETIMEDOUT) {
        /* 未满足 acquire fence 时稍后重试，期间保持 buffer 和 fence 引用。 */
        if (!deferred_schedule(display_complete_commit, pending)) {
            display_clear_pending();
            (void)gpu_fence_fail(pending->signal_fence, K_EBUSY);
            display_release_pending(pending);
        }
        return;
    }
    if (wait_status != K_OK) {
        display_clear_pending();
        (void)gpu_fence_fail(pending->signal_fence, wait_status);
        display_release_pending(pending);
        return;
    }

    /*
    * 获取栅栏完成是统一的分发点。后端仅允许注册一次，且提交待处理期间禁止注册，因此提交过程中后端不会
    * 在此决策下途切换。
     */
    {
        display_scanout_backend_t backend;

        if (display_scanout_backend_snapshot(&backend)) {
            display_submit_native_scanout(
                pending,
                &backend);
            return;
        }
    }

    first_row = pending->next_row;
    last_row = first_row + DISPLAY_COPY_ROW_BUDGET;
    if (last_row < first_row || last_row > pending->height) last_row = pending->height;
    map_length = (uint64_t)(pending->height - 1U) * pending->stride +
                 (uint64_t)pending->width * sizeof(uint32_t);
    mapped = pending->buffer != 0 &&
             gpu_allocation_cpu_map(pending->buffer, pending->offset,
                                    map_length, &source_mapping) == K_OK;
    display_lock();
    if (g_display.initialized && g_display.framebuffer != 0U &&
        pending->buffer != 0 && mapped) {
        const uint8_t *source = (const uint8_t *)source_mapping;
        volatile uint32_t *destination =
            (volatile uint32_t *)(uintptr_t)g_display.framebuffer;
        for (uint32_t y = first_row; y < last_row; ++y) {
            const uint32_t *row =
                (const uint32_t *)(const void *)(
                    source + (uint64_t)y * pending->stride);

            volatile uint32_t *out =
                destination +
                (uint64_t)y *
                    g_display.pixels_per_scanline;

            display_core_publish_xrgb8888_span(
                out,
                row,
                pending->width);
        }

        /*
         * 延迟续接任务可能在另一 CPU 上运行。MOVNTI/MOVDIR64B 的排序仅对本 CPU 有效，因此释放显示锁并
         * 调度下一块前，必须完成当前块的 WC 存储。
         */
        __asm__ volatile ("sfence" : : : "memory");

        copied = true;
    }
    display_unlock();
    if (mapped) {
        gpu_allocation_cpu_unmap(pending->buffer, source_mapping, map_length);
    }
    if (!copied) {
        display_clear_pending();
        (void)gpu_fence_fail(pending->signal_fence, K_EIO);
        display_release_pending(pending);
        return;
    }
    pending->next_row = last_row;
    complete = pending->next_row >= pending->height;
    if (!complete) {
        if (!deferred_schedule(display_complete_commit, pending)) {
            display_clear_pending();
            (void)gpu_fence_fail(pending->signal_fence, K_EBUSY);
            display_release_pending(pending);
        }
        return;
    }
    /* GOP 帧缓冲区采用 WC 映射。触发栅栏前必须完成写合并事务，因为调用方可能立即读取首像素，或扫描输出
     * 可能开始读取此帧。 */
    __asm__ volatile ("sfence" : : : "memory");
    display_clear_pending();
    (void)gpu_fence_signal(pending->signal_fence, pending->signal_value);
    (void)telemetry_record_latency(TELEMETRY_CATEGORY_GPU_SUBMIT,
                                   pending->buffer->gpu_va.value,
                                   pending->start_tsc);
    display_release_pending(pending);
}


kstatus_t display_core_register_scanout_backend(
    uint32_t output,
    const display_scanout_backend_t *backend) {

    kstatus_t status = K_OK;

    if (output != 0U ||
        backend == 0 ||
        backend->atomic_commit == 0 ||
        !display_initialize()) {

        return K_EINVAL;
    }

    display_lock();

    if (g_display.scanout_backend_registered) {
        status = K_EEXIST;
    } else if (g_display.pending_commit) {
        /*
         * 提交已被接受后，不得从 GOP 复制切换到原生扫描输出。通常 GPU 初始化时只注册一次后端。
         */
        status = K_EBUSY;
    } else {
        g_display.scanout_backend =
            *backend;

        g_display.scanout_backend_registered =
            true;
    }

    display_unlock();
    return status;
}


bool display_core_has_native_scanout(uint32_t output) {
    bool registered;

    if (output != 0U ||
        !display_initialize()) {

        return false;
    }

    display_lock();

    registered =
        g_display.scanout_backend_registered;

    display_unlock();
    return registered;
}


bool display_core_native_self_test(void) {
    gpu_allocation_t *buffer = 0;
    gpu_fence_t *fence = 0;
    uint32_t width;
    uint32_t height;
    uint64_t pixels;
    uint64_t bytes;
    uint64_t stride;
    uint64_t start_tsc;
    uint64_t timeout_tsc;
    uint64_t deadline;
    uint64_t flags;
    kstatus_t status;
    bool success = false;

    if (!display_core_has_native_scanout(0U)) {
        return false;
    }

    display_lock();
    width = g_display.width;
    height = g_display.height;
    display_unlock();

    pixels = (uint64_t)width * (uint64_t)height;
    if (width == 0U || height == 0U || pixels == 0U ||
        pixels > UINT64_MAX / sizeof(uint32_t)) {
        return false;
    }
    bytes = pixels * sizeof(uint32_t);
    stride = (uint64_t)width * sizeof(uint32_t);

    if (gpu_allocation_create(bytes, &buffer) != K_OK || buffer == 0 ||
        gpu_fence_create(&fence) != K_OK || fence == 0) {
        if (fence != 0) object_put(fence);
        if (buffer != 0) object_put(buffer);
        return false;
    }

    {
        void *mapped = 0;

        status = gpu_allocation_cpu_map(buffer, 0U, sizeof(uint32_t),
                                        &mapped);
        if (status != K_OK || mapped == 0) {
            object_put(fence);
            object_put(buffer);
            return false;
        }
        ((uint32_t *)mapped)[0] = 0x0030C0A0U;
        gpu_allocation_cpu_unmap(buffer, mapped, sizeof(uint32_t));
    }

    /* 进入原生 KAPI 路径前刷新此检查点。 */
    realtest_checkpoint("OS_NVKMS_NATIVE_COMMIT_BEGIN");
    status = display_commit_submit(
        0U, buffer, 0U, stride, width, height, 1U,
        0, 0U, fence, 1U);
    if (status != K_OK) {
        realtest_checkpoint("OS_NVKMS_NATIVE_COMMIT_SUBMIT_FAIL");
        object_put(fence);
        object_put(buffer);
        return false;
    }
    realtest_checkpoint("OS_NVKMS_NATIVE_COMMIT_QUEUED");

    start_tsc = x86_read_tsc();
    timeout_tsc = x86_timeout_ns_to_tsc(
        DISPLAY_NATIVE_SELF_TEST_TIMEOUT_NS);
    deadline = timeout_tsc == UINT64_MAX ||
               timeout_tsc > UINT64_MAX - start_tsc ?
               UINT64_MAX : start_tsc + timeout_tsc;
    __asm__ volatile ("pushfq; popq %0" : "=r"(flags) : : "memory");

    for (;;) {
        display_core_poll_scanout();
        status = gpu_fence_wait(fence, 1U, 0U);
        if (status == K_OK) {
            success = true;
            break;
        }
        if (status != K_ETIMEDOUT ||
            (deadline != UINT64_MAX &&
             (int64_t)(x86_read_tsc() - deadline) >= 0)) {
            break;
        }

        /* 启动早期尚未启动常驻延迟工作线程。 */
        (void)deferred_run(64U);
        __asm__ volatile ("sti; pause" : : : "memory");
    }

    if ((flags & (1ULL << 9)) == 0U) {
        __asm__ volatile ("cli" : : : "memory");
    }
    kernel_serial_printf_serial_only(
        "OS_NVKMS_NATIVE_FLIP_STATUS=%d\r\n", (int)status);
    realtest_checkpoint(success ?
                               "OS_NVKMS_NATIVE_FLIP_OK" :
                               "OS_NVKMS_NATIVE_FLIP_FAIL");
    object_put(fence);
    object_put(buffer);
    return success;
}


bool display_core_init(uint64_t framebuffer_virtual, uint64_t framebuffer_size,
                       uint32_t width, uint32_t height,
                       uint32_t pixels_per_scanline, uint32_t framebuffer_format,
                       const uint32_t masks[4]) {
    uint64_t required;
    if (!display_initialize()) return false;
    if (framebuffer_virtual == 0U || framebuffer_size == 0U || width == 0U ||
        height == 0U || pixels_per_scanline < width || masks == 0 ||
        pixels_per_scanline > UINT64_MAX / height || framebuffer_format > 2U) {
        return false;
    }
    required = (uint64_t)pixels_per_scanline * height * sizeof(uint32_t);
    if (required > framebuffer_size) return false;
    display_lock();
    g_display.framebuffer = framebuffer_virtual;
    g_display.framebuffer_size = framebuffer_size;
    g_display.width = width;
    g_display.height = height;
    g_display.pixels_per_scanline = pixels_per_scanline;
    g_display.format = framebuffer_format;
    for (uint32_t i = 0U; i < 4U; ++i) g_display.masks[i] = masks[i];
    g_display.initialized = true;
    display_unlock();
    return true;
}

bool display_core_is_initialized(void) {
    bool initialized;
    if (!display_initialize()) return false;
    display_lock();
    initialized = g_display.initialized;
    display_unlock();
    return initialized;
}

uint64_t display_core_framebuffer_virtual(void) {
    uint64_t framebuffer;
    if (!display_initialize()) return 0U;
    display_lock();
    framebuffer = g_display.framebuffer;
    display_unlock();
    return framebuffer;
}

bool display_core_query(uint32_t output, uint32_t *width, uint32_t *height,
                        uint32_t *stride, uint32_t *format) {
    if (width == 0 || height == 0 || stride == 0 || format == 0 ||
        output != 0U || !display_initialize()) return false;
    display_lock();
    if (!g_display.initialized) {
        display_unlock();
        return false;
    }
    *width = g_display.width;
    *height = g_display.height;
    *stride = g_display.pixels_per_scanline;
    /* 用户态提交的源缓冲统一使用 XRGB8888。 */
    *format = 1U;
    display_unlock();
    return true;
}

kstatus_t display_commit_submit(uint32_t output, gpu_allocation_t *buffer,
                                uint64_t offset, uint64_t stride,
                                uint32_t width, uint32_t height, uint32_t format,
                                gpu_fence_t *wait_fence, uint64_t wait_value,
                                gpu_fence_t *signal_fence, uint64_t signal_value) {
    display_pending_commit_t *pending;
    bool valid;
    bool busy;
    kstatus_t status;
    if (!display_initialize()) return K_EIO;
    if (output != 0U || signal_fence == 0 || signal_value == 0U ||
        (wait_fence == 0 && wait_value != 0U) ||
        (wait_fence != 0 && (wait_fence->object.type != KOBJECT_TYPE_GPU_FENCE ||
                             wait_value == 0U)) ||
        signal_fence->object.type != KOBJECT_TYPE_GPU_FENCE || format != 1U) {
        return K_EINVAL;
    }
    if (!display_commit_shape_valid(buffer, offset, stride, width, height)) {
        return K_EINVAL;
    }
    display_lock();
    /*
     * DISPLAY_COMMIT 是异步的；GOP copy 和 native page-flip 共用一个
     * pending 槽。这样 fence / buffer ownership 始终只有一个明确的状态
     * 转移点，也避免高频提交把 deferred 队列或 GPU flip queue 填满。
     * 收到 K_EBUSY 的调用者稍后重试即可。
     */
    busy = g_display.pending_commit;
    valid = g_display.initialized && !busy &&
            width <= g_display.width && height <= g_display.height &&
            g_display.format != 3U;
    if (valid) g_display.pending_commit = true;
    display_unlock();
    if (!valid) return busy ? K_EBUSY : K_EINVAL;
    pending = (display_pending_commit_t *)kzalloc(sizeof(*pending), 0);
    if (pending == 0) {
        display_clear_pending();
        return K_ENOMEM;
    }
    pending->buffer = buffer;
    pending->offset = offset;
    pending->stride = stride;
    pending->width = width;
    pending->height = height;
    pending->wait_fence = wait_fence;
    pending->wait_value = wait_value;
    pending->signal_fence = signal_fence;
    pending->signal_value = signal_value;
    pending->start_tsc = telemetry_timestamp();
    object_get(buffer);
    status = gpu_allocation_pin(buffer);
    if (status != K_OK) {
        object_put(buffer);
        kfree(pending);
        display_clear_pending();
        return status;
    }
    if (wait_fence != 0) object_get(wait_fence);
    object_get(signal_fence);
    if (!deferred_schedule(display_complete_commit, pending)) {
        display_clear_pending();
        display_release_pending(pending);
        return K_EBUSY;
    }
    return K_OK;
}

bool display_core_self_test(void) {
    gpu_allocation_t *buffer = 0;
    gpu_fence_t *fence = 0;
    uint32_t width;
    uint32_t height;
    uint64_t bytes;
    uint64_t start_tsc = 0U;
    uint64_t timeout_tsc = 0U;
    uint64_t deadline = 0U;
    uint64_t flags = 0U;
    volatile uint32_t *first_pixel = 0;
    uint32_t original_pixel = 0U;
    bool submitted = false;
    bool success;
    kstatus_t fence_status = K_ETIMEDOUT;
    if (!display_initialize()) return false;
    display_lock();
    width = g_display.width;
    height = g_display.height;
    bytes = (uint64_t)width * height * sizeof(uint32_t);
    if (g_display.initialized && g_display.framebuffer != 0U) {
        first_pixel = (volatile uint32_t *)(uintptr_t)g_display.framebuffer;
        original_pixel = *first_pixel;
    }
    display_unlock();
    if (bytes == 0U || first_pixel == 0 || bytes > UINT64_MAX - PAGE_SIZE + 1U) {
        return false;
    }
    success = gpu_allocation_create(bytes, &buffer) == K_OK &&
              gpu_fence_create(&fence) == K_OK && buffer != 0 && fence != 0;
    if (success) {
        void *mapped = 0;
        success = gpu_allocation_cpu_map(buffer, 0U, sizeof(uint32_t),
                                         &mapped) == K_OK && mapped != 0;
        if (success) {
            ((uint32_t *)mapped)[0] = 0x00112233U;
            gpu_allocation_cpu_unmap(buffer, mapped, sizeof(uint32_t));
        }
    }
    if (success) {
        success = display_commit_submit(
            0U, buffer, 0U,
            (uint64_t)width * sizeof(uint32_t),
            width, height, 1U, 0, 0U, fence, 1U) == K_OK;
        submitted = success;
        if (success) {
            start_tsc = x86_read_tsc();
            timeout_tsc = x86_timeout_ns_to_tsc(
                DISPLAY_NATIVE_SELF_TEST_TIMEOUT_NS);
            deadline = timeout_tsc == UINT64_MAX ||
                       timeout_tsc > UINT64_MAX - start_tsc ?
                       UINT64_MAX : start_tsc + timeout_tsc;
            __asm__ volatile ("pushfq; popq %0" : "=r"(flags) : : "memory");

            success = false;
            for (;;) {
                fence_status = gpu_fence_wait(fence, 1U, 0U);
                if (fence_status == K_OK) {
                    success = true;
                    break;
                }
                if (fence_status != K_ETIMEDOUT ||
                    (deadline != UINT64_MAX &&
                     (int64_t)(x86_read_tsc() - deadline) >= 0)) {
                    break;
                }

                /* 完整帧 GOP 复制可能跨越多个延迟工作批次。 */
                (void)deferred_run(64U);
                __asm__ volatile ("sti; pause" : : : "memory");
            }

            if ((flags & (1ULL << 9)) == 0U) {
                __asm__ volatile ("cli" : : : "memory");
            }
            if (!success) {
                kernel_serial_printf_serial_only(
                    "OS_DISPLAY_SELF_TEST_FENCE_FAIL status=%d\r\n",
                    (int)fence_status);
            }
        }
    }
    if (submitted) {
        *first_pixel = original_pixel;
    }
    if (fence != 0) object_put(fence);
    if (buffer != 0) object_put(buffer);
    return success;
}
