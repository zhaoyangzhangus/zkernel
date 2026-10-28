#include "internal.h"
#include <arch/x86_64/paging.h>
#include <kernel/vm.h>
#include <kernel/string.h>

/* REFACTOR_FS_VFS_PAGE_CACHE_OWNER：文件页缓存生命周期和 I/O。 */

static uint32_t vfs_page_cache_bucket(uint64_t page_index, uint32_t count) {
    page_index ^= page_index >> 32;
    page_index ^= page_index >> 16;
    page_index ^= page_index >> 8;
    return (uint32_t)(page_index & (count - 1U));
}

static uint32_t vfs_page_cache_bucket_count(vfs_page_cache_t *cache) {
    return cache->bucket_count != 0U ? cache->bucket_count : VFS_PAGE_CACHE_BUCKETS;
}

/* 回写和调整大小共用此逻辑；窄且密集的范围适合哈希探测，宽范围或稀疏范围则遍历一次常驻链表。 */
static inline bool vfs_page_cache_use_range_index(vfs_page_cache_t *cache,
                                                 uint64_t page_count) {
    return page_count <= 16U ||
        (page_count <= vfs_page_cache_bucket_count(cache) &&
         page_count <= cache->entry_count / 4U);
}

static vfs_cached_page_t **vfs_page_cache_buckets(vfs_page_cache_t *cache) {
    return cache->expanded_buckets != 0 ? cache->expanded_buckets : cache->buckets;
}

/* 调用方持有 vnode 锁。扩容是可选优化：分配失败不能丢弃缓存页或使需求读取失败。
 * 每插入最多 256 页后再重试，且每文件索引上限为 128 KiB。 */
static void vfs_page_cache_grow(vfs_page_cache_t *cache) {
    uint32_t old_count = vfs_page_cache_bucket_count(cache);
    if (old_count >= 16384U || cache->entry_count < (uint64_t)old_count * 4U ||
        cache->entry_count < cache->growth_retry_at) return;
    uint32_t new_count = old_count * 2U;
    /* 分配失败期间，小索引中可能累积许多插入。内存恢复后一次性重新哈希追上，
     * 不要在每个中间的 2 次幂容量处再次遍历所有常驻页。 */
    while (new_count < 16384U &&
           cache->entry_count >= (uint64_t)new_count * 4U)
        new_count *= 2U;
    vfs_cached_page_t **replacement = kzalloc(
        (size_t)new_count * sizeof(*replacement), 0);
    if (replacement == 0) {
        cache->growth_retry_at = cache->entry_count + VFS_PAGE_CACHE_BUCKETS;
        return;
    }
    for (vfs_cached_page_t *entry = cache->head; entry != 0; entry = entry->next) {
        uint32_t bucket = vfs_page_cache_bucket(entry->index, new_count);
        entry->hash_next = replacement[bucket];
        replacement[bucket] = entry;
    }
    vfs_cached_page_t **previous = cache->expanded_buckets;
    cache->expanded_buckets = replacement;
    cache->bucket_count = new_count;
    cache->growth_retry_at = 0U;
    kfree(previous);
}

static void vfs_page_cache_insert(vfs_page_cache_t *cache, vfs_cached_page_t *entry) {
    vfs_cached_page_t **buckets = vfs_page_cache_buckets(cache);
    uint32_t bucket = vfs_page_cache_bucket(entry->index, vfs_page_cache_bucket_count(cache));
    entry->hash_next = buckets[bucket];
    buckets[bucket] = entry;
    entry->next = cache->head;
    cache->head = entry;
    ++cache->entry_count;
}

static vfs_cached_page_t *vfs_page_cache_lookup(vfs_page_cache_t *cache,
                                                 uint64_t page_index) {
    if (cache == 0) return 0;
    for (vfs_cached_page_t *entry =
             vfs_page_cache_buckets(cache)[vfs_page_cache_bucket(
                 page_index, vfs_page_cache_bucket_count(cache))];
         entry != 0; entry = entry->hash_next) {
        if (entry->index == page_index) return entry;
    }
    return 0;
}

static void vfs_vm_mapping_retain(void *mapping) {
    object_get(mapping);
}

static void vfs_vm_mapping_release(void *mapping) {
    object_put(mapping);
}

