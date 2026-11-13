#include <kernel/block.h>
#include <kernel/device.h>
#include <kernel/telemetry.h>
#include <arch/x86_64/acpi.h>
#include <kernel/wait.h>
#include <kernel/sched.h>

#define BLOCK_DEVICE_QUEUE_LIMIT 64U
#define BLOCK_QUEUE_MAX_COUNT     8U

typedef struct block_queue_state {
    struct device *device;
    atomic_uint next_queue;
    uint16_t queue_count;
} block_queue_state_t;

static block_queue_state_t g_block_queues[BLOCK_DEVICE_QUEUE_LIMIT];
static uint32_t g_block_queue_count;
static spinlock_t g_block_lock;
static atomic_uint g_block_init_state;
static wait_queue_t g_block_completion_queue;

static void block_lock(void) {
    sched_preempt_disable();
    spinlock_lock(&g_block_lock);
}

static void block_unlock(void) {
    spinlock_unlock(&g_block_lock);
    sched_preempt_enable();
}

static bool block_request_completed(void *context) {
    io_request_t *request = (io_request_t *)context;
    if (request == 0) return true;
    /* 回调发生在 COMPLETING 阶段，必须等 finalize 写入终态后再返回。 */
    return io_request_is_terminal(request);
}

static void block_request_notify(void) {
    (void)wake_all(&g_block_completion_queue);
}

static void block_initialize(void) {
    /* Acquire 与队列及等待队列状态的发布配对。
     * 普通 BIO 提交不得修改 once-state 所在的缓存行。 */
    if (atomic_load_explicit(&g_block_init_state, memory_order_acquire) == 2U) return;
    unsigned expected = 0U;
    if (atomic_compare_exchange_strong_explicit(&g_block_init_state, &expected, 1U,
                                                memory_order_acq_rel,
                                                memory_order_acquire)) {
        spinlock_init(&g_block_lock);
        wait_queue_init(&g_block_completion_queue);
        g_block_queue_count = 0U;
        for (uint32_t i = 0; i < BLOCK_DEVICE_QUEUE_LIMIT; ++i) {
            g_block_queues[i].device = 0;
            g_block_queues[i].queue_count = 0;
            atomic_init(&g_block_queues[i].next_queue, 0U);
        }
        atomic_store_explicit(&g_block_init_state, 2U, memory_order_release);
        return;
    }
    while (atomic_load_explicit(&g_block_init_state, memory_order_acquire) != 2U) {
        __asm__ volatile ("pause");
    }
}

static block_queue_state_t *block_queue_for(struct device *device) {
    if (device == 0) return 0;
    block_initialize();
    block_lock();
    /* 槽位只追加，因此只有已发布的前缀可能包含匹配项。 */
    for (uint32_t i = 0; i < g_block_queue_count; ++i) {
        if (g_block_queues[i].device == device) {
            block_unlock();
            return &g_block_queues[i];
        }
    }
    block_queue_state_t *free_slot = g_block_queue_count < BLOCK_DEVICE_QUEUE_LIMIT ?
        &g_block_queues[g_block_queue_count] : 0;
    if (free_slot != 0) {
        const x86_acpi_platform_t *platform = x86_acpi_platform();
        uint32_t count = platform != 0 ? platform->cpu_count : 1U;
        if (count == 0) count = 1U;
        if (count > BLOCK_QUEUE_MAX_COUNT) count = BLOCK_QUEUE_MAX_COUNT;
        free_slot->device = device;
        free_slot->queue_count = (uint16_t)count;
        atomic_store_explicit(&free_slot->next_queue, 0U, memory_order_release);
        ++g_block_queue_count;
    }
    block_unlock();
    return free_slot;
}

static bool bio_page_vectors_valid(const bio_t *bio) {
    if (bio->vec_count != 0 && bio->vecs == 0) return false;
    for (uint16_t i = 0; i < bio->vec_count; ++i) {
        const bio_vec_t *vec = &bio->vecs[i];
        if (vec->page == 0 || (vec->page->flags & PAGE_FREE) != 0 ||
            (vec->page->flags & PAGE_COMPOUND_TAIL) != 0) return false;
        uint64_t page_capacity = PAGE_SIZE;
        if ((vec->page->flags & PAGE_COMPOUND_HEAD) != 0U) {
            if (vec->page->order > BUDDY_MAX_ORDER) return false;
            page_capacity <<= vec->page->order;
        }
        if (vec->offset >= page_capacity || vec->length == 0 ||
            vec->length > page_capacity - vec->offset) return false;
        if (page_to_phys(vec->page).value == UINT64_MAX) return false;
    }
    /* 每个通过验证的向量长度都非零，因此无需累加字节数。 */
    return bio->vec_count != 0 || bio->op == BIO_OP_FLUSH;
}

