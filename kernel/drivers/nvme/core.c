#include <arch/x86_64/apic.h>
#include <arch/x86_64/cpu.h>
#include <arch/x86_64/paging.h>
#include <kernel/block.h>
#include <kernel/dma.h>
#include <kernel/deferred.h>
#include <kernel/irq.h>
#include <kernel/io.h>
#include <kernel/kmem.h>
#include <kernel/nvme_core.h>
#include <kernel/sched.h>

#include "internal.h"

#define NVME_REG_CAP             0x000U
#define NVME_REG_VS              0x008U
#define NVME_REG_CC              0x014U
#define NVME_REG_CSTS            0x01CU
#define NVME_REG_AQA             0x024U
#define NVME_REG_ASQ             0x028U
#define NVME_REG_ACQ             0x030U
#define NVME_REG_DBS             0x1000U

#define NVME_CC_ENABLE           (1U << 0)
#define NVME_CSTS_READY          (1U << 0)
#define NVME_CSTS_FATAL          (1U << 1)
#define NVME_ADMIN_IDENTIFY      0x06U
#define NVME_ADMIN_SET_FEATURES  0x09U
#define NVME_ADMIN_CREATE_CQ     0x05U
#define NVME_ADMIN_CREATE_SQ     0x01U
#define NVME_FEATURE_NUMBER_QUEUES 0x07U
#define NVME_MMIO_BASE           (X86_64_MMIO_BASE + 0x01000000ULL)
#define NVME_MMIO_SLOT_SIZE      0x00100000ULL
/* REFACTOR_P8_NVME_CORE_OWNER：控制器生命周期、探测和公开状态。 */

bool g_nvme_hardware_seen;

bool nvme_hardware_present(void) {
    return g_nvme_hardware_seen;
}

static driver_t g_nvme_driver;
nvme_controller_t *g_nvme_controllers[NVME_MAX_CONTROLLERS];
static atomic_uint g_nvme_driver_state;
static uint32_t g_nvme_mmio_slots;
static kstatus_t g_nvme_last_status;
static uint32_t g_nvme_last_stage;
static uint16_t g_nvme_last_completion;

typedef struct nvme_deferred_dma_cleanup {
    dma_mapping_t *mapping;
    page_t *page;
} nvme_deferred_dma_cleanup_t;

static void nvme_retry_dma_cleanup(void *argument) {
    nvme_deferred_dma_cleanup_t *cleanup =
        (nvme_deferred_dma_cleanup_t *)argument;
    if (cleanup == 0 || cleanup->mapping == 0) return;
    if (dma_unmap_checked(cleanup->mapping) == K_OK) {
        if (cleanup->page != 0) page_free(cleanup->page);
        kfree(cleanup->mapping);
        kfree(cleanup);
        return;
    }
    /* 闃熷垪婊℃椂淇濈暀 cleanup锛涙槧灏勪粛鐢?DMA 娉ㄥ唽琛ㄥ拰璇ュ爢瀵硅薄鎸佹湁銆?*/
    (void)deferred_schedule(nvme_retry_dma_cleanup, cleanup);
}

kstatus_t nvme_release_transient_dma(dma_mapping_t *mapping, page_t *page) {
    if (mapping == 0) return K_EINVAL;
    kstatus_t status = dma_unmap_checked(mapping);
    if (status == K_OK) {
        if (page != 0) page_free(page);
        kfree(mapping);
        return K_OK;
    }
    nvme_deferred_dma_cleanup_t *cleanup =
        (nvme_deferred_dma_cleanup_t *)kzalloc(sizeof(*cleanup), 0);
    if (cleanup != 0) {
        cleanup->mapping = mapping;
        cleanup->page = page;
        if (deferred_schedule(nvme_retry_dma_cleanup, cleanup)) return status;
        /* 璋冨害澶辫触鏃舵晠鎰忎笉閲婃斁 cleanup锛涘畠浠嶄繚瀛樼潃瀹夊叏鐨勬寔鏈夎€呫€?*/
    }
    return status;
}

static kstatus_t nvme_record_status(kstatus_t status) {
    g_nvme_last_status = status;
    return status;
}

void nvme_record_completion_status(uint16_t status) {
    g_nvme_last_completion = status;
}

/*
 * NVMe 队列/管理锁保护的状态也会被其他 CPU 上的调度线程访问。当另一 CPU 为同一把锁
 * 自旋时，本地定时器不能强制切走锁的所有者。
 *
 * 硬 IRQ 处理程序绝不获取这些锁，只会将任务加入延迟队列。
 */
void nvme_lock(spinlock_t *lock) {
    sched_preempt_disable();
    spinlock_lock(lock);
}

bool nvme_try_lock(spinlock_t *lock) {
    sched_preempt_disable();
    if (!spinlock_try_lock(lock)) {
        sched_preempt_enable();
        return false;
    }
    return true;
}

