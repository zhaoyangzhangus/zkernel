#include <arch/x86_64/paging.h>
#include <arch/x86_64/cpu.h>
#include <kernel/console.h>
#include <kernel/frame_trace.h>
#include <kernel/kmem.h>
#include <kernel/string.h>
#include <kernel/perf.h>
#include <kernel/telemetry.h>
#include <kernel/vm.h>
#include "internal.h"
#include "../process/internal.h"

/* REFACTOR_P7A_WINDOW_OWNER：注册表生命周期、窗口移除及公共窗口 API。 */

void window_object_destroy(void *raw_object) {
    window_server_window_t *window = (window_server_window_t *)raw_object;
    if (window == 0) return;

    window_control_document_put(window->controls);
    caption_text_put(window->caption);

    if (window->compositor_cache != 0) {
        kfree(window->compositor_cache);
        window->compositor_cache = 0;
    }

    if (window->section != 0) {
        object_put(window->section);
        window->section = 0;
    }
    if (window->owner != 0) {
        object_put(window->owner);
        window->owner = 0;
    }
    kfree(window);
}

static void window_handle_close(void *object) {
    window_server_handle_closed((window_server_window_t *)object);
}

static bool window_is_signaled(const void *raw_object) {
    const window_server_window_t *window =
        (const window_server_window_t *)raw_object;
    bool signaled = false;
    if (window == 0 || !window_server_init()) return false;
    window_lock();
    signaled = window->event_count != 0U;
    signaled = signaled || atomic_load_explicit(&window->registry_removed,
                                               memory_order_acquire);
    window_unlock();
    /* 对 poll/select 用户而言，已移除窗口会持续保持 HUP 状态。
     * 等待者观察期间，fd 引用会使对象保持存活。 */
    return signaled;
}

static uint64_t window_tsc_to_us(uint64_t elapsed_tsc) {
    if (x86_boot_cpu_features.tsc_hz == 0U) return 0U;
    return x86_tsc_to_ns(elapsed_tsc) / 1000U;
}

typedef struct window_present_wait_context {
    window_server_window_t *window;
    uint64_t sequence;
} window_present_wait_context_t;

static bool window_present_sequence_ready(void *raw_context) {
    window_present_wait_context_t *context =
        (window_present_wait_context_t *)raw_context;

    if (context == 0 || context->window == 0) return false;
    return atomic_load_explicit(&context->window->registry_removed,
                                memory_order_acquire) || atomic_load_explicit(
               &context->window->compositor_presented_sequence,
               memory_order_acquire) >= context->sequence;
}

static kstatus_t window_present_wait_result(
    window_server_window_t *window, kstatus_t status) {
    if (status == K_OK && atomic_load_explicit(&window->registry_removed,
                                               memory_order_acquire)) {
        return K_ECANCELED;
    }
    return status;
}

static kstatus_t window_server_update_internal(
    process_t *process, uint32_t identifier, int32_t x, int32_t y,
    uint32_t width, uint32_t height, uint32_t flags,
    uint64_t *present_sequence);

uint64_t window_surface_page_span(uint32_t width, uint32_t height) {
    uint64_t pixels = (uint64_t)width * height;
    uint64_t bytes;

    if (width == 0U || height == 0U ||
        pixels > UINT64_MAX / sizeof(uint32_t)) return 0U;
    bytes = pixels * sizeof(uint32_t);
    if (bytes > UINT64_MAX - (PAGE_SIZE - 1ULL)) return 0U;
    return (bytes + PAGE_SIZE - 1ULL) & ~(PAGE_SIZE - 1ULL);
}

/* 窗口 surface 会在发布前完整分配后备页。客户端和合成器之后可访问任意像素，
 * 包括后续缩放才会显示的像素，而不会触发首次访问缺页。 */
static kstatus_t window_surface_allocate(shared_section_t *section,
                                         uint64_t bytes) {
    if (section == 0 || section->vm_object == 0 || bytes == 0U ||
        (bytes & (PAGE_SIZE - 1ULL)) != 0U) {
        return K_EINVAL;
    }

    for (uint64_t offset = 0U; offset < bytes; offset += PAGE_SIZE) {
        uint8_t *direct_page = 0;
        kstatus_t status = vm_object_shared_page_direct(
            section->vm_object, offset, true, &direct_page);
        if (status != K_OK) return status;
        (void)direct_page;
    }
    return K_OK;
}

/* 一次分配同时拥有两个槽位，因此缩放时无需延迟扩展任一槽位。失败时释放所有页面，
 * 不得发布后备存储不完整的 surface。 */
static kstatus_t window_surface_create_backing(uint64_t span, uint32_t slots,
                                               shared_section_t **out) {
    if (out == 0) return K_EINVAL;
    *out = 0;
    if (span == 0U || (span & (PAGE_SIZE - 1ULL)) != 0U ||
        (slots != 1U && slots != 2U) || span > UINT64_MAX / slots)
        return K_EINVAL;
    uint64_t bytes = span * slots;
    shared_section_t *section = 0;
    kstatus_t status = shared_section_create(bytes, &section);
    if (status != K_OK) return status;
    vm_object_mark_window_surface(section->vm_object);
    status = window_surface_allocate(section, bytes);
    if (status != K_OK) {
        object_put(section);
        return status;
    }
    *out = section;
    return K_OK;
}

/* 捕获操作固定准确的前台槽位，而不只是延长分配区生命周期。
 * 场景锁会将槽位发布与此读者获取操作串行化。 */
void window_surface_snapshot_capture_locked(compositor_window_view_t *view) {
    window_server_window_t *window = view->reference;
    view->surface_offset = 0U;
    view->surface_slot = 0U;
    view->surface_lease = false;
    view->surface_width = view->surface_height = 0U;
    if (window == 0 || !window->surface_exchange_enabled) return;
    view->buffer_size = window->surface_span;
    if (!window_surface_read_begin(&window->surface_exchange,
                                   &view->surface_slot)) {
        view->resize_pending = true;
        return;
    }
    view->surface_offset = view->surface_slot * window->surface_span;
    view->surface_width = window->surface_front_width;
    view->surface_height = window->surface_front_height;
    view->surface_lease = true;
}

/* 遮挡裁剪和常规帧完成都会在此释放租约。无锁唤醒期间保留引用，确保等待队列仍然有效。 */
void window_surface_snapshot_release(compositor_window_view_t *view) {
    window_server_window_t *window = view->reference;
    if (window == 0 || !view->surface_lease) return;
    window_lock();
    bool released = window_surface_read_end(&window->surface_exchange,
                                            view->surface_slot);
    bool writable = released &&
        window->surface_exchange.readers[view->surface_slot] == 0U &&
        view->surface_slot != window->surface_exchange.front &&
        !window->surface_exchange.closed;
    view->surface_lease = false;
    window_unlock();
    if (writable) (void)wake_all(&window->surface_waitq);
}

const object_ops_t g_window_object_ops = {
    .destroy = window_object_destroy,
    .handle_close = window_handle_close,
    .type_name = "Window",
    .is_signaled = window_is_signaled,
    .wait_value = 0,
};

void window_registry_reset_locked(void) {
    for (uint32_t index = 0U; index < g_window_server.scene.count; ++index) {
        window_server_window_t *window = g_window_server.scene.children[index];
        if (window != 0) window->scene_parent = 0;
    }
    g_window_server.scene.count = 0U;
    g_window_server.scene.hovered_window_identifier = 0U;
    g_window_server.scene.hovered_title_button = WINDOW_TITLE_BUTTON_NONE;
    g_window_server.next_identifier = 1U;

    for (uint32_t index = 0U;
         index < WINDOW_SERVER_MAX_WINDOWS;
         ++index) {
        g_window_server.scene.children[index] = 0;
    }
}

bool window_registry_append_locked(window_server_window_t *window) {
    uint32_t identifier;

    if (window == 0 || window->scene_parent != 0 ||
        g_window_server.scene.count >= WINDOW_SERVER_MAX_WINDOWS) {
        return false;
    }

    identifier = g_window_server.next_identifier++;
    if (identifier == 0U) {
        identifier = g_window_server.next_identifier++;
    }

    window->identifier = identifier;
    window->desktop_app = desktop_window_app_locked(window);
    window->scene_parent = &g_window_server.scene;
    g_window_server.scene.children[g_window_server.scene.count++] = window;
    desktop_chrome_window_opened_locked(window->owner);
    return true;
}

bool window_registry_remove_locked(window_server_window_t *window) {
    if (window == 0 || window->scene_parent != &g_window_server.scene) {
        return false;
    }

    for (uint32_t index = 0U;
         index < g_window_server.scene.count;
         ++index) {
        if (g_window_server.scene.children[index] != window) {
            continue;
        }

        for (uint32_t move = index + 1U;
             move < g_window_server.scene.count;
             ++move) {
            g_window_server.scene.children[move - 1U] =
                g_window_server.scene.children[move];
        }

        --g_window_server.scene.count;
        g_window_server.scene.children[g_window_server.scene.count] = 0;
        window->scene_parent = 0;
        return true;
    }

    return false;
}

