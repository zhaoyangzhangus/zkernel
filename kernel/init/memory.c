#include <kernel/init_memory.h>

#include <arch/x86_64/apic.h>
#include <arch/x86_64/paging.h>
#include <arch/x86_64/uaccess.h>
#include <kernel/debug_stage.h>
#include <kernel/console.h>
#include <kernel/mm.h>
#include <kernel/mm_boot.h>
#include <kernel/perf.h>
#include <kernel/telemetry.h>
#include <kernel/pci.h>

#define FRAMEBUFFER_KERNEL_VIRTUAL_BASE (X86_64_MMIO_BASE + 0x20000000ULL)

static BOOLEAN memory_fail_at(const kernel_init_memory_hooks_t *hooks,
                              const CHAR8 *message, const char *file,
                              uint32_t line) {
    kernel_debug_stage_fail_at(OS_DEBUG_PHASE_MEMORY,
                               OS_DEBUG_STEP_FAIL, K_EIO, file, line);
    hooks->write(message);
    hooks->halt();
    return 0;
}

#define memory_fail(hooks, message) \
    memory_fail_at((hooks), (message), __FILE__, __LINE__)

BOOLEAN map_framebuffer_wc(const OS_BOOT_INFO *info,
                                  UINT64 *virtual_base) {
    if (info == 0 || virtual_base == 0 || info->FrameBufferBase == 0 ||
        info->FrameBufferSize == 0) return 0;

    UINT64 physical_page = info->FrameBufferBase & ~(PAGE_SIZE - 1ULL);
    UINT64 page_offset = info->FrameBufferBase - physical_page;
    if (info->FrameBufferSize > UINT64_MAX - page_offset) return 0;
    UINT64 span = info->FrameBufferSize + page_offset;
    if (span > UINT64_MAX - (PAGE_SIZE - 1ULL)) return 0;
    span = (span + PAGE_SIZE - 1ULL) & ~(PAGE_SIZE - 1ULL);
    if (span == 0 ||
        span > X86_64_MMIO_END - FRAMEBUFFER_KERNEL_VIRTUAL_BASE + 1ULL) {
        return 0;
    }

    paddr_t root = x86_current_root_table();
    UINT64 mapped = 0;
    while (mapped < span) {
        kstatus_t status = x86_map_page(
            root,
            (vaddr_t)(FRAMEBUFFER_KERNEL_VIRTUAL_BASE + mapped),
            paddr_make(physical_page + mapped),
            X86_PAGE_WRITE | X86_PAGE_GLOBAL,
            /*
             * 早期启动阶段尚不知道 PCI 资源归属。在 framebuffer_enable_wc 确认显示 BAR 受支持且不存在
             * 冲突别名前，保持此映射为 UC。与 NVIDIA BAR 重叠的区域继续使用 UC。
             */
            X86_CACHE_UC);
        if (status != K_OK) {
            while (mapped != 0) {
                mapped -= PAGE_SIZE;
                (void)x86_unmap_page(
                    root,
                    (vaddr_t)(FRAMEBUFFER_KERNEL_VIRTUAL_BASE + mapped),
                    0);
            }
            return 0;
        }
        mapped += PAGE_SIZE;
    }
    *virtual_base = FRAMEBUFFER_KERNEL_VIRTUAL_BASE + page_offset;
    return 1;
}

/* PCI 资源归属尚未确定时，早期启动使用 UC。仅将完整且可预取的 AMD/QEMU 显示 BAR 升级；
 * NVIDIA BAR 别名仍采用保守映射。 */
static bool framebuffer_wc_owner(const OS_BOOT_INFO *info, const pci_host_t *host) {
    if (info == 0 || host == 0 || !host->initialized ||
        host->device_count > PCI_MAX_DEVICES ||
        info->FrameBufferBase == 0U || info->FrameBufferSize == 0U ||
        info->FrameBufferSize > UINT64_MAX - info->FrameBufferBase) return false;
    uint64_t start = info->FrameBufferBase & ~(PAGE_SIZE - 1ULL);
    uint64_t end = info->FrameBufferBase + info->FrameBufferSize;
    if (end > UINT64_MAX - (PAGE_SIZE - 1ULL)) return false;
    end = (end + PAGE_SIZE - 1ULL) & ~(PAGE_SIZE - 1ULL);
    bool owner = false;
    for (uint32_t i = 0U; i < host->device_count; ++i) {
        const pci_device_t *device = &host->devices[i];
        for (uint32_t j = 0U; j < PCI_MAX_BARS; ++j) {
            const pci_bar_t *bar = &device->bars[j];
            if ((bar->flags & PCI_RESOURCE_MEMORY) == 0U || bar->length == 0U ||
                bar->length > UINT64_MAX - bar->address) continue;
            uint64_t limit = bar->address + bar->length;
            if (start >= limit || end <= bar->address) continue;
            bool supported = device->class_code == 3U &&
                (device->vendor_id == 0x1002U ||
                 (device->vendor_id == 0x1234U && device->device_id == 0x1111U));
            if (owner || !supported || (bar->flags & PCI_RESOURCE_PREFETCH) == 0U ||
                start < bar->address || end > limit) return false;
            owner = true;
        }
    }
    return owner;
}

