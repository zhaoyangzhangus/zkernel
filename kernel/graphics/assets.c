#include <arch/x86_64/context.h>
#include <arch/x86_64/cpu.h>
#include <arch/x86_64/smp.h>
#include <kernel/console.h>
#include <kernel/kmem.h>
#include <kernel/sched.h>
#include <kernel/string.h>
#include <kernel/vfs.h>
#include <kernel/control_tree.h>
#include "internal.h"
#include "../../user/runtime/control_file.h"

/* REFACTOR_P7A_SHELL_ASSETS_OWNER: cold desktop asset I/O and worker policy. */

#define WINDOW_DESKTOP_ASSET_WORKER_STACK_SIZE (32U * 1024U)
#define DESKTOP_ASSET_MAX_BYTES               (16ULL * 1024ULL * 1024ULL)

static wait_queue_t g_desktop_asset_waitq;
static atomic_bool g_desktop_asset_worker_starting;
static uint8_t g_desktop_asset_worker_stack
    [WINDOW_DESKTOP_ASSET_WORKER_STACK_SIZE]
    __attribute__((aligned(16)));

atomic_bool g_desktop_assets_available;
desktop_asset_image_t g_desktop_wallpaper_asset;
desktop_asset_image_t g_desktop_app_assets[8];
desktop_app_binding_t g_desktop_programs[8];
static ui_file_document_t g_desktop_document;

/* 一个请求槽串行化配置更新；申请线程可睡眠，不占用输入或合成线程。 */
enum desktop_wallpaper_request_state {
    DESKTOP_WALLPAPER_IDLE,
    DESKTOP_WALLPAPER_CLAIMED,
    DESKTOP_WALLPAPER_READY,
    DESKTOP_WALLPAPER_RUNNING,
    DESKTOP_WALLPAPER_DONE,
};
static atomic_uint g_wallpaper_request_state;
static char g_wallpaper_request_path[256];
static kstatus_t g_wallpaper_request_status;
static bool desktop_wallpaper_done(void *unused) {
    (void)unused;return atomic_load_explicit(&g_wallpaper_request_state,memory_order_acquire)==DESKTOP_WALLPAPER_DONE;
}
kstatus_t desktop_wallpaper_set(const char *path) {
    if (!desktop_shell_assets_available()) return K_EAGAIN;
    uint32_t idle=DESKTOP_WALLPAPER_IDLE;
    if (!atomic_compare_exchange_strong(&g_wallpaper_request_state,&idle,DESKTOP_WALLPAPER_CLAIMED)) return K_EBUSY;
    size_t length=0;while (length<sizeof(g_wallpaper_request_path) && path[length]) ++length;
    if (!length || length==sizeof(g_wallpaper_request_path)) {
        atomic_store(&g_wallpaper_request_state,DESKTOP_WALLPAPER_IDLE);return K_EINVAL;
    }
    memcpy(g_wallpaper_request_path,path,length+1U);
    atomic_store_explicit(&g_wallpaper_request_state,DESKTOP_WALLPAPER_READY,memory_order_release);
    (void)wake_all(&g_desktop_asset_waitq);
    while (!desktop_wallpaper_done(0))
        (void)wait_on_queue_uninterruptible(&g_desktop_asset_waitq,desktop_wallpaper_done,0,UINT64_MAX);
    kstatus_t result=g_wallpaper_request_status;
    atomic_store_explicit(&g_wallpaper_request_state,DESKTOP_WALLPAPER_IDLE,memory_order_release);return result;
}

void desktop_shell_init(void) {
    wait_queue_init(&g_desktop_asset_waitq);
    atomic_init(&g_desktop_assets_available, false);
    g_desktop_wallpaper_asset = (desktop_asset_image_t){0};
    memset(g_desktop_app_assets, 0, sizeof(g_desktop_app_assets));
    memset(g_desktop_programs, 0, sizeof(g_desktop_programs));
}

