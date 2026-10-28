#include <arch/x86_64/context.h>
#include <arch/x86_64/cpu.h>
#include <arch/x86_64/smp.h>
#include <kernel/console.h>
#include <kernel/elf_loader.h>
#include <kernel/kmem.h>
#include <kernel/sched.h>
#include <kernel/string.h>
#include <kernel/vfs.h>
#include "internal.h"

#ifdef OS_KERNEL_BUILD

/* REFACTOR_P7A_SHELL_LAUNCHER_OWNER：桌面应用启动和标题策略。 */

#define DESKTOP_PROGRAM_MAX_BYTES (16ULL * 1024ULL * 1024ULL)

#define DESKTOP_LAUNCH_WORKER_STACK_SIZE (32U * 1024U)
#define DESKTOP_CHROME_LAUNCH_STARTING UINT64_MAX
static atomic_bool g_desktop_launch_worker_starting;

static uint8_t g_desktop_launch_worker_stack
    [DESKTOP_LAUNCH_WORKER_STACK_SIZE]
    __attribute__((aligned(16)));

static bool desktop_chrome_launch_claim(
    atomic_uint_fast64_t *launch_pid) {
    uint_fast64_t expected = 0U;
    return atomic_compare_exchange_strong_explicit(
        launch_pid, &expected, DESKTOP_CHROME_LAUNCH_STARTING,
        memory_order_acq_rel, memory_order_acquire);
}

static bool desktop_chrome_launch_publish_pid(
    atomic_uint_fast64_t *launch_pid, uint64_t pid) {
    uint_fast64_t expected = DESKTOP_CHROME_LAUNCH_STARTING;
    return pid != 0U && pid != DESKTOP_CHROME_LAUNCH_STARTING &&
           atomic_compare_exchange_strong_explicit(
               launch_pid, &expected, pid,
               memory_order_acq_rel, memory_order_acquire);
}

static bool desktop_chrome_launch_release(
    atomic_uint_fast64_t *launch_pid, uint64_t pid) {
    uint_fast64_t expected = pid;
    return pid != 0U && atomic_compare_exchange_strong_explicit(
        launch_pid, &expected, 0U,
        memory_order_acq_rel, memory_order_acquire);
}

/* 窗口生命周期和启动请求共用 WindowLock。启动期间的双击会合并；最后一个窗口关闭后发生的点击
 * 则会跨越进程销毁保留下来。 */
static bool desktop_chrome_request_launch(void) {
    bool claimed;
    window_lock();
    claimed = desktop_chrome_launch_claim(
        &g_window_server.desktop_chrome_launch_pid);
    if (claimed) {
        g_window_server.desktop_chrome_closing_pid = 0U;
        atomic_store_explicit(&g_window_server.desktop_chrome_relaunch_pending,
                              false, memory_order_release);
    } else {
        uint64_t pid = atomic_load_explicit(
            &g_window_server.desktop_chrome_launch_pid, memory_order_acquire);
        if (pid != 0U && pid != DESKTOP_CHROME_LAUNCH_STARTING &&
            pid == g_window_server.desktop_chrome_closing_pid) {
            atomic_store_explicit(
                &g_window_server.desktop_chrome_relaunch_pending,
                true, memory_order_release);
        }
    }
    window_unlock();
    return claimed;
}

void desktop_chrome_window_opened_locked(process_t *owner) {
    if (owner == 0 || owner->pid != atomic_load_explicit(
            &g_window_server.desktop_chrome_launch_pid, memory_order_acquire)) {
        return;
    }
    g_window_server.desktop_chrome_closing_pid = 0U;
    atomic_store_explicit(&g_window_server.desktop_chrome_relaunch_pending,
                          false, memory_order_release);
}

void desktop_chrome_window_closed_locked(process_t *owner) {
    if (owner == 0 || owner->pid != atomic_load_explicit(
            &g_window_server.desktop_chrome_launch_pid, memory_order_acquire)) {
        return;
    }
    for (uint32_t i = 0U; i < g_window_server.scene.count; ++i) {
        if (g_window_server.scene.children[i] != 0 &&
            g_window_server.scene.children[i]->owner == owner) return;
    }
    g_window_server.desktop_chrome_closing_pid = owner->pid;
}

bool desktop_chrome_process_released_locked(uint64_t pid) {
    if (!desktop_chrome_launch_release(
            &g_window_server.desktop_chrome_launch_pid, pid)) return false;
    g_window_server.desktop_chrome_closing_pid = 0U;
    return atomic_load_explicit(&g_window_server.desktop_chrome_relaunch_pending,
                                memory_order_acquire);
}

