#include <kernel/file_lock.h>
#include <uapi/linux_abi.h>

/* 在 fd 锁保护下有界运行且不分配内存。容量耗尽时以 ENOLCK 原子失败；
 * 绝不能静默丢弃数据库排他锁。 */
#define FILE_LOCK_CAPACITY 512U
typedef struct record_lock {
    vnode_t *node;
    file_lock_range_t range;
} record_lock_t;
static spinlock_t g_record_lock;
static record_lock_t g_records[FILE_LOCK_CAPACITY];
#define FILE_LOCK_WAIT_CAPACITY 256U
static record_lock_t g_lock_waiters[FILE_LOCK_WAIT_CAPACITY];
/* 临时数据由 g_record_lock 保护，不放在内核栈上。 */
static uint64_t g_wait_owners[FILE_LOCK_CAPACITY + FILE_LOCK_WAIT_CAPACITY];

static bool overlaps(const file_lock_range_t *a, const file_lock_range_t *b) {
    return a->first <= b->last && b->first <= a->last;
}
static bool same_owner(const record_lock_t *r, vnode_t *node, uint64_t owner) {
    return r->node == node && r->range.owner == owner;
}
static record_lock_t *conflict_locked(vnode_t *node,
                                      const file_lock_range_t *request) {
    for (size_t i = 0; i < FILE_LOCK_CAPACITY; ++i) {
        record_lock_t *r = &g_records[i];
        if (r->node == node && r->range.owner != request->owner &&
            overlaps(&r->range, request) &&
            (r->range.type == LINUX_F_WRLCK || request->type == LINUX_F_WRLCK))
            return r;
    }
    return 0;
}

static bool incompatible(const record_lock_t *r, const record_lock_t *waiting) {
    return r->node == waiting->node && r->range.owner != waiting->range.owner &&
           overlaps(&r->range, &waiting->range) &&
           (r->range.type == LINUX_F_WRLCK || waiting->range.type == LINUX_F_WRLCK);
}

kstatus_t file_lock_begin_wait(vnode_t *node, const file_lock_range_t *request,
                               uint32_t *ticket) {
    spinlock_lock(&g_record_lock);
    *ticket = UINT32_MAX;
    if (!conflict_locked(node, request)) {
        spinlock_unlock(&g_record_lock);
        return K_OK;
    }
    size_t slot = FILE_LOCK_WAIT_CAPACITY;
    for (size_t i = 0; i < FILE_LOCK_WAIT_CAPACITY; ++i)
        if (!g_lock_waiters[i].node) { slot = i; break; }
    if (slot == FILE_LOCK_WAIT_CAPACITY) {
        spinlock_unlock(&g_record_lock);
        return K_ENOLCK;
    }
    g_lock_waiters[slot] = (record_lock_t){node, *request};
    /* 遍历所有等待边，包括多个共享锁所有者，以及同一进程中的多个阻塞线程。 */
    size_t count = 1;
    g_wait_owners[0] = request->owner;
    for (size_t cursor = 0; cursor < count; ++cursor) {
        for (size_t w = 0; w < FILE_LOCK_WAIT_CAPACITY; ++w) {
            record_lock_t *waiting = &g_lock_waiters[w];
            if (!waiting->node || waiting->range.owner != g_wait_owners[cursor]) continue;
            for (size_t i = 0; i < FILE_LOCK_CAPACITY; ++i) {
                record_lock_t *r = &g_records[i];
                if (!incompatible(r, waiting)) continue;
                if (r->range.owner == request->owner) {
                    g_lock_waiters[slot].node = 0;
                    spinlock_unlock(&g_record_lock);
                    return K_EDEADLK;
                }
                size_t seen = 0;
                while (seen < count && g_wait_owners[seen] != r->range.owner) ++seen;
                if (seen == count) g_wait_owners[count++] = r->range.owner;
            }
        }
    }
    /* 系统调用固定的 fd 在整个等待期间持有该节点。 */
    *ticket = (uint32_t)slot;
    spinlock_unlock(&g_record_lock);
    return K_OK;
}