BOOLEAN framebuffer_enable_wc(const OS_BOOT_INFO *info, UINT64 virtual_base) {
    if (!framebuffer_wc_owner(info, pci_current_host()) ||
        virtual_base == 0U ||
        (virtual_base & (PAGE_SIZE - 1ULL)) !=
            (info->FrameBufferBase & (PAGE_SIZE - 1ULL))) return 0;
    uint64_t offset = info->FrameBufferBase & (PAGE_SIZE - 1ULL);
    uint64_t span = (info->FrameBufferSize + offset + PAGE_SIZE - 1ULL) &
                    ~(PAGE_SIZE - 1ULL);
    uint64_t base = virtual_base - offset;
    if (span > UINT64_MAX - base) return 0;
    paddr_t root = x86_current_root_table();
    uint64_t changed = 0U;
    __asm__ volatile ("sfence" : : : "memory");
    while (changed < span) {
        if (x86_protect_page(root, base + changed,
                X86_PAGE_WRITE | X86_PAGE_GLOBAL, X86_CACHE_WC) != K_OK) {
            while (changed != 0U) {
                changed -= PAGE_SIZE;
                (void)x86_protect_page(root, base + changed,
                    X86_PAGE_WRITE | X86_PAGE_GLOBAL, X86_CACHE_UC);
            }
            return 0;
        }
        changed += PAGE_SIZE;
    }
    return 1;
}

static BOOLEAN direct_map_self_test(const OS_BOOT_INFO *info) {
    if (info == 0) return 0;
    UINT64 boot_physical = info->BootInfoPhysicalBase != 0 ?
                           info->BootInfoPhysicalBase : (UINT64)(uintptr_t)info;
    paddr_t translated;
    paddr_t root = x86_current_root_table();
    UINT64 boot_virtual = X86_64_DIRECT_MAP_BASE + boot_physical;
    if (x86_translate_page(root, (vaddr_t)boot_virtual, &translated, 0) != K_OK ||
        translated.value != boot_physical ||
        phys_to_direct(paddr_make(boot_physical)) !=
            (void *)(uintptr_t)boot_virtual) return 0;

    paddr_t lapic = paddr_make(OS_LAPIC_BASE);
    if (!direct_map_range_is_ram(lapic, 1U)) {
        if (phys_to_direct(lapic) != 0 ||
            x86_translate_page(root, X86_64_DIRECT_MAP_BASE + OS_LAPIC_BASE,
                               &translated, 0) != K_ENOENT) return 0;
    }

    paddr_t framebuffer = paddr_make(info->FrameBufferBase);
    if (info->FrameBufferBase != 0 &&
        !direct_map_range_is_ram(framebuffer, 1U)) {
        if (phys_to_direct(framebuffer) != 0 ||
            x86_translate_page(root,
                               X86_64_DIRECT_MAP_BASE + info->FrameBufferBase,
                               &translated, 0) != K_ENOENT) return 0;
    }
    return 1;
}

static BOOLEAN canonical_uaccess_self_test(void) {
    uint64_t value = 0x1122334455667788ULL;
    void __user *unmapped = (void __user *)(uintptr_t)0x0000700000000000ULL;
    if (!x86_user_range_valid(unmapped, sizeof(value)) ||
        x86_user_range_valid((void __user *)(uintptr_t)0x8000ULL,
                             sizeof(value))) return 0;
    if (copy_from_user(&value, unmapped, sizeof(value)) != K_EACCES) return 0;
    if (copy_to_user(unmapped, &value, sizeof(value)) != K_EACCES) return 0;
    return 1;
}

