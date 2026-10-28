#pragma once
#pragma once
#include "base.h"
#include "object.h"
#include "list.h"
#include "rbtree.h"
#include "spinlock.h"
#include "fat32.h"
#include "wait.h"
#include "io_mutex.h"

struct page;
struct vm_file_ops;
struct vm_area;

typedef struct vnode_ops vnode_ops_t;
typedef struct file_ops file_ops_t;

typedef struct vnode {
    object_header_t object;
    uint64_t inode_id;
    uint32_t mode;
    uint32_t flags;
    uint64_t size;

    const vnode_ops_t *ops;
    void *fs_private;

    io_mutex_t lock;
    void *page_cache;
    wait_queue_t record_lock_waitq;
    atomic_uint record_lock_sequence;
} vnode_t;

typedef struct file {
    object_header_t object;
    vnode_t *vnode;
    const file_ops_t *ops;
    uint64_t position;
    uint32_t flags;
    uint32_t rights;
    void *private_data;
    /* 同一打开实例共享的偏移须跨读、写、定位和目录遍历串行更新。 */
    io_mutex_t position_lock;
} file_t;

static inline void vfs_file_position_init(file_t *file) {
    io_mutex_init(&file->position_lock, true);
}

static inline kstatus_t vfs_file_position_lock(file_t *file) {
    return io_mutex_lock(&file->position_lock);
}

static inline void vfs_file_position_unlock(file_t *file) {
    io_mutex_unlock_required(&file->position_lock);
}

kstatus_t vfs_open(const char __user *path, uint32_t flags, uint32_t mode, file_t **out);
kstatus_t vfs_read(file_t *file, void __user *buf, size_t len, uint64_t *bytes);
kstatus_t vfs_write(file_t *file, const void __user *buf, size_t len, uint64_t *bytes);
kstatus_t vfs_fsync(file_t *file);
/* 执行电源控制前，刷新缓存文件数据和所有已挂载的 FAT 卷。 */
kstatus_t vfs_sync_all(void);
void vfs_close(file_t *file);
#include <uapi/file.h>

/* 规范 VFS 的最小打开标志；文件系统后端可以继续扩展更多权限位。 */
#define VFS_OPEN_READ   (1U << 0)
#define VFS_OPEN_WRITE  (1U << 1)
#define VFS_OPEN_CREATE (1U << 2)
#define VFS_OPEN_EXCLUSIVE (1U << 3)
#define VFS_OPEN_TRUNCATE (1U << 4)
#define VFS_OPEN_APPEND (1U << 5)
#define VFS_OPEN_DIRECTORY (1U << 6)

/* 文件对象使用独立的内核对象类型，句柄权限与打开标志保持一致。 */
#define KOBJECT_TYPE_VNODE 0x0103U
#define KOBJECT_TYPE_FILE  0x0104U
#define FILE_RIGHT_READ    VFS_OPEN_READ
#define FILE_RIGHT_WRITE   VFS_OPEN_WRITE

struct page;
/* 统一文件后端使用内核缓冲区，避免文件系统直接接触用户地址。 */
typedef kstatus_t (*vfs_backend_read_t)(void *context, uint64_t offset,
                                        void *buffer, size_t length,
                                        uint64_t *bytes);
typedef kstatus_t (*vfs_backend_write_t)(void *context, uint64_t offset,
                                         const void *buffer, size_t length,
                                         uint64_t *bytes);
typedef kstatus_t (*vfs_backend_fsync_t)(void *context);
typedef kstatus_t (*vfs_backend_truncate_t)(void *context, uint64_t size);

kstatus_t vfs_register_backend_file(const char *path, uint64_t size, uint32_t mode,
                                    vfs_backend_read_t read,
                                    vfs_backend_write_t write,
                                    vfs_backend_fsync_t fsync,
                                    void *context);
kstatus_t vfs_register_backend_file_ex(const char *path, uint64_t size, uint32_t mode,
                                       vfs_backend_read_t read,
                                       vfs_backend_write_t write,
                                       vfs_backend_fsync_t fsync,
                                       vfs_backend_truncate_t truncate,
                                       void *context);
kstatus_t vfs_mount_fat32(const char *prefix, OS_FAT32 *filesystem);
kstatus_t vfs_unmount_fat32(void);
kstatus_t vfs_create_kernel(const char *path, uint32_t mode, bool directory);
kstatus_t vfs_mkdir_kernel(const char *path, uint32_t mode);
/* 当前进程的工作目录使用同一命名空间及用户路径解析规则。 */
kstatus_t vfs_change_directory(const char __user *path);
kstatus_t vfs_symlink_kernel(const char *target, const char *path);
kstatus_t vfs_readlink_kernel(const char *path, char *buffer, size_t capacity,
                             size_t *bytes);