bool desktop_chrome_launch_guard_self_test(void) {
    atomic_uint_fast64_t launch_pid;
    atomic_init(&launch_pid, 0U);

    if (!desktop_chrome_launch_claim(&launch_pid) ||
        desktop_chrome_launch_claim(&launch_pid) ||
        !desktop_chrome_launch_publish_pid(&launch_pid, 42U) ||
        desktop_chrome_launch_release(&launch_pid, 41U) ||
        atomic_load_explicit(&launch_pid, memory_order_acquire) != 42U ||
        desktop_chrome_launch_claim(&launch_pid) ||
        !desktop_chrome_launch_release(&launch_pid, 42U) ||
        atomic_load_explicit(&launch_pid, memory_order_acquire) != 0U ||
        !desktop_chrome_launch_claim(&launch_pid) ||
        !desktop_chrome_launch_release(
            &launch_pid, DESKTOP_CHROME_LAUNCH_STARTING) ||
        !desktop_chrome_launch_claim(&launch_pid) ||
        !desktop_chrome_launch_publish_pid(&launch_pid, 43U) ||
        !desktop_chrome_launch_release(&launch_pid, 43U)) {
        return false;
    }
    return true;
}

const char *window_shell_program_path(uint32_t app) {
    if (app==DESKTOP_APP_WALLPAPER) return "/usr/apps/settings/wallpaper";
    return desktop_shell_assets_available() && app > 0U &&
        app < sizeof(g_desktop_programs) / sizeof(g_desktop_programs[0]) &&
        g_desktop_programs[app].path[0] ? g_desktop_programs[app].path : 0;
}

const char *window_shell_app_title(uint32_t app) {
    if (app==DESKTOP_APP_WALLPAPER) return "更换壁纸";
    return desktop_shell_assets_available() && app > 0U &&
        app < sizeof(g_desktop_programs) / sizeof(g_desktop_programs[0]) &&
        g_desktop_programs[app].title[0] ? g_desktop_programs[app].title : 0;
}

bool window_shell_title_matches(const char *title, const char *expected) {
    uint32_t index = 0U;
    if (title == 0 || expected == 0) return false;
    while (index < 32U) {
        if (title[index] != expected[index]) return false;
        if (expected[index] == '\0') return true;
        ++index;
    }
    return false;
}

static bool desktop_launch_worker_ready(void *context) {
    (void)context;
    return atomic_load_explicit(&g_window_server.desktop_launch_queue_count,
                                memory_order_acquire) != 0U ||
           (atomic_load_explicit(&g_window_server.desktop_chrome_relaunch_pending,
                                 memory_order_acquire) &&
            atomic_load_explicit(&g_window_server.desktop_chrome_launch_pid,
                                 memory_order_acquire) == 0U);
}

static bool desktop_launch_dequeue(uint32_t *app) {
    uint32_t count;

    if (app == 0) return false;

    window_lock();
    count = atomic_load_explicit(&g_window_server.desktop_launch_queue_count,
                                 memory_order_relaxed);
    if (count == 0U) {
        if (atomic_load_explicit(
                &g_window_server.desktop_chrome_relaunch_pending,
                memory_order_relaxed) &&
            atomic_load_explicit(&g_window_server.desktop_chrome_launch_pid,
                                 memory_order_acquire) == 0U) {
            atomic_store_explicit(
                &g_window_server.desktop_chrome_relaunch_pending,
                false, memory_order_release);
            *app = DESKTOP_APP_CHROME;
            window_unlock();
            return true;
        }
        window_unlock();
        return false;
    }

    *app = g_window_server.desktop_launch_queue[
        g_window_server.desktop_launch_queue_head];
    g_window_server.desktop_launch_queue_head =
        (g_window_server.desktop_launch_queue_head + 1U) %
        WINDOW_DESKTOP_LAUNCH_QUEUE_CAPACITY;
    atomic_store_explicit(&g_window_server.desktop_launch_queue_count,
                          count - 1U, memory_order_release);
    window_unlock();
    return true;
}