/* 直接测试旧版 Buddy 阶；0..5 阶由每 CPU 小页池服务，因此不能证明 Buddy 合并正确。 */
static BOOLEAN canonical_buddy_random_self_test(void) {
#define MM_BUDDY_STRESS_COUNT 32U
    page_t *blocks[MM_BUDDY_STRESS_COUNT] = {0};
    UINT8 orders[MM_BUDDY_STRESS_COUNT] = {0};
    UINT32 random_state = 0xA17E5EEDU;
    UINT32 allocated = 0U;

    for (UINT32 index = 0U; index < MM_BUDDY_STRESS_COUNT; ++index) {
        random_state = random_state * 1664525U + 1013904223U;
        UINT8 order = (UINT8)(6U + ((random_state >> 8) % 4U));
        page_t *page = page_alloc(order, 0);
        if (page == 0) goto buddy_fail;

        UINT64 size = (1ULL << order) * PAGE_SIZE;
        UINT64 address = page_to_phys(page).value;
        if (address == UINT64_MAX || (address & (size - 1ULL)) != 0U ||
            address > UINT64_MAX - size) {
            page_free(page);
            goto buddy_fail;
        }

        UINT64 end = address + size;
        for (UINT32 previous = 0U; previous < allocated; ++previous) {
            UINT64 previous_size =
                (1ULL << orders[previous]) * PAGE_SIZE;
            UINT64 previous_address = page_to_phys(blocks[previous]).value;
            UINT64 previous_end = previous_address + previous_size;
            if (address < previous_end && previous_address < end) {
                page_free(page);
                goto buddy_fail;
            }
        }

        blocks[allocated] = page;
        orders[allocated] = order;
        ++allocated;
    }

    /* 按确定性的伪随机顺序释放，以覆盖两个合并方向及重复拆分/合并决策。 */
    for (UINT32 remaining = MM_BUDDY_STRESS_COUNT; remaining != 0U;
         --remaining) {
        random_state = random_state * 1664525U + 1013904223U;
        UINT32 index = random_state % remaining;
        page_free(blocks[index]);
        blocks[index] = blocks[remaining - 1U];
        orders[index] = orders[remaining - 1U];
        blocks[remaining - 1U] = 0;
    }

    /* 完整随机释放后，高阶合并块必须再次可用；否则上方测试只覆盖了拆分路径。 */
    page_t *coalesced = page_alloc(9U, 0);
    if (coalesced == 0 || coalesced->order != 9U) {
        if (coalesced != 0) page_free(coalesced);
        return 0;
    }
    page_free(coalesced);
    return 1;

buddy_fail:
    while (allocated != 0U) {
        --allocated;
        if (blocks[allocated] != 0) page_free(blocks[allocated]);
    }
    return 0;
#undef MM_BUDDY_STRESS_COUNT
}

static BOOLEAN canonical_mm_self_test(void) {
    uint64_t benchmark_start;
    uint64_t total_pages_before;
    uint64_t total_pages_after;
    uint64_t free_pages_before;
    uint64_t free_pages_after;
    page_t *stats_probe;

    if (page_memory_get_stats(&total_pages_before, &free_pages_before) != K_OK ||
        total_pages_before == 0U || free_pages_before == 0U) return 0;
    stats_probe = page_alloc(0U, 0U);
    if (stats_probe == 0) return 0;
    if (page_memory_get_stats(&total_pages_after, &free_pages_after) != K_OK ||
        total_pages_after != total_pages_before ||
        free_pages_after + 1U != free_pages_before) {
        page_free(stats_probe);
        return 0;
    }
    page_free(stats_probe);
    if (page_memory_get_stats(&total_pages_after, &free_pages_after) != K_OK ||
        total_pages_after != total_pages_before ||
        free_pages_after != free_pages_before) return 0;

    benchmark_start = telemetry_timestamp();
    page_t *single = page_alloc(0, PAGE_ALLOC_ZERO | PAGE_ALLOC_DMA32);
    if (single == 0 ||
        atomic_load_explicit(&single->refs, memory_order_relaxed) != 1U) {
        if (single != 0) page_free(single);
        return 0;
    }
    UINT8 *bytes = (UINT8 *)phys_to_direct(page_to_phys(single));
    if (bytes == 0 || bytes[0] != 0 || bytes[PAGE_SIZE - 1U] != 0) {
        page_free(single);
        return 0;
    }
    page_free(single);
    kernel_perf_emit_scope("mm.order0_alloc_free", benchmark_start);

    benchmark_start = telemetry_timestamp();
    page_t *compound = page_alloc(2U, 0);
    if (compound == 0 || compound->order != 2U ||
        (compound->flags & PAGE_COMPOUND_HEAD) == 0) {
        if (compound != 0) page_free(compound);
        return 0;
    }
    page_free(compound);
    kernel_perf_emit_scope("mm.multi_page_alloc_free", benchmark_start);

    /* 直接测试规范 page_t Buddy 拆分/合并路径。 */
#define MM_ALLOCATOR_STRESS_COUNT 96U
    page_t *blocks[MM_ALLOCATOR_STRESS_COUNT] = {0};
    UINT32 random_state = 0x13579BDFU;
    UINT32 allocated = 0;
    for (UINT32 index = 0; index < MM_ALLOCATOR_STRESS_COUNT; ++index) {
        random_state = random_state * 1664525U + 1013904223U;
        UINT8 order = (UINT8)((random_state >> 8) % 6U);
        page_t *page = page_alloc(order, 0);
        if (page == 0) goto allocator_fail;
        blocks[allocated++] = page;

        UINT64 size = (1ULL << order) * PAGE_SIZE;
        UINT64 address = page_to_phys(page).value;
        if (address == UINT64_MAX || (address & (size - 1ULL)) != 0 ||
            address > UINT64_MAX - size) goto allocator_fail;
        UINT64 end = address + size;
        for (UINT32 previous = 0; previous + 1U < allocated; ++previous) {
            page_t *previous_page = blocks[previous];
            UINT8 previous_order = previous_page->order;
            UINT64 previous_size = (1ULL << previous_order) * PAGE_SIZE;
            UINT64 previous_address = page_to_phys(previous_page).value;
            UINT64 previous_end = previous_address + previous_size;
            if (address < previous_end && previous_address < end) {
                goto allocator_fail;
            }
        }
    }

    for (UINT32 remaining = MM_ALLOCATOR_STRESS_COUNT; remaining != 0;
         --remaining) {
        random_state = random_state * 1664525U + 1013904223U;
        UINT32 index = random_state % remaining;
        page_t *released = blocks[index];
        blocks[index] = blocks[remaining - 1U];
        blocks[remaining - 1U] = 0;
        page_free(released);
    }
    if (!canonical_buddy_random_self_test()) return 0;
    return 1;

allocator_fail:
    while (allocated != 0) {
        --allocated;
        if (blocks[allocated] != 0) page_free(blocks[allocated]);
    }
    return 0;
#undef MM_ALLOCATOR_STRESS_COUNT
}

