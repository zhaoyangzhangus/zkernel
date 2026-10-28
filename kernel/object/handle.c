#include <kernel/handle.h>
#include <kernel/mm.h>
#include <kernel/sched.h>

#define HANDLE_INITIAL_CHUNKS 4U
#define HANDLE_MAX_CHUNKS     65536U
#define HANDLE_INVALID_INDEX  UINT32_MAX

static atomic_uintptr_t g_handle_waiting_lock;

static void handle_table_release(handle_table_t *table, bool close_objects);

static void handle_lock(spinlock_t *lock) {
    /*
    * 分配新分块或后备数组时也会持有表锁。在发布完成前让锁持有者留在当前 CPU；
    * 否则本地定时器可能将其切走，而另一颗 CPU 会在此处持续自旋。
    */
    sched_preempt_disable();
    /* 无竞争时不触碰全局诊断缓存行；只有实际等待者才发布等待目标。 */
    if (spinlock_try_lock(lock)) return;
    atomic_store_explicit(&g_handle_waiting_lock, (uintptr_t)lock,
                          memory_order_release);
    spinlock_lock(lock);
    atomic_store_explicit(&g_handle_waiting_lock, 0U, memory_order_release);
}

static void handle_unlock(spinlock_t *lock) {
    spinlock_unlock(lock);
    sched_preempt_enable();
}

static void chunk_lock(handle_chunk_t *chunk) {
    handle_lock(&chunk->lock);
}

static void chunk_unlock(handle_chunk_t *chunk) {
    handle_unlock(&chunk->lock);
}

static handle_t make_handle(uint32_t index, uint32_t generation) {
    return ((uint64_t)generation << HANDLE_INDEX_BITS) | index;
}

static uint32_t handle_index(handle_t handle) {
    return (uint32_t)handle;
}

static uint32_t handle_generation(handle_t handle) {
    return (uint32_t)(handle >> HANDLE_INDEX_BITS);
}

static uint32_t next_generation(uint32_t generation) {
    ++generation;
    return generation == 0U ? 1U : generation;
}

/* 句柄存储不依赖具体对象所有者的类型。 */
static void handle_close_object(void *object) {
    if (object == 0) return;
    object_header_t *header = (object_header_t *)object;
    if (header->ops != 0 && header->ops->handle_close != 0) {
        header->ops->handle_close(object);
    }
}

static handle_chunk_t *chunk_allocate(void) {
    handle_chunk_t *chunk = (handle_chunk_t *)kzalloc(sizeof(handle_chunk_t), 0);
    if (chunk == 0) return 0;
    spinlock_init(&chunk->lock);
    for (uint32_t i = 0; i < HANDLE_CHUNK_ENTRIES; ++i) {
        chunk->entries[i].generation = 1U;
        chunk->entries[i].next_free = HANDLE_INVALID_INDEX;
    }
    return chunk;
}

static kstatus_t grow_table_locked(handle_table_t *table) {
    if (table->chunk_capacity >= HANDLE_MAX_CHUNKS) return K_ENOMEM;
    uint32_t new_capacity = table->chunk_capacity == 0 ? HANDLE_INITIAL_CHUNKS :
                            table->chunk_capacity * 2U;
    if (new_capacity > HANDLE_MAX_CHUNKS || new_capacity < table->chunk_capacity) {
        new_capacity = HANDLE_MAX_CHUNKS;
    }
    handle_chunk_t **chunks = (handle_chunk_t **)kmalloc(
        (size_t)new_capacity * sizeof(handle_chunk_t *), 0);
    if (chunks == 0) return K_ENOMEM;
    /* 发布前恰好初始化每个指针一次：复制已使用的前缀（包括空洞），
     * 仅清空尚未使用的后缀。 */
    for (uint32_t i = 0; i < table->chunk_count; ++i) chunks[i] = table->chunks[i];
    for (uint32_t i = table->chunk_count; i < new_capacity; ++i) chunks[i] = 0;
    handle_chunk_t **old = table->chunks;
    table->chunks = chunks;
    table->chunk_capacity = new_capacity;
    kfree(old);
    return K_OK;
}