void nvme_unlock(spinlock_t *lock) {
    spinlock_unlock(lock);
    sched_preempt_enable();
}

static uint32_t nvme_read32(const nvme_controller_t *controller, uint32_t offset) {
    volatile const uint32_t *address =
        (volatile const uint32_t *)(controller->registers + offset);
    uint32_t value;

    /*
     * 此处不能使用普通的 volatile uint32_t 读取：调用方只检查低位时，GCC 可能将其
     * 折叠为 8 位测试。NVMe 控制器寄存器要求 32 位 MMIO 事务（QEMU 会拒绝更窄的访问），
     * 因此必须明确指定访问宽度。
     */
    __asm__ volatile ("movl %1, %0" : "=r"(value) : "m"(*address) : "memory");
    return value;
}

static uint64_t nvme_read64(const nvme_controller_t *controller, uint32_t offset) {
    uint32_t low = nvme_read32(controller, offset);
    uint32_t high = nvme_read32(controller, offset + 4U);
    return ((uint64_t)high << 32) | low;
}

void nvme_write32(const nvme_controller_t *controller, uint32_t offset,
                  uint32_t value) {
    volatile uint32_t *address = (volatile uint32_t *)(controller->registers + offset);
    __asm__ volatile ("movl %1, %0" : "=m"(*address) : "r"(value) : "memory");
}

static void nvme_write64(const nvme_controller_t *controller, uint32_t offset,
                         uint64_t value) {
    nvme_write32(controller, offset, (uint32_t)value);
    nvme_write32(controller, offset + 4U, (uint32_t)(value >> 32));
}

static kstatus_t nvme_wait_ready(const nvme_controller_t *controller, bool ready) {
    uint64_t deadline = nvme_timeout_deadline(controller);
    for (;;) {
        uint32_t status = nvme_read32(controller, NVME_REG_CSTS);
        if ((status & NVME_CSTS_FATAL) != 0) return K_EIO;
        if (((status & NVME_CSTS_READY) != 0) == ready) return K_OK;
        if (nvme_deadline_reached(deadline)) return K_ETIMEDOUT;
        __asm__ volatile ("pause");
    }
}

static void nvme_unmap_registers(nvme_controller_t *controller) {
    if (controller == 0 || controller->registers_va == 0) return;
    paddr_t root = x86_current_root_table();
    uint64_t pages = (controller->registers_size + PAGE_SIZE - 1ULL) >> PAGE_SHIFT;
    for (uint64_t i = 0; i < pages; ++i) {
        (void)x86_unmap_page(root, controller->registers_va + i * PAGE_SIZE, 0);
    }
    controller->registers = 0;
    controller->registers_va = 0;
    controller->registers_size = 0;
}

static kstatus_t nvme_map_registers(nvme_controller_t *controller) {
    if (controller == 0 || controller->pci == 0 ||
        controller->pci->bars[0].address == 0 || controller->pci->bars[0].length < PAGE_SIZE) {
        return K_EINVAL;
    }
    uint64_t offset = controller->pci->bars[0].address & (PAGE_SIZE - 1ULL);
    uint64_t size = controller->pci->bars[0].length + offset;
    size = (size + PAGE_SIZE - 1ULL) & ~(PAGE_SIZE - 1ULL);
    if (size == 0 || size > NVME_MMIO_SLOT_SIZE ||
        g_nvme_mmio_slots >= NVME_MAX_CONTROLLERS) return K_EINVAL;
    vaddr_t virtual_base = NVME_MMIO_BASE +
                           (uint64_t)g_nvme_mmio_slots++ * NVME_MMIO_SLOT_SIZE;
    paddr_t root = x86_current_root_table();
    uint64_t physical_base = controller->pci->bars[0].address & ~(PAGE_SIZE - 1ULL);
    uint64_t pages = size >> PAGE_SHIFT;
    for (uint64_t i = 0; i < pages; ++i) {
        kstatus_t status = x86_map_page(root, virtual_base + i * PAGE_SIZE,
                                        paddr_make(physical_base + i * PAGE_SIZE),
                                        X86_PAGE_WRITE | X86_PAGE_GLOBAL, X86_CACHE_UC);
        if (status != K_OK) {
            for (uint64_t j = 0; j < i; ++j) {
                (void)x86_unmap_page(root, virtual_base + j * PAGE_SIZE, 0);
            }
            return status;
        }
    }
    controller->registers_va = virtual_base;
    controller->registers_size = size;
    controller->registers = (volatile uint8_t *)(uintptr_t)(virtual_base + offset);
    return K_OK;
}

