#pragma once
#include "base.h"
#include "object.h"
#include "spinlock.h"

typedef uint64_t handle_t;

#define HANDLE_INDEX_BITS 32u
#define HANDLE_GENERATION_BITS 32u
#define HANDLE_CHUNK_SHIFT 8u
#define HANDLE_CHUNK_ENTRIES (1u << HANDLE_CHUNK_SHIFT)

/* 标志属于单个句柄表条目，不属于对象本身。 */
#define HANDLE_FLAG_CLOEXEC (1u << 0)
#define HANDLE_FLAG_MASK    HANDLE_FLAG_CLOEXEC

typedef struct handle_entry {
    void *object;
    uint32_t generation;
    uint32_t rights;
    uint32_t flags;
} handle_entry_t;

typedef struct handle_chunk {
    spinlock_t lock;
    handle_entry_t entries[HANDLE_CHUNK_ENTRIES];
} handle_chunk_t;

typedef struct handle_table {
    spinlock_t grow_lock;
    handle_chunk_t **chunks;
    uint32_t chunk_count;
    uint32_t chunk_capacity;
    uint32_t free_hint;
} handle_table_t;

kstatus_t handle_table_init(handle_table_t *table);
/* fork 快照保留槽位与代数，但不继承声明 OBJECT_FLAG_NO_FORK 的对象。 */
kstatus_t handle_table_clone(handle_table_t *source, handle_table_t *destination);
kstatus_t handle_create(handle_table_t *table, void *object, uint32_t rights, handle_t *out);
kstatus_t handle_create_with_flags(handle_table_t *table, void *object,
                                   uint32_t rights, uint32_t flags,
                                   handle_t *out);
kstatus_t handle_duplicate(handle_table_t *table, handle_t source,
                            uint32_t flags, handle_t *out);
kstatus_t handle_lookup(handle_table_t *table, handle_t h, uint32_t rights, void **out_object);
kstatus_t handle_get_flags(handle_table_t *table, handle_t handle,
                           uint32_t *flags);
kstatus_t handle_set_flags(handle_table_t *table, handle_t handle,
                           uint32_t flags);
kstatus_t handle_close(handle_table_t *table, handle_t h);
kstatus_t handle_table_close_on_exec(handle_table_t *table);

void handle_table_destroy(handle_table_t *table);
/* 运行时自检失败时输出的锁诊断信息。 */
uintptr_t handle_debug_waiting_lock(void);