kstatus_t vfs_resolve_path_kernel(const char *path, bool follow_final,
                                bool allow_missing_leaf, char output[256]);
/* FAT 不含链接；内存命名空间和特殊分量需逐段解析。 */
bool vfs_path_needs_resolution(const char *path);
/* 为需要保留实际路径的调用者解析用户路径，包括进程当前目录。 */
kstatus_t vfs_resolve_user_path(const char __user *path, bool follow_final,
                                bool allow_missing_leaf, char output[256]);
kstatus_t vfs_remove_kernel(const char *path);
kstatus_t vfs_remove_typed_kernel(const char *path, int directory);
kstatus_t vfs_directory_path_kernel(file_t *file, char *path, size_t capacity);
/* 引用由调用方持有；仅复制命名空间元数据，不执行后备 I/O。 */
kstatus_t vfs_vnode_path_kernel(vnode_t *vnode, char *path, size_t capacity,
                                bool *deleted);
kstatus_t vfs_open_kernel(const char *path, uint32_t flags, uint32_t mode,
                          file_t **out);
kstatus_t vfs_enumerate_file(file_t *file, uint32_t index,
                             os_file_info_t *out);
kstatus_t vfs_enumerate_kernel_batch(const char *path, uint32_t index,
                                     os_file_info_t *out, uint32_t capacity,
                                     uint32_t *count);
kstatus_t vfs_enumerate_batch(const char __user *path, uint32_t index,
                              os_file_info_t *out, uint32_t capacity,
                              uint32_t *count);
/* 从一次命名空间枚举中复制一批有界记录。失败时 count 为零；返回记录为私有副本，
 * 而非注册表指针。 */
kstatus_t vfs_enumerate_file_batch(file_t *file, uint32_t index,
                                   os_file_info_t *out, uint32_t capacity,
                                   uint32_t *count);
kstatus_t vfs_stat_kernel(const char *path, os_file_info_t *out);
/* 查询实际后备文件系统；空闲空间并不代表已预留。 */
typedef struct vfs_space_info {
    uint64_t block_size, blocks, blocks_free;
    uint64_t type, filesystem_id;
    uint64_t name_max;
} vfs_space_info_t;
kstatus_t vfs_space_kernel(const char *path, vfs_space_info_t *out);
kstatus_t vfs_file_space(file_t *file, vfs_space_info_t *out);
kstatus_t vfs_file_stat(file_t *file, os_file_info_t *out);
kstatus_t vfs_rename_kernel(const char *old_path, const char *new_path);
kstatus_t vfs_enumerate_kernel(const char *path, uint32_t index,
                               os_file_info_t *out);
kstatus_t vfs_stat(const char __user *path, os_file_info_t *out);
kstatus_t vfs_enumerate(const char __user *path, uint32_t index,
                        os_file_info_t *out);
kstatus_t vfs_rename(const char __user *old_path,
                     const char __user *new_path);
kstatus_t vfs_file_page_get(vnode_t *vnode, uint64_t page_index,
                            struct page **out);
void vfs_file_page_mark_dirty(vnode_t *vnode, uint64_t page_index);
kstatus_t vfs_page_cache_sync(vnode_t *vnode, uint64_t first_page,
                              uint64_t page_count);
const struct vm_file_ops *vfs_vm_file_ops(void);
kstatus_t vfs_read_kernel(file_t *file, void *buf, size_t len, uint64_t *bytes);
/* 有界整文件读取；成功后调用者用 kfree 释放缓冲区，失败时输出为空。
 * 读取打开后观察到的长度，不保证与并发文件写入之间的原子快照。 */
kstatus_t vfs_load_file_kernel(const char *path, size_t minimum, size_t maximum,
                               void **buffer, size_t *length);
/* 可执行文件加载与 mmap 共用一致的文件页。 */
kstatus_t vfs_read_cached_kernel(file_t *file, void *buf, size_t len,
                                 uint64_t *bytes);
kstatus_t vfs_write_kernel(file_t *file, const void *buf, size_t len,
                           uint64_t *bytes);
kstatus_t vfs_seek(file_t *file, int64_t offset, uint32_t whence,
                   uint64_t *position);
kstatus_t vfs_truncate(file_t *file, uint64_t size);
kstatus_t vfs_truncate_kernel(file_t *file, uint64_t size);
kstatus_t vfs_remove(const char __user *path);
kstatus_t vfs_mkdir(const char __user *path, uint32_t mode);