void window_registry_move_to_front_locked(window_server_window_t *window) {
    uint32_t position;

    if (window == 0 || window->scene_parent != &g_window_server.scene) {
        return;
    }

    /* 目标已在末尾时直接返回，避免再次扫描整个场景。 */
    if (g_window_server.scene.count == 0U ||
        g_window_server.scene.children[g_window_server.scene.count - 1U] == window) return;

    for (position = 0U;
         position < g_window_server.scene.count;
         ++position) {
        if (g_window_server.scene.children[position] == window) {
            break;
        }
    }

    if (position >= g_window_server.scene.count ||
        position + 1U >= g_window_server.scene.count) {
        return;
    }

    for (uint32_t index = position + 1U;
         index < g_window_server.scene.count;
         ++index) {
        g_window_server.scene.children[index - 1U] =
            g_window_server.scene.children[index];
    }

    g_window_server.scene.children[g_window_server.scene.count - 1U] = window;
}

static bool window_lifecycle_test_fail(uint32_t step, kstatus_t status) {
    uint32_t code = status < 0 ? (uint32_t)(-status) : (uint32_t)status;
    kernel_serial_write("OS_WINDOW_TEST_FAIL STEP=");
    kernel_serial_write_u32(step);
    kernel_serial_write(" STATUS=");
    kernel_serial_write_u32(code);
    kernel_serial_write("\r\n");
    return false;
}

/* REFACTOR_P7A_WINDOW_TEST_OWNER: canonical lifecycle and registry coverage. */
bool window_lifecycle_self_test(void) {
    process_t *owner = 0;
    window_server_window_t *first = 0;
    window_server_window_t *second = 0;
    window_server_snapshot_t snapshot = {0};
    uint64_t first_address = 0U;
    uint64_t second_address = 0U;
    uint64_t initial_pages;
    uint64_t expected_surface_pages;
    uint64_t resize_pages_before;
    uint64_t resize_pages_after;
    uint32_t initial_count = 0U;
    uint32_t dirty_rects;
    uint64_t benchmark_start;
    input_event_t pointer_event = {0};
    kstatus_t status;
    bool success = false;
    bool count_restored = false;

    if (process_create(0, &owner) != K_OK) {
        success = window_lifecycle_test_fail(1U, K_EIO);
        goto cleanup;
    }
    if (!window_server_kernel_ready()) {
        success = window_lifecycle_test_fail(2U, K_EIO);
        goto cleanup;
    }

    window_lock();
    initial_count = g_window_server.scene.count;
    window_unlock();

    if (!compositor_copy_self_test()) {
        success = window_lifecycle_test_fail(3U, K_EIO);
        goto cleanup;
    }

    compositor_snapshot_test_clear_damage_tiles();
    if (!compositor_snapshot_test_set_damage_tile(0U)) {
        success = window_lifecycle_test_fail(4U, K_EIO);
        goto cleanup;
    }
    benchmark_start = telemetry_timestamp();
    dirty_rects = compositor_snapshot_tiles_to_rects(
        &g_compositor_snapshot,
        WINDOW_DAMAGE_MAX_SNAPSHOT_RECTS);
    kernel_perf_emit_scope("graphics.single_dirty_tile", benchmark_start);
    kernel_perf_emit_value("graphics.dirty_tiles_frame", dirty_rects);
    compositor_snapshot_test_clear_damage_tiles();
    if (dirty_rects != 1U) {
        success = window_lifecycle_test_fail(4U, K_EIO);
        goto cleanup;
    }

    benchmark_start = telemetry_timestamp();
    if (window_server_create(owner, 8, 8, 96U, 56U,
                             OS_WINDOW_VISIBLE, 0x00102030U,
                             "back", &first) != K_OK) {
        success = window_lifecycle_test_fail(3U, K_EIO);
        goto cleanup;
    }
    if (window_server_map(first, owner, 0U, &first_address) != K_OK ||
        window_server_set_owner_address(first, first_address) != K_OK ||
        window_server_set(first, 8, 8, 1U) != K_OK) {
        success = window_lifecycle_test_fail(3U, K_EIO);
        goto cleanup;
    }
    kernel_perf_emit_value("window_create_us",
                           window_tsc_to_us(telemetry_timestamp() -
                                            benchmark_start));
    initial_pages = vm_object_populated_pages(first->section->vm_object);
    if (initial_pages != window_surface_page_span(96U, 56U) / PAGE_SIZE) {
        success = window_lifecycle_test_fail(3U, K_EIO);
        goto cleanup;
    }
    benchmark_start = telemetry_timestamp();
    compositor_frame_run();
    kernel_perf_emit_scope("graphics.single_window_compose", benchmark_start);

    benchmark_start = telemetry_timestamp();
    if (window_server_create(owner, 48, 24, 96U, 56U,
                             OS_WINDOW_VISIBLE | OS_WINDOW_RESIZABLE,
                             0x00203040U,
                             "front", &second) != K_OK) {
        success = window_lifecycle_test_fail(4U, K_EIO);
        goto cleanup;
    }
    expected_surface_pages = second->buffer_size / PAGE_SIZE;
    if (expected_surface_pages == 0U ||
        vm_object_populated_pages(second->section->vm_object) !=
            expected_surface_pages) {
        success = window_lifecycle_test_fail(4U, K_EIO);
        goto cleanup;
    }
    if (window_server_map(second, owner, 0U, &second_address) != K_OK ||
        window_server_set_owner_address(second, second_address) != K_OK ||
        window_server_set(second, 48, 24, 1U) != K_OK) {
        success = window_lifecycle_test_fail(4U, K_EIO);
        goto cleanup;
    }
    kernel_perf_emit_value("window_create_us",
                           window_tsc_to_us(telemetry_timestamp() -
                                            benchmark_start));
    second->surface_metrics_reported = true;

    benchmark_start = telemetry_timestamp();
    compositor_frame_run();
    kernel_perf_emit_scope("graphics.overlap_2", benchmark_start);

    if (window_server_snapshot(initial_count, &snapshot) != K_OK ||
        snapshot.identifier != first->identifier ||
        snapshot.owner_pid != (uint32_t)owner->pid ||
        snapshot.x != 8 || snapshot.y != 8 ||
        snapshot.width != 96U || snapshot.height != 56U ||
        snapshot.visible == 0U || snapshot.title[0] != 'b') {
        success = window_lifecycle_test_fail(5U, K_EIO);
        goto cleanup;
    }
    if (window_server_snapshot(initial_count + 1U, &snapshot) != K_OK ||
        snapshot.identifier != second->identifier ||
        snapshot.focused == 0U || snapshot.title[0] != 'f') {
        success = window_lifecycle_test_fail(6U, K_EIO);
        goto cleanup;
    }
    if (window_server_focus(first->identifier) != K_OK) {
        success = window_lifecycle_test_fail(7U, K_EIO);
        goto cleanup;
    }
    benchmark_start = telemetry_timestamp();
    status = window_server_set(first, 21, 17, 1U);
    kernel_perf_emit_scope("graphics.window_move_latency", benchmark_start);
    if (status != K_OK) {
        success = window_lifecycle_test_fail(8U, K_EIO);
        goto cleanup;
    }
    if (window_server_update(owner, first->identifier, 21, 17,
                             96U, 56U, 0U) != K_OK) {
        success = window_lifecycle_test_fail(9U, K_EIO);
        goto cleanup;
    }
    resize_pages_before = vm_object_populated_pages(second->section->vm_object);
    window_lock();
    g_window_server.resize_edges = WINDOW_RESIZE_RIGHT | WINDOW_RESIZE_BOTTOM;
    bool resized = window_resize_locked(second, 200, 100);
    g_window_server.resize_edges = 0U;
    window_unlock();
    if (!resized) {
        success = window_lifecycle_test_fail(10U, K_EIO);
        goto cleanup;
    }
    resize_pages_after = vm_object_populated_pages(second->section->vm_object);
    if (resize_pages_after != resize_pages_before) {
        success = window_lifecycle_test_fail(10U, K_EBUSY);
        goto cleanup;
    }
    status = window_server_update(owner, second->identifier, 0, 0,
                                  second->width, second->height, 0U);
    if (status != K_OK) {
        success = window_lifecycle_test_fail(10U, status);
        goto cleanup;
    }
    kernel_perf_emit_value("window_test_initial_pages", initial_pages);
    kernel_perf_emit_value("window_test_resize_new_pages",
                           resize_pages_after - resize_pages_before);
    status = window_server_snapshot(initial_count + 1U, &snapshot);
    if (status != K_OK || snapshot.identifier != first->identifier ||
        snapshot.x != 21 || snapshot.y != 17 ||
        snapshot.focused == 0U) {
        success = window_lifecycle_test_fail(10U, K_EIO);
        goto cleanup;
    }

    pointer_event.device_id = 0xC0DEU;
    pointer_event.type = INPUT_EVENT_POINTER;
    pointer_event.code = 0U;
    pointer_event.value2 = 1;
    pointer_event.value3 = 0;
    pointer_event.value4 = 0;
    benchmark_start = telemetry_timestamp();
    status = input_core_push(&pointer_event);
    if (status != K_OK) {
        success = window_lifecycle_test_fail(13U, status);
        goto cleanup;
    }
    window_server_pump_input_mode(true);
    kernel_perf_emit_scope("graphics.input_present", benchmark_start);

    if (window_server_set(second, 48, 24, 0U) != K_OK) {
        success = window_lifecycle_test_fail(11U, K_EIO);
        goto cleanup;
    }
    if (window_server_snapshot(initial_count, &snapshot) != K_OK ||
        snapshot.identifier != second->identifier ||
        snapshot.visible != 0U) {
        success = window_lifecycle_test_fail(12U, K_EIO);
        goto cleanup;
    }

    success = true;

cleanup:
    if (second != 0) {
        window_server_handle_closed(second);
        window_server_put(second);
    }
    if (first != 0) {
        window_server_handle_closed(first);
        window_server_put(first);
    }
    if (owner != 0) {
        window_lock();
        count_restored = g_window_server.scene.count == initial_count;
        window_unlock();
        (void)process_abort(owner);
        object_put(owner);
    }
    if (!count_restored) {
        (void)window_lifecycle_test_fail(13U, K_EIO);
        return false;
    }
    return success;
}