static kstatus_t vfs_vm_mapping_page_get(void *mapping, uint64_t page_index,
                                         page_t **out) {
    return vfs_file_page_get((vnode_t *)mapping, page_index, out);
}

static void vfs_vm_mapping_page_mark_dirty(void *mapping, uint64_t page_index) {
    vfs_file_page_mark_dirty((vnode_t *)mapping, page_index);
}

static kstatus_t vfs_vm_mapping_sync(void *mapping, uint64_t first_page,
                                     uint64_t page_count) {
    return vfs_page_cache_sync((vnode_t *)mapping, first_page, page_count);
}

static const vm_file_ops_t g_vfs_vm_file_ops = {
    .retain = vfs_vm_mapping_retain,
    .release = vfs_vm_mapping_release,
    .page_get = vfs_vm_mapping_page_get,
    .page_mark_dirty = vfs_vm_mapping_page_mark_dirty,
    .sync = vfs_vm_mapping_sync,
};

const struct vm_file_ops *vfs_vm_file_ops(void) {
    return &g_vfs_vm_file_ops;
}

void vfs_page_cache_destroy(vnode_t *vnode) {
    vfs_page_cache_t *cache = vnode != 0 ?
                              (vfs_page_cache_t *)vnode->page_cache : 0;
    if (cache == 0) return;
    vfs_cached_page_t *entry = cache->head;
    while (entry != 0) {
        vfs_cached_page_t *next = entry->next;
        page_free(entry->page);
        kfree(entry);
        entry = next;
    }
    kfree(cache->expanded_buckets);
    kfree(cache);
    __atomic_store_n(&vnode->page_cache, 0, __ATOMIC_RELEASE);
}

static bool vfs_page_cache_still_mapped(vfs_cached_page_t *entry) {
    /* 页分配器以 -1 表示未映射，每安装一个 PTE 加一，成功撤销映射后减一。 */
    return atomic_load_explicit(&entry->page->mapcount, memory_order_acquire) >= 0;
}

/* 调用方持有 vnode->lock。限制页面范围可避免 VM_SYNC 刷新恰好共用此 vnode 的无关脏映射。 */
static kstatus_t vfs_page_cache_flush_entry(vnode_t *vnode,
                                            vfs_backend_node_t *backend,
                                            vfs_cached_page_t *entry,
                                            uint64_t first_page,
                                            uint64_t last_page) {
    if (entry == 0 || !entry->dirty) return K_OK;
    vfs_page_cache_t *cache = vnode->page_cache;
    if (cache->writeback_generation != 0U &&
        entry->writeback_generation == cache->writeback_generation) return K_OK;
    if (entry->index > UINT64_MAX / PAGE_SIZE) return K_EINVAL;
    uint64_t offset = entry->index * PAGE_SIZE;
    if (offset >= vnode->size) {
        entry->dirty = false;
        return K_OK;
    }
    /* 在请求范围内合并物理连续的脏页，直接借用页缓存，不分配或复制中间缓冲。
     * 常驻链表按插入顺序排列，因此向前、向后都检查，避免逆序遍历退回逐页写入。 */
    vfs_cached_page_t *entries[16];
    entries[0] = entry;
    unsigned count = 1U;
    uint64_t start = entry->index;
    uintptr_t memory = (uintptr_t)phys_to_direct(page_to_phys(entry->page));
    while (count < 16U && start > first_page && memory >= PAGE_SIZE) {
        vfs_cached_page_t *previous = vfs_page_cache_lookup(cache, start - 1U);
        if (previous == 0 || !previous->dirty ||
            (cache->writeback_generation != 0U &&
             previous->writeback_generation == cache->writeback_generation) ||
            (uintptr_t)phys_to_direct(page_to_phys(previous->page)) != memory - PAGE_SIZE)
            break;
        entries[count++] = previous;
        --start;
        memory -= PAGE_SIZE;
    }
    while (memory != 0U && count < 16U && start + count <= last_page &&
           start + count <= (vnode->size - 1U) / PAGE_SIZE &&
           memory <= UINTPTR_MAX - (uintptr_t)count * PAGE_SIZE) {
        vfs_cached_page_t *next = vfs_page_cache_lookup(cache, start + count);
        if (next == 0 || !next->dirty ||
            (cache->writeback_generation != 0U &&
             next->writeback_generation == cache->writeback_generation) ||
            (uintptr_t)phys_to_direct(page_to_phys(next->page)) != memory + count * PAGE_SIZE)
            break;
        entries[count++] = next;
    }
    offset = start * PAGE_SIZE;
    uint64_t length = vnode->size - offset;
    if (length > (uint64_t)count * PAGE_SIZE) length = (uint64_t)count * PAGE_SIZE;
    uint64_t written = 0U;
    vfs_backend_lock(backend);
    kstatus_t status = backend->write == 0 ? K_EACCES :
        backend->write(backend->context, offset,
                       (void *)memory,
                       (size_t)length, &written);
    vfs_backend_unlock(backend);
    if (status != K_OK || written != length) {
        return status == K_OK ? K_EIO : status;
    }
    /* 短写或失败时整组保留脏位，下次同步可以安全重试。 */
    /* 用户仍可通过已有 PTE 继续写入，不保证再次触发缺页。在具备跨地址空间的写保护追踪前，
     * 映射中的脏页必须继续参与后续 msync/fsync；最后一次解映射后的回写才能清除脏位。 */
    for (unsigned index = 0U; index < count; ++index) {
        entries[index]->dirty = vfs_page_cache_still_mapped(entries[index]);
        entries[index]->writeback_generation = cache->writeback_generation;
    }
    return K_OK;
}