static bool nvme_free_admin_pages(nvme_controller_t *controller) {
    bool submission_released;
    bool completion_released;
    if (controller == 0) return false;
    submission_released = controller->admin_submission_dma.device == 0;
    completion_released = controller->admin_completion_dma.device == 0;
    if (!submission_released) {
        submission_released = dma_unmap_checked(&controller->admin_submission_dma) == K_OK;
    }
    if (!completion_released) {
        completion_released = dma_unmap_checked(&controller->admin_completion_dma) == K_OK;
    }
    if (submission_released) {
        if (controller->admin_submission_page != 0) page_free(controller->admin_submission_page);
        controller->admin_submission_page = 0;
        controller->admin_submission = 0;
    }
    if (completion_released) {
        if (controller->admin_completion_page != 0) page_free(controller->admin_completion_page);
        controller->admin_completion_page = 0;
        controller->admin_completion = 0;
    }
    return submission_released && completion_released;
}

static bool nvme_free_io_queue(nvme_queue_t *queue) {
    bool submission_released;
    bool completion_released;
    if (queue == 0) return false;
    if (queue->pending_ios.next == 0 || queue->pending_ios.prev == 0) {
        list_init(&queue->pending_ios);
        atomic_init(&queue->pending_count, 0U);
        atomic_init(&queue->completion_queued, false);
        atomic_init(&queue->completion_work_refs, 0U);
        atomic_init(&queue->lock.state, 0U);
    }
    /* 璋冪敤鑰呭繀椤诲厛鍋滄鎺у埗鍣紱杩欓噷鍐嶆挙閿€闃熷垪锛岀‘淇?DMA 鏄犲皠鏈€鍚庨噴鏀俱€?*/
    queue->active = false;
    nvme_stop_completion_work(queue);
    nvme_abort_queue_pending(queue, K_EDEVREMOVED);
    if (queue->irq_vector != 0 && queue->controller != 0) {
        (void)irq_unregister(queue->irq_vector, nvme_msix_handler,
                              queue->controller);
        queue->irq_vector = 0;
    }
    submission_released = queue->submission_dma.device == 0;
    completion_released = queue->completion_dma.device == 0;
    if (!submission_released) {
        submission_released = dma_unmap_checked(&queue->submission_dma) == K_OK;
    }
    if (!completion_released) {
        completion_released = dma_unmap_checked(&queue->completion_dma) == K_OK;
    }
    if (submission_released) {
        if (queue->submission_page != 0) page_free(queue->submission_page);
        queue->submission_page = 0;
        queue->submission = 0;
    }
    if (completion_released) {
        if (queue->completion_page != 0) page_free(queue->completion_page);
        queue->completion_page = 0;
        queue->completion = 0;
    }
    if (!submission_released || !completion_released) {
        queue->active = false;
        return false;
    }
    queue->queue_id = 0;
    queue->depth = 0;
    queue->submission_tail = 0;
    queue->completion_head = 0;
    queue->phase = 0;
    queue->controller = 0;
    atomic_store_explicit(&queue->pending_count, 0U, memory_order_release);
    list_init(&queue->pending_ios);
    atomic_store_explicit(&queue->completion_queued, false, memory_order_release);
    return true;
}

static kstatus_t nvme_alloc_io_queue(nvme_controller_t *controller,
                                     nvme_queue_t *queue, uint16_t queue_id) {
    if (controller == 0 || queue == 0 || queue_id == 0) return K_EINVAL;
    if (!nvme_free_io_queue(queue)) return K_EBUSY;
    queue->submission_page = page_alloc(0, PAGE_ALLOC_ZERO | PAGE_ALLOC_DMA32);
    queue->completion_page = page_alloc(0, PAGE_ALLOC_ZERO | PAGE_ALLOC_DMA32);
    if (queue->submission_page == 0 || queue->completion_page == 0) {
        nvme_free_io_queue(queue);
        return K_ENOMEM;
    }
    queue->submission_page->owner = PAGE_OWNER_DEVICE;
    queue->completion_page->owner = PAGE_OWNER_DEVICE;
    page_t *submission_pages[1] = {queue->submission_page};
    page_t *completion_pages[1] = {queue->completion_page};
    if (dma_map_pages(controller->device, submission_pages, 1U,
                      DMA_BIDIRECTIONAL, &queue->submission_dma) != K_OK ||
        dma_map_pages(controller->device, completion_pages, 1U,
                      DMA_BIDIRECTIONAL, &queue->completion_dma) != K_OK) {
        nvme_free_io_queue(queue);
        return K_EIO;
    }
    queue->submission = (nvme_command_t *)phys_to_direct(
        page_to_phys(queue->submission_page));
    queue->completion = (nvme_completion_t *)phys_to_direct(
        page_to_phys(queue->completion_page));
    if (queue->submission == 0 || queue->completion == 0) {
        nvme_free_io_queue(queue);
        return K_EIO;
    }
    queue->controller = controller;
    queue->queue_id = queue_id;
    queue->depth = NVME_IO_QUEUE_DEPTH;
    queue->submission_tail = 0;
    queue->completion_head = 0;
    queue->phase = 1U;
    queue->active = false;
    atomic_init(&queue->lock.state, 0U);
    atomic_init(&queue->pending_count, 0U);
    atomic_init(&queue->completion_queued, false);
    atomic_init(&queue->completion_work_refs, 0U);
    list_init(&queue->pending_ios);
    return K_OK;
}

