#include <arch/x86_64/context.h>
#include <arch/x86_64/cpu.h>
#include <arch/x86_64/smp.h>
#include <kernel/console.h>
#include <kernel/display.h>
#include <kernel/perf.h>
#include <kernel/sched.h>
#include <kernel/string.h>
#include <kernel/telemetry.h>

#include <ascii_font.h>

#include "internal.h"
#include <uapi/window_theme.h>

static atomic_uint g_theme;
uint32_t window_server_theme(void) {
    return atomic_load_explicit(&g_theme,memory_order_relaxed);
}
kstatus_t window_server_set_theme(uint32_t theme,uint32_t background) {
    if (theme>=OS_THEME_COUNT || background>=OS_THEME_BACKGROUND_COUNT) return K_EINVAL;
    theme|=background<<8U;
    window_lock();
    if (window_server_theme()!=theme) {
        atomic_store_explicit(&g_theme,theme,memory_order_relaxed);
        window_mark_dirty_locked();
    }
    window_unlock();
    window_server_notify_worker();
    return K_OK;
}

/* REFACTOR_P7A_SERVER_OWNER：状态、生命周期、工作线程和就绪管理。 */

#define WINDOW_SERVER_WORKER_STACK_SIZE (64U * 1024U)
atomic_bool g_window_dirty_hint __attribute__((aligned(64)));

atomic_uint_fast64_t g_window_dirty_generation
    __attribute__((aligned(64)));

atomic_uint_fast64_t g_window_dirty_notified_generation
    __attribute__((aligned(64)));

/* System 桌面 shell：壁纸、图标和启动器。 */
/*
 * 快照本地的 tile floor 缓存。
 *
 * 合成器只有一个所有者（g_window_server.composing），因此无需使用 atomic。
 * 将其放在 compositor_snapshot_t 外，避免已很大的不可变快照在无关代码中额外携带 16 KiB。
 */
window_server_state_t g_window_server;

static uint8_t g_window_server_worker_stack[WINDOW_SERVER_WORKER_STACK_SIZE]
    __attribute__((aligned(16)));

static atomic_uint g_window_server_init_state;
static atomic_bool g_window_worker_running_reported;
static atomic_bool g_window_worker_starting;

void window_mark_dirty_locked(void);
void window_mark_rect_locked(int32_t x, int32_t y,
                             uint32_t width, uint32_t height);
void window_mark_window_locked(const window_server_window_t *window);
void window_mark_surface_locked(const window_server_window_t *window,
                                int32_t x, int32_t y,
                                uint32_t width, uint32_t height);
void window_mark_moved_rect_locked(int32_t old_x, int32_t old_y,
                                   int32_t new_x, int32_t new_y,
                                   uint32_t width, uint32_t height,
                                   bool render_new_position);
void window_mark_moved_cursor_locked(uint32_t old_x, uint32_t old_y,
                                     uint32_t new_x, uint32_t new_y);