kstatus_t handle_table_init(handle_table_t *table) {
    if (table == 0) return K_EINVAL;
    table->chunks = 0;
    table->chunk_count = 0;
    table->chunk_capacity = 0;
    table->free_hint = 0;
    table->reserved = 0;
    spinlock_init(&table->grow_lock);
    handle_lock(&table->grow_lock);
    kstatus_t status = grow_table_locked(table);
    handle_unlock(&table->grow_lock);
    return status;
}

static kstatus_t handle_table_clone_internal(handle_table_t *source,
                                             handle_table_t *destination,
                                             bool exclude_type,
                                             object_type_id_t excluded_type) {
    if (source == 0 || destination == 0 || source == destination) return K_EINVAL;

    destination->chunks = 0;
    destination->chunk_count = 0;
    destination->chunk_capacity = 0;
    destination->free_hint = 0;
    destination->reserved = 0;
    spinlock_init(&destination->grow_lock);

    /* 保持索引和代数一致：COW fork 后，用户态描述符表仍包含原有内核句柄值。 */
    handle_lock(&source->grow_lock);
    uint32_t capacity = source->chunk_capacity;
    uint32_t chunk_count = source->chunk_count;
    if (capacity == 0U || capacity > HANDLE_MAX_CHUNKS ||
        chunk_count > capacity) {
        handle_unlock(&source->grow_lock);
        return K_EINVAL;
    }

    destination->chunks = (handle_chunk_t **)kzalloc(
        (size_t)capacity * sizeof(handle_chunk_t *), 0);
    if (destination->chunks == 0) {
        handle_unlock(&source->grow_lock);
        return K_ENOMEM;
    }
    destination->chunk_capacity = capacity;
    destination->chunk_count = chunk_count;
    destination->free_hint = source->free_hint;
    destination->reserved = source->reserved;

    kstatus_t status = K_OK;
    for (uint32_t chunk_index = 0U; chunk_index < chunk_count; ++chunk_index) {
        handle_chunk_t *source_chunk = source->chunks[chunk_index];
        if (source_chunk == 0) continue;
        /* 下方会复制每个条目，包括空闲槽位的代数。只需初始化锁；若先清零条目，就会在持有源表锁时将
         * 整个内存块写两遍。 */
        handle_chunk_t *destination_chunk = (handle_chunk_t *)kmalloc(sizeof(*destination_chunk), 0);
        if (destination_chunk == 0) {
            status = K_ENOMEM;
            break;
        }
        destination->chunks[chunk_index] = destination_chunk;
        spinlock_init(&destination_chunk->lock);
        chunk_lock(source_chunk);
        for (uint32_t entry_index = 0U;
             entry_index < HANDLE_CHUNK_ENTRIES; ++entry_index) {
            handle_entry_t *source_entry = &source_chunk->entries[entry_index];
            handle_entry_t *destination_entry =
                &destination_chunk->entries[entry_index];
            *destination_entry = *source_entry;
            if (source_entry->object == 0) continue;
            if (exclude_type &&
                ((object_header_t *)source_entry->object)->type == excluded_type) {
                destination_entry->object = 0;
                destination_entry->rights = 0;
                destination_entry->flags = 0;
                destination_entry->generation =
                    next_generation(destination_entry->generation);
                uint32_t index = (chunk_index << HANDLE_CHUNK_SHIFT) | entry_index;
                if (index < destination->free_hint) destination->free_hint = index;
                continue;
            }
            object_get(source_entry->object);
        }
        chunk_unlock(source_chunk);
    }
    handle_unlock(&source->grow_lock);

    if (status != K_OK) {
        /* 尚未发布的克隆条目持有引用，而非逻辑句柄。 */
        handle_table_release(destination, false);
        return status;
    }
    return K_OK;
}