static kstatus_t vfs_page_cache_flush_locked(vnode_t *vnode,
                                              uint64_t first_page,
                                              uint64_t page_count) {
    uint64_t last_page;
    if (vnode == 0) return K_EINVAL;
    if (page_count == 0U) return K_EINVAL;
    if (page_count == UINT64_MAX) {
        if (first_page != 0U) return K_EINVAL;
        last_page = UINT64_MAX;
    } else {
        if (first_page > UINT64_MAX - (page_count - 1U)) return K_EOVERFLOW;
        last_page = first_page + page_count - 1U;
    }
    vfs_backend_node_t *backend =
        (vfs_backend_node_t *)vnode->fs_private;
    vfs_page_cache_t *cache =
        (vfs_page_cache_t *)vnode->page_cache;
    if (backend == 0 || cache == 0) return K_OK;
    /* 映射中的页在回写后继续保持脏位；同一轮中只能提交一次，避免合并组被后续遍历重复写入。 */
    if (++cache->writeback_generation == 0U) {
        for (vfs_cached_page_t *entry = cache->head; entry != 0; entry = entry->next)
            entry->writeback_generation = 0U;
        cache->writeback_generation = 1U;
    }
    /* 即使文件很大，窄范围仍使用索引：固定 16 页阈值会使 17 页 msync 遍历每个常驻页。
     * 探测次数同时受桶数和常驻页数限制；宽范围/全范围则只遍历一次。 */
    if (vfs_page_cache_use_range_index(cache, page_count)) {
        for (uint64_t i = 0U; i < page_count; ++i) {
            kstatus_t status = vfs_page_cache_flush_entry(vnode, backend,
                vfs_page_cache_lookup(cache, first_page + i), first_page, last_page);
            if (status != K_OK) return status;
        }
        return K_OK;
    }
    for (vfs_cached_page_t *entry = cache->head; entry != 0;
         entry = entry->next) {
        if (!entry->dirty || entry->index < first_page ||
            entry->index > last_page) continue;
        kstatus_t status = vfs_page_cache_flush_entry(vnode, backend, entry,
                                                      first_page, last_page);
        if (status != K_OK) return status;
    }
    return K_OK;
}

kstatus_t vfs_page_cache_flush(vnode_t *vnode) {
    if (vnode == 0) return K_EINVAL;
    return vfs_page_cache_flush_locked(vnode, 0U, UINT64_MAX);
}

kstatus_t vfs_page_cache_flush_prefix(vnode_t *vnode, uint64_t size) {
    if (vnode == 0) return K_EINVAL;
    if (size == 0U) return K_OK;
    return vfs_page_cache_flush_locked(vnode, 0U, (size - 1U) / PAGE_SIZE + 1U);
}

