#include <kernel/io_mutex.h>
#include <arch/x86_64/paging.h>

void io_mutex_init(io_mutex_t *mutex, bool priority_inheritance) {
    kmutex_init(&mutex->scheduled, priority_inheritance);
    atomic_init(&mutex->boot_owned, false);
}

/* 启动阶段的 idle 续执行路径没有可用于休眠的已保存上下文。
 * 遵循现有块缓存协议：未保存上下文的续执行路径等待时，远端所有者仍须收到 BSP 的 TLB 确认。 */
static kstatus_t io_mutex_lock_idle(kmutex_t *mutex) {
    uint64_t flags = 0U;
    bool delivering = false;
    bool already_waiting = false;
    kstatus_t status;
    do {
        status = kmutex_try_lock_bootstrap(mutex);
        if (status == K_EAGAIN) {
            /* idle 续执行路径无法运行其他本地线程来释放互斥锁。拒绝此情况，不要无限自旋；
             * 竞争的启动工作必须使用实际工作线程。 */
            if (!delivering) {
                __asm__ volatile ("pushfq; popq %0" : "=r"(flags) : : "memory");
                sched_preempt_disable();
                already_waiting = x86_tlb_shootdown_active();
                if (!already_waiting) x86_tlb_wait_begin();
                __asm__ volatile ("sti" : : : "memory");
                delivering = true;
            }
            __asm__ volatile ("pause");
        }
    } while (status == K_EAGAIN);
    if (delivering) {
        if ((flags & (1ULL << 9)) == 0U)
            __asm__ volatile ("cli" : : : "memory");
        if (!already_waiting) x86_tlb_wait_end();
        sched_preempt_enable();
    }
    return status;
}

kstatus_t io_mutex_lock(io_mutex_t *mutex) {
    if (mutex == 0 || !mutex->scheduled.initialized) return K_EINVAL;
    thread_t *current = sched_current_thread();
    if (current == 0) {
        /* 拒绝递归获取或跨越启动/运行阶段边界。 */
        if (mutex->scheduled.owner != 0 ||
            atomic_exchange_explicit(&mutex->boot_owned, true,
                                      memory_order_acq_rel)) return K_EBUSY;
        return K_OK;
    }
    if (atomic_load_explicit(&mutex->boot_owned, memory_order_acquire))
        return K_EBUSY;
    if (current->sched_class == SCHED_CLASS_IDLE)
        return io_mutex_lock_idle(&mutex->scheduled);
    return kmutex_lock_uninterruptible(&mutex->scheduled, UINT64_MAX);
}

kstatus_t io_mutex_unlock(io_mutex_t *mutex) {
    if (mutex == 0 || !mutex->scheduled.initialized) return K_EINVAL;
    if (sched_current_thread() == 0) {
        if (!atomic_exchange_explicit(&mutex->boot_owned, false,
                                      memory_order_acq_rel)) return K_EPERM;
        return K_OK;
    }
    if (atomic_load_explicit(&mutex->boot_owned, memory_order_acquire))
        return K_EPERM;
    return kmutex_unlock(&mutex->scheduled);
}