void file_lock_end_wait(uint32_t ticket) {
    if (ticket == UINT32_MAX) return;
    spinlock_lock(&g_record_lock);
    g_lock_waiters[ticket].node = 0;
    spinlock_unlock(&g_record_lock);
}
kstatus_t file_lock_query(vnode_t *node, const file_lock_range_t *request,
                          file_lock_range_t *conflict) {
    spinlock_lock(&g_record_lock);
    record_lock_t *r = conflict_locked(node, request);
    *conflict = r ? r->range : (file_lock_range_t){.type = LINUX_F_UNLCK};
    spinlock_unlock(&g_record_lock);
    return K_OK;
}
static record_lock_t *empty_locked(void) {
    for (size_t i = 0; i < FILE_LOCK_CAPACITY; ++i)
        if (!g_records[i].node) return &g_records[i];
    return 0;
}
kstatus_t file_lock_set(vnode_t *node, const file_lock_range_t *request) {
    size_t available = 0, needed = request->type != LINUX_F_UNLCK;
    size_t released = 0;
    bool owner_overlap = false;
    spinlock_lock(&g_record_lock);
    if (request->type != LINUX_F_UNLCK && conflict_locked(node, request)) {
        spinlock_unlock(&g_record_lock);
        return K_EAGAIN;
    }
    for (size_t i = 0; i < FILE_LOCK_CAPACITY; ++i) {
        record_lock_t *r = &g_records[i];
        if (!r->node) { ++available; continue; }
        if (!same_owner(r, node, request->owner) || !overlaps(&r->range, request))
            continue;
        owner_overlap = true;
        /* 规范化后的所有者区间使被覆盖且模式相同的请求成为真正的空操作。不要拆分/合并区间、
         * 反复固定 vnode、消耗备用记录，或向阻塞中的锁用户发布变更。 */
        if (r->range.type == request->type &&
            r->range.first <= request->first && r->range.last >= request->last) {
            spinlock_unlock(&g_record_lock);
            return K_OK;
        }
        if (request->first <= r->range.first && request->last >= r->range.last)
            ++available;
        else if (request->first > r->range.first && request->last < r->range.last)
            ++needed;
    }
    if (request->type == LINUX_F_UNLCK && !owner_overlap) {
        spinlock_unlock(&g_record_lock);
        return K_OK;
    }
    if (needed > available) {
        spinlock_unlock(&g_record_lock);
        return K_ENOLCK;
    }
    /* 先释放被完全覆盖的记录，使拆分操作可以复用这些槽位。 */
    for (size_t i = 0; i < FILE_LOCK_CAPACITY; ++i) {
        record_lock_t *r = &g_records[i];
        if (same_owner(r, node, request->owner) &&
            request->first <= r->range.first && request->last >= r->range.last) {
            r->node = 0;
            ++released;
        }
    }
    for (size_t i = 0; i < FILE_LOCK_CAPACITY; ++i) {
        record_lock_t *r = &g_records[i];
        if (!same_owner(r, node, request->owner) || !overlaps(&r->range, request))
            continue;
        if (r->range.first < request->first && r->range.last > request->last) {
            record_lock_t *right = empty_locked();
            *right = *r;
            right->range.first = request->last + 1;
            object_get(node);
            r->range.last = request->first - 1;
        } else if (r->range.first < request->first)
            r->range.last = request->first - 1;
        else
            r->range.first = request->last + 1;
    }
    if (request->type != LINUX_F_UNLCK) {
        record_lock_t *added = empty_locked();
        *added = (record_lock_t){node, *request};
        object_get(node);
        /* 保持所有者的锁区间规范化，包括相邻区间。 */
        for (size_t i = 0; i < FILE_LOCK_CAPACITY; ++i) {
            record_lock_t *r = &g_records[i];
            if (r == added || !same_owner(r, node, request->owner) ||
                r->range.type != added->range.type) continue;
            if ((r->range.first <= added->range.last && r->range.last >= added->range.first) ||
                (r->range.last != INT64_MAX && r->range.last + 1 == added->range.first) ||
                (added->range.last != INT64_MAX && added->range.last + 1 == r->range.first)) {
                if (r->range.first < added->range.first) added->range.first = r->range.first;
                if (r->range.last > added->range.last) added->range.last = r->range.last;
                r->node = 0;
                ++released;
                i = (size_t)-1; /* 合并可能连接到之前已扫描的区间 */
            }
        }
    }
    atomic_fetch_add_explicit(&node->record_lock_sequence, 1U, memory_order_release);
    spinlock_unlock(&g_record_lock);
    while (released--) object_put(node);
    return K_OK;
}
void file_lock_release_file(vnode_t *node, uint64_t owner) {
    size_t released = 0;
    spinlock_lock(&g_record_lock);
    for (size_t i = 0; i < FILE_LOCK_CAPACITY; ++i) {
        if (same_owner(&g_records[i], node, owner)) {
            g_records[i].node = 0;
            ++released;
        }
    }
    /* 关闭未加锁的别名 fd 时，也必须唤醒在该 fd 上阻塞的请求。 */
    atomic_fetch_add_explicit(&node->record_lock_sequence, 1U, memory_order_release);
    spinlock_unlock(&g_record_lock);
    while (released--) object_put(node);
}
void file_lock_release_owner(uint64_t owner) {
    vnode_t *released[FILE_LOCK_CAPACITY];
    size_t count = 0;
    spinlock_lock(&g_record_lock);
    for (size_t i = 0; i < FILE_LOCK_WAIT_CAPACITY; ++i)
        if (g_lock_waiters[i].node && g_lock_waiters[i].range.owner == owner)
            g_lock_waiters[i].node = 0;
    for (size_t i = 0; i < FILE_LOCK_CAPACITY; ++i) {
        if (g_records[i].node && g_records[i].range.owner == owner) {
            vnode_t *node = g_records[i].node;
            released[count++] = node;
            g_records[i].node = 0;
            atomic_fetch_add_explicit(&node->record_lock_sequence, 1U, memory_order_release);
        }
    }
    spinlock_unlock(&g_record_lock);
    for (size_t i = 0; i < count; ++i) {
        wake_all(&released[i]->record_lock_waitq);
        object_put(released[i]);
    }
}