void window_coalesce_damage_locked(void);
bool window_server_init(void) {
    unsigned expected = 0U;
    if (atomic_compare_exchange_strong_explicit(&g_window_server_init_state,
                                                &expected, 1U,
                                                memory_order_acq_rel,
                                                memory_order_acquire)) {
        atomic_init(&g_window_server.lock.state, 0U);
        wait_queue_init(&g_window_server.event_waitq);
        wait_queue_init(&g_window_server.worker_waitq);
        input_core_bind_wakeup(&g_window_server.worker_waitq);
        desktop_shell_init();
        atomic_init(&g_window_server.worker_generation, 0U);
        wait_queue_init(&g_window_server.desktop_launch_waitq);
        atomic_init(&g_window_server.desktop_launch_queue_count, 0U);
        atomic_init(&g_window_server.desktop_chrome_launch_pid, 0U);
        atomic_init(&g_window_server.desktop_chrome_relaunch_pending, false);
        g_window_server.desktop_chrome_closing_pid = 0U;
        g_window_server.desktop_launch_queue_head = 0U;
        g_window_server.desktop_launch_queue_tail = 0U;
        atomic_init(&g_window_dirty_hint, false);
        atomic_init(&g_window_dirty_generation, 0U);
        atomic_init(&g_window_dirty_notified_generation, 0U);
        window_event_reset_ready_locked();

        window_registry_reset_locked();
        window_scene_set_focus_identifier_locked(0U);
        g_window_server.manager = 0;
        window_display_reset_locked();
        window_buffer_reset_locked();
        window_input_set_pointer_locked(0U, 0U);
        window_present_cursor_reset_locked();
        desktop_set_hovered_app_locked(DESKTOP_APP_NONE);
        window_input_reset_desktop_state_locked();
        window_input_reset_drag_locked();
        window_clear_title_capture_locked();
        atomic_init(&g_window_server.worker_started, false);
        atomic_init(&g_window_server.desktop_launch_worker_started, false);
        atomic_init(&g_window_server.desktop_asset_worker_started, false);
        atomic_init(&g_window_worker_running_reported, false);
        g_window_server.kernel_ready = false;
        compositor_reset_state_locked();
        window_damage_reset_locked();
        if (process_register_teardown_callback(window_server_close_process) !=
            K_OK) {
            atomic_store_explicit(&g_window_server_init_state, 0U,
                                  memory_order_release);
            return false;
        }
        atomic_store_explicit(&g_window_server_init_state, 2U,
                              memory_order_release);
        return true;
    }
    while (atomic_load_explicit(&g_window_server_init_state,
                                memory_order_acquire) != 2U) {
        __asm__ volatile ("pause");
    }
    return true;
}

static bool window_server_worker_ready(void *context) {
    (void)context;

    /*
    * 工作线程会在首次提交显示模式前发布，使之后的输入能走常规调度路径。但显示所有者完成
    * window_server_kernel_ready() 前，dirty 仅表示启动意图，compositor_frame_run() 不得因此自旋。
     */
    if (!g_window_server.kernel_ready) return false;

    if (input_core_pending() != 0U) return true;
    if (window_animation_ready()) return true;

    return atomic_load_explicit(&g_window_dirty_hint,
                                memory_order_acquire) &&
           compositor_native_retry_ready();
}

void window_server_notify_worker(void) {
    bool input_pending;

    if (!window_server_init() ||
        !atomic_load_explicit(
            &g_window_server.worker_started,
            memory_order_acquire)) {
        return;
    }

    input_pending =
        input_core_pending() != 0U;

    /*
    * 输入状态独立于场景 dirty：仅光标移动的 HID 事件仍必须立即唤醒工作线程。
     */
    if (!input_pending &&
        atomic_load_explicit(
            &g_window_dirty_hint,
            memory_order_acquire)) {

        uint64_t generation =
            atomic_load_explicit(
                &g_window_dirty_generation,
                memory_order_acquire);

        uint64_t notified =
            atomic_load_explicit(
                &g_window_dirty_notified_generation,
                memory_order_relaxed);

        /*
         * 每个场景代数仅允许一个解锁后的通知者获胜。
         * 渲染不会延迟：首次 clean->dirty 的生产者仍会立即唤醒工作线程。
         */
        for (;;) {
            if (notified == generation) {
                return;
            }

            if (atomic_compare_exchange_weak_explicit(
                    &g_window_dirty_notified_generation,
                    &notified,
                    generation,
                    memory_order_acq_rel,
                    memory_order_relaxed)) {
                break;
            }
        }
    }

    /*
    * 保留现有计数器作为实际通知次数的诊断信息。
     */
    atomic_fetch_add_explicit(
        &g_window_server.worker_generation,
        1U,
        memory_order_release);

    (void)wake_one(
        &g_window_server.worker_waitq);
}

static uint64_t window_server_worker_wait_ns(void) {
    uint64_t retry=compositor_native_retry_wait_ns(), animation=window_animation_wait_ns();
    return retry<animation?retry:animation;
}