static bool bio_request_ready(const bio_t *bio) {
    if (bio == 0 || bio->io == 0 || bio->io->device == 0 ||
        atomic_load_explicit(&bio->io->state, memory_order_acquire) != IOREQ_NEW ||
        bio->io->completion_target != 0) return false;
    return true;
}

static bool bio_submission_valid(const bio_t *bio) {
    if (!bio_request_ready(bio) ||
        (bio->op != BIO_OP_READ && bio->op != BIO_OP_WRITE &&
         bio->op != BIO_OP_FLUSH)) return false;
    if ((bio->op == BIO_OP_READ && bio->io->opcode != IO_READ) ||
        (bio->op == BIO_OP_WRITE && bio->io->opcode != IO_WRITE) ||
        (bio->op == BIO_OP_FLUSH && bio->io->opcode != IO_FLUSH)) return false;
    if (bio->op != BIO_OP_FLUSH && bio->io->vec_count == 0) return false;
    return bio_page_vectors_valid(bio);
}

kstatus_t block_submit_bio(bio_t *bio) {
    if (!bio_submission_valid(bio)) return K_EINVAL;
    block_queue_state_t *queue = block_queue_for(bio->io->device);
    if (queue == 0 || queue->queue_count == 0) return K_ENOMEM;
    uint32_t selected = atomic_fetch_add_explicit(&queue->next_queue, 1U,
                                                  memory_order_relaxed) %
                        queue->queue_count;
    bio->io->flags = (bio->io->flags &
                      ~(IOREQ_BLOCK_QUEUE_VALID |
                        (IOREQ_BLOCK_QUEUE_MASK << IOREQ_BLOCK_QUEUE_SHIFT))) |
                     IOREQ_BLOCK_QUEUE_VALID |
                     ((selected & IOREQ_BLOCK_QUEUE_MASK) << IOREQ_BLOCK_QUEUE_SHIFT);
    /* 设备数据面通过此字段取得经过校验的 BIO，驱动完成前该对象必须保持有效。 */
    if (bio->io->completion_target != 0) return K_EBUSY;
    bio->io->completion_target = bio;
    bio->io->notify = block_request_notify;
    kstatus_t status = io_submit(bio->io);
    unsigned state = atomic_load_explicit(&bio->io->state, memory_order_acquire);
    if (!io_request_is_terminal(bio->io) && state != IOREQ_NEW) {
        status = wait_on_queue(&g_block_completion_queue, block_request_completed,
                               bio->io, UINT64_MAX);
        if (status == K_OK) status = bio->io->status;
    } else if (status == K_OK && io_request_is_terminal(bio->io)) {
        status = bio->io->status;
    }
    if (status != K_OK &&
        atomic_load_explicit(&bio->io->state, memory_order_acquire) == IOREQ_NEW &&
        bio->io->completion_target == bio) {
        bio->io->completion_target = 0;
    }
    return status;
}

kstatus_t block_submit_bio_batch(bio_t *bios, uint32_t count,
                                 uint32_t *submitted) {
    uint32_t completed = 0U;
    if (submitted == 0 || count == 0U || count > BLOCK_BIO_BATCH_MAX || bios == 0) {
        return K_EINVAL;
    }
    *submitted = 0U;
    /* 先完整校验，避免批量调用在第二个元素处才发现明显参数错误。 */
    for (uint32_t i = 0; i < count; ++i) {
        if (!bio_submission_valid(&bios[i])) return K_EINVAL;
    }
    for (; completed < count; ++completed) {
        kstatus_t status = block_submit_bio(&bios[completed]);
        if (status != K_OK) {
            *submitted = completed;
            (void)telemetry_record(TELEMETRY_CATEGORY_STORAGE_BATCH,
                                   bios[0].io->device->device_id, count, completed);
            return status;
        }
    }
    *submitted = completed;
    (void)telemetry_record(TELEMETRY_CATEGORY_STORAGE_BATCH,
                           bios[0].io->device->device_id, count, completed);
    return K_OK;
}

bool block_multiqueue_self_test(void) {
    static device_t first;
    static device_t second;
    block_queue_state_t *a = block_queue_for(&first);
    block_queue_state_t *b = block_queue_for(&second);
    if (a == 0 || b == 0 || a == b || a->queue_count == 0 || b->queue_count == 0) {
        return false;
    }
    uint32_t first_choice = atomic_fetch_add_explicit(&a->next_queue, 1U,
                                                       memory_order_relaxed) %
                            a->queue_count;
    uint32_t second_choice = atomic_fetch_add_explicit(&a->next_queue, 1U,
                                                        memory_order_relaxed) %
                             a->queue_count;
    return a->queue_count == 1U || first_choice != second_choice;
}