kstatus_t handle_table_clone(handle_table_t *source, handle_table_t *destination) {
    return handle_table_clone_internal(source, destination, false, 0U);
}

kstatus_t handle_table_clone_without_type(handle_table_t *source,
                                          handle_table_t *destination,
                                          object_type_id_t excluded_type) {
    return handle_table_clone_internal(source, destination, true, excluded_type);
}

/* 仅在条目发布后才消费转移来的引用。失败时引用仍归调用方所有，并由调用方在解锁后释放。 */
static kstatus_t handle_create_internal(handle_table_t *table, void *object,
                                   uint32_t rights, uint32_t flags,
                                   handle_t *out, bool transfer_reference) {
    if (table == 0 || object == 0 || out == 0 || table->chunks == 0 ||
        (flags & ~HANDLE_FLAG_MASK) != 0U) return K_EINVAL;
    handle_lock(&table->grow_lock);
    /* 创建者等待 grow_lock 时，参数校验之后数组可能已被销毁流程摘除。绝不能重新启用该表。 */
    if (table->chunks == 0) {
        handle_unlock(&table->grow_lock);
        return K_EINVAL;
    }
    for (;;) {
        uint64_t capacity = (uint64_t)table->chunk_capacity * HANDLE_CHUNK_ENTRIES;
        for (uint64_t raw_index = table->free_hint; raw_index < capacity;
             raw_index = (raw_index | (HANDLE_CHUNK_ENTRIES - 1U)) + 1U) {
            uint32_t index = (uint32_t)raw_index;
            uint32_t chunk_index = index >> HANDLE_CHUNK_SHIFT;
            uint32_t entry_index = index & (HANDLE_CHUNK_ENTRIES - 1U);
            handle_chunk_t *chunk = table->chunks[chunk_index];
            if (chunk == 0) {
                chunk = chunk_allocate();
                if (chunk == 0) {
                    handle_unlock(&table->grow_lock);
                    return K_ENOMEM;
                }
                table->chunks[chunk_index] = chunk;
                if (chunk_index + 1U > table->chunk_count) table->chunk_count = chunk_index + 1U;
            }
            chunk_lock(chunk);
            /* grow_lock 会固定该块；一次加锁扫描其已占用后缀，而不是为每个已占用条目分别加锁。 */
            for (; entry_index < HANDLE_CHUNK_ENTRIES; ++entry_index, ++index) {
                handle_entry_t *entry = &chunk->entries[entry_index];
                if (entry->object != 0) continue;
                if (!transfer_reference) object_get(object);
                entry->object = object;
                entry->rights = rights;
                entry->flags = flags;
                entry->next_free = HANDLE_INVALID_INDEX;
                table->free_hint = index + 1U;
                *out = make_handle(index, entry->generation);
                chunk_unlock(chunk);
                handle_unlock(&table->grow_lock);
                return K_OK;
            }
            chunk_unlock(chunk);
        }
        kstatus_t status = grow_table_locked(table);
        if (status != K_OK) {
            handle_unlock(&table->grow_lock);
            return status;
        }
    }
}

kstatus_t handle_create_with_flags(handle_table_t *table, void *object,
                                   uint32_t rights, uint32_t flags,
                                   handle_t *out) {
    return handle_create_internal(table, object, rights, flags, out, false);
}

kstatus_t handle_create(handle_table_t *table, void *object, uint32_t rights,
                        handle_t *out) {
    return handle_create_with_flags(table, object, rights, 0U, out);
}

static void handle_entry_unlock(handle_table_t *table, handle_chunk_t *chunk) {
    chunk_unlock(chunk);
    handle_unlock(&table->grow_lock);
}

