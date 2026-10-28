#include <arch/x86_64/paging.h>
#include <arch/x86_64/uaccess.h>
#include <kernel/futex.h>
#include <kernel/kmem.h>
#include <kernel/sched.h>
#include <kernel/wait.h>
#include <kernel/vm.h>

#define FUTEX_BUCKET_COUNT 256U
#define FUTEX_USER_BASE 0x0000000000010000ULL
#define FUTEX_USER_END  (X86_64_USER_TOP + 1ULL)

typedef struct futex_entry {
    list_head_t node;
    /* 共享键使用物理字；私有键使用地址空间虚拟地址。 */
    uint64_t key;
    vm_space_t *space;
    bool private_key;
    process_t *process;
    uint32_t refs;
    wait_queue_t waitq;
} futex_entry_t;

typedef struct futex_key {
    uint64_t value;
    vm_space_t *space;
    bool private_key;
} futex_key_t;

typedef struct {
    spinlock_t lock;
    list_head_t entries;
} futex_bucket_t;

typedef struct {
    uint32_t __user *address;
    uint32_t expected;
    bool checked;
    bool mismatch;
    kstatus_t fault;
} futex_predicate_t;

static futex_bucket_t g_futex_buckets[FUTEX_BUCKET_COUNT];
static atomic_uint g_futex_init_state;

/*
 * Futex 桶所有权用于保护条目链表。唤醒路径也可能从另一颗 CPU 获取此锁，因此当其他 CPU 正在自旋等待时，
 * 不能将锁持有者调度离开当前 CPU。
 */
static void futex_bucket_lock(spinlock_t *lock) {
    sched_preempt_disable();
    spinlock_lock(lock);
}

static void futex_bucket_unlock(spinlock_t *lock) {
    spinlock_unlock(lock);
    sched_preempt_enable();
}

static futex_entry_t *entry_from_node(list_head_t *node) {
    return (futex_entry_t *)((uint8_t *)node -
                             __builtin_offsetof(futex_entry_t, node));
}

static void futex_init(void) {
    if (atomic_load_explicit(&g_futex_init_state, memory_order_acquire) == 2U) return;
    unsigned expected = 0;
    if (atomic_compare_exchange_strong_explicit(&g_futex_init_state, &expected, 1U,
                                                 memory_order_acq_rel,
                                                 memory_order_acquire)) {
        for (uint32_t i = 0; i < FUTEX_BUCKET_COUNT; ++i) {
            atomic_init(&g_futex_buckets[i].lock.state, 0U);
            list_init(&g_futex_buckets[i].entries);
        }
        atomic_store_explicit(&g_futex_init_state, 2U, memory_order_release);
        return;
    }
    while (atomic_load_explicit(&g_futex_init_state, memory_order_acquire) != 2U) {
        __asm__ volatile ("pause");
    }
}

static uint32_t futex_hash(const futex_key_t *key) {
    uint64_t value = key->value >> 2;
    value ^= key->value >> 21;
    value ^= value >> 32;
    if (key->private_key) {
        uint64_t space = (uint64_t)(uintptr_t)key->space;
        value ^= space >> 4;
        value ^= space >> 21;
        value ^= space >> 32;
    }
    return (uint32_t)value & (FUTEX_BUCKET_COUNT - 1U);
}

static futex_entry_t *find_locked(futex_bucket_t *bucket,
                                  const futex_key_t *key) {
    for (list_head_t *node = bucket->entries.next; node != &bucket->entries;
         node = node->next) {
        futex_entry_t *entry = entry_from_node(node);
        if (entry->key == key->value &&
            entry->private_key == key->private_key &&
            (!key->private_key || entry->space == key->space)) {
            return entry;
        }
    }
    return 0;
}

static kstatus_t futex_resolve_key(process_t *process, vaddr_t address,
                                   bool private_key, futex_key_t *out_key) {
    kstatus_t status;

    if (process == 0 || process->vm == 0 || out_key == 0 ||
        address < FUTEX_USER_BASE || address >= FUTEX_USER_END) {
        return K_EACCES;
    }

    if (private_key) {
        /* Linux 私有 futex 的标识为（地址空间、虚拟地址）。此前每次 pthread 加锁/解锁都会遍历四级页表，
         * 并获取全局页表自旋锁，尽管物理页与该键无关。改为通过 uaccess 验证并访问此字：已映射页面走硬件
         * 地址转换快速路径，缺失页面仍由 VM 正常按需缺页处理。 */
        uint32_t value;
        status = get_user_u32(&value, (const uint32_t __user *)address);
        if (status != K_OK) {
            status = vm_handle_fault(process->vm, &(vm_fault_info_t){
                .address = address,
                .access = VM_PROT_READ,
                .cpu_error = 0,
            });
            if (status != K_OK) return status;
            status = get_user_u32(&value,
                                  (const uint32_t __user *)address);
            if (status != K_OK) return status;
        }

        out_key->value = address;
        out_key->space = process->vm;
        out_key->private_key = true;
        return K_OK;
    }

    paddr_t physical;
    status = x86_translate_page(process->vm->root_table, address, &physical, 0);
    if (status != K_OK) {
        status = vm_handle_fault(process->vm, &(vm_fault_info_t){
            .address = address,
            .access = VM_PROT_READ,
            .cpu_error = 0,
        });
        if (status != K_OK) return status;
        status = x86_translate_page(process->vm->root_table, address,
                                    &physical, 0);
        if (status != K_OK) return status;
    }

    /* 将字节偏移也纳入键，避免同一共享页中的不同字相互唤醒。调用方已保证用户地址按 4 字节对齐。 */
    out_key->value = physical.value;
    out_key->space = 0;
    out_key->private_key = false;
    return K_OK;
}