BOOLEAN kernel_init_memory(OS_BOOT_INFO *info,
                           const kernel_init_memory_hooks_t *hooks,
                           UINT64 *framebuffer_virtual_base) {
    if (info == 0 || hooks == 0 || hooks->write == 0 || hooks->halt == 0 ||
        hooks->console_init == 0 || hooks->console_disable == 0 ||
        framebuffer_virtual_base == 0) return 0;

    kernel_debug_stage_enter(OS_DEBUG_PHASE_SPEC_2);
    kernel_debug_stage_enter(OS_DEBUG_PHASE_SPEC_3);
    if (!canonical_uaccess_self_test()) {
        return memory_fail(hooks, "OS_UACCESS_TEST_FAIL\r\n");
    }
    hooks->write("OS_UACCESS_OK\r\n");
    if (!mm_boot_init(info)) {
        return memory_fail(hooks, "OS_MM_INIT_FAIL\r\n");
    }
    if (!arch_rebuild_ram_direct_map(info)) {
        return memory_fail(hooks, "OS_DIRECT_MAP_FAIL\r\n");
    }
    if (!map_framebuffer_wc(info, framebuffer_virtual_base)) {
        hooks->console_disable();
        return memory_fail(hooks, "OS_FRAMEBUFFER_MAP_FAIL\r\n");
    }
    if (!hooks->console_init(info, *framebuffer_virtual_base)) {
        hooks->console_disable();
    }
    if (!direct_map_self_test(info)) {
        return memory_fail(hooks, "OS_DIRECT_MAP_FAIL\r\n");
    }
    /* 直接映射验证属于启动诊断。保留串口和 realtest 捕获输出，但不要绘制到桌面 GOP surface 上。 */
    kernel_serial_write_serial_only("OS_DIRECT_MAP_OK\r\n");
    if (!lapic_use_kernel_mapping()) {
        return memory_fail(hooks, "OS_MM_INIT_FAIL\r\n");
    }
    if (!canonical_mm_self_test()) {
        return memory_fail(hooks, "OS_MM_ALLOCATOR_FAIL\r\n");
    }
    hooks->write("OS_BUDDY_RANDOM_OK\r\n");
    hooks->write("OS_MM_ALLOCATOR_OK\r\n");
    hooks->write("OS_MM_OK\r\n");
    kernel_debug_stage(OS_DEBUG_PHASE_REFACTOR_2,
                       OS_DEBUG_STEP_PROGRESS, 7U);
    kernel_debug_stage(OS_DEBUG_PHASE_MEMORY,
                       OS_DEBUG_STEP_READY, 1U);
    kernel_debug_stage_ready(OS_DEBUG_PHASE_SPEC_2);
    kernel_debug_stage(OS_DEBUG_PHASE_REFACTOR_3,
                       OS_DEBUG_STEP_PROGRESS, 2U);
    return 1;
}