kstatus_t vfs_page_cache_sync(vnode_t *vnode, uint64_t first_page,
                              uint64_t page_count) {
    vfs_backend_node_t *backend;
    kstatus_t status;
    if (vnode == 0 || page_count == 0U) return K_EINVAL;
    if (page_count == UINT64_MAX && first_page != 0U) return K_EINVAL;
    if (page_count != UINT64_MAX && first_page > UINT64_MAX - (page_count - 1U))
        return K_EOVERFLOW;
    backend = (vfs_backend_node_t *)vnode->fs_private;
    if (backend == 0) return K_OK;
    /* 不能让旧缓存回写穿插在直接写入的后端提交与缓存更新之间。
     * 必须先持有事务锁，再取得 vnode 和后端锁，与写入及截断保持同一锁序。 */
    status = io_mutex_lock(&backend->write_mutex);
    if (status != K_OK) return status;
    vfs_vnode_lock(vnode);
    status = vfs_page_cache_flush_locked(vnode, first_page, page_count);
    vfs_vnode_unlock(vnode);
    if (status == K_OK && backend->fsync != 0) {
        vfs_backend_lock(backend);
        status = backend->fsync(backend->context);
        vfs_backend_unlock(backend);
    }
    io_mutex_unlock_required(&backend->write_mutex);
    return status;
}

kstatus_t vfs_page_cache_writeback(vnode_t *vnode) {
    if (vnode == 0) return K_EINVAL;
    vfs_backend_node_t *backend = (vfs_backend_node_t *)vnode->fs_private;
    if (backend == 0) return K_OK;
    kstatus_t status = io_mutex_lock(&backend->write_mutex);
    if (status != K_OK) return status;
    vfs_vnode_lock(vnode);
    status = vfs_page_cache_flush_locked(vnode, 0U, UINT64_MAX);
    vfs_vnode_unlock(vnode);
    io_mutex_unlock_required(&backend->write_mutex);
    return status;
}