void remove_window_locked(window_server_window_t *window) {
    window_server_window_t *new_focused = 0;

    if (window == 0 ||
        !window_registry_remove_locked(window)) {
        return;
    }

    /* 在关闭路径唤醒 event_waitq 前先发布状态。已移除的表面不可能再获得后续显示栅栏；
     * 取消操作不等同于确认。 */
    window_animation_depart_locked(window, false);
    atomic_store_explicit(&window->registry_removed, true, memory_order_release);
    window_surface_exchange_close(&window->surface_exchange);
    desktop_chrome_window_closed_locked(window->owner);

    window_mark_window_locked(window);

    if (g_window_server.focused_identifier == window->identifier) {
        window_scene_set_focus_identifier_locked(0U);
        for (uint32_t j = g_window_server.scene.count; j != 0U; --j) {
            window_server_window_t *candidate =
                g_window_server.scene.children[j - 1U];
            if (!candidate->minimized && !candidate->client_show_inactive &&
                (candidate->flags & OS_WINDOW_VISIBLE) != 0U) {
                window_scene_set_focus_identifier_locked(candidate->identifier);
                new_focused = candidate;
                break;
            }
        }
    }

    if (g_window_server.title_pressed_identifier ==
        window->identifier) {
        window_clear_title_capture_locked();
    }

    if (g_window_server.dragging_identifier == window->identifier) {
        window_input_clear_drag_locked();
    }

    window_mark_window_locked(new_focused);
    object_put(window); /* 注册表引用 */
}


kstatus_t window_server_register_manager(process_t *process) {
    (void)process;
    /* 合成和输入路由已经属于 Ring0，用户进程不能接管窗口服务器。 */
    return window_server_kernel_ready() ? K_EPERM : K_EIO;
}

bool window_server_is_manager(process_t *process) {
    (void)process;
    return false;
}

kstatus_t window_server_create(process_t *owner, int32_t x, int32_t y,
                               uint32_t width, uint32_t height,
                               uint32_t flags, uint32_t background,
                               const char *title, window_server_window_t **out) {
    uint64_t pixels;
    uint64_t bytes;
#if OS_ENABLE_FRAME_TRACE
    uint64_t create_tsc = telemetry_timestamp();
#endif
    shared_section_t *section = 0;
    window_server_window_t *window;
    window_server_window_t *old_focused;

    if (out == 0 || owner == 0 || !window_server_init() || width == 0U ||
        height == 0U || width > UINT32_MAX / height ||
        (flags & ~(OS_WINDOW_VISIBLE | OS_WINDOW_RESIZABLE |
                   OS_WINDOW_CLIENT_DECORATIONS |
                   OS_WINDOW_PREMULTIPLIED_ALPHA |
                   OS_WINDOW_STANDARD_CAPTION_CONTROLS |
                   OS_WINDOW_SURFACE_EXCHANGE |
                   OS_WINDOW_DARK_CAPTION |
                   OS_WINDOW_EXPLICIT_MOVE_RESIZE |
                   OS_WINDOW_CAPTION_HEIGHT_MASK)) != 0U ||
        !os_window_caption_create_valid(flags, height)) {
        return K_EINVAL;
    }

    /* 可调整大小的窗口使用足以覆盖任意屏幕尺寸的稳定表面。在发布窗口前分配完整后备区，
     * 使后续呈现和调整大小时无需再触发新页面缺页。 */
    if ((flags & OS_WINDOW_RESIZABLE) != 0U) {
        if (!window_server_kernel_ready()) return K_EIO;
        if (width > g_window_server.display_width ||
            height > g_window_server.display_height) {
            return K_EINVAL;
        }
        pixels = (uint64_t)g_window_server.display_width *
                 g_window_server.display_height;
    } else {
        pixels = (uint64_t)width * height;
    }

    if (pixels > UINT64_MAX / sizeof(uint32_t)) return K_EINVAL;
    bytes = pixels * sizeof(uint32_t);
    if (bytes > UINT64_MAX - (PAGE_SIZE - 1U)) return K_EINVAL;
    bytes = (bytes + PAGE_SIZE - 1U) &
            ~(uint64_t)(PAGE_SIZE - 1U);

    uint32_t slots = (flags & OS_WINDOW_SURFACE_EXCHANGE) != 0U ? 2U : 1U;
    kstatus_t allocation_status = window_surface_create_backing(bytes, slots, &section);
    if (allocation_status != K_OK) return allocation_status;
    window = (window_server_window_t *)kzalloc(sizeof(*window), 0);
    if (window == 0) {
        object_put(section);
        return K_ENOMEM;
    }
    object_header_init(&window->object, KOBJECT_TYPE_WINDOW,
                       &g_window_object_ops);
    window->owner = owner;
    object_get(owner);
    window->section = section;
    window->x = x;
    window->y = y;
    window->width = width;
    window->height = height;
    /* 仅在调用方映射已分配后备区的表面后发布窗口。 */
    window->flags = flags & ~OS_WINDOW_VISIBLE;
    window->background = background;
    window->buffer_size = bytes * slots;
    window->surface_span = bytes;
    window->surface_exchange_enabled = slots == 2U;
    wait_queue_init(&window->surface_waitq);
    window->owner_address = 0U;
#if OS_ENABLE_FRAME_TRACE
    window->create_tsc = create_tsc;
    window->surface_fault_count_at_create =
        vm_object_fault_count(section->vm_object);
#endif
    window->surface_populated_bytes = bytes * slots;
    window->surface_populate_pending_end = 0U;
    window->surface_metrics_reported = false;
    window->present_sequence = 0U;
    atomic_init(&window->compositor_presented_sequence, 0U);
    atomic_init(&window->registry_removed, false);
    window->compositor_cache = 0;
    window->compositor_presented_x = x;
    window->compositor_presented_y = y;
    window->compositor_presented_width = 0U;
    window->compositor_presented_height = 0U;
    window->compositor_presented_valid = false;
    window->dirty = true;
    window->resize_pending = false;
    window->maximized = false;
    window->minimized = false;
    window->restore_x = x;
    window->restore_y = y;
    window->restore_width = width;
    window->restore_height = height;
    window->event_read = 0U;
    window->event_write = 0U;
    window->event_count = 0U;
    window->event_wake_pending = false;
    wait_queue_init(&window->event_waitq);
    for (uint32_t i = 0U; i + 1U < sizeof(window->title); ++i) {
        window->title[i] = title != 0 ? title[i] : '\0';
        if (window->title[i] == '\0') break;
    }
    window->title[sizeof(window->title) - 1U] = '\0';
    window->caption=caption_text_create(window->title,caption_text_height(flags));
    if (!window->caption) {object_put(window);return K_ENOMEM;}
    window_lock();
    old_focused = window_scene_find_locked(g_window_server.focused_identifier);
    if (!window_registry_append_locked(window)) {
        window_unlock();
        object_put(window);
        return K_ENOMEM;
    }
    object_get(window); /* 注册表引用 */
    window_mark_window_locked(old_focused);
    window_mark_window_locked(window);
    window_unlock();
    window_server_notify_worker();
    *out = window;
    return K_OK;
}

kstatus_t window_server_lookup(uint32_t identifier, window_server_window_t **out) {
    window_server_window_t *window;
    if (out == 0 || !window_server_init()) return K_EINVAL;
    window_lock();
    window = window_scene_find_locked(identifier);
    if (window == 0 || !object_try_get(window)) {
        window_unlock();
        return K_ENOENT;
    }
    window_unlock();
    *out = window;
    return K_OK;
}

kstatus_t window_server_lookup_owner(process_t *owner,
                                     window_server_window_t **out) {
    if (owner == 0 || out == 0 || !window_server_init()) return K_EINVAL;
    window_lock();
    for (uint32_t index = 0U; index < g_window_server.scene.count; ++index) {
        window_server_window_t *window = g_window_server.scene.children[index];
        if (window != 0 && window->owner == owner && object_try_get(window)) {
            *out = window;
            window_unlock();
            return K_OK;
        }
    }
    window_unlock();
    return K_ENOENT;
}