static bool nvme_free_io_queues(nvme_controller_t *controller) {
    bool success = true;
    if (controller == 0) return false;
    for (uint32_t i = 0; i < NVME_MAX_IO_QUEUES; ++i) {
        if (!nvme_free_io_queue(&controller->io_queues[i])) success = false;
    }
    controller->io_queue_count = 0;
    return success;
}

static kstatus_t nvme_alloc_admin_pages(nvme_controller_t *controller) {
    controller->admin_submission_page = page_alloc(0, PAGE_ALLOC_ZERO | PAGE_ALLOC_DMA32);
    controller->admin_completion_page = page_alloc(0, PAGE_ALLOC_ZERO | PAGE_ALLOC_DMA32);
    if (controller->admin_submission_page == 0 || controller->admin_completion_page == 0) {
        nvme_free_admin_pages(controller);
        return K_ENOMEM;
    }
    controller->admin_submission_page->owner = PAGE_OWNER_DEVICE;
    controller->admin_completion_page->owner = PAGE_OWNER_DEVICE;
    page_t *submission_pages[1] = {controller->admin_submission_page};
    page_t *completion_pages[1] = {controller->admin_completion_page};
    if (dma_map_pages(controller->device, submission_pages, 1U,
                      DMA_BIDIRECTIONAL, &controller->admin_submission_dma) != K_OK ||
        dma_map_pages(controller->device, completion_pages, 1U,
                      DMA_BIDIRECTIONAL, &controller->admin_completion_dma) != K_OK) {
        nvme_free_admin_pages(controller);
        return K_EIO;
    }
    controller->admin_submission = (nvme_command_t *)phys_to_direct(
        page_to_phys(controller->admin_submission_page));
    controller->admin_completion = (nvme_completion_t *)phys_to_direct(
        page_to_phys(controller->admin_completion_page));
    if (controller->admin_submission == 0 || controller->admin_completion == 0) {
        nvme_free_admin_pages(controller);
        return K_EIO;
    }
    controller->admin_depth = NVME_ADMIN_QUEUE_DEPTH;
    uint16_t maximum = (uint16_t)((controller->capabilities & 0xFFFFU) + 1U);
    if (controller->admin_depth > maximum) controller->admin_depth = maximum;
    if (controller->admin_depth < 2U) {
        nvme_free_admin_pages(controller);
        return K_EINVAL;
    }
    controller->admin_submission_tail = 0;
    controller->admin_completion_head = 0;
    controller->admin_phase = 1;
    atomic_init(&controller->next_command_id, 1U);
    return K_OK;
}

static uint16_t nvme_desired_io_queue_count(const nvme_controller_t *controller) {
    uint32_t count = 1U;
    const x86_acpi_platform_t *platform = x86_acpi_platform();
    if (platform != 0 && platform->cpu_count > count) count = platform->cpu_count;
    if (count > NVME_MAX_IO_QUEUES) count = NVME_MAX_IO_QUEUES;
    if (controller != 0) {
        paddr_t table;
        uint16_t entries;
        if (pci_msix_table(controller->pci, &table, &entries) == K_OK &&
            entries != 0 && count > entries) count = entries;
    }
    return (uint16_t)count;
}

static kstatus_t nvme_set_io_queue_count(nvme_controller_t *controller,
                                         uint16_t desired, uint16_t *actual) {
    if (controller == 0 || desired == 0 || actual == 0) return K_EINVAL;
    nvme_command_t command = {0};
    command.opcode_flags = NVME_ADMIN_SET_FEATURES;
    command.cdw10 = NVME_FEATURE_NUMBER_QUEUES;
    command.cdw11 = ((uint32_t)(desired - 1U) << 16) | (desired - 1U);
    nvme_completion_t completion = {0};
    kstatus_t status = nvme_admin_submit(controller, &command, &completion);
    if (status != K_OK) return status;
    uint16_t completion_queues = (uint16_t)((completion.result & 0xFFFFU) + 1U);
    uint16_t submission_queues = (uint16_t)(((completion.result >> 16) & 0xFFFFU) + 1U);
    if (completion_queues == 0 || submission_queues == 0) return K_EIO;
    *actual = completion_queues < submission_queues ? completion_queues : submission_queues;
    if (*actual > desired) *actual = desired;
    return K_OK;
}