/* 调用方负责校验参数。成功时保留两把锁；失败时不持有任何锁。grow_lock 会固定返回的块和条目。 */
static handle_entry_t *handle_entry_lock(handle_table_t *table, handle_t handle,
                                          handle_chunk_t **out_chunk) {
    uint32_t index = handle_index(handle);
    uint32_t chunk_index = index >> HANDLE_CHUNK_SHIFT;
    /* 并发扩展句柄表也无法使任一条件变为有效：代数会跳过零，分块存储有固定实现上限。 */
    if (handle_generation(handle) == 0U || chunk_index >= HANDLE_MAX_CHUNKS)
        return 0;
    handle_lock(&table->grow_lock);
    if (chunk_index >= table->chunk_capacity ||
        table->chunks[chunk_index] == 0) {
        handle_unlock(&table->grow_lock);
        return 0;
    }
    handle_chunk_t *chunk = table->chunks[chunk_index];
    chunk_lock(chunk);
    handle_entry_t *entry = &chunk->entries[index & (HANDLE_CHUNK_ENTRIES - 1U)];
    if (entry->object == 0 || entry->generation != handle_generation(handle)) {
        handle_entry_unlock(table, chunk);
        return 0;
    }
    *out_chunk = chunk;
    return entry;
}

/* 读取源条目期间，暂时增加对象引用。 */
static kstatus_t handle_reference(handle_table_t *table, handle_t handle,
                                  uint32_t required_rights,
                                  uint32_t *granted_rights, void **object) {
    if (table == 0 || object == 0 || handle == 0) return K_EINVAL;
    handle_chunk_t *chunk;
    handle_entry_t *entry = handle_entry_lock(table, handle, &chunk);
    if (entry == 0) return K_ENOENT;
    kstatus_t status = K_OK;
    if ((entry->rights & required_rights) != required_rights) {
        status = K_EACCES;
    } else if (!object_try_get(entry->object)) {
        status = K_ENOENT;
    } else {
        if (granted_rights != 0) *granted_rights = entry->rights;
        *object = entry->object;
    }
    handle_entry_unlock(table, chunk);
    return status;
}

kstatus_t handle_duplicate(handle_table_t *table, handle_t source,
                           uint32_t flags, handle_t *out) {
    void *object = 0;
    uint32_t rights = 0U;
    kstatus_t status;

    if ((flags & ~HANDLE_FLAG_MASK) != 0U || out == 0) return K_EINVAL;
    status = handle_reference(table, source, 0U, &rights, &object);
    if (status != K_OK) return status;
    status = handle_create_internal(table, object, rights, flags, out, true);
    if (status != K_OK) object_put(object);
    return status;
}

kstatus_t handle_lookup(handle_table_t *table, handle_t handle, uint32_t rights,
                        void **out_object) {
    return handle_reference(table, handle, rights, 0, out_object);
}

kstatus_t handle_get_flags(handle_table_t *table, handle_t handle,
                           uint32_t *flags) {
    if (table == 0 || flags == 0 || handle == 0) return K_EINVAL;
    handle_chunk_t *chunk;
    handle_entry_t *entry = handle_entry_lock(table, handle, &chunk);
    if (entry == 0) return K_ENOENT;
    *flags = entry->flags;
    handle_entry_unlock(table, chunk);
    return K_OK;
}

kstatus_t handle_set_flags(handle_table_t *table, handle_t handle,
                           uint32_t flags) {
    if (table == 0 || handle == 0 || (flags & ~HANDLE_FLAG_MASK) != 0U) {
        return K_EINVAL;
    }
    handle_chunk_t *chunk;
    handle_entry_t *entry = handle_entry_lock(table, handle, &chunk);
    if (entry == 0) return K_ENOENT;
    entry->flags = flags;
    handle_entry_unlock(table, chunk);
    return K_OK;
}

/* 调用方持有表锁和分块锁；返回的引用归关闭方所有，其回调必须在释放这些锁后执行。 */
static void *handle_detach_locked(handle_table_t *table, handle_entry_t *entry,
                                   uint32_t index) {
    void *object = entry->object;
    entry->object = 0;
    entry->rights = 0;
    entry->flags = 0;
    entry->generation = next_generation(entry->generation);
    if (index < table->free_hint) table->free_hint = index;
    return object;
}