bool desktop_shell_assets_available(void) {
    return atomic_load_explicit(&g_desktop_assets_available,
                                memory_order_acquire);
}

static bool desktop_load_png_asset(const char *path,
                                   desktop_asset_image_t *asset) {
    void *encoded = 0;
    size_t file_size = 0U;
    if (path == 0 || asset == 0 || asset->storage != 0) return false;
    if (vfs_load_file_kernel(path, 1U, DESKTOP_ASSET_MAX_BYTES,
                             &encoded, &file_size) != K_OK) return false;
    bool loaded = desktop_png_decode(encoded, file_size, asset);
    kfree(encoded);
    return loaded;
}

/* UI 冷加载只在资源线程执行；先验证整份文件，失败不发布半份启动路径。 */
static bool desktop_load_ui(void) {
    /* 仅支持启动时发布；运行中的不可变树不能被资源线程就地替换。 */
    if (g_desktop_document.nodes || g_window_server.display_width > INT32_MAX ||
        g_window_server.display_height > INT32_MAX) return false;
    void *source = 0;
    size_t size = 0U;
    if (vfs_load_file_kernel("/usr/apps/desktop/desktop.ui", 1U, UI_FILE_MAX_BYTES,
                             &source, &size) != K_OK) return false;
    ui_file_document_t document = {0};
    bool ok = ui_file_parse(source, size, &document, 0);
    kfree(source);
    if (!ok) return false;
    static const char *const names[] = {0, "fileman", "gshell", "notepad", "settings", "taskmgr", "chrome", "imageview"};
    desktop_app_binding_t bindings[8] = {0};
    uint32_t seen = 0;
    for (uint32_t i = 0; ok && i < document.resource_count; ++i) {
        const ui_file_resource_t *resource = document.resources + i;
        const os_control_node_t *node = document.nodes + resource->node_index;
        uint32_t app;
        for (app = 1; app < sizeof(names) / sizeof(names[0]); ++app)
            if (node->identifier == ui_file_identifier(names[app])) break;
        if (app == sizeof(names) / sizeof(names[0]) || (seen & (1U << app)) ||
            !node->text_length || node->text_length >= sizeof(bindings[app].title)) { ok = false; break; }
        ok = ui_file_resource_path(&document, resource, UI_RESOURCE_PROGRAM, bindings[app].path, sizeof(bindings[app].path)) &&
             ui_file_resource_path(&document, resource, UI_RESOURCE_ICON, bindings[app].icon, sizeof(bindings[app].icon));
        memcpy(bindings[app].title, document.text + node->text_offset, node->text_length);
        bindings[app].node_index = resource->node_index;
        document.nodes[resource->node_index].user_data = app;
        seen |= 1U << app;
    }
    ok = ok && seen == (1U << (sizeof(names)/sizeof(names[0]))) - 2U;
    if (ok) {
        control_tree_box_t *boxes = kmalloc((size_t)document.request.count * sizeof(*boxes), 0U);
        ok = boxes && control_tree_layout(document.nodes, document.request.count, document.request.text_size,
            (Rect){0, 0, (int32_t)g_window_server.display_width, (int32_t)g_window_server.display_height}, boxes);
        kfree(boxes);
    }
    if (ok) {
        memcpy(g_desktop_programs, bindings, sizeof(bindings));
        g_desktop_document = document;
        g_window_server.scene.controls = document.nodes;
        g_window_server.scene.control_count = document.request.count;
        return true;
    }
    ui_file_destroy(&document);
    return ok;
}

/* 仅清理尚未发布的启动资源，失败不能留下半份桌面或已解码的大图。 */
static void desktop_discard_assets(void) {
    kfree(g_desktop_wallpaper_asset.storage);
    g_desktop_wallpaper_asset=(desktop_asset_image_t){0};
    for(uint32_t app=1U;app<sizeof(g_desktop_app_assets)/sizeof(g_desktop_app_assets[0]);++app) {
        kfree(g_desktop_app_assets[app].storage);
        g_desktop_app_assets[app]=(desktop_asset_image_t){0};
    }
    ui_file_destroy(&g_desktop_document);
    g_window_server.scene.controls=0;g_window_server.scene.control_count=0;
    memset(g_desktop_programs,0,sizeof(g_desktop_programs));
}