static void nvme_try_bind_msix(nvme_controller_t *controller,
                               nvme_queue_t *queue, uint16_t queue_index) {
    if (controller == 0 || queue == 0 || controller->pci == 0) return;
    paddr_t table;
    uint16_t entries;
    if (pci_msix_table(controller->pci, &table, &entries) != K_OK ||
        queue_index >= entries) return;
    uint8_t vector = (uint8_t)(0x60U + queue_index);
    if (vector > IRQ_VECTOR_LAST ||
        irq_register(vector, nvme_msix_handler, controller) != K_OK) return;
    if (pci_msix_configure(controller->pci, queue_index,
                           x86_current_apic_id(), vector) != K_OK) {
        (void)irq_unregister(vector, nvme_msix_handler, controller);
        return;
    }
    queue->irq_vector = vector;
}

bool nvme_rebind_msix(nvme_controller_t *controller,
                      nvme_queue_t *queue, uint16_t msix_entry,
                      uint8_t new_vector) {
    uint8_t old_vector;
    bool restored = false;
    if (controller == 0 || queue == 0 || controller->pci == 0 ||
        queue->irq_vector == 0U || new_vector < IRQ_VECTOR_FIRST ||
        new_vector > IRQ_VECTOR_LAST || new_vector == queue->irq_vector) {
        return new_vector == (queue != 0 ? queue->irq_vector : 0U);
    }
    old_vector = queue->irq_vector;

    /* 鍏堝睆钄借澶囷紝鍐嶆媶闄ゆ棫璺敱锛岄伩鍏嶆崲缁戠獥鍙ｄ骇鐢熸湭鐭ヤ腑鏂€?*/
    if (pci_msix_mask(controller->pci, msix_entry, true) != K_OK ||
        irq_unregister(old_vector, nvme_msix_handler, controller) != K_OK) {
        (void)pci_msix_mask(controller->pci, msix_entry, false);
        return false;
    }
    if (irq_register(new_vector, nvme_msix_handler, controller) == K_OK &&
        pci_msix_configure(controller->pci, msix_entry, x86_current_apic_id(),
                           new_vector) == K_OK) {
        queue->irq_vector = new_vector;
        return true;
    }

    /* 鏂拌矾鐢卞け璐ユ椂鎭㈠鏃ц矾鐢憋紱鎭㈠澶辫触鍒欎繚鎸佹棤涓柇鐘舵€侊紝鐢辫疆璇㈣矾寰勫厹搴曘€?*/
    (void)irq_unregister(new_vector, nvme_msix_handler, controller);
    if (irq_register(old_vector, nvme_msix_handler, controller) == K_OK &&
        pci_msix_configure(controller->pci, msix_entry, x86_current_apic_id(),
                           old_vector) == K_OK) {
        restored = true;
        queue->irq_vector = old_vector;
    } else {
        queue->irq_vector = 0U;
    }
    return restored;
}

bool nvme_msix_rebind_self_test(const nvme_controller_t *controller) {
    paddr_t table;
    uint16_t entries;
    bool rebound = false;
    if (controller == 0 || controller->pci == 0) return false;
    if (pci_msix_table(controller->pci, &table, &entries) != K_OK) {
        /* 娌℃湁 MSI-X 鐨勬帶鍒跺櫒浣跨敤杞瀹屾垚璺緞锛屼笉闇€瑕佹崲缁戜腑鏂€?*/
        return true;
    }
    (void)table;
    for (uint16_t index = 0; index < controller->io_queue_count; ++index) {
        nvme_queue_t *queue = (nvme_queue_t *)&controller->io_queues[index];
        uint8_t old_vector = queue->irq_vector;
        uint8_t alternate = (uint8_t)(0xA0U + index);
        if (old_vector == 0U || index >= entries || alternate > IRQ_VECTOR_LAST) {
            continue;
        }
        rebound = true;
        if (!nvme_rebind_msix((nvme_controller_t *)controller, queue, index,
                              alternate) ||
            queue->irq_vector != alternate ||
            !nvme_rebind_msix((nvme_controller_t *)controller, queue, index,
                              old_vector) ||
            queue->irq_vector != old_vector) {
            return false;
        }
    }
    return rebound;
}