uint32_t window_server_window_identifier(
    const window_server_window_t *window) {
    return window != 0 ? window->identifier : 0U;
}

uint64_t window_server_window_buffer_size(
    const window_server_window_t *window) {
    return window != 0 ? window->buffer_size : 0U;
}

void window_server_put(window_server_window_t *window) {
    object_put(window);
}

void window_server_handle_closed(window_server_window_t *window) {
    bool referenced = false;
    if (window == 0 || !window_server_init()) return;
    window_lock();
    object_get(window);
    referenced = true;
    remove_window_locked(window);
    window_unlock();
    window_server_notify_worker();
    (void)wake_all(&window->event_waitq);
    (void)wake_all(&window->surface_waitq);
    object_notify_signaled(window);
    if (referenced) object_put(window);
}

void window_server_close_process(process_t *owner) {
    window_server_window_t *removed[WINDOW_SERVER_MAX_WINDOWS];
    uint32_t removed_count = 0U;
    bool relaunch_ready;
    if (owner == 0 || !window_server_init()) return;
    window_lock();
    for (uint32_t index = 0U; index < g_window_server.scene.count;) {
        window_server_window_t *window = g_window_server.scene.children[index];
        if (window == 0 || window->owner != owner) {
            ++index;
            continue;
        }
        /* remove_window_locked() 会压缩数组，因此保留当前索引。 */
        object_get(window);
        remove_window_locked(window);
        if (removed_count < WINDOW_SERVER_MAX_WINDOWS) {
            removed[removed_count++] = window;
        } else {
            object_put(window);
        }
    }
    /* 仅释放此 PID 所拥有的槽位。较早 Chrome 实例延迟到达的拆除回调不得清除较新启动实例
     * 的保护状态。 */
    relaunch_ready = desktop_chrome_process_released_locked(owner->pid);
    window_unlock();
    if (relaunch_ready) (void)wake_one(&g_window_server.desktop_launch_waitq);
    for (uint32_t index = 0U; index < removed_count; ++index) {
        (void)wake_all(&removed[index]->event_waitq);
        (void)wake_all(&removed[index]->surface_waitq);
        object_notify_signaled(removed[index]);
        object_put(removed[index]);
    }
    if (removed_count != 0U) window_server_notify_worker();
}

kstatus_t window_server_set_owner_address(window_server_window_t *window,
                                          uint64_t address) {
    if (window == 0 || address == 0U || (address & (PAGE_SIZE - 1U)) != 0U) {
        return K_EINVAL;
    }
    window_lock();
    window->owner_address = address;
    window_mark_window_locked(window);
    window_unlock();
    window_server_notify_worker();
    return K_OK;
}

static bool window_server_owner_locked(process_t *owner,
                                       window_server_window_t *window) {
    return owner != 0 && window != 0 && window->owner == owner &&
           window->identifier != 0U &&
           window_scene_find_locked(window->identifier) == window;
}

typedef struct window_surface_wait_context {
    process_t *owner;
    window_server_window_t *window;
    os_surface_frame_t frame;
    kstatus_t status;
    bool acquired;
} window_surface_wait_context_t;

static bool window_surface_writer_ready(void *raw) {
    window_surface_wait_context_t *context = raw;
    window_server_window_t *window = context->window;
    if (context->acquired) return true;
    window_lock();
    if (atomic_load_explicit(&window->registry_removed, memory_order_acquire)) {
        context->status = K_ECANCELED;
    } else if (!window_server_owner_locked(context->owner, window)) {
        context->status = K_EPERM;
    } else if (!window->surface_exchange_enabled || !window->owner_address ||
               window->surface_exchange.serial == UINT64_MAX ||
               !window->width || !window->height ||
               window->width > UINT32_MAX / sizeof(uint32_t) ||
               window->surface_span > UINT64_MAX / 2U ||
               window->buffer_size != 2U * window->surface_span ||
               window->owner_address > UINT64_MAX - window->buffer_size ||
               window->height > window->surface_span /
                   ((uint64_t)window->width * sizeof(uint32_t))) {
        context->status = K_EINVAL;
    } else {
        uint32_t slot;
        uint64_t token;
        if (window_surface_write_begin(&window->surface_exchange, &slot, &token)) {
            window->surface_writer_width = window->width;
            window->surface_writer_height = window->height;
            context->frame = (os_surface_frame_t){
                .token = token,
                .address = window->owner_address + slot * window->surface_span,
                .capacity = window->surface_span,
                .width = window->width,
                .height = window->height,
                .stride = window->width * sizeof(uint32_t),
                .damage_width = window->width,
                .damage_height = window->height,
            };
            context->acquired = true;
        }
    }
    bool ready = context->acquired || context->status != K_OK;
    window_unlock();
    return ready;
}

kstatus_t window_server_surface_begin(process_t *owner,
    window_server_window_t *window, os_surface_frame_t *frame) {
    if (!owner || !window || !frame) return K_EINVAL;
    *frame = (os_surface_frame_t){0};
    window_surface_wait_context_t context = {.owner = owner, .window = window};
    kstatus_t status = wait_on_queue(&window->surface_waitq,
        window_surface_writer_ready, &context, 5000000000ULL);
    if (status == K_OK) status = context.status;
    window_lock();
    if (status == K_OK && window->surface_exchange.closed) status = K_ECANCELED;
    if (status != K_OK && context.acquired) {
        (void)window_surface_write_end(&window->surface_exchange,
                                      context.frame.token, false);
    }
    if (status == K_OK) *frame = context.frame;
    window_unlock();
    if (status != K_OK && context.acquired)
        (void)wake_all(&window->surface_waitq);
    return status;
}

kstatus_t window_server_surface_end(process_t *owner,
    window_server_window_t *window, const os_surface_frame_t *frame, bool commit) {
    if (!owner || !window || !frame || !frame->token || frame->reserved)
        return K_EINVAL;
    window_lock();
    if (!window_server_owner_locked(owner, window)) {
        window_unlock();
        return K_EPERM;
    }
    if (!window->surface_exchange_enabled || !window->surface_exchange.writing ||
        frame->token != window->surface_exchange.writer_token) {
        window_unlock();
        return K_EINVAL;
    }
    kstatus_t status = K_OK;
    if (commit && (frame->address != window->owner_address +
            window->surface_exchange.writer_slot * window->surface_span ||
        frame->capacity != window->surface_span ||
        frame->width != window->surface_writer_width ||
        frame->height != window->surface_writer_height ||
        frame->stride != frame->width * sizeof(uint32_t) ||
        frame->damage_x < 0 || frame->damage_y < 0 ||
        frame->damage_width == 0U || frame->damage_height == 0U ||
        (uint64_t)(uint32_t)frame->damage_x + frame->damage_width > frame->width ||
        (uint64_t)(uint32_t)frame->damage_y + frame->damage_height > frame->height)) {
        window_unlock();
        return K_EINVAL; // 提交无效时保留租约，等待显式取消。
    }
    if (commit && (window->width != frame->width || window->height != frame->height))
        status = K_EAGAIN; // 调整大小与写入发生竞争；保留旧的完整帧。
    if (commit && window->present_sequence == UINT64_MAX) status = K_EINVAL;
    bool publish = commit && status == K_OK;
    (void)window_surface_write_end(&window->surface_exchange, frame->token, publish);
    bool show = publish && (window->flags & OS_WINDOW_VISIBLE) == 0U;
    int32_t x = window->x, y = window->y;
    if (publish) {
        bool full_damage = show || window->resize_pending ||
            window->surface_front_width != frame->width ||
            window->surface_front_height != frame->height;
        int32_t damage_x = full_damage ? 0 : frame->damage_x;
        int32_t damage_y = full_damage ? 0 : frame->damage_y;
        uint32_t damage_width = full_damage ? frame->width : frame->damage_width;
        uint32_t damage_height = full_damage ? frame->height : frame->damage_height;
        ++window->present_sequence;
        window->surface_front_width = frame->width;
        window->surface_front_height = frame->height;
        window->resize_pending = false;
        window->dirty = true;
        if (window->controls)
            window_control_surface_damage_locked(window, damage_x, damage_y,
                damage_width, damage_height);
        else window_mark_surface_locked(window, damage_x, damage_y,
                damage_width, damage_height);
    }
    window_unlock();
    if (!publish) (void)wake_all(&window->surface_waitq);
    if (publish) window_server_notify_worker();
    if (show) status = window_server_set(window, x, y, 1U);
    return status;
}

kstatus_t window_server_geometry_owned(process_t *owner,
    window_server_window_t *window, int32_t *x, int32_t *y,
    uint32_t *width, uint32_t *height, uint32_t *maximized) {
    if (!owner || !window || !x || !y || !width || !height || !maximized ||
        !window_server_kernel_ready()) return K_EINVAL;
    window_lock();
    if (!window_server_owner_locked(owner, window)) {
        window_unlock();
        return K_EPERM;
    }
    *x = window->x + (int32_t)window_client_offset_x(window->flags);
    *y = window->y + (int32_t)window_client_offset_y(window->flags);
    *width = window->width;
    *height = window->height;
    *maximized = (window->maximized ? 1U : 0U) | (window->fullscreen ? 2U : 0U);
    window_unlock();
    return K_OK;
}