static kstatus_t vfs_file_page_get_batch(vnode_t *vnode, uint64_t page_index,
                                        unsigned maximum, struct page **out) {
    if (vnode == 0 || vnode->object.type != KOBJECT_TYPE_VNODE || out == 0) {
        return K_EINVAL;
    }
    vfs_backend_node_t *backend =
        (vfs_backend_node_t *)vnode->fs_private;
    if (backend == 0 || backend->read == 0 ||
        page_index > UINT64_MAX / PAGE_SIZE) return K_EINVAL;
    vfs_vnode_lock(vnode);
    vfs_page_cache_t *cache =
        (vfs_page_cache_t *)vnode->page_cache;
    if (cache == 0) {
        cache = (vfs_page_cache_t *)kzalloc(sizeof(*cache), 0);
        if (cache == 0) {
            vfs_vnode_unlock(vnode);
            return K_ENOMEM;
        }
        cache->head = 0;
        __atomic_store_n(&vnode->page_cache, cache, __ATOMIC_RELEASE);
    }
    vfs_cached_page_t *cached = vfs_page_cache_lookup(cache, page_index);
    if (cached != 0) {
        *out = cached->page;
        vfs_vnode_unlock(vnode);
        return K_OK;
    }

    uint64_t file_offset = page_index * PAGE_SIZE;
    unsigned eligible = 1U;
    /* 缺页调用以零选择自适应预读：孤立访问保留两页上限，前两页已常驻时
     * 才将连续读取扩至 64 KiB。普通 read 的显式范围不参与扩大。 */
    if (maximum == 0U) {
        maximum = 2U;
        if (page_index >= 2U &&
            vfs_page_cache_lookup(cache, page_index - 1U) != 0 &&
            vfs_page_cache_lookup(cache, page_index - 2U) != 0)
            maximum = 16U;
    }
    if (maximum > 16U) maximum = 16U;
    /* RAM 和已脱离设备的快照无需摊销设备延迟。设备文件仅合并所选范围内的
     * 连续缺失页，不能覆盖相邻的常驻页或脏映射。 */
    /* 预读长度只需近似值；vnode 大小受当前持有的锁保护，
     * 即使后端正在改变大小也不会发生无锁访问。实际读取前仍会重新检查后端大小。 */
    uint64_t visible_size = vnode->size;
    if (backend->read != vfs_memory_backend_read &&
        !__atomic_load_n(&backend->detached, __ATOMIC_ACQUIRE) &&
        file_offset < visible_size) {
        uint64_t remaining = visible_size - file_offset;
        while (eligible < maximum &&
               (uint64_t)eligible * PAGE_SIZE < remaining &&
               vfs_page_cache_lookup(cache, page_index + eligible) == 0)
            ++eligible;
    }
    vfs_cached_page_t *entries[16] = {0};
    unsigned target = 1U;
    while (target * 2U <= eligible) target *= 2U;
    unsigned count = 0U;
    while (count < target) {
        entries[count] = kzalloc(sizeof(*entries[count]), 0);
        if (entries[count] == 0) break;
        ++count;
    }
    page_t *page = 0;
    kstatus_t status = K_ENOMEM;
    if (count == 0U) goto failed;
    unsigned order = 0U;
    while ((2U << order) <= count) ++order;
    unsigned allocated = count;
    for (;;) {
        count = 1U << order;
        while (allocated > count) {
            --allocated;
            kfree(entries[allocated]);
            entries[allocated] = 0;
        }
        page = page_alloc(order, 0);
        if (page != 0 || order == 0U) break;
        /* 大块连续内存是优化，不是需求页的前置条件。逐级缩小直至单页。 */
        --order;
    }
    if (page == 0) goto failed;
    paddr_t physical = page_to_phys(page);
    uint8_t *memory = phys_to_direct(physical);
    if (memory == 0) {
        status = K_EIO;
        goto failed;
    }
    size_t capacity = (size_t)count * PAGE_SIZE;
    /* 后端大小与 read 必须在同一锁内取得；截断可能发生在预读分配期间。 */
    vfs_backend_lock(backend);
    uint64_t read_length = backend->size > file_offset ? backend->size - file_offset : 0U;
    if (read_length > capacity) read_length = capacity;
    uint64_t bytes = 0U;
    status = K_OK;
    if (read_length != 0U) {
        status = backend->read(backend->context, file_offset,
                                         memory,
                                         (size_t)read_length, &bytes);
    }
    vfs_backend_unlock(backend);
    if (status != K_OK || bytes != read_length) {
        if (status == K_OK) status = K_EIO;
        goto failed;
    }
    /* 不得发布 EOF 之后的陈旧分配字节。完整读取的页面无需预先清零；失败/短读页面已在上方释放。 */
    if (read_length < capacity) memset(memory + read_length, 0, capacity - read_length);
    /* 发布任一页面前验证整个预读分配。失败时仅需释放临时对象，不会留下部分哈希/链表项。 */
    entries[0]->page = page;
    for (unsigned i = 1U; i < count; ++i) {
        page_t *tail = phys_to_page(paddr_make(physical.value + (uint64_t)i * PAGE_SIZE));
        if (tail == 0 || (tail->flags & PAGE_COMPOUND_TAIL) == 0U) {
            status = K_EIO;
            goto failed;
        }
        entries[i]->page = tail;
    }
    for (unsigned i = 0U; i < count; ++i) {
        entries[i]->page->owner = PAGE_OWNER_FILE;
        entries[i]->page->u.file.mapping = vnode;
        entries[i]->page->u.file.index = page_index + i;
        entries[i]->index = page_index + i;
        entries[i]->dirty = false;
        vfs_page_cache_insert(cache, entries[i]);
    }
    vfs_page_cache_grow(cache);
    *out = page;
    vfs_vnode_unlock(vnode);
    return K_OK;

failed:
    /* 所有可能失败的准备工作完成前，不将任何内容加入索引。 */
    if (page != 0) page_free(page);
    for (unsigned i = 0U; i < 16U; ++i) kfree(entries[i]);
    vfs_vnode_unlock(vnode);
    return status;
}

kstatus_t vfs_file_page_get(vnode_t *vnode, uint64_t page_index,
                            struct page **out) {
    return vfs_file_page_get_batch(vnode, page_index, 0U, out);
}