static kstatus_t nvme_create_io_queues(nvme_controller_t *controller,
                                       uint16_t queue_count) {
    if (controller == 0 || controller->namespace_count == 0) return K_ENOENT;
    if (queue_count == 0 || queue_count > NVME_MAX_IO_QUEUES) return K_EINVAL;
    if (!nvme_free_io_queues(controller)) return K_EBUSY;

    for (uint16_t index = 0; index < queue_count; ++index) {
        nvme_queue_t *queue = &controller->io_queues[index];
        kstatus_t status = nvme_alloc_io_queue(controller, queue,
                                                (uint16_t)(index + 1U));
        if (status != K_OK) {
            if (!nvme_free_io_queues(controller)) return K_EBUSY;
            return status;
        }
        nvme_try_bind_msix(controller, queue, index);

        nvme_command_t command = {0};
        command.opcode_flags = NVME_ADMIN_CREATE_CQ;
        command.prp1 = queue->completion_dma.segments[0].addr.value;
        /* CDW10锛氫綆 16 浣嶄负 CQID锛岄珮 16 浣嶄负 QSIZE銆?*/
        command.cdw10 = ((uint32_t)(queue->depth - 1U) << 16) |
                        queue->queue_id;
        uint32_t cq_flags = queue->irq_vector != 0 ? 3U : 1U;
        /* CQ 鍛戒护鐨勯珮 16 浣嶆槸 MSI-X 琛ㄩ」绱㈠紩锛屼笉鏄?IDT 鍚戦噺鍙枫€?*/
        command.cdw11 = ((uint32_t)index << 16) | cq_flags;
        nvme_completion_t completion = {0};
        g_nvme_last_stage = 710U + index * 2U;
        status = nvme_admin_submit(controller, &command, &completion);
        if (status != K_OK) {
            if (!nvme_free_io_queues(controller)) return K_EBUSY;
            return status;
        }

        command = (nvme_command_t){0};
        command.opcode_flags = NVME_ADMIN_CREATE_SQ;
        command.prp1 = queue->submission_dma.segments[0].addr.value;
        /* CDW10锛氫綆 16 浣嶄负 SQID锛岄珮 16 浣嶄负 QSIZE銆?*/
        command.cdw10 = ((uint32_t)(queue->depth - 1U) << 16) |
                        queue->queue_id;
        command.cdw11 = ((uint32_t)queue->queue_id << 16) | 1U;
        completion = (nvme_completion_t){0};
        g_nvme_last_stage = 711U + index * 2U;
        status = nvme_admin_submit(controller, &command, &completion);
        if (status != K_OK) {
            nvme_free_io_queues(controller);
            return status;
        }
        queue->active = true;
        ++controller->io_queue_count;
    }
    atomic_store_explicit(&controller->next_io_queue, 0U, memory_order_release);
    return K_OK;
}


static kstatus_t nvme_stop(nvme_controller_t *controller) {
    if (controller == 0 || controller->registers == 0) return K_EINVAL;
    if ((nvme_read32(controller, NVME_REG_CC) & NVME_CC_ENABLE) == 0) return K_OK;
    nvme_write32(controller, NVME_REG_CC, 0);
    return nvme_wait_ready(controller, false);
}

static kstatus_t nvme_start(device_t *device) {
    g_nvme_last_stage = 1U;
    if (device == 0 || device->driver_data == 0) return nvme_record_status(K_EINVAL);
    nvme_controller_t *controller = (nvme_controller_t *)device->driver_data;
    g_nvme_last_stage = 2U;
    kstatus_t status = pci_enable_memory_busmaster(controller->pci);
    if (status != K_OK) return nvme_record_status(status);
    controller->capabilities = nvme_read64(controller, NVME_REG_CAP);
    controller->doorbell_stride = 4U << ((controller->capabilities >> 32) & 0xFU);
    if (controller->doorbell_stride == 0 || controller->doorbell_stride > 4096U) {
        return nvme_record_status(K_EINVAL);
    }
    g_nvme_last_stage = 3U;
    status = nvme_stop(controller);
    if (status != K_OK) return nvme_record_status(status);
    bool queues_released = nvme_free_io_queues(controller);
    bool admin_released = nvme_free_admin_pages(controller);
    if (!queues_released || !admin_released) {
        return nvme_record_status(K_EBUSY);
    }
    g_nvme_last_stage = 4U;
    status = nvme_alloc_admin_pages(controller);
    if (status != K_OK) return nvme_record_status(status);
    nvme_write32(controller, NVME_REG_AQA,
                 ((uint32_t)(controller->admin_depth - 1U) << 16) |
                 (controller->admin_depth - 1U));
    nvme_write64(controller, NVME_REG_ASQ,
                 controller->admin_submission_dma.segments[0].addr.value);
    nvme_write64(controller, NVME_REG_ACQ,
                 controller->admin_completion_dma.segments[0].addr.value);
    /* IOSQES=6銆両OCQES=4锛孧PS=0锛孋SS=0銆?*/
    nvme_write32(controller, NVME_REG_CC, (6U << 16) | (4U << 20) | NVME_CC_ENABLE);
    g_nvme_last_stage = 5U;
    status = nvme_wait_ready(controller, true);
    if (status != K_OK) return nvme_record_status(status);
    controller->started = true;
    g_nvme_last_stage = 6U;
    status = nvme_identify_controller(controller);
    if (status != K_OK) {
        controller->started = false;
        (void)nvme_stop(controller);
        if (!nvme_free_io_queues(controller) ||
            !nvme_free_admin_pages(controller)) status = K_EBUSY;
        return nvme_record_status(status);
    }
    if (controller->namespace_count != 0U) {
        status = nvme_identify_namespace(controller);
        if (status != K_OK) {
            controller->started = false;
            (void)nvme_stop(controller);
            if (!nvme_free_io_queues(controller) ||
                !nvme_free_admin_pages(controller)) status = K_EBUSY;
            return nvme_record_status(status);
        }
    }
    if (controller->namespace_count != 0) {
        g_nvme_last_stage = 7U;
        uint16_t desired = nvme_desired_io_queue_count(controller);
        uint16_t actual = 0;
        status = nvme_set_io_queue_count(controller, desired, &actual);
        if (status == K_OK) status = nvme_create_io_queues(controller, actual);
        if (status != K_OK) {
            controller->started = false;
            (void)nvme_stop(controller);
            if (!nvme_free_io_queues(controller) ||
                !nvme_free_admin_pages(controller)) status = K_EBUSY;
            return nvme_record_status(status);
        }
    }
    g_nvme_last_status = K_OK;
    return K_OK;
}