kstatus_t window_server_set_fullscreen_owned(process_t *owner,
    window_server_window_t *window, uint32_t fullscreen) {
    if (!owner || !window || fullscreen > 1U || !window_server_kernel_ready())
        return K_EINVAL;
    window_lock();
    if (!window_server_owner_locked(owner, window)) {
        window_unlock();
        return K_EPERM;
    }
    if (window->fullscreen == (fullscreen != 0U)) {
        window_unlock();
        return K_OK;
    }
    uint32_t width = fullscreen ? g_window_server.display_width :
        window->fullscreen_restore_width;
    uint32_t height = fullscreen ? g_window_server.display_height :
        window->fullscreen_restore_height;
    if (!width || !height || (uint64_t)width * height >
        (window->surface_exchange_enabled ? window->surface_span :
         window->buffer_size) / sizeof(uint32_t)) {
        window_unlock();
        return K_EINVAL;
    }
    window_mark_window_locked(window);
    if (g_window_server.title_pressed_identifier == window->identifier)
        window_clear_title_capture_locked();
    if (g_window_server.dragging_identifier == window->identifier)
        window_input_clear_drag_locked();
    if (fullscreen) {
        window->fullscreen_restore_x = window->x;
        window->fullscreen_restore_y = window->y;
        window->fullscreen_restore_width = window->width;
        window->fullscreen_restore_height = window->height;
        window->fullscreen_restore_flags = window->flags;
        window->x = 0;
        window->y = 0;
        window->flags = (window->flags & ~OS_WINDOW_STANDARD_CAPTION_CONTROLS) |
            OS_WINDOW_CLIENT_DECORATIONS;
    } else {
        window->x = window->fullscreen_restore_x;
        window->y = window->fullscreen_restore_y;
        window->flags = (window->fullscreen_restore_flags & ~OS_WINDOW_VISIBLE) |
            (window->flags & OS_WINDOW_VISIBLE);
    }
    window->width = width;
    window->height = height;
    window->fullscreen = fullscreen != 0U;
    window->dirty = true;
    window->resize_pending = true;
    window_mark_window_locked(window);
    window_enqueue_resize_event_locked(window);
    window_input_refresh_pointer_locked();
    window_unlock();
    window_server_notify_worker();
    return K_OK;
}

kstatus_t window_server_begin_interactive_owned(process_t *owner,
    window_server_window_t *window, uint32_t edges) {
    const uint32_t horizontal = WINDOW_RESIZE_LEFT | WINDOW_RESIZE_RIGHT;
    const uint32_t vertical = WINDOW_RESIZE_TOP | WINDOW_RESIZE_BOTTOM;
    if (!owner || !window || !window_server_kernel_ready() ||
        (edges & ~(horizontal | vertical)) != 0U ||
        (edges & horizontal) == horizontal || (edges & vertical) == vertical)
        return K_EINVAL;
    window_lock();
    if (!window_server_owner_locked(owner, window)) {
        window_unlock();
        return K_EPERM;
    }
    if ((window->flags & (OS_WINDOW_VISIBLE | OS_WINDOW_EXPLICIT_MOVE_RESIZE)) !=
            (OS_WINDOW_VISIBLE | OS_WINDOW_EXPLICIT_MOVE_RESIZE) ||
        !window->client_button_capture || window->client_hidden ||
        window->minimized || window->maximized || window->fullscreen ||
        (edges && !(window->flags & OS_WINDOW_RESIZABLE))) {
        window_unlock();
        return K_EINVAL;
    }
    if (g_window_server.dragging_identifier != 0U) {
        window_unlock();
        return K_EBUSY;
    }
    for (uint32_t i = 0; i < g_window_server.scene.count; ++i) {
        window_server_window_t *other = g_window_server.scene.children[i];
        if (other && other != window &&
            (other->client_pointer_capture || other->client_button_capture)) {
            window_unlock();
            return K_EBUSY;
        }
    }
    window_clear_title_capture_locked();
    window->client_button_capture = false;
    window->client_pointer_capture = false;
    window_input_begin_drag_locked(window->identifier,
        edges ? 0 : (int32_t)g_window_server.pointer_x - window->x,
        edges ? 0 : (int32_t)g_window_server.pointer_y - window->y, edges);
    window_mark_moved_cursor_locked(g_window_server.pointer_x,
        g_window_server.pointer_y, g_window_server.pointer_x, g_window_server.pointer_y);
    window_unlock();
    window_server_notify_worker();
    return K_OK;
}

kstatus_t window_server_set_decorations_owned(process_t *owner,
    window_server_window_t *window, uint32_t server_frame) {
    if (!owner || !window || server_frame > 1U || !window_server_kernel_ready())
        return K_EINVAL;
    window_lock();
    if (!window_server_owner_locked(owner, window)) {
        window_unlock();
        return K_EPERM;
    }
    uint32_t old_flags = window->flags;
    if (window->fullscreen) { window_unlock(); return K_EINVAL; }
    uint32_t flags = old_flags & ~(OS_WINDOW_CLIENT_DECORATIONS |
                                   OS_WINDOW_STANDARD_CAPTION_CONTROLS);
    flags |= server_frame ? OS_WINDOW_STANDARD_CAPTION_CONTROLS : OS_WINDOW_CLIENT_DECORATIONS;
    if (flags == old_flags) { window_unlock(); return K_OK; }
    uint32_t outer_width = window_outer_width(window->width, old_flags);
    uint32_t outer_height = window_outer_height(window->height, old_flags);
    uint32_t extra_x = window_frame_extra(flags);
    uint32_t extra_y = extra_x + window_titlebar_height(flags);
    uint32_t restore_width = window->restore_width;
    uint32_t restore_height = window->restore_height;
    if (outer_width <= extra_x || outer_height <= extra_y ||
        (server_frame && (outer_width - extra_x < WINDOW_MIN_WIDTH ||
                          outer_height - extra_y < WINDOW_MIN_HEIGHT)) ||
        (uint64_t)(outer_width - extra_x) * (outer_height - extra_y) >
            (window->surface_exchange_enabled ? window->surface_span :
             window->buffer_size) / sizeof(uint32_t)) {
        window_unlock();
        return K_EINVAL;
    }
    if (window->maximized) {
        uint32_t rw = window_outer_width(restore_width, old_flags);
        uint32_t rh = window_outer_height(restore_height, old_flags);
        if (rw <= extra_x || rh <= extra_y ||
            (server_frame && (rw - extra_x < WINDOW_MIN_WIDTH ||
                              rh - extra_y < WINDOW_MIN_HEIGHT))) {
            window_unlock();
            return K_EINVAL;
        }
        restore_width = rw - extra_x;
        restore_height = rh - extra_y;
    }
    window_mark_window_locked(window);
    if (g_window_server.title_pressed_identifier == window->identifier)
        window_clear_title_capture_locked();
    if (g_window_server.dragging_identifier == window->identifier)
        window_input_clear_drag_locked();
    window->flags = flags;
    window->width = outer_width - extra_x;
    window->height = outer_height - extra_y;
    window->restore_width = restore_width;
    window->restore_height = restore_height;
    window->dirty = true;
    window->resize_pending = true;
    window_mark_window_locked(window);
    window_enqueue_resize_event_locked(window);
    window_unlock();
    window_server_notify_worker();
    return K_OK;
}