static bool desktop_load_assets(void) {
    /* 小文件先校验，避免界面无效时仍读取并解码数兆字节壁纸。 */
    if (!desktop_load_ui()) return false;
    char wallpaper[256];
    bool complete = ui_file_wallpaper_path(&g_desktop_document,wallpaper,sizeof(wallpaper)) &&
        desktop_load_png_asset(wallpaper,&g_desktop_wallpaper_asset);
    for (uint32_t app = 1U; complete && app < sizeof(g_desktop_programs) / sizeof(g_desktop_programs[0]); ++app) {
        complete=desktop_load_png_asset(g_desktop_programs[app].icon, &g_desktop_app_assets[app]);
    }
    if(!complete)desktop_discard_assets();
    return complete;
}

static bool desktop_wallpaper_request_ready(void *context) {
    (void)context;return atomic_load_explicit(&g_wallpaper_request_state,memory_order_acquire)==DESKTOP_WALLPAPER_READY;
}

static kstatus_t desktop_wallpaper_replace(void) {
    if (desktop_wallpaper_pending()) return K_EBUSY;
    desktop_asset_image_t candidate={0};
    if (!desktop_load_png_asset(g_wallpaper_request_path,&candidate)) return K_EINVAL;
    uint32_t *cache=desktop_wallpaper_build_cache(&candidate);
    kfree(candidate.storage);
    if (!cache) return K_ENOMEM;
    void *source=0;size_t size=0;char *updated=0;size_t updated_size=0;
    kstatus_t status=vfs_load_file_kernel("/usr/apps/desktop/desktop.ui",1U,UI_FILE_MAX_BYTES,&source,&size);
    if (status==K_OK && !ui_file_set_wallpaper(source,size,g_wallpaper_request_path,&updated,&updated_size)) status=K_EINVAL;
    kfree(source);
    file_t *file=0;
    const char *temporary="/usr/apps/desktop/wallpaper.tmp";
    if (status==K_OK) status=vfs_open_kernel(temporary,VFS_OPEN_WRITE|VFS_OPEN_CREATE|VFS_OPEN_TRUNCATE,0,&file);
    bool temporary_created=status==K_OK;
    if (temporary_created) {
        uint64_t written=0;
        status=vfs_write_kernel(file,updated,updated_size,&written);
        if (status==K_OK && written!=updated_size) status=K_EIO;
        /* 数据确认写盘后才替换原配置，关闭句柄本身不保证回写完成。 */
        if (status==K_OK) status=vfs_fsync(file);
        vfs_close(file);
    }
    kfree(updated);
    if (status==K_OK) {
        status=vfs_rename_kernel(temporary,"/usr/apps/desktop/desktop.ui");
        if (status==K_OK) temporary_created=false;
    }
    /* 删除未发布的临时文件，保留最初的写入或重命名错误。 */
    if (temporary_created) (void)vfs_remove_kernel(temporary);
    if (status==K_OK) status=desktop_wallpaper_stage(cache);
    if (status!=K_OK) kfree(cache);
    return status;
}