/* 空闲时无限期休眠；仅活动动画和原生提交退避使用限时唤醒。 */
static void __attribute__((noreturn)) window_server_worker_main(void *argument) {
    (void)argument;
    if (!atomic_exchange_explicit(&g_window_worker_running_reported, true,
                                  memory_order_acq_rel)) {
        kernel_serial_printf_serial_only(
            "OS_DIAG_WINDOW_WORKER_RUNNING CPU=%u\r\n",
            x86_current_cpu_index());
    }
    for (;;) {
        /* 输入报告和客户端 WINDOW_UPDATE 提交都会唤醒工作线程。wait_on_queue 中的条件/阻塞
         * 握手可关闭生产者到休眠之间的丢失唤醒窗口。 */
        if (wait_on_queue(&g_window_server.worker_waitq,
                          window_server_worker_ready, 0,
                          window_server_worker_wait_ns()) != K_OK) continue;

        /*
         * 普通的纯指针移动使用 HID 速率的直接光标路径；QEMU 与真机使用相同发布逻辑。
         *
         * 活动拖动使用单次输入处理，并提高处理预算。将队列中整段纯移动事件放入一个
         * window_motion_batch_t，可确保确定是否复用保留内容前不会产生中间脏几何状态。
         *
         * 直接合成已完成路由的状态。此处再调用一次输入处理会在 dirty=1 时产生第二次几何
         * 状态转换，从而使保留场景的正确性证明失效。
         */
        for (;;) {
            window_animation_tick();
            bool defer_cursor_for_drag = false;
            if (input_core_pending() != 0U) {
                /*
                 * 不采用 FPS 策略：立即处理当前已排队的输入。现有移动批次只合并已经待处理的
                 * 事件；场景发布仍完全由脏状态驱动。
                 */
                window_server_pump_input_mode(false);

                window_lock();
                defer_cursor_for_drag =
                    g_window_server.dragging_identifier != 0U &&
                    g_window_server.dirty;
                window_unlock();

                if (!defer_cursor_for_drag) {
                    compositor_present_cursor_direct(false);
                }
            }

            if (!atomic_load_explicit(&g_window_dirty_hint,
                                      memory_order_acquire) ||
                !compositor_native_retry_ready()) {
                break;
            }

            /*
             * 渲染期间到达的报告会留到下一帧处理；下一帧的保留内容来源将是本帧已提交的
             * compositor_presented_* 几何信息。
             */
                    compositor_frame_run();
        }
    }
}

bool window_server_start_worker(void) {
    if (!window_server_init()) return false;
    if (atomic_load_explicit(&g_window_server.worker_started,
                             memory_order_acquire)) {
        (void)desktop_shell_start_launch_worker(
            g_window_server.worker.owner_cpu);
        return true;
    }

    thread_t *current = sched_current_thread();
    uint32_t cpu_id = x86_current_cpu_index();
    uint32_t worker_cpu = cpu_id;
    if (current == 0 || cpu_id >= MAX_CPUS) return false;
    if (!window_worker_claim_start(&g_window_worker_starting,
                                    &g_window_server.worker_started))
        return atomic_load_explicit(&g_window_server.worker_started, memory_order_acquire);

    /*
    * AP 上线后，避免合成器占用引导 CPU。xHCI/延迟工作和用户态早期启动主要集中在 CPU0；
    * 将高开销合成器固定到该 CPU，会使指针输入等待完整的保留缓冲区发布。单个 AP 已足够，
    * 因为 window_lock 仍会串行化场景修改和合成。
     */
    for (uint32_t candidate = 0U;
         candidate < x86_smp_discovered_count() && candidate < MAX_CPUS;
         ++candidate) {
        if (candidate != cpu_id && x86_smp_cpu_online(candidate)) {
            worker_cpu = candidate;
            break;
        }
    }

    thread_t *worker = &g_window_server.worker;
    memset(worker, 0, sizeof(*worker));

    refcount_init(&worker->object.refs, 1U);
    worker->object.type = KOBJECT_TYPE_THREAD;
    worker->object.flags = 0U;
    worker->object.ops = 0;
    worker->tid = UINT64_MAX - 1ULL;
    worker->process = 0;
    /* 合成任务使用普通公平调度类，避免大型渲染饿死生成下一帧的应用。 */
    sched_initialize_new_thread(worker, worker_cpu, true);
    atomic_store_explicit(&worker->state, THREAD_READY, memory_order_relaxed);
    worker->kernel_stack_base = g_window_server_worker_stack;
    worker->kernel_stack_size = sizeof(g_window_server_worker_stack);
    worker->kernel_stack_top =
        ((vaddr_t)(uintptr_t)g_window_server_worker_stack +
         sizeof(g_window_server_worker_stack)) & ~(vaddr_t)0x0FULL;
    list_init(&worker->process_node);
    list_init(&worker->owned_mutexes);

    x86_prepare_kernel_thread(&worker->arch, worker->kernel_stack_top,
                              window_server_worker_main, 0);

    /*
    * 根目录资源每个都有数 MB。首帧不依赖这些资源，因此服务启动时不要同步等待三次 VFS
    * 读取。先发布确定性的纯色占位表面，再由一次性资源工作线程异步发布实际图像。
     */
    window_lock();
    window_mark_dirty_locked();
    window_unlock();

    atomic_store_explicit(&g_window_server.worker_started, true,
                          memory_order_release);
    if (!sched_enqueue_bootstrap(worker)) {
        atomic_store_explicit(&g_window_server.worker_started, false,
                              memory_order_release);
        atomic_store_explicit(&g_window_worker_starting, false, memory_order_release);
        return false;
    }
    sched_monitor_register(worker, "compositor");
    atomic_store_explicit(&g_window_worker_starting, false, memory_order_release);
    /* 应用映像加载有意独立于输入和合成器所属线程。输入处理只负责将启动请求入队；VFS 读取
     * 和 ELF 映射由此工作线程执行。 */
    (void)desktop_shell_start_launch_worker(worker_cpu);
    /* 下一个普通调度边界会启动本地合成器。发布启动状态时绝不能切离引导调用方；在单 CPU
     * 客户机上，这会使剩余启动流程无法继续。 */
    return true;
}

