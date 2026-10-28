#pragma once

#include <kernel/kmem.h>
#include <kernel/sched.h>
#include <kernel/mutex.h>
#include <kernel/vfs.h>

#define VFS_PATH_LIMIT 256U
#define VFS_FAT32_MOUNT_LIMIT 32U

typedef struct {
    char prefix[VFS_PATH_LIMIT];
    OS_FAT32 *filesystem;
    bool mounted;
} vfs_fat32_mount_t;

extern vfs_fat32_mount_t g_fat32_mounts[VFS_FAT32_MOUNT_LIMIT];

/* 返回最长匹配挂载项及相对于该挂载项的路径。 */
vfs_fat32_mount_t *vfs_fat32_mount_for_path(const char *path,
                                             char relative[VFS_PATH_LIMIT]);

/* VFS 私有后端状态。公共头文件只公开回调类型；对象和页缓存所有权留在此目录内。 */
typedef struct vfs_backend_node {
    char path[VFS_PATH_LIMIT];
    vnode_t *namespace_next;
    uint32_t namespace_slot;
    uint8_t *data;
    uint64_t size;
    uint32_t mode;
    bool directory;
    bool mutable_node;
    bool detached;
    bool unlink_prepared;
    bool fat32_node;
    OS_FAT32_FILE *fat_file;
    OS_FAT32 *fat_filesystem;
    vfs_backend_read_t read;
    vfs_backend_write_t write;
    vfs_backend_fsync_t fsync;
    vfs_backend_truncate_t truncate;
    void *context;
    io_mutex_t io_lock;
    /* 串行化整次直接写入、截断、缓存回写，以及普通读取的后端读取和缓存合并。
     * 此锁绝不作为自旋/禁止抢占保护使用。 */
    io_mutex_t write_mutex;
    uint64_t data_capacity;
    uint64_t content_revision;
    uint64_t snapshot_revision;
    uint64_t snapshot_size;
} vfs_backend_node_t;

typedef struct vfs_cached_page {
    uint64_t index;
    page_t *page;
    bool dirty;
    struct vfs_cached_page *next;
    struct vfs_cached_page *hash_next;
    uint64_t writeback_generation;
} vfs_cached_page_t;

#define VFS_PAGE_CACHE_BUCKETS 256U

typedef struct vfs_page_cache {
    vfs_cached_page_t *head;
    vfs_cached_page_t *buckets[VFS_PAGE_CACHE_BUCKETS];
    vfs_cached_page_t **expanded_buckets;
    uint32_t bucket_count;
    uint64_t entry_count;
    uint64_t growth_retry_at;
    uint64_t writeback_generation;
} vfs_page_cache_t;

/* 仅用于短暂的命名空间状态操作；绝不跨后端 I/O 持有。
 * 等待时只释放本次获取的保护，不释放外层保护。 */
static inline void vfs_state_lock(atomic_uint *state) {
    for (;;) {
        /* 交换前先保护所有权，但当另一颗 CPU 执行页面分配/后备 I/O 时，不要固定当前 CPU。
         * 每次获取失败时，任何外层保护都保持不变。 */
        sched_preempt_disable();
        if (atomic_exchange_explicit(state, 1U,
                                     memory_order_acquire) == 0U) return;
        sched_preempt_enable();
        /* 只读轮询可避免锁持有者的缓存行来回抖动。 */
        while (atomic_load_explicit(state,
                                    memory_order_relaxed) != 0U) {
            __asm__ volatile ("pause");
        }
    }
}

static inline void vfs_state_unlock(atomic_uint *state) {
    atomic_store_explicit(state, 0U,
                          memory_order_release);
    sched_preempt_enable();
}

static inline void vfs_backend_lock(vfs_backend_node_t *backend) {
    io_mutex_lock_required(&backend->io_lock);
}

static inline void vfs_backend_unlock(vfs_backend_node_t *backend) {
    io_mutex_unlock_required(&backend->io_lock);
}

/* 读取的数据必须与直接写入的后端更新及页缓存更新处于同一个事务次序。 */
static inline kstatus_t vfs_backend_consistent_read_lock(vfs_backend_node_t *backend) {
    return io_mutex_lock(&backend->write_mutex);
}

static inline void vfs_backend_consistent_read_unlock(vfs_backend_node_t *backend) {
    io_mutex_unlock_required(&backend->write_mutex);
}

static inline bool vfs_page_cache_present(vnode_t *vnode) {
    return __atomic_load_n(&vnode->page_cache, __ATOMIC_ACQUIRE) != 0;
}

static inline void vfs_vnode_lock(vnode_t *vnode) {
    io_mutex_lock_required(&vnode->lock);
}

static inline void vfs_vnode_unlock(vnode_t *vnode) {
    io_mutex_unlock_required(&vnode->lock);
}

size_t vfs_string_length(const char *text);
bool vfs_fat32_relative_path(const char *path, char relative[VFS_PATH_LIMIT]);
kstatus_t vfs_register_node(vfs_backend_node_t *backend);

kstatus_t vfs_memory_backend_read(void *context, uint64_t offset,
                                   void *buffer, size_t length,
                                   uint64_t *bytes);
kstatus_t vfs_memory_backend_write(void *context, uint64_t offset,
                                    const void *buffer, size_t length,
                                    uint64_t *bytes);
kstatus_t vfs_memory_backend_truncate(void *context, uint64_t size);
kstatus_t vfs_fat32_backend_read(void *context, uint64_t offset,
                                  void *buffer, size_t length,
                                  uint64_t *bytes);
kstatus_t vfs_fat32_backend_write(void *context, uint64_t offset,
                                   const void *buffer, size_t length,
                                   uint64_t *bytes);
kstatus_t vfs_fat32_backend_fsync(void *context);
kstatus_t vfs_fat32_backend_truncate(void *context, uint64_t size);
kstatus_t vfs_fat32_backend_prepare_unlink(vfs_backend_node_t *backend);
/* 文件系统移除和提交期间，调用方持有后端 I/O 锁。 */
void vfs_fat32_backend_commit_unlink_locked(vfs_backend_node_t *backend);
void vfs_fat32_backend_abort_unlink(vfs_backend_node_t *backend);

/* 调用方持有 vnode 锁，仅用于最后引用释放等不存在并发写入的路径。 */
kstatus_t vfs_page_cache_flush(vnode_t *vnode);
/* 用于没有持有 vnode 锁的全局回写；与直接写入和截断保持事务顺序。 */
kstatus_t vfs_page_cache_writeback(vnode_t *vnode);
/* 调用方持有 vnode->lock；刷新与保留前缀相交的页面。 */
kstatus_t vfs_page_cache_flush_prefix(vnode_t *vnode, uint64_t size);
void vfs_page_cache_destroy(vnode_t *vnode);
kstatus_t vfs_truncate_node(vnode_t *vnode, uint64_t size);
kstatus_t vfs_file_page_get(vnode_t *vnode, uint64_t page_index,
                            struct page **out);
void vfs_file_page_mark_dirty(vnode_t *vnode, uint64_t page_index);
kstatus_t vfs_page_cache_overlay_read(vnode_t *vnode, uint64_t offset,
                                      void *buffer, size_t length);
bool vfs_page_cache_buffer_alias(vnode_t *vnode, const void *buffer, size_t length);
void vfs_page_cache_copy_in(vnode_t *vnode, uint64_t offset,
                            const uint8_t *source, size_t length);
void vfs_page_cache_resize(vnode_t *vnode, uint64_t size);