kstatus_t vfs_read_cached_kernel(file_t *file, void *buffer, size_t length,
                                 uint64_t *bytes) {
    if (file == 0 || file->object.type != KOBJECT_TYPE_FILE ||
        file->vnode == 0 || buffer == 0 || bytes == 0 ||
        (file->rights & VFS_OPEN_READ) == 0U) return K_EINVAL;
    *bytes = 0U;
    if (length == 0U) return K_OK;
    kstatus_t status = vfs_file_position_lock(file);
    if (status != K_OK) return status;
    size_t copied = 0U;
    kstatus_t result = K_OK;
    vnode_t *vnode = file->vnode;
    bool locked = false;
    unsigned batch_pages = 0U;
    while (copied < length) {
        /* 连续常驻页共用短批次保护；最多复制 16 页，缺页前立即释放锁。 */
        if (!locked) {
            vfs_vnode_lock(vnode);
            locked = true;
            batch_pages = 0U;
        }
        uint64_t position = file->position;
        if (position >= vnode->size) {
            vfs_vnode_unlock(vnode);
            break;
        }
        /* 常驻可执行页无需分配，也无需额外执行 get/unlock/relock 往返。复制期间持续持有
         * vnode 保护锁，避免写入/截断在下方更改借用的页面。即使命中缓存，也保留 page_get 的验证。 */
        vfs_backend_node_t *backend = vnode->fs_private;
        if (vnode->object.type != KOBJECT_TYPE_VNODE ||
            backend == 0 || backend->read == 0) {
            vfs_vnode_unlock(vnode);
            result = K_EINVAL;
            break;
        }
        vfs_cached_page_t *cached = vfs_page_cache_lookup(
            (vfs_page_cache_t *)vnode->page_cache, position / PAGE_SIZE);
        page_t *page = cached != 0 ? cached->page : 0;
        if (cached == 0) {
            vfs_vnode_unlock(vnode);
            locked = false;
            size_t span = length - copied;
            if (span > 16U * PAGE_SIZE) span = 16U * PAGE_SIZE;
            unsigned request_pages = (unsigned)((span +
                (position & (PAGE_SIZE - 1ULL)) + PAGE_SIZE - 1U) / PAGE_SIZE);
            kstatus_t status = vfs_file_page_get_batch(vnode,
                position / PAGE_SIZE, request_pages, &page);
            if (status != K_OK) {
                result = status;
                break;
            }
            vfs_vnode_lock(vnode);
            locked = true;
            batch_pages = 0U;
        }
        if (page == 0) {
            vfs_vnode_unlock(vnode);
            result = K_EIO;
            break;
        }
        uint8_t *source = phys_to_direct(page_to_phys(page));
        if (source == 0) {
            vfs_vnode_unlock(vnode);
            result = K_EIO;
            break;
        }
        size_t offset = position & (PAGE_SIZE - 1ULL);
        size_t chunk = PAGE_SIZE - offset;
        if (chunk > length - copied) chunk = length - copied;
        /* 文件所有权保证借用的缓存页保持存活。复制时与缓存更新串行化，并在后备 I/O 后重新检查 EOF。 */
        if (position >= vnode->size) {
            vfs_vnode_unlock(vnode);
            break;
        }
        if (chunk > vnode->size - position) chunk = vnode->size - position;
        __builtin_memcpy((uint8_t *)buffer + copied, source + offset, chunk);
        file->position = position + chunk;
        copied += chunk;
        if (++batch_pages == 16U || copied == length) {
            vfs_vnode_unlock(vnode);
            locked = false;
        }
    }
    *bytes = copied;
    vfs_file_position_unlock(file);
    return result;
}

static void vfs_page_cache_zero_range(vfs_cached_page_t *entry,
                                      uint64_t start, uint64_t end,
                                      uint64_t size) {
    if (entry == 0) return;
    uint64_t base = entry->index * PAGE_SIZE;
    if (base >= end) return;
    uint64_t low = start > base ? start - base : 0U;
    uint64_t high = end - base;
    if (low >= PAGE_SIZE) return;
    if (high > PAGE_SIZE) high = PAGE_SIZE;
    uint8_t *memory = phys_to_direct(page_to_phys(entry->page));
    if (memory != 0) memset(memory + low, 0, (size_t)(high - low));
    if (base >= size) entry->dirty = false;
}