/* 大体积图片读取留在资源线程，避免阻塞合成器所在的 CPU。 */
static void __attribute__((noreturn)) desktop_asset_worker_main(void *argument) {
    bool loaded;
    (void)argument;

    loaded = desktop_load_assets();
    if (loaded) {
        kernel_serial_write_serial_only("OS_DESKTOP_ASSETS_OK\r\n");
        atomic_store_explicit(&g_desktop_assets_available, true,
                              memory_order_release);
        if (!desktop_shell_build_cache()) {
            kernel_serial_write_serial_only(
                "OS_DESKTOP_CACHE_FAIL\r\n");
        } else {
            kernel_serial_write_serial_only(
                "OS_DESKTOP_CACHE_OK\r\n");
            window_lock();
            desktop_shell_publish_cache_locked();
            window_unlock();
            window_server_notify_worker();
        }
    } else {
        kernel_serial_write_serial_only("OS_DESKTOP_ASSETS_FAIL\r\n");
    }

    for (;;) {
        (void)wait_on_queue(&g_desktop_asset_waitq,desktop_wallpaper_request_ready,0,UINT64_MAX);
        uint32_t ready=DESKTOP_WALLPAPER_READY;
        if (!atomic_compare_exchange_strong(&g_wallpaper_request_state,&ready,DESKTOP_WALLPAPER_RUNNING)) continue;
        g_wallpaper_request_status=desktop_wallpaper_replace();
        atomic_store_explicit(&g_wallpaper_request_state,DESKTOP_WALLPAPER_DONE,memory_order_release);
        (void)wake_all(&g_desktop_asset_waitq);
    }
}

bool desktop_shell_start_asset_worker(uint32_t compositor_cpu) {
    uint32_t current_cpu = x86_current_cpu_index();
    uint32_t asset_cpu = UINT32_MAX;
    thread_t *worker;

    if (atomic_load_explicit(&g_window_server.desktop_asset_worker_started,
                             memory_order_acquire)) {
        return true;
    }

    if (current_cpu >= MAX_CPUS || sched_current_thread() == 0) return false;
    if (!window_worker_claim_start(&g_desktop_asset_worker_starting,
                                    &g_window_server.desktop_asset_worker_started))
        return atomic_load_explicit(&g_window_server.desktop_asset_worker_started,
                                    memory_order_acquire);

    /* 优先使用第三个 CPU，避免长时间 VFS 读取与合成器或引导线程竞争。 */
    for (uint32_t candidate = 0U;
         candidate < x86_smp_discovered_count() && candidate < MAX_CPUS;
         ++candidate) {
        if (candidate == current_cpu || candidate == compositor_cpu ||
            !x86_smp_cpu_online(candidate)) {
            continue;
        }
        asset_cpu = candidate;
        break;
    }
    if (asset_cpu == UINT32_MAX) {
        /* 无空闲 CPU 时仍需加载图像，在当前 CPU 上调度此任务。 */
        asset_cpu = current_cpu;
    }

    worker = &g_window_server.desktop_asset_worker;
    memset(worker, 0, sizeof(*worker));

    refcount_init(&worker->object.refs, 1U);
    worker->object.type = KOBJECT_TYPE_THREAD;
    worker->object.flags = 0U;
    worker->object.ops = 0;
    worker->tid = UINT64_MAX - 4ULL;
    worker->process = 0;
    sched_initialize_new_thread(worker, asset_cpu, true);
    atomic_store_explicit(&worker->state, THREAD_READY, memory_order_relaxed);
    worker->kernel_stack_base = g_desktop_asset_worker_stack;
    worker->kernel_stack_size = sizeof(g_desktop_asset_worker_stack);
    worker->kernel_stack_top =
        ((vaddr_t)(uintptr_t)g_desktop_asset_worker_stack +
         sizeof(g_desktop_asset_worker_stack)) & ~(vaddr_t)0x0FULL;
    list_init(&worker->process_node);
    list_init(&worker->owned_mutexes);

    x86_prepare_kernel_thread(&worker->arch, worker->kernel_stack_top,
                              desktop_asset_worker_main, 0);

    atomic_store_explicit(&g_window_server.desktop_asset_worker_started, true,
                          memory_order_release);
    if (!sched_enqueue_bootstrap(worker)) {
        atomic_store_explicit(&g_window_server.desktop_asset_worker_started, false,
                              memory_order_release);
        atomic_store_explicit(&g_desktop_asset_worker_starting, false,
                              memory_order_release);
        return false;
    }
    sched_monitor_register(worker, "desktop-assets");
    atomic_store_explicit(&g_desktop_asset_worker_starting, false, memory_order_release);
    return true;
}