static bool nvme_stop_device_checked(device_t *device) {
    if (device == 0 || device->driver_data == 0) return false;
    nvme_controller_t *controller = (nvme_controller_t *)device->driver_data;
    controller->started = false;
    kstatus_t stop_status = nvme_stop(controller);
    bool queues_released = nvme_free_io_queues(controller);
    bool admin_released = nvme_free_admin_pages(controller);
    return stop_status == K_OK && queues_released && admin_released;
}

static void nvme_stop_device(device_t *device) {
    (void)nvme_stop_device_checked(device);
}

kstatus_t nvme_recover_after_timeout(device_t *device) {
    if (device == 0 || device->driver_data == 0) return K_EINVAL;

    nvme_controller_t *controller =
        (nvme_controller_t *)device->driver_data;

    /*
     * nvme_start() 禁用控制器并拆除旧队列时，忽略迟到的 MSI-X。只有控制器不再允许
     * 执行 DMA 后，nvme_free_io_queues() 才会中止待处理映射。
     */
    controller->started = false;

    kstatus_t status = nvme_start(device);
    g_nvme_last_status = status;
    return status;
}


void nvme_ring_io_submission(nvme_controller_t *controller,
                             const nvme_queue_t *queue) {
    nvme_queue_ring_submission(controller, queue);
}

void nvme_ring_io_completion(nvme_controller_t *controller,
                             const nvme_queue_t *queue) {
    nvme_queue_ring_completion(controller, queue);
}


bool nvme_read_lba0_self_test(const nvme_controller_t *controller,
                              uint32_t forced_queue) {
    if (controller->device == 0 || controller->namespace_count == 0) return false;
    if (forced_queue != UINT32_MAX && forced_queue >= controller->io_queue_count) {
        return false;
    }

    page_t *page = page_alloc(0, PAGE_ALLOC_ZERO | PAGE_ALLOC_DMA32);
    if (page == 0) return false;
    void *buffer = phys_to_direct(page_to_phys(page));
    if (buffer == 0) {
        page_free(page);
        return false;
    }

    io_vec_t io_vector = { .base = buffer, .length = 512U };
    io_request_t request;
    io_request_init(&request, IO_READ, controller->device, 0, &io_vector, 1U);
    bio_vec_t bio_vector = { .page = page, .offset = 0U, .length = 512U };
    bio_t bio = {0};
    bio.lba = 0;
    bio.op = BIO_OP_READ;
    bio.vecs = &bio_vector;
    bio.vec_count = 1U;
    bio.io = &request;

    request.completion_target = &bio;
    if (forced_queue != UINT32_MAX) {
        request.flags = IOREQ_BLOCK_QUEUE_VALID |
                        ((forced_queue & IOREQ_BLOCK_QUEUE_MASK) <<
                         IOREQ_BLOCK_QUEUE_SHIFT);
    }
    kstatus_t status = io_submit(&request);
    bool page_safe = true;
    g_nvme_last_stage = 900U;
    if (status == K_OK) {
        uint64_t deadline = nvme_timeout_deadline(controller);
        while (!io_request_is_terminal(&request) &&
               !nvme_deadline_reached(deadline)) {
            /* 鍚姩闃舵鍙兘杩樻病鏈夊彲璋冨害绾跨▼锛岀洿鎺ユ秷璐规湁闄?deferred 閰嶉銆?*/
            (void)nvme_schedule_deferred_poll();
            (void)deferred_run(8U);
            __asm__ volatile ("pause");
        }
        if (!io_request_is_terminal(&request)) {
            status = K_ETIMEDOUT;
            g_nvme_last_stage = 901U;
        } else {
            status = request.status;
            g_nvme_last_stage = 902U +
                atomic_load_explicit(&request.state, memory_order_acquire);
        }
    }
    g_nvme_last_status = status;
    if (status != K_OK &&
        !io_request_is_terminal(&request)) {
        (void)io_cancel(&request);
    }
    if (status == K_ETIMEDOUT) {
        /* 瓒呮椂鍚庤澶囦粛鍙兘鎸佹湁 PRP锛屽繀椤诲厛鍋滄帶鍒跺櫒鍐嶉噴鏀炬祴璇曢〉銆?*/
        page_safe = nvme_stop_device_checked(controller->device);
        while (!io_request_is_terminal(&request)) {
            (void)nvme_schedule_deferred_poll();
            (void)deferred_run(8U);
            __asm__ volatile ("pause");
        }
    }
    bool success = status == K_OK && request.status == K_OK &&
                   request.bytes_done == 512U &&
                   atomic_load_explicit(&request.state, memory_order_acquire) ==
                       IOREQ_COMPLETED;
    if (page_safe) page_free(page);
    return success;
}