static kstatus_t futex_get(process_t *process, vaddr_t address, bool create,
                           bool private_key, futex_entry_t **out) {
    futex_key_t key;
    kstatus_t status = futex_resolve_key(process, address, private_key, &key);
    if (status != K_OK) return status;
    uint32_t index = futex_hash(&key);
    futex_bucket_t *bucket = &g_futex_buckets[index];
    futex_entry_t *candidate = 0;
    for (;;) {
        futex_bucket_lock(&bucket->lock);
        futex_entry_t *entry = find_locked(bucket, &key);
        if (entry != 0) {
            ++entry->refs;
            futex_bucket_unlock(&bucket->lock);
            kfree(candidate);
            *out = entry;
            return K_OK;
        }
        if (!create) {
            futex_bucket_unlock(&bucket->lock);
            kfree(candidate);
            return K_ENOENT;
        }
        if (candidate != 0) {
            candidate->key = key.value;
            candidate->space = key.space;
            candidate->private_key = key.private_key;
            candidate->process = process;
            candidate->refs = 1U;
            if (key.private_key) vm_space_get(key.space);
            object_get(process);
            wait_queue_init(&candidate->waitq);
            list_add_tail(&bucket->entries, &candidate->node);
            futex_bucket_unlock(&bucket->lock);
            *out = candidate;
            return K_OK;
        }
        futex_bucket_unlock(&bucket->lock);
        candidate = (futex_entry_t *)kzalloc(sizeof(*candidate), 0);
        if (candidate == 0) return K_ENOMEM;
        list_init(&candidate->node);
    }
}

static void futex_put(futex_entry_t *entry) {
    futex_key_t key = {
        .value = entry->key,
        .space = entry->space,
        .private_key = entry->private_key,
    };
    uint32_t index = futex_hash(&key);
    futex_bucket_t *bucket = &g_futex_buckets[index];
    bool destroy = false;
    futex_bucket_lock(&bucket->lock);
    if (entry->refs != 0 && --entry->refs == 0) {
        list_del(&entry->node);
        destroy = true;
    }
    futex_bucket_unlock(&bucket->lock);
    if (destroy) {
        if (entry->private_key) vm_space_put(entry->space);
        object_put(entry->process);
        kfree(entry);
    }
}

static bool futex_predicate(void *raw_context) {
    futex_predicate_t *context = (futex_predicate_t *)raw_context;
    if (context->checked) return true; /* 被显式唤醒后允许伪唤醒语义。 */
    context->checked = true;
    uint32_t value = 0;
    context->fault = get_user_u32(&value, context->address);
    context->mismatch = context->fault == K_OK && value != context->expected;
    return context->fault != K_OK || context->mismatch;
}

kstatus_t futex_wait_keyed(process_t *process, uint32_t __user *address,
                           uint32_t expected, uint64_t timeout_ns,
                           bool private_key) {
    if (process == 0 || address == 0 || ((uintptr_t)address & 3U) != 0) return K_EINVAL;
    futex_init();
    futex_entry_t *entry = 0;
    kstatus_t status = futex_get(process, (vaddr_t)(uintptr_t)address, true,
                                 private_key, &entry);
    if (status != K_OK) return status;
    futex_predicate_t predicate = {
        .address = address,
        .expected = expected,
        .checked = false,
        .mismatch = false,
        .fault = K_OK,
    };
    status = wait_on_queue(&entry->waitq, futex_predicate, &predicate, timeout_ns);
    futex_put(entry);
    if (predicate.fault != K_OK) return predicate.fault;
    if (predicate.mismatch) return K_EAGAIN;
    return status;
}

kstatus_t futex_wait(process_t *process, uint32_t __user *address,
                     uint32_t expected, uint64_t timeout_ns) {
    return futex_wait_keyed(process, address, expected, timeout_ns, true);
}

kstatus_t futex_wake_keyed(process_t *process, uint32_t __user *address,
                           uint32_t maximum, uint32_t *woken,
                           bool private_key) {
    if (process == 0 || address == 0 || woken == 0 ||
        ((uintptr_t)address & 3U) != 0) return K_EINVAL;
    futex_init();
    *woken = 0;
    if (maximum == 0) return K_OK;
    futex_entry_t *entry = 0;
    /* 没有已注册等待者时，wake 是空操作。等待者会先发布条目，再在等待队列锁下检查用户字，因此并发的
     * store+wake 不会丢失：要么 wake 能看到条目，要么等待者后续谓词会观察到已变化的字。
     * 在此创建条目会导致每次无竞争互斥量解锁都进行分配和释放。 */
    kstatus_t status = futex_get(process, (vaddr_t)(uintptr_t)address, false,
                                 private_key, &entry);
    if (status == K_ENOENT) return K_OK;
    if (status != K_OK) return status;
    while (*woken < maximum && wake_one(&entry->waitq) != 0) ++*woken;
    futex_put(entry);
    return K_OK;
}

kstatus_t futex_wake(process_t *process, uint32_t __user *address,
                     uint32_t maximum, uint32_t *woken) {
    return futex_wake_keyed(process, address, maximum, woken, true);
}