kstatus_t window_server_present(process_t *owner,
                                window_server_window_t *window,
                                uint64_t address, uint64_t length,
                                uint32_t stride, int32_t damage_x,
                                int32_t damage_y, uint32_t damage_width,
                                uint32_t damage_height) {
    uint64_t row_bytes;
    uint64_t required_bytes;
    uint64_t end_address;
    uint32_t identifier;
    uint32_t surface_width;
    uint32_t surface_height;
    int32_t x;
    int32_t y;
    bool show;
#if OS_ENABLE_FRAME_TRACE
    bool report_geometry;
#endif
    uint64_t present_sequence;
    kstatus_t status;

    if (owner == 0 || window == 0 || address == 0U || length == 0U ||
        stride == 0U || damage_x < 0 || damage_y < 0 ||
        damage_width == 0U || damage_height == 0U ||
        address > UINT64_MAX - length) return K_EINVAL;

    window_lock();
    if (!window_server_owner_locked(owner, window)) {
        window_unlock();
        return K_EPERM;
    }
    surface_width = window->width;
    surface_height = window->height;
    if (window->surface_exchange_enabled) {
        window_unlock();
        return K_EINVAL;
    }
    if (surface_width == 0U || surface_height == 0U ||
        surface_width > UINT32_MAX / sizeof(uint32_t) ||
        (uint64_t)(uint32_t)damage_x + damage_width > surface_width ||
        (uint64_t)(uint32_t)damage_y + damage_height > surface_height) {
        window_unlock();
        return K_EINVAL;
    }
    row_bytes = (uint64_t)surface_width * sizeof(uint32_t);
    if ((uint64_t)stride < row_bytes ||
        (uint64_t)stride > UINT64_MAX / surface_height) {
        window_unlock();
        return K_EINVAL;
    }
    required_bytes = (uint64_t)stride * surface_height;
    end_address = window->owner_address > UINT64_MAX - window->buffer_size ?
                  UINT64_MAX : window->owner_address + window->buffer_size;
    if (address != window->owner_address || window->owner_address == 0U ||
        length > window->buffer_size || required_bytes > window->buffer_size ||
        required_bytes > length ||
        end_address < address || address + length > end_address) {
        window_unlock();
        return K_EINVAL;
    }
    identifier = window->identifier;
    x = window->x;
    y = window->y;
    show = (window->flags & OS_WINDOW_VISIBLE) == 0U;
#if OS_ENABLE_FRAME_TRACE
    report_geometry = !window->compositor_presented_valid;
#endif
    window_unlock();

    status = window_server_update_internal(
        owner, identifier, damage_x, damage_y, damage_width, damage_height,
        0U, &present_sequence);
    if (status == K_OK && show) status = window_server_set(window, x, y, 1U);
    if (status != K_OK) return status;

#if OS_ENABLE_FRAME_TRACE
    if (report_geometry) {
        kernel_serial_printf_serial_only(
            "OS_WINDOW_PRESENT_GEOMETRY x=%d y=%d width=%u height=%u stride=%u display=%ux%u\r\n",
            (int)x, (int)y, surface_width, surface_height, stride,
            g_window_server.display_width, g_window_server.display_height);
    }
#endif

    window_present_wait_context_t wait_context = {
        .window = window,
        .sequence = present_sequence,
    };
    status = wait_on_queue(&window->event_waitq,
                         window_present_sequence_ready,
                         &wait_context,
                         5000000000ULL);
    return window_present_wait_result(window, status);
}

kstatus_t window_server_move(process_t *owner,
                             window_server_window_t *window,
                             int32_t x, int32_t y) {
    uint32_t width;
    uint32_t height;
    uint32_t flags;
    if (owner == 0 || window == 0 || x < 0 || y < 0 ||
        !window_server_kernel_ready()) return K_EINVAL;
    window_lock();
    if (!window_server_owner_locked(owner, window)) {
        window_unlock();
        return K_EPERM;
    }
    width = window->width;
    height = window->height;
    flags = window->flags;
    if (window->fullscreen && (x != window->x || y != window->y)) {
        window_unlock();
        return K_EINVAL;
    }
    if ((uint64_t)(uint32_t)x + window_outer_width(width, flags) >
            g_window_server.display_width ||
        (uint64_t)(uint32_t)y + window_outer_height(height, flags) >
            g_window_server.display_height) {
        window_unlock();
        return K_EINVAL;
    }
    window_unlock();
    return window_server_set(window, x, y,
                             (flags & OS_WINDOW_VISIBLE) != 0U ? 1U : 0U);
}

kstatus_t window_server_resize(process_t *owner,
                               window_server_window_t *window,
                               uint32_t width, uint32_t height) {
    uint32_t old_width;
    uint32_t old_height;
    uint32_t flags;
    if (owner == 0 || window == 0 || width == 0U || height == 0U ||
        !window_server_kernel_ready()) return K_EINVAL;

    window_lock();
    if (!window_server_owner_locked(owner, window)) {
        window_unlock();
        return K_EPERM;
    }
    flags = window->flags;
    /* 客户端装饰也包含 Chromium 的小型弹出窗口/工具提示。
     * 原生拖动最小尺寸限制保护的是服务端装饰，而非这些表面。 */
    if ((window->fullscreen && (width != window->width || height != window->height)) ||
        ((flags & OS_WINDOW_CLIENT_DECORATIONS) == 0U &&
         (width < WINDOW_MIN_WIDTH || height < WINDOW_MIN_HEIGHT)) ||
        (flags & OS_WINDOW_RESIZABLE) == 0U ||
        width > g_window_server.display_width ||
        height > g_window_server.display_height ||
        (uint64_t)(uint32_t)window->x + window_outer_width(width, flags) >
            g_window_server.display_width ||
        (uint64_t)(uint32_t)window->y + window_outer_height(height, flags) >
            g_window_server.display_height) {
        window_unlock();
        return K_EINVAL;
    }
    old_width = window->width;
    old_height = window->height;
    if (old_width == width && old_height == height) {
        window_unlock();
        return K_OK;
    }
    window_mark_rect_locked(window->x, window->y,
                            window_outer_width(old_width, flags),
                            window_outer_height(old_height, flags));
    window->width = width;
    window->height = height;
    window->dirty = true;
    window->resize_pending = true;
    window_mark_window_locked(window);
    window_enqueue_resize_event_locked(window);
    window_unlock();
    window_server_notify_worker();
    return K_OK;
}

kstatus_t window_server_close_owned(process_t *owner,
                                    window_server_window_t *window) {
    if (owner == 0 || window == 0 || !window_server_init()) return K_EINVAL;
    window_lock();
    if (!window_server_owner_locked(owner, window)) {
        window_unlock();
        return K_EPERM;
    }
    object_get(window);
    remove_window_locked(window);
    window_unlock();
    window_server_notify_worker();
    (void)wake_all(&window->event_waitq);
    (void)wake_all(&window->surface_waitq);
    object_notify_signaled(window);
    object_put(window);
    return K_OK;
}

kstatus_t window_server_snapshot(uint32_t index, window_server_snapshot_t *out) {
    window_server_window_t *window;
    if (out == 0 || !window_server_init()) return K_EINVAL;
    window_lock();
    if (index >= g_window_server.scene.count) {
        window_unlock();
        return K_ENOENT;
    }
    window = g_window_server.scene.children[index];
    out->identifier = window->identifier;
    out->owner_pid = window->owner != 0 ? (uint32_t)window->owner->pid : 0U;
    out->x = window->x;
    out->y = window->y;
    out->width = window->width;
    out->height = window->height;
    out->visible = (window->flags & OS_WINDOW_VISIBLE) != 0U;
    out->focused = window->identifier == g_window_server.focused_identifier;
    out->z_order = index;
    out->buffer_size = window->buffer_size;
    for (uint32_t i = 0U; i < sizeof(out->title); ++i) out->title[i] = window->title[i];
    window_unlock();
    return K_OK;
}

static kstatus_t window_server_set_internal(process_t *owner,
    window_server_window_t *window, int32_t x, int32_t y,
    uint32_t visible, bool move, bool client_visibility) {
    window_server_window_t *old_focused;
    int32_t old_x;
    int32_t old_y;
    bool defer_first_surface = false;
    bool old_visible, old_hidden;
    if (window == 0 || (visible & ~(client_visibility ? 3U : 1U)) != 0U ||
        visible == WINDOW_SERVER_SHOW_INACTIVE) return K_EINVAL;
    window_lock();
    if (owner != 0 && !window_server_owner_locked(owner, window)) {
        window_unlock();
        return K_EPERM;
    }
    old_visible = (window->flags & OS_WINDOW_VISIBLE) != 0U;
    old_hidden = window->client_hidden;
    if (!move) { x = window->x; y = window->y; }
    if (client_visibility) {
        window->client_show_inactive =
            (visible & WINDOW_SERVER_SHOW_INACTIVE) != 0U;
        visible &= 1U;
        window->client_hidden = visible == 0U;
        /* 不要公开尚未初始化的首个表面。 */
        defer_first_surface = visible != 0U && window->present_sequence == 0U;
        if (defer_first_surface) visible = 0U;
    } else if (visible != 0U && window->client_hidden) {
        window_unlock();
        return K_OK;
    }
    old_x = window->x;
    old_y = window->y;
    old_focused = window_scene_find_locked(g_window_server.focused_identifier);
    window_mark_moved_rect_locked(old_x, old_y, x, y,
                                  window_outer_width(window->width, window->flags),
                                  window_outer_height(window->height, window->flags),
                                  true);
    window->x = x;
    window->y = y;
    if (visible != 0U) {
        window->flags |= OS_WINDOW_VISIBLE;
        if (!window->client_show_inactive)
            window_scene_set_focus_identifier_locked(window->identifier);
    } else {
        window->flags &= ~OS_WINDOW_VISIBLE;
    }
    if (visible == 0U && g_window_server.focused_identifier == window->identifier) {
        window_scene_set_focus_identifier_locked(0U);
        for (uint32_t i = g_window_server.scene.count; i != 0U; --i) {
            window_server_window_t *candidate = g_window_server.scene.children[i - 1U];
            if (!candidate->client_show_inactive &&
                (candidate->flags & OS_WINDOW_VISIBLE) != 0U) {
                window_scene_set_focus_identifier_locked(candidate->identifier);
                break;
            }
        }
    }
    window_mark_window_locked(old_focused);
    window_mark_window_locked(window);
    /* 更新场景后再清理旧捕获，保留离开事件所需的控件信息。 */
    if (old_x != x || old_y != y ||
        old_visible != ((window->flags & OS_WINDOW_VISIBLE) != 0U) ||
        old_hidden != window->client_hidden)
        window_input_refresh_pointer_locked();
    if (visible == 0U && !defer_first_surface)
        window_input_release_pointer_locked(window);
    window_unlock();
    window_server_notify_worker();
    return K_OK;
}