static kstatus_t nvme_reset(device_t *device, uint32_t level) {
    (void)level;
    if (device == 0 || device->driver_data == 0) return K_EINVAL;
    nvme_controller_t *controller = (nvme_controller_t *)device->driver_data;
    controller->started = false;
    return nvme_start(device);
}

static const device_ops_t g_nvme_device_ops = {
    .start = nvme_start,
    .stop = nvme_stop_device,
    .submit_io = nvme_submit_io,
    .reset = nvme_reset,
    .set_power = 0,
};

static kstatus_t nvme_probe(device_t *device) {
    if (device == 0 || device->bus_data == 0) return K_EINVAL;
    pci_device_t *pci = (pci_device_t *)device->bus_data;
    if (!nvme_pci_is_controller(pci)) {
        return K_ENOENT;
    }
    g_nvme_hardware_seen = true;
    nvme_controller_t *controller = (nvme_controller_t *)kzalloc(sizeof(*controller), 0);
    if (controller == 0) return K_ENOMEM;
    controller->device = device;
    controller->pci = pci;
    atomic_init(&controller->admin_lock.state, 0U);
    atomic_init(&controller->next_io_queue, 0U);
    kstatus_t status = nvme_map_registers(controller);
    if (status != K_OK) {
        g_nvme_last_status = status;
        kfree(controller);
        return status;
    }
    device->driver_data = controller;
    device->ops = &g_nvme_device_ops;
    for (uint32_t i = 0; i < NVME_MAX_CONTROLLERS; ++i) {
        if (g_nvme_controllers[i] == 0) {
            g_nvme_controllers[i] = controller;
            return K_OK;
        }
    }
    device->driver_data = 0;
    device->ops = 0;
    nvme_unmap_registers(controller);
    kfree(controller);
    g_nvme_last_status = K_ENOMEM;
    return K_ENOMEM;
}

static void nvme_remove(device_t *device) {
    if (device == 0 || device->driver_data == 0) return;
    nvme_controller_t *controller = (nvme_controller_t *)device->driver_data;
    if (!nvme_stop_device_checked(device)) return;
    for (uint32_t i = 0; i < NVME_MAX_CONTROLLERS; ++i) {
        if (g_nvme_controllers[i] == controller) g_nvme_controllers[i] = 0;
    }
    nvme_unmap_registers(controller);
    device->driver_data = 0;
    device->ops = 0;
    kfree(controller);
}

kstatus_t nvme_driver_register(void) {
    unsigned expected = 0U;
    if (!atomic_compare_exchange_strong_explicit(&g_nvme_driver_state, &expected, 1U,
                                                 memory_order_acq_rel,
                                                 memory_order_acquire)) {
        while (atomic_load_explicit(&g_nvme_driver_state, memory_order_acquire) == 1U) {
            __asm__ volatile ("pause");
        }
        return K_OK;
    }
    driver_object_init(&g_nvme_driver, "nvme", nvme_probe, nvme_remove);
    kstatus_t status = driver_register(&g_nvme_driver);
    atomic_store_explicit(&g_nvme_driver_state, 2U, memory_order_release);
    return status;
}

const nvme_controller_t *nvme_active_controller(void) {
    for (uint32_t i = 0; i < NVME_MAX_CONTROLLERS; ++i) {
        if (g_nvme_controllers[i] != 0 && g_nvme_controllers[i]->started) {
            return g_nvme_controllers[i];
        }
    }
    return 0;
}

nvme_controller_t *nvme_controller_at(uint32_t index) {
    if (index >= NVME_MAX_CONTROLLERS) return 0;
    return g_nvme_controllers[index];
}

bool nvme_all_controllers_ready(void) {
    for (uint32_t i = 0; i < NVME_MAX_CONTROLLERS; ++i) {
        nvme_controller_t *controller = g_nvme_controllers[i];
        if (controller != 0 && (!controller->started || !controller->identified)) {
            return false;
        }
    }
    return true;
}

kstatus_t nvme_last_error(void) {
    return g_nvme_last_status;
}

uint32_t nvme_last_stage(void) {
    return g_nvme_last_stage;
}

uint16_t nvme_last_completion_status(void) {
    return g_nvme_last_completion;
}