void vfs_page_cache_resize(vnode_t *vnode, uint64_t size) {
    if (vnode == 0) return;
    vfs_vnode_lock(vnode);
    uint64_t old_size = vnode->size;
    if (size == old_size) {
        vfs_vnode_unlock(vnode);
        return;
    }
    uint64_t start = size < old_size ? size : old_size;
    uint64_t end = size < old_size ? UINT64_MAX : size;
    vfs_page_cache_t *cache = vnode->page_cache;
    if (cache != 0 && size > old_size) {
        uint64_t first = start / PAGE_SIZE;
        uint64_t count = (size - 1U) / PAGE_SIZE - first + 1U;
        /* 小幅扩展不能扫描整个大型可执行文件缓存。像范围回写一样限制哈希探测；
         * 宽范围/稀疏扩展仍只遍历一次。缩小操作也会清除旧 EOF 之后的页面。 */
        if (vfs_page_cache_use_range_index(cache, count)) {
            for (uint64_t i = 0; i < count; ++i) {
                vfs_page_cache_zero_range(vfs_page_cache_lookup(cache, first + i),
                                           start, end, size);
            }
            vnode->size = size;
            vfs_vnode_unlock(vnode);
            return;
        }
    }
    for (vfs_cached_page_t *entry = cache != 0 ? cache->head : 0;
         entry != 0; entry = entry->next) {
        vfs_page_cache_zero_range(entry, start, end, size);
    }
    vnode->size = size;
    vfs_vnode_unlock(vnode);
}

void vfs_file_page_mark_dirty(vnode_t *vnode, uint64_t page_index) {
    if (vnode == 0) return;
    vfs_vnode_lock(vnode);
    vfs_page_cache_t *cache =
        (vfs_page_cache_t *)vnode->page_cache;
    if (cache != 0) {
        vfs_cached_page_t *entry = vfs_page_cache_lookup(cache, page_index);
        if (entry != 0) entry->dirty = true;
    }
    vfs_vnode_unlock(vnode);
}

/* 内核目的缓冲可能通过直接映射或 vmalloc 别名指向同一文件页；用户读取缓冲是私有中转区。 */
bool vfs_page_cache_buffer_alias(vnode_t *vnode, const void *buffer, size_t length) {
    if (vnode == 0 || length == 0U) return false;
    uintptr_t address = (uintptr_t)buffer;
    if (length - 1U > UINTPTR_MAX - address) return true;
    uintptr_t last = address + length - 1U;
    paddr_t root = paddr_make(0U);
    bool have_root = false;
    for (;;) {
        paddr_t physical = direct_to_phys((const void *)address);
        if (physical.value == UINT64_MAX) {
            if (!have_root) { root = x86_current_root_table(); have_root = true; }
            if (x86_translate_page(root, (vaddr_t)address, &physical, 0) != K_OK)
                return true;
        }
        page_t *page = phys_to_page(physical);
        if (page != 0 && page->owner == PAGE_OWNER_FILE &&
            page->u.file.mapping == vnode) return true;
        size_t step = PAGE_SIZE - (address & (PAGE_SIZE - 1U));
        if (last - address < step) return false;
        address += step;
    }
}

/* 后端数据已读入独立缓冲；常驻页面是最新视图，包含尚未回写的共享映射。
 * 调用方持有后端写入事务锁，但不能持有后端 I/O 锁，保持 vnode -> 后端的锁序。 */
kstatus_t vfs_page_cache_overlay_read(vnode_t *vnode, uint64_t offset,
                                      void *buffer, size_t length) {
    if (vnode == 0 || (buffer == 0 && length != 0U)) return K_EINVAL;
    if (length == 0U) return K_OK;
    if (offset > UINT64_MAX - length) return K_EIO;
    /* vnode 存活期间缓存只会从空变为非空；大多数未映射文件无需取得 vnode 锁。 */
    if (!vfs_page_cache_present(vnode)) return K_OK;
    uint64_t end = offset + length;
    uint64_t first = offset / PAGE_SIZE;
    uint64_t count = (end - 1U) / PAGE_SIZE - first + 1U;
    kstatus_t status = K_OK;
    vfs_vnode_lock(vnode);
    vfs_page_cache_t *cache = vnode->page_cache;
    if (cache != 0) {
        bool indexed = vfs_page_cache_use_range_index(cache, count);
        vfs_cached_page_t *entry = indexed ? 0 : cache->head;
        for (uint64_t i = 0U; indexed ? i < count : entry != 0; ++i) {
            vfs_cached_page_t *current = indexed ?
                vfs_page_cache_lookup(cache, first + i) : entry;
            if (!indexed) entry = entry->next;
            if (current == 0 || current->index > UINT64_MAX / PAGE_SIZE) continue;
            uint64_t base = current->index * PAGE_SIZE;
            uint64_t start = offset > base ? offset : base;
            if (start >= end || start - base >= PAGE_SIZE) continue;
            if (current->page == 0) { status = K_EIO; break; }
            uint8_t *source = phys_to_direct(page_to_phys(current->page));
            if (source == 0) { status = K_EIO; break; }
            size_t page_offset = (size_t)(start - base);
            size_t output_offset = (size_t)(start - offset);
            size_t copied = PAGE_SIZE - page_offset;
            if (copied > length - output_offset) copied = length - output_offset;
            memmove((uint8_t *)buffer + output_offset, source + page_offset, copied);
        }
    }
    vfs_vnode_unlock(vnode);
    return status;
}