kstatus_t handle_close(handle_table_t *table, handle_t handle) {
    if (table == 0 || handle == 0) return K_EINVAL;
    uint32_t index = handle_index(handle);
    handle_chunk_t *chunk;
    handle_entry_t *entry = handle_entry_lock(table, handle, &chunk);
    if (entry == 0) return K_ENOENT;
    void *object = handle_detach_locked(table, entry, index);
    handle_entry_unlock(table, chunk);
    handle_close_object(object);
    object_put(object);
    return K_OK;
}

kstatus_t handle_table_close_on_exec(handle_table_t *table) {
    if (table == 0) return K_EINVAL;

    /* 从已摘除槽位继续，而非每个句柄都重新开始扫描。每轮移除完成后再开启新一轮：回调可能在游标后方
     * 创建/标记 CLOEXEC 条目。解锁后不保留条目/分块指针，且会在移除时检查标志。 */
    uint32_t cursor = 0U;
    bool removed = false;
    for (;;) {
        void *object = 0;
        handle_lock(&table->grow_lock);
        for (uint32_t chunk_index = cursor >> HANDLE_CHUNK_SHIFT;
             chunk_index < table->chunk_count && object == 0;
             ++chunk_index) {
            handle_chunk_t *chunk = table->chunks[chunk_index];
            if (chunk == 0) continue;
            chunk_lock(chunk);
            uint32_t first = chunk_index == (cursor >> HANDLE_CHUNK_SHIFT) ?
                             cursor & (HANDLE_CHUNK_ENTRIES - 1U) : 0U;
            for (uint32_t entry_index = first;
                 entry_index < HANDLE_CHUNK_ENTRIES; ++entry_index) {
                handle_entry_t *entry = &chunk->entries[entry_index];
                if (entry->object != 0 &&
                    (entry->flags & HANDLE_FLAG_CLOEXEC) != 0U) {
                    uint32_t index = (chunk_index << HANDLE_CHUNK_SHIFT) |
                                     entry_index;
                    object = handle_detach_locked(table, entry, index);
                    cursor = index + 1U;
                    break;
                }
            }
            chunk_unlock(chunk);
        }
        handle_unlock(&table->grow_lock);
        if (object == 0) {
            if (!removed) return K_OK;
            cursor = 0U;
            removed = false;
            continue;
        }
        removed = true;
        handle_close_object(object);
        object_put(object);
    }
}

static void handle_table_release(handle_table_t *table, bool close_objects) {
    if (table == 0) return;
    handle_lock(&table->grow_lock);
    /* 在一个有界临界区中摘除存储。所有条目使用方都持有 grow_lock，因此此状态发布后不会有分块指针继续
     * 存活。对象回调/析构函数可能获取其他锁或重入此表，执行时不得持有表锁或任何分块锁。 */
    handle_chunk_t **chunks = table->chunks;
    uint32_t chunk_count = table->chunk_count;
    table->chunks = 0;
    table->chunk_count = 0;
    table->chunk_capacity = 0;
    table->free_hint = 0;
    handle_unlock(&table->grow_lock);
    /* 已发布高水位及其以上的槽位从未分配。 */
    for (uint32_t chunk_index = 0; chunk_index < chunk_count; ++chunk_index) {
        handle_chunk_t *chunk = chunks[chunk_index];
        if (chunk == 0) continue;
        for (uint32_t i = 0; i < HANDLE_CHUNK_ENTRIES; ++i) {
            void *object = chunk->entries[i].object;
            chunk->entries[i].object = 0;
            if (object != 0) {
                if (close_objects) handle_close_object(object);
                object_put(object);
            }
        }
        kfree(chunk);
    }
    kfree(chunks);
}

void handle_table_destroy(handle_table_t *table) {
    handle_table_release(table, true);
}

uintptr_t handle_debug_waiting_lock(void) {
    return atomic_load_explicit(&g_handle_waiting_lock, memory_order_acquire);
}