bool desktop_launch_enqueue(uint32_t app) {
    uint32_t count;

    if (window_shell_program_path(app) == 0 ||
        !atomic_load_explicit(&g_window_server.desktop_launch_worker_started,
                              memory_order_acquire)) {
        return false;
    }

    window_lock();
    count = atomic_load_explicit(&g_window_server.desktop_launch_queue_count,
                                 memory_order_relaxed);
    if (count >= WINDOW_DESKTOP_LAUNCH_QUEUE_CAPACITY) {
        window_unlock();
        return false;
    }

    g_window_server.desktop_launch_queue[
        g_window_server.desktop_launch_queue_tail] = app;
    g_window_server.desktop_launch_queue_tail =
        (g_window_server.desktop_launch_queue_tail + 1U) %
        WINDOW_DESKTOP_LAUNCH_QUEUE_CAPACITY;
    atomic_store_explicit(&g_window_server.desktop_launch_queue_count,
                          count + 1U, memory_order_release);
    window_unlock();

    /* 工作线程会在再次等待前清空队列。仅从空变为非空时需要通知；后续追加已能被其就绪条件
     * 观察到，无需额外获取等待锁。 */
    if (count == 0U) (void)wake_one(&g_window_server.desktop_launch_waitq);
    return true;
}

static void __attribute__((noreturn)) desktop_launch_worker_main(
    void *argument) {
    uint32_t app;
    (void)argument;

    for (;;) {
        if (wait_on_queue(&g_window_server.desktop_launch_waitq,
                          desktop_launch_worker_ready, 0,
                          UINT64_MAX) != K_OK) {
            continue;
        }

        while (desktop_launch_dequeue(&app)) {
            if (desktop_restore_minimized_app(app)) continue;

            kstatus_t status = desktop_launch_program(app);
            (void)status;
        }
    }
}

bool desktop_shell_start_launch_worker(uint32_t compositor_cpu) {
    uint32_t current_cpu = x86_current_cpu_index();
    uint32_t launch_cpu = UINT32_MAX;
    thread_t *worker;

    if (atomic_load_explicit(&g_window_server.desktop_launch_worker_started,
                             memory_order_acquire)) {
        return true;
    }

    if (current_cpu >= MAX_CPUS || sched_current_thread() == 0) return false;

    /* 存在空闲 AP 时，启动 I/O 不应占用引导 CPU 或合成器。在双 CPU 客户机上，即使与引导
     * CPU 共用，合成器仍可及时处理指针输入。 */
    for (uint32_t candidate = 0U;
         candidate < x86_smp_discovered_count() && candidate < MAX_CPUS;
         ++candidate) {
        if (candidate == current_cpu || candidate == compositor_cpu ||
            !x86_smp_cpu_online(candidate)) {
            continue;
        }
        launch_cpu = candidate;
        break;
    }
    if (launch_cpu == UINT32_MAX) launch_cpu = current_cpu;
    if (launch_cpu >= MAX_CPUS) return false;
    if (!window_worker_claim_start(&g_desktop_launch_worker_starting,
                                    &g_window_server.desktop_launch_worker_started))
        return atomic_load_explicit(&g_window_server.desktop_launch_worker_started,
                                    memory_order_acquire);

    worker = &g_window_server.desktop_launch_worker;
    memset(worker, 0, sizeof(*worker));

    refcount_init(&worker->object.refs, 1U);
    worker->object.type = KOBJECT_TYPE_THREAD;
    worker->object.flags = 0U;
    worker->object.ops = 0;
    worker->tid = UINT64_MAX - 5ULL;
    worker->process = 0;
    sched_initialize_new_thread(worker, launch_cpu, true);
    atomic_store_explicit(&worker->state, THREAD_READY, memory_order_relaxed);
    worker->kernel_stack_base = g_desktop_launch_worker_stack;
    worker->kernel_stack_size = sizeof(g_desktop_launch_worker_stack);
    worker->kernel_stack_top =
        ((vaddr_t)(uintptr_t)g_desktop_launch_worker_stack +
         sizeof(g_desktop_launch_worker_stack)) & ~(vaddr_t)0x0FULL;
    list_init(&worker->process_node);
    list_init(&worker->owned_mutexes);

    x86_prepare_kernel_thread(&worker->arch, worker->kernel_stack_top,
                              desktop_launch_worker_main, 0);

    atomic_store_explicit(&g_window_server.desktop_launch_worker_started,
                          true, memory_order_release);
    if (!sched_enqueue_bootstrap(worker)) {
        atomic_store_explicit(&g_window_server.desktop_launch_worker_started,
                              false, memory_order_release);
        atomic_store_explicit(&g_desktop_launch_worker_starting, false,
                              memory_order_release);
        return false;
    }
    sched_monitor_register(worker, "app-launcher");
    atomic_store_explicit(&g_desktop_launch_worker_starting, false, memory_order_release);
    return true;
}