kstatus_t window_server_set(window_server_window_t *window, int32_t x,
                            int32_t y, uint32_t visible) {
    return window_server_set_internal(0, window, x, y, visible, true, false);
}

kstatus_t window_server_set_visibility_owned(process_t *owner,
    window_server_window_t *window, uint32_t visible) {
    if (owner == 0) return K_EINVAL;
    return window_server_set_internal(owner, window, 0, 0, visible, false, true);
}

kstatus_t window_server_capture_owned(process_t *owner,
    window_server_window_t *window, uint32_t capture) {
    bool repaint = false;
    if (owner == 0 || window == 0 || capture > 1U ||
        !window_server_kernel_ready()) return K_EINVAL;
    window_lock();
    if (!window_server_owner_locked(owner, window)) {
        window_unlock();
        return K_EPERM;
    }
    if (capture != 0U) {
        /* 已显示的弹出窗口会在首帧前预留输入捕获。尚未初始化的表面保持在屏幕外时，不得
         * 因此拒绝其输入捕获。 */
        if (window->client_hidden || window->minimized ||
            ((window->flags & OS_WINDOW_VISIBLE) == 0U &&
             window->present_sequence != 0U)) {
            window_unlock();
            return K_EINVAL;
        }
        for (uint32_t i = 0; i < g_window_server.scene.count; ++i) {
            window_server_window_t *other = g_window_server.scene.children[i];
            if (other != 0 && other != window &&
                (other->client_pointer_capture ||
                 (other->client_button_capture && other->owner != owner))) {
                window_unlock();
                return K_EBUSY;
            }
        }
        /* 菜单会在父窗口按下按钮期间打开。它的显式捕获可替代同一进程的隐式捕获，但不能
         * 抢占其他显式捕获或其他进程的输入。修改状态前先验证所有冲突，避免后续冲突导致
         * 捕获状态只更新一半。 */
        for (uint32_t i = 0; i < g_window_server.scene.count; ++i) {
            window_server_window_t *other = g_window_server.scene.children[i];
            if (other != 0 && other != window && other->owner == owner &&
                other->client_button_capture) {
                /* 释放事件将交给弹出窗口，原控件的按压必须在此结束。
                 * 悬停仍归场景路由管理，捕获转移不改变物理命中目标。 */
                repaint |= window_control_cancel_pointer_press_locked(other);
            }
        }
    }
    window->client_pointer_capture = capture != 0U;
    window_unlock();
    if (repaint) window_server_notify_worker();
    return K_OK;
}

kstatus_t window_server_set_minimized_owned(process_t *owner,
    window_server_window_t *window, uint32_t minimized) {
    if (owner == 0 || window == 0 || minimized > 1U ||
        !window_server_kernel_ready()) return K_EINVAL;
    window_lock();
    if (!window_server_owner_locked(owner, window)) {
        window_unlock();
        return K_EPERM;
    }
    if (minimized != 0U && (window->flags & OS_WINDOW_VISIBLE) == 0U) {
        window_unlock();
        return K_EINVAL;
    }
    if (window->minimized != (minimized != 0U)) {
        if (minimized != 0U) {
            (void)window_minimize_locked(window);
        } else {
            window_animation_restore_locked(window);
            window->minimized = false;
            window->dirty = true;
            window_mark_window_locked(window);
            if ((window->flags & OS_WINDOW_VISIBLE) != 0U)
                window_scene_focus_locked(window);
            window_enqueue_minimized_event_locked(window);
            window_input_refresh_pointer_locked();
        }
    }
    window_unlock();
    window_server_notify_worker();
    return K_OK;
}

kstatus_t window_server_focus(uint32_t identifier) {
    window_server_window_t *window;
    if (!window_server_init()) return K_EINVAL;
    window_lock();
    window = window_scene_find_locked(identifier);
    if (window == 0 || (window->flags & OS_WINDOW_VISIBLE) == 0U) {
        window_unlock();
        return K_ENOENT;
    }
    window_scene_focus_locked(window);
    window_input_refresh_pointer_locked();
    window_unlock();
    window_server_notify_worker();
    return K_OK;
}

kstatus_t window_server_map_space(window_server_window_t *window, vm_space_t *space,
                            uint64_t requested_address, uint64_t *mapped_address) {
    vm_object_t *object;
    vaddr_t address;
    kstatus_t status;
#if OS_ENABLE_FRAME_TRACE
    uint64_t populate_tsc;
#endif
    if (window == 0 || space == 0 || mapped_address == 0 ||
        window->section == 0) return K_EINVAL;
    object = window->section->vm_object;
    if (object == 0) {
        return K_EIO;
    }
    vm_object_get(object);
    address = (vaddr_t)requested_address;
    status = vm_map_object(space, object, &address, 0U,
                           (size_t)window->buffer_size,
                           VM_PROT_USER | VM_PROT_READ | VM_PROT_WRITE,
                           VM_MAP_SHARED);
    vm_object_put(object);
    if (status != K_OK) {
        return status;
    }

#if OS_ENABLE_FRAME_TRACE
    populate_tsc = telemetry_timestamp();
#endif
    status = vm_populate_range(space, address,
                               (size_t)window->buffer_size);
    if (status != K_OK) {
        (void)vm_unmap(space, address, (size_t)window->buffer_size);
        return status;
    }
#if OS_ENABLE_FRAME_TRACE
    kernel_frame_trace_emit_scope("window.surface_populate", populate_tsc);
    kernel_frame_trace_emit_value(
        "surface_populate_us",
        window_tsc_to_us(telemetry_timestamp() - populate_tsc));
#endif
    *mapped_address = (uint64_t)address;
    return K_OK;
}

kstatus_t window_server_map(window_server_window_t *window, process_t *process,
                            uint64_t requested_address, uint64_t *mapped_address) {
    if (window == 0 || process == 0 || mapped_address == 0 ||
        window->section == 0) return K_EINVAL;
    vm_space_t *space = process_vm_reference(process);
    kstatus_t status = window_server_map_space(window, space, requested_address,
                                               mapped_address);
    vm_space_put(space);
    return status;
}

kstatus_t window_server_dispatch(uint32_t identifier, const os_input_event_t *event) {
    window_server_window_t *window;
    if (event == 0 || !window_server_init()) return K_EINVAL;
    window_lock();
    window = window_scene_find_locked(identifier);
    if (window == 0 || window->owner == 0) {
        window_unlock();
        return K_ENOENT;
    }
    window_enqueue_client_input_locked(window, event);
    window_unlock();

    if (window_flush_event_wakes()) {
        (void)wake_all(
            &g_window_server.event_waitq);
    }

    return K_OK;
}

typedef struct window_event_wait_context {
    process_t *process;
    uint32_t identifier;
    os_window_event_t *event;
    window_server_window_t *window;
    bool cancelled;
} window_event_wait_context_t;

static bool window_event_pop_locked(window_server_window_t *window,
                                    os_window_event_t *event) {
    if (window->event_count == 0U) return false;
    *event = window->events[window->event_read];
    window->event_read = (window->event_read + 1U) % WINDOW_EVENT_CAPACITY;
    --window->event_count;
    return true;
}

static bool window_event_available(void *raw_context) {
    window_event_wait_context_t *context =
        (window_event_wait_context_t *)raw_context;
    bool available = false;
    window_lock();
    if (context->window != 0) {
        /* 阻塞读取持有此引用。即使没有剩余输入，close 也会在发布 registry_removed 后唤醒
         * 内嵌队列。 */
        context->cancelled = atomic_load_explicit(
            &context->window->registry_removed, memory_order_acquire);
        available = context->cancelled ||
                    window_event_pop_locked(context->window, context->event);
        window_unlock();
        return available;
    }
    for (uint32_t i = 0U; i < g_window_server.scene.count; ++i) {
        window_server_window_t *window = g_window_server.scene.children[i];
        if (window->owner != context->process ||
            (context->identifier != 0U && window->identifier != context->identifier) ||
            window->event_count == 0U) continue;
        available = window_event_pop_locked(window, context->event);
        break;
    }
    window_unlock();
    return available;
}