bool window_server_start_asset_worker(void) {
    if (!window_server_init()) return false;
    if (desktop_shell_assets_available()) {
        return true;
    }
    if (!atomic_load_explicit(&g_window_server.worker_started,
                              memory_order_acquire)) {
        return false;
    }

    /*
    * 调用方仅在加载 /EFI/OS/init-runtime 后启动此任务。大型图像读取仍可能与用户服务共享 VFS，
    * 但不再会延迟启动关键的运行时交接。
     */
    return desktop_shell_start_asset_worker(
        g_window_server.worker.owner_cpu);
}

bool window_server_kernel_ready(void) {
    uint64_t benchmark_start;

    if (!window_server_init()) return false;
    if (g_window_server.kernel_ready) return true;
    if (!ascii_font_load()) return false;
    if (!ui_font_load()) return false;
    if (!window_display_prepare()) return false;
    window_buffer_prepare();
    window_input_set_pointer_locked(
        g_window_server.display_width / 2U,
        g_window_server.display_height / 2U);
    window_lock();
    g_window_server.kernel_ready = true;
    window_mark_dirty_locked();
    window_unlock();

    /* 立即显示初始桌面。合成器工作线程可能被放置在 AP 上，并且在首次唤醒前可以合法地保持
     * 休眠；启动过程不得依赖键盘或鼠标报告来显示已经标记为脏的桌面。 */
    benchmark_start = telemetry_timestamp();
    window_server_pump_input_mode(true);
    /* 工作线程此时可能已经因等待显示条件而阻塞。 */
    window_server_notify_worker();
    kernel_perf_emit_scope("graphics.static_desktop_cpu", benchmark_start);
    return true;
}

/*
 * 服务端锁是窗口修改、输入路由、损坏区域发布和合成器快照共用的同步边界。锁实现应与
 * compat/graphics/window_server.c 中的过渡策略实现保持独立。
 */
void window_lock(void) {
    sched_preempt_disable();

    for (;;) {
        if (atomic_exchange_explicit(&g_window_server.lock.state, 1U,
                                     memory_order_acquire) == 0U) {
            return;
        }

        /* test-test-and-set 在发生竞争时可让缓存行保持共享状态。 */
        while (atomic_load_explicit(&g_window_server.lock.state,
                                    memory_order_relaxed) != 0U) {
            __asm__ volatile ("pause");
        }
    }
}

void window_unlock(void) {
    atomic_store_explicit(&g_window_server.lock.state, 0U,
                          memory_order_release);
    sched_preempt_enable();
}
