#include <nv.h>
#include <nv-priv.h>

#include <kernel/deferred.h>

#include "../../port/include/nv_port.h"

/*
 * 595.84 Unix RM 会向 os_queue_work_item() 传递不透明队列。第 4 阶段桥接层特意将
 * nv_state_t::queue 保持为 NULL，以选择厂商全局队列。使用现有系统延迟工作线程处理该队列，
 * 不另建 NVIDIA 私有调度器。
 *
 * 池大小符合厂商 OS_MAX_QUEUED_WORKITEMS 约定。将分发记录及其专用 NVIDIA 栈都保存在池中，
 * 还能让原子/中断调用方在入队时无需从内核堆分配内存。
 */
#define NV_RM_WORK_CAPACITY 32U

typedef struct nv_rm_work_entry {
    nv_port_work_t work;
    nvidia_stack_t stack;
    void *data;
    atomic_bool in_use;
    atomic_bool completed;
} nv_rm_work_entry_t;

static nv_port_once_t g_nv_rm_work_once = { ATOMIC_VAR_INIT(0U) };
static nv_port_spinlock_t g_nv_rm_work_lock;
static atomic_bool g_nv_rm_work_flushing = ATOMIC_VAR_INIT(false);
static atomic_uint g_nv_rm_work_publishers = ATOMIC_VAR_INIT(0U);
static nv_rm_work_entry_t g_nv_rm_work_entries[NV_RM_WORK_CAPACITY];

static void nv_rm_work_init_once(void *argument)
{
    uint32_t index;

    (void)argument;
    nv_port_spin_init(&g_nv_rm_work_lock);
    atomic_init(&g_nv_rm_work_flushing, false);
    atomic_init(&g_nv_rm_work_publishers, 0U);
    for (index = 0U; index < NV_RM_WORK_CAPACITY; ++index) {
        atomic_init(&g_nv_rm_work_entries[index].in_use, false);
        atomic_init(&g_nv_rm_work_entries[index].completed, false);
    }
}

static NV_STATUS nv_rm_work_ready(void)
{
    return nv_port_once_run(&g_nv_rm_work_once,
                            nv_rm_work_init_once, NULL) == K_OK ?
           NV_OK : NV_ERR_OPERATING_SYSTEM;
}

static void nv_rm_work_reclaim_locked(nv_rm_work_entry_t *entry)
{
    unsigned state;

    if (entry == NULL ||
        !atomic_load_explicit(&entry->in_use, memory_order_acquire) ||
        !atomic_load_explicit(&entry->completed, memory_order_acquire))
        return;

    state = atomic_load_explicit(&entry->work.state, memory_order_acquire);
    if (state != NV_PORT_WORK_IDLE) return;

    entry->data = NULL;
    atomic_store_explicit(&entry->completed, false, memory_order_release);
    atomic_store_explicit(&entry->in_use, false, memory_order_release);
}

static nv_rm_work_entry_t *nv_rm_work_alloc_locked(void)
{
    uint32_t index;

    for (index = 0U; index < NV_RM_WORK_CAPACITY; ++index) {
        nv_rm_work_entry_t *entry = &g_nv_rm_work_entries[index];

        nv_rm_work_reclaim_locked(entry);
        if (!atomic_load_explicit(&entry->in_use, memory_order_acquire)) {
            atomic_store_explicit(&entry->in_use, true, memory_order_release);
            return entry;
        }
    }
    return NULL;
}

static void nv_rm_work_execute(void *argument)
{
    nv_rm_work_entry_t *entry = (nv_rm_work_entry_t *)argument;

    if (entry == NULL) return;

    /* rm_execute_work_item() 是厂商指定的分发函数。根据 595.84 约定，它负责处理
     * nv_work_item_t 及其 pData。 */
    rm_execute_work_item(&entry->stack, entry->data);
    entry->data = NULL;
    atomic_store_explicit(&entry->completed, true, memory_order_release);
}

static kstatus_t nv_rm_work_drain_entry(
    nv_rm_work_entry_t *entry)
{
    if (entry == NULL)
        return K_EINVAL;

    for (;;) {
        unsigned state = atomic_load_explicit(&entry->work.state,
                                              memory_order_acquire);
        if (state == NV_PORT_WORK_IDLE || state == NV_PORT_WORK_CANCELLED)
            return K_OK;

        /* 第 4 阶段会在 System 启动持久延迟工作线程前探测 RM。现有延迟执行器明确支持
         * 在引导早期安全运行，因此使用它执行真正排队的回调。若工作线程尚未启动且无法
         * 运行任何项目，应报告此情况，而不是永久自旋或谎称已完成刷新。 */
        if (!deferred_worker_started()) {
            if (deferred_run(64U) == 0U)
                return K_EAGAIN;
            continue;
        }

        return nv_port_work_drain(&entry->work, UINT64_MAX);
    }
}