uint32_t desktop_window_app_locked(const window_server_window_t *window) {
    if (!window || !window->owner || !desktop_shell_assets_available()) return DESKTOP_APP_NONE;
    process_t *owner = window->owner;
    uint32_t result = DESKTOP_APP_NONE;
    spinlock_lock(&owner->info_lock);
    for (uint32_t app=1; app<DESKTOP_APP_SLOTS; ++app) {
        const char *path = g_desktop_programs[app].path, *base = path;
        for (const char *p=path; *p; ++p) if (*p == '/') base = p+1;
        if (window_shell_title_matches(owner->name, app == DESKTOP_APP_CHROME ? "chrome" : base)) {
            result = app;
            break;
        }
    }
    if (window_shell_title_matches(owner->name,"wallpaper")) result=DESKTOP_APP_WALLPAPER;
    spinlock_unlock(&owner->info_lock);
    return result;
}

static bool desktop_window_matches_app_locked(
    const window_server_window_t *window, uint32_t app,
    const char *expected_title) {
    (void)expected_title;
    return window->desktop_app == app;
}

bool desktop_restore_minimized_app(uint32_t app) {
    const char *expected = window_shell_app_title(app);
    window_server_window_t *window = 0;
    if (expected == 0) return false;

    window_lock();
    for (uint32_t index = g_window_server.scene.count; index != 0U; --index) {
        window_server_window_t *candidate =
            g_window_server.scene.children[index - 1U];
        if (candidate == 0 ||
            (candidate->flags & OS_WINDOW_VISIBLE) == 0U ||
            !desktop_window_matches_app_locked(candidate, app, expected)) {
            continue;
        }
        window = candidate;
        break;
    }
    if (window == 0) {
        window_unlock();
        return false;
    }

    bool was_minimized = window->minimized;
    window_animation_restore_locked(window);
    window->minimized = false;
    window->dirty = true;
    window_scene_focus_locked(window);
    window_mark_window_locked(window);
    if (was_minimized) window_enqueue_minimized_event_locked(window);
    window_input_refresh_pointer_locked();
    window_unlock();
    (void)wake_all(&g_window_server.event_waitq);
    return true;
}

kstatus_t desktop_launch_program(uint32_t app) {
    const char *path = window_shell_program_path(app);
    bool chrome_launch_claimed = app == DESKTOP_APP_CHROME;
    uint64_t chrome_launch_token = 0U;
    process_t *process = 0;
    thread_t *thread = 0;
    user_elf_image_info_t info;
    vaddr_t stack_pointer = 0U;
    void *image = 0;
    size_t image_size = 0U;
    kstatus_t status;

    if (path == 0) return K_EINVAL;
    if (chrome_launch_claimed) {
        if (!desktop_chrome_request_launch()) {
            return K_OK;
        }
        chrome_launch_token = DESKTOP_CHROME_LAUNCH_STARTING;
    }
    status = vfs_load_file_kernel(path, 1U, DESKTOP_PROGRAM_MAX_BYTES,
                                  &image, &image_size);
    if (status != K_OK) goto done;

    status = process_create(0, &process);
    if (status == K_OK) {
        if (chrome_launch_claimed) {
            if (!desktop_chrome_launch_publish_pid(
                    &g_window_server.desktop_chrome_launch_pid,
                    process->pid)) {
                status = K_EBUSY;
                goto done;
            }
            chrome_launch_token = process->pid;
        }
        process_set_name(process, path);
        status = process_load_elf_image(process, image, image_size,
                                        &info, &stack_pointer);
    }
    kfree(image);
    image = 0;
    if (status == K_OK) {
        status = thread_create_user_suspended(process, info.entry,
                                              stack_pointer, 0U, 0U,
                                              &thread);
    }
    if (status == K_OK) {
        status = thread_start(thread);
    }
done:
    /* 每个指针都持有一个引用，直到上方或此处释放。包括 Chrome 启动声明丢失在内的失败情况，
     * 都走相同处理路径。 */
    if (image != 0) kfree(image);
    if (thread != 0) {
        if (status != K_OK) (void)thread_terminate(thread, status);
        object_put(thread);
    }
    if (process != 0) {
        if (status != K_OK && thread == 0) (void)process_abort(process);
        object_put(process);
    }
    if (status != K_OK && chrome_launch_claimed) {
        (void)desktop_chrome_launch_release(
            &g_window_server.desktop_chrome_launch_pid,
            chrome_launch_token);
    }
    return status;
}

#endif