kstatus_t window_server_event_read(process_t *process, uint32_t identifier,
                                   os_window_event_t *event, uint64_t timeout_ns) {
    window_event_wait_context_t context;
    window_server_window_t *wait_window = 0;
    wait_queue_t *wait_queue = &g_window_server.event_waitq;
    kstatus_t status;
    if (process == 0 || event == 0 || !window_server_init()) return K_EINVAL;
    context.process = process;
    context.identifier = identifier;
    context.event = event;
    context.window = 0;
    context.cancelled = false;
    if (window_event_available(&context)) return K_OK;
    if (timeout_ns == 0U) return K_EAGAIN;
    if (identifier != 0U) {
        /* 阻塞读取期间保持内嵌队列有效。拆除过程会先唤醒此队列，再释放注册表引用。 */
        if (window_server_lookup(identifier, &wait_window) != K_OK ||
            wait_window->owner != process) {
            if (wait_window != 0) window_server_put(wait_window);
            return K_EPERM;
        }
        wait_queue = &wait_window->event_waitq;
        context.window = wait_window;
    }
    status = wait_on_queue(wait_queue, window_event_available,
                           &context, timeout_ns);
    if (wait_window != 0) window_server_put(wait_window);
    if (status == K_OK && context.cancelled) return K_ECANCELED;
    return status;
}

static kstatus_t window_server_update_internal(
    process_t *process, uint32_t identifier, int32_t x, int32_t y,
    uint32_t width, uint32_t height, uint32_t flags,
    uint64_t *present_sequence) {
    window_server_window_t *window;
    caption_text_t *previous_caption=0;
#if OS_ENABLE_FRAME_TRACE
    bool report_surface = false;
    uint64_t first_present_tsc = 0U;
    uint64_t first_present_faults = 0U;
    uint64_t populated_pages = 0U;
    uint64_t prefaulted_pages = 0U;
    uint64_t fault_around_pages = 0U;
    uint64_t create_tsc = 0U;
    uint64_t create_begin_faults = 0U;
#endif
    uint32_t caption = (flags & OS_WINDOW_CAPTION_HEIGHT_MASK) >> 16U;
    if (process == 0 || identifier == 0U ||
        (flags & ~OS_WINDOW_CAPTION_HEIGHT_MASK) != 0U ||
        (flags != 0U && (caption < 40U || caption > 128U)) ||
        (width == 0U) != (height == 0U)) return K_EINVAL;
    if (!window_server_kernel_ready()) return K_EIO;
    window_lock();
    window = window_scene_find_locked(identifier);
    if (window == 0 || window->owner != process) {
        window_unlock();
        return K_EPERM;
    }

    if (flags != 0U) {
        if (!window_standard_caption(window->flags) || caption > window->height) {
            window_unlock();
            return K_EINVAL;
        }
        uint32_t next_flags = (window->flags & ~OS_WINDOW_CAPTION_HEIGHT_MASK) | flags;
        if (next_flags != window->flags) {
            char title[32];memcpy(title,window->title,sizeof(title));
            window_unlock();
            caption_text_t *next=caption_text_create(title,caption_text_height(next_flags));
            if (!next) return K_ENOMEM;
            window_lock();
            window=window_scene_find_locked(identifier);
            if (!window || window->owner!=process) {
                window_unlock();caption_text_put(next);return K_EPERM;
            }
            if (memcmp(title,window->title,sizeof(title)) || !window_standard_caption(window->flags) ||
                caption>window->height) {
                window_unlock();caption_text_put(next);return K_EAGAIN;
            }
            next_flags=(window->flags & ~OS_WINDOW_CAPTION_HEIGHT_MASK) | flags;
            previous_caption=window->caption;window->caption=next;
            /* 标题栏度量会改变外边界。在较小标题栏隐藏旧底边前，也要使旧占用区域失效。 */
            window_mark_window_locked(window);
            window->flags = next_flags;
            window_mark_window_locked(window);
        }
    }

#if OS_ENABLE_FRAME_TRACE
    if (width != 0U && !window->surface_metrics_reported &&
        window->section != 0 && window->section->vm_object != 0) {
        window->surface_metrics_reported = true;
        report_surface = true;
        first_present_tsc = telemetry_timestamp();
        create_tsc = window->create_tsc;
        create_begin_faults = window->surface_fault_count_at_create;
        first_present_faults = vm_object_fault_count(
            window->section->vm_object);
        populated_pages = vm_object_populated_pages(
            window->section->vm_object);
        prefaulted_pages = vm_object_prefaulted_pages(
            window->section->vm_object);
        fault_around_pages = vm_object_fault_around_pages(
            window->section->vm_object);
    }
#endif

    /*
    * 可调整大小的客户端通过提交宽高恰好等于当前尺寸的完整表面损坏矩形，确认当前行跨度。
    * 旧尺寸的过期重绘不能因此暴露行跨度错误的像素。
     */
    if (window->resize_pending && x == 0 && y == 0 &&
        width == window->width && height == window->height) {
        window->resize_pending = false;
    }

    /* 普通 UPDATE 不产生呈现栅栏，但也是有效的首次内容提交。 */
    window->surface_content_ready = true;
    window->dirty = true;
    if (width == 0U) {
        window_mark_window_locked(window);
    } else if (window->controls) {
        window_control_surface_damage_locked(window, x, y, width, height);
    } else {
        window_mark_surface_locked(window, x, y, width, height);
    }
    /* 在同一个注册表事务中发布栅栏及其损坏区域。无关光标/窗口损坏的快照绝不能在已提交的
     * 表面损坏可见前确认此序列。 */
    if (present_sequence != 0) {
        if (window->present_sequence == UINT64_MAX) {
            window->present_sequence = 1U;
        } else {
            ++window->present_sequence;
        }
        *present_sequence = window->present_sequence;
    }
    window_unlock();
    caption_text_put(previous_caption);
#if OS_ENABLE_FRAME_TRACE
    if (report_surface) {
        kernel_frame_trace_emit_value(
            "window_create_begin_fault_count",
            create_begin_faults);
        kernel_frame_trace_emit_value(
            "window_first_present_fault_count", first_present_faults);
        kernel_frame_trace_emit_value(
            "window_surface_faults",
            first_present_faults - create_begin_faults);
        kernel_frame_trace_emit_value("window_pages_allocated", populated_pages);
        kernel_frame_trace_emit_value("window_pages_prefaulted", prefaulted_pages);
        kernel_frame_trace_emit_value("window_pages_fault_around", fault_around_pages);
        kernel_frame_trace_emit_value("fault_count",
                               first_present_faults - create_begin_faults);
        kernel_frame_trace_emit_value("shared_fault_count",
                               first_present_faults - create_begin_faults);
        kernel_frame_trace_emit_value("pages_allocated", populated_pages);
        kernel_frame_trace_emit_value("pages_prefaulted", prefaulted_pages);
        kernel_frame_trace_emit_value("pages_fault_around", fault_around_pages);
        kernel_frame_trace_emit_value(
            "window_first_present_latency_tsc",
            first_present_tsc >= create_tsc ?
                first_present_tsc - create_tsc : 0U);
        kernel_frame_trace_emit_value(
            "first_present_us",
            first_present_tsc >= create_tsc ?
                window_tsc_to_us(first_present_tsc - create_tsc) : 0U);
        vm_fault_telemetry_report();
    }
#endif
    /* 客户端更新独立于输入。即使输入队列为空，也要唤醒合成器；否则工作线程可能休眠至下次
     * 鼠标报告到达，导致每次重绘都晚一个事件。 */
    window_server_notify_worker();
    return K_OK;
}

kstatus_t window_server_title_owned(process_t *owner, uint32_t identifier,
                                     const char title[32]) {
    if (owner == 0 || identifier == 0U || title == 0) return K_EINVAL;
    size_t length = 0U;
    while (length < 32U && title[length] != '\0') ++length;
    if (length == 32U) return K_EINVAL;
    if (!window_server_kernel_ready()) return K_EIO;
    window_lock();
    window_server_window_t *window = window_scene_find_locked(identifier);
    if (window == 0 || window->owner != owner) {
        window_unlock();
        return K_EPERM;
    }
    if (!memcmp(window->title,title,length+1U)) {window_unlock();return K_OK;}
    uint32_t title_height=caption_text_height(window->flags);
    window_unlock();
    caption_text_t *caption=caption_text_create(title,title_height);
    if (!caption) return K_ENOMEM;
    window_lock();
    window=window_scene_find_locked(identifier);
    if (window==0 || window->owner!=owner) {
        window_unlock();caption_text_put(caption);return K_EPERM;
    }
    if (caption_text_height(window->flags)!=title_height) {
        window_unlock();caption_text_put(caption);return K_EAGAIN;
    }
    bool changed = false;
    for (size_t index = 0U; index < sizeof(window->title); ++index) {
        char character = index < length ? title[index] : '\0';
        changed |= window->title[index] != character;
        window->title[index] = character;
    }
    caption_text_t *previous=window->caption;
    window->caption=caption;
    if (changed) window_mark_rect_locked(window->x, window->y,
        window_outer_width(window->width, window->flags),
        window_client_decorations(window->flags) ? os_window_caption_height(window->flags) :
        window_client_offset_y(window->flags));
    window_unlock();
    caption_text_put(previous);
    if (changed) window_server_notify_worker();
    return K_OK;
}

kstatus_t window_server_update(process_t *process, uint32_t identifier,
                               int32_t x, int32_t y, uint32_t width,
                               uint32_t height, uint32_t flags) {
    return window_server_update_internal(process, identifier, x, y,
                                         width, height, flags, 0);
}