NV_STATUS NV_API_CALL os_queue_work_item(struct os_work_queue *queue,
                                          void *data)
{
    nv_rm_work_entry_t *entry;
    kstatus_t status;

    /* 生命周期的后续阶段才会为每个 GPU 创建独立队列。 */
    if (queue != NULL || data == NULL) return NV_ERR_NOT_SUPPORTED;
    if (nv_rm_work_ready() != NV_OK) return NV_ERR_OPERATING_SYSTEM;

    nv_port_spin_lock(&g_nv_rm_work_lock);
    if (atomic_load_explicit(&g_nv_rm_work_flushing,
                             memory_order_acquire)) {
        nv_port_spin_unlock(&g_nv_rm_work_lock);
        return NV_ERR_NOT_READY;
    }

    entry = nv_rm_work_alloc_locked();
    if (entry == NULL) {
        nv_port_spin_unlock(&g_nv_rm_work_lock);
        return NV_ERR_INSUFFICIENT_RESOURCES;
    }

    entry->data = data;
    entry->stack.size = (NvU32)sizeof(entry->stack.stack);
    entry->stack.top = entry->stack.stack + entry->stack.size;
    atomic_store_explicit(&entry->completed, false, memory_order_release);
    nv_port_work_init(&entry->work, nv_rm_work_execute, entry);

    /* 单独跟踪发布者，以便实际入队操作不必持有池锁。工作回调可以继续排入后续项目；
     * 若在入队期间一直持有此锁，这种有效的厂商调用路径就会死锁。 */
    atomic_fetch_add_explicit(&g_nv_rm_work_publishers, 1U,
                              memory_order_acq_rel);
    nv_port_spin_unlock(&g_nv_rm_work_lock);

    status = nv_port_work_queue(&entry->work);

    nv_port_spin_lock(&g_nv_rm_work_lock);
    if (status != K_OK) {
        entry->data = NULL;
        atomic_store_explicit(&entry->in_use, false, memory_order_release);
    }
    atomic_fetch_sub_explicit(&g_nv_rm_work_publishers, 1U,
                              memory_order_acq_rel);
    nv_port_spin_unlock(&g_nv_rm_work_lock);
    return status == K_OK ? NV_OK :
           (status == K_ENOSPC ? NV_ERR_INSUFFICIENT_RESOURCES :
                                 NV_ERR_OPERATING_SYSTEM);
}

NV_STATUS NV_API_CALL os_flush_work_queue(struct os_work_queue *queue,
                                           NvBool is_unload)
{
    nv_rm_work_entry_t *pending[NV_RM_WORK_CAPACITY];
    uint32_t pending_count = 0U;
    uint32_t index;
    NV_STATUS result = NV_OK;

    (void)is_unload;
    if (queue != NULL) return NV_ERR_NOT_SUPPORTED;
    if (nv_rm_work_ready() != NV_OK) return NV_ERR_OPERATING_SYSTEM;

    nv_port_spin_lock(&g_nv_rm_work_lock);
    /* 只有刷新所有者可以重新开放发布。第二个刷新操作不得获取竞争快照并清除所有者的
     * 接纳屏障。 */
    if (atomic_load_explicit(&g_nv_rm_work_flushing, memory_order_acquire)) {
        nv_port_spin_unlock(&g_nv_rm_work_lock);
        return NV_ERR_NOT_READY;
    }
    atomic_store_explicit(&g_nv_rm_work_flushing, true,
                          memory_order_release);
    nv_port_spin_unlock(&g_nv_rm_work_lock);

    /* 生产者可能在获取池锁前刚好通过刷新状态检查。取得刷新快照前，先等待该生产者完成
     * 入队或释放其槽位。 */
    while (atomic_load_explicit(&g_nv_rm_work_publishers,
                                memory_order_acquire) != 0U)
        __asm__ volatile ("pause");

    nv_port_spin_lock(&g_nv_rm_work_lock);
    for (index = 0U; index < NV_RM_WORK_CAPACITY; ++index) {
        nv_rm_work_entry_t *entry = &g_nv_rm_work_entries[index];

        if (atomic_load_explicit(&entry->in_use, memory_order_acquire))
            pending[pending_count++] = entry;
    }
    nv_port_spin_unlock(&g_nv_rm_work_lock);

    for (index = 0U; index < pending_count; ++index) {
        if (nv_rm_work_drain_entry(pending[index]) != K_OK) {
            result = NV_ERR_OPERATING_SYSTEM;
            break;
        }
    }

    nv_port_spin_lock(&g_nv_rm_work_lock);
    for (index = 0U; index < pending_count; ++index)
        nv_rm_work_reclaim_locked(pending[index]);
    atomic_store_explicit(&g_nv_rm_work_flushing, false,
                          memory_order_release);
    nv_port_spin_unlock(&g_nv_rm_work_lock);
    return result;
}

NvBool NV_API_CALL os_is_queue_flush_ongoing(struct os_work_queue *queue)
{
    if (queue != NULL) return NV_FALSE;
    if (nv_rm_work_ready() != NV_OK) return NV_TRUE;
    return atomic_load_explicit(&g_nv_rm_work_flushing,
                                memory_order_acquire) ? NV_TRUE : NV_FALSE;
}