static void vfs_page_cache_copy_bytes(vfs_cached_page_t *entry, size_t in_page,
                                      const uint8_t *source, size_t length) {
    if (entry == 0) return;
    uint8_t *memory = phys_to_direct(page_to_phys(entry->page));
    if (memory == 0) return;
    memmove(memory + in_page, source, length);
    /* 部分直接写入不会消费其他 mmap 脏字节。 */
    if (length == PAGE_SIZE) entry->dirty = vfs_page_cache_still_mapped(entry);
}

/* 仅当源缓冲区独立时，才可更改遍历顺序。使用减法判断区间重叠，避免高位内核指针发生溢出。
 * 此扫描只用于宽范围稀疏更新，不用于小型 I/O。 */
static bool vfs_page_cache_source_alias(vfs_page_cache_t *cache,
                                       const uint8_t *source, size_t length) {
    uintptr_t input = (uintptr_t)source;
    for (vfs_cached_page_t *entry = cache->head; entry != 0; entry = entry->next) {
        uintptr_t memory = (uintptr_t)phys_to_direct(page_to_phys(entry->page));
        if (memory != 0 && (memory >= input ? memory - input < length :
                                               input - memory < PAGE_SIZE))
            return true;
    }
    return false;
}

void vfs_page_cache_copy_in(vnode_t *vnode, uint64_t offset,
                            const uint8_t *source, size_t length) {
    if (vnode == 0 || source == 0 || length == 0U) return;
    vfs_vnode_lock(vnode);
    vfs_page_cache_t *cache =
        (vfs_page_cache_t *)vnode->page_cache;
    if (cache != 0 && offset <= UINT64_MAX - length) {
        uint64_t end = offset + length;
        uint64_t count = (end - 1U) / PAGE_SIZE - offset / PAGE_SIZE + 1U;
        if (!vfs_page_cache_use_range_index(cache, count) &&
            !vfs_page_cache_source_alias(cache, source, length)) {
            /* 只处理常驻页，不遍历大型直接写入范围中的每个空洞。 */
            for (vfs_cached_page_t *entry = cache->head; entry != 0; entry = entry->next) {
                if (entry->index > UINT64_MAX / PAGE_SIZE) continue;
                uint64_t base = entry->index * PAGE_SIZE;
                uint64_t current = offset > base ? offset : base;
                if (current >= end || current - base >= PAGE_SIZE) continue;
                size_t in_page = (size_t)(current - base);
                size_t copied = (size_t)(current - offset);
                size_t chunk = PAGE_SIZE - in_page;
                if (chunk > length - copied) chunk = length - copied;
                vfs_page_cache_copy_bytes(entry, in_page, source + copied, chunk);
            }
            if (end > vnode->size) vnode->size = end;
            vfs_vnode_unlock(vnode);
            return;
        }
    }
    size_t copied = 0U;
    while (cache != 0 && copied < length) {
        uint64_t current = offset + copied;
        if (current < offset) break;
        uint64_t index = current / PAGE_SIZE;
        size_t in_page = (size_t)(current & (PAGE_SIZE - 1ULL));
        size_t chunk = PAGE_SIZE - in_page;
        if (chunk > length - copied) chunk = length - copied;
        vfs_cached_page_t *entry = vfs_page_cache_lookup(cache, index);
        vfs_page_cache_copy_bytes(entry, in_page, source + copied, chunk);
        copied += chunk;
    }
    if (offset <= UINT64_MAX - length && offset + length > vnode->size) {
        vnode->size = offset + length;
    }
    vfs_vnode_unlock(vnode);
}
