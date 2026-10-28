#include <kernel/kmem.h>
#include <kernel/console.h>
#include <kernel/string.h>
#include "internal.h"

#ifdef OS_KERNEL_BUILD

/* REFACTOR_FS_VFS_BACKEND_OWNER：内存/FAT32 适配器和后端注册。 */

/* 在进程销毁释放内存前保留首次分配失败信息。状态 0=空，1=写入中，2=已发布
 *（读取前需 acquire）。仅失败时记录，不分配且不阻塞：调用方持有后端锁。
 * 物理页计数是跨 CPU 的近似采样，不代表最大连续空闲块大小。后续失败只递增计数。 */
struct {
    uint64_t failures;
    uint32_t state;
    uint64_t required;
    uint64_t attempted_capacity;
    uint64_t previous_size;
    uint64_t previous_capacity;
    uint64_t total_pages;
    uint64_t free_pages;
    kstatus_t stats_status;
} g_vfs_memory_allocation_failure;

static void vfs_memory_note_failure(const vfs_backend_node_t *backend,
                                    uint64_t required, uint64_t capacity) {
    __atomic_fetch_add(&g_vfs_memory_allocation_failure.failures, 1U,
                       __ATOMIC_RELAXED);
    uint32_t expected = 0U;
    if (!__atomic_compare_exchange_n(&g_vfs_memory_allocation_failure.state,
                                    &expected, 1U, false,
                                    __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) return;
    uint64_t total = 0U, free = 0U;
    kstatus_t status = page_memory_get_stats(&total, &free);
    g_vfs_memory_allocation_failure.required = required;
    g_vfs_memory_allocation_failure.attempted_capacity = capacity;
    g_vfs_memory_allocation_failure.previous_size = backend->size;
    g_vfs_memory_allocation_failure.previous_capacity = backend->data_capacity;
    g_vfs_memory_allocation_failure.total_pages = total;
    g_vfs_memory_allocation_failure.free_pages = free;
    g_vfs_memory_allocation_failure.stats_status = status;
    __atomic_store_n(&g_vfs_memory_allocation_failure.state, 2U,
                     __ATOMIC_RELEASE);
}

/* 对追加写入采用摊销扩容，不用于显式指定大小的共享内存文件。截断大小向上取整到 2 的
 * 幂后，计入头部/红区可能使 kmalloc 进入下一个 Buddy 阶（例如 3 MiB -> 4 MiB ->
 * 物理分配 8 MiB）。可见文件大小仍是 EOF，而不是分配容量。 */
static kstatus_t vfs_memory_reserve(vfs_backend_node_t *backend,
                                   uint64_t required, bool amortize,
                                   uint8_t **retired, bool *zeroed) {
    if (retired != 0) *retired = 0;
    if (zeroed != 0) *zeroed = false;
    uint64_t capacity = backend->data_capacity;
    if (backend->data != 0 && capacity < backend->size) capacity = backend->size;
    if (required <= capacity) return K_OK;
    if (required > SIZE_MAX) return K_EOVERFLOW;
    if (!amortize) capacity = required;
    /* 只有追加写入需要增长下限。显式指定大小的小文件可以使用 slab 槽位，
     * 避免 4 KiB 负载加分配器元数据后被迫占用 8 KiB 物理块。 */
    if (amortize && capacity < 4096U) capacity = 4096U;
    while (capacity < required) {
        if (capacity > SIZE_MAX / 2U) { capacity = required; break; }
        capacity *= 2U;
    }
    /* 大型 kmalloc 块在下一个阶边界前通常仍有可用余量。通过移动保护区使用这部分空间，
     * 无需在新旧物理块同时存活时复制整个文件。额外容量是可选的：摊销扩容失败时，
     * 再尝试精确请求大小。 */
    if (backend->data != 0) {
        if (kmalloc_try_grow(backend->data, (size_t)capacity)) {
            backend->data_capacity = capacity;
            return K_OK;
        }
        if (capacity != required &&
            kmalloc_try_grow(backend->data, (size_t)required)) {
            backend->data_capacity = required;
            return K_OK;
        }
    }
    /* 显式扩展必须使新增字节为零。新后备存储只需初始化一次，不要在这里填毒后又在
     * truncate 中清零。追加写入的源数据可能别名到旧余量：保留原有的先复制、后清洞顺序，
     * 并遵循分配器填毒规则。 */
    uint8_t *replacement = (uint8_t *)(amortize ?
        kmalloc((size_t)capacity, 0) : kzalloc((size_t)capacity, 0));
    /* 内存压力下可以不保留额外容量。 */
    if (replacement == 0 && capacity != required) {
        capacity = required;
        replacement = (uint8_t *)kmalloc((size_t)capacity, 0);
    }
    if (replacement == 0) {
        vfs_memory_note_failure(backend, required, capacity);
        return K_ENOMEM;
    }
    if (backend->size != 0U)
        memcpy(replacement, backend->data, (size_t)backend->size);
    /* 写入源可能与此分配区别名。只有最后一次 memmove 消费完源数据后，
     * 调用方才能释放退役缓冲区。 */
    if (retired != 0) *retired = backend->data;
    else kfree(backend->data);
    backend->data = replacement;
    backend->data_capacity = capacity;
    if (zeroed != 0) *zeroed = !amortize;
    return K_OK;
}

kstatus_t vfs_memory_backend_read(void *context, uint64_t offset,
                                   void *buffer, size_t length,
                                   uint64_t *bytes) {
    vfs_backend_node_t *backend = (vfs_backend_node_t *)context;
    if (backend == 0 || bytes == 0 || (length != 0 && buffer == 0) ||
        offset > backend->size) return K_EINVAL;
    uint64_t count = backend->size - offset;
    if (count > length) count = length;
    if (count != 0U) memmove(buffer, backend->data + offset, (size_t)count);
    *bytes = count;
    return K_OK;
}

kstatus_t vfs_memory_backend_write(void *context, uint64_t offset,
                                    const void *buffer, size_t length,
                                    uint64_t *bytes) {
    vfs_backend_node_t *backend = (vfs_backend_node_t *)context;
    uint64_t required;
    if (backend == 0 || backend->directory || bytes == 0 ||
        (length != 0U && buffer == 0) || offset > UINT64_MAX - length) {
        return K_EINVAL;
    }
    required = offset + length;
    if (length == 0U) { *bytes = 0U; return K_OK; }
    uint8_t *retired = 0;
    if (required > backend->size) {
        kstatus_t status = vfs_memory_reserve(backend, required, true, &retired, 0);
        if (status != K_OK) return status;
    }
    /* 清除空洞前先消费别名源数据：有效调用方数据可能位于此分配区的额外容量中，
     * 恰好落在该空洞内。空洞止于 offset，不会与目标写入范围重叠。 */
    memmove(backend->data + offset, buffer, length);
    if (offset > backend->size)
        memset(backend->data + backend->size, 0, (size_t)(offset - backend->size));
    if (required > backend->size) backend->size = required;
    if (retired != 0) kfree(retired);
    *bytes = length;
    return K_OK;
}

kstatus_t vfs_memory_backend_truncate(void *context, uint64_t size) {
    vfs_backend_node_t *backend = (vfs_backend_node_t *)context;
    if (backend == 0 || backend->directory || size > SIZE_MAX) return K_EINVAL;
    if (size == backend->size) return K_OK;
    if (size == 0U) {
        kfree(backend->data);
        backend->data = 0;
        backend->size = 0U;
        backend->data_capacity = 0U;
        return K_OK;
    }
    /* 缩小 EOF 后不能永久保留曾经很大的缓存/日志区。滞回策略可避免小幅编辑和小型后备区频繁重分配。
     * 回收是可选操作，因此内存碎片不能导致 truncate 失败，也不能让成功的缩小操作产生扩容失败诊断。 */
    if (size < backend->size && backend->data_capacity > 4096U &&
        size <= backend->data_capacity / 4U) {
        /* 与显式扩展相同，缩小操作知道精确的新 EOF。此处向上取整可能在计入 kmalloc 元数据后
         * 再次使物理分配阶翻倍；摊销扩容仅用于实际追加写入。 */
        uint64_t capacity = size;
        uint8_t *replacement = (uint8_t *)kmalloc((size_t)capacity, 0);
        if (replacement != 0) {
            memcpy(replacement, backend->data, (size_t)size);
            uint8_t *previous = backend->data;
            backend->data = replacement;
            backend->data_capacity = capacity;
            kfree(previous);
        }
    }
    bool zeroed;
    kstatus_t status = vfs_memory_reserve(backend, size, false, 0, &zeroed);
    if (status != K_OK) return status;
    if (size > backend->size && !zeroed)
        memset(backend->data + backend->size, 0, (size_t)(size - backend->size));
    backend->size = size;
    return K_OK;
}

kstatus_t vfs_fat32_backend_read(void *context, uint64_t offset,
                                  void *buffer, size_t length,
                                  uint64_t *bytes) {
    vfs_backend_node_t *backend = (vfs_backend_node_t *)context;
    char relative[VFS_PATH_LIMIT];
    vfs_fat32_mount_t *mount;
    UINT32 transferred = 0U;
    if (backend != 0 && backend->detached) {
        return vfs_memory_backend_read(backend, offset, buffer, length, bytes);
    }
    if (backend == 0 || bytes == 0 || length > UINT32_MAX) return K_EINVAL;
    if (backend->fat_file != 0) {
        if (!fat32_read_file(backend->fat_file, offset, buffer,
                                    (UINT32)length, &transferred)) {
            return K_EIO;
        }
        *bytes = transferred;
        return K_OK;
    }
    if (
        (mount = vfs_fat32_mount_for_path(backend->path, relative)) == 0 ||
        relative[0] == 0 ||
        !fat32_read_path(mount->filesystem, relative, offset,
                                 buffer, (UINT32)length, &transferred)) return K_EIO;
    *bytes = transferred;
    return K_OK;
}

kstatus_t vfs_fat32_backend_write(void *context, uint64_t offset,
                                   const void *buffer, size_t length,
                                   uint64_t *bytes) {
    vfs_backend_node_t *backend = (vfs_backend_node_t *)context;
    char relative[VFS_PATH_LIMIT];
    vfs_fat32_mount_t *mount;
    UINT32 transferred = 0U;
    if (backend != 0 && backend->detached) {
        return vfs_memory_backend_write(backend, offset, buffer, length, bytes);
    }
    /* 写入失败或部分完成后也要使快照失效。 */
    if (backend != 0 && length != 0U) ++backend->content_revision;
    if (backend != 0 && backend->fat_file != 0) {
        if (bytes == 0 || length > UINT32_MAX ||
            !fat32_write_file(backend->fat_file, offset, buffer,
                                     (UINT32)length, &transferred)) return K_EIO;
        *bytes = transferred;
        backend->size = backend->fat_file->Size;
        return K_OK;
    }
    if (backend == 0 || bytes == 0 || length > UINT32_MAX ||
        (mount = vfs_fat32_mount_for_path(backend->path, relative)) == 0 ||
        relative[0] == 0 ||
        !fat32_write_path(mount->filesystem, relative, offset,
                                 buffer, (UINT32)length, &transferred)) return K_EIO;
    *bytes = transferred;
    if (offset <= UINT64_MAX - transferred && offset + transferred > backend->size) {
        backend->size = offset + transferred;
    }
    return K_OK;
}

kstatus_t vfs_fat32_backend_fsync(void *context) {
    vfs_backend_node_t *backend = (vfs_backend_node_t *)context;
    char relative[VFS_PATH_LIMIT];
    vfs_fat32_mount_t *mount;
    if (backend != 0 && backend->detached) return K_OK;
    if (backend != 0 && backend->fat_file != 0) {
        /* 使用已打开 inode 所属的文件系统，而不是可能变化的命名空间路径。
         * 这也避免普通文件 fsync 反复扫描挂载点。 */
        OS_FAT32_FILE *file = backend->fat_file;
        if (!file->Used || file->FileSystem == 0) return K_EIO;
        return fat32_sync(file->FileSystem) ? K_OK : K_EIO;
    }
    if (backend == 0 ||
        (mount = vfs_fat32_mount_for_path(backend->path, relative)) == 0) {
        return K_EIO;
    }
    return fat32_sync(mount->filesystem) ? K_OK : K_EIO;
}

kstatus_t vfs_fat32_backend_truncate(void *context, uint64_t size) {
    vfs_backend_node_t *backend = (vfs_backend_node_t *)context;
    char relative[VFS_PATH_LIMIT];
    vfs_fat32_mount_t *mount;
    if (backend != 0 && backend->detached) {
        return vfs_memory_backend_truncate(backend, size);
    }
    if (backend != 0 && size != backend->size) ++backend->content_revision;
    if (backend != 0 && backend->fat_file != 0) {
        if (!fat32_truncate_file(backend->fat_file, size)) return K_EIO;
        backend->size = backend->fat_file->Size;
        return K_OK;
    }
    if (backend == 0 ||
        (mount = vfs_fat32_mount_for_path(backend->path, relative)) == 0 ||
        relative[0] == 0 || !fat32_truncate_path(mount->filesystem,
                                                        relative, size)) return K_EIO;
    backend->size = size;
    return K_OK;
}

/* FAT 路径辅助函数会按名称重新打开文件，而 POSIX unlink 会保持已打开 inode 存活。
 * 删除目录项前先为缓存 vnode 创建快照，使已有句柄和延迟文件映射继续看到同一文件。 */
kstatus_t vfs_fat32_backend_prepare_unlink(vfs_backend_node_t *backend) {
    char relative[VFS_PATH_LIMIT];
    uint8_t *snapshot = 0;
    uint64_t offset = 0U;
    uint64_t size;
    uint64_t revision;
    OS_FAT32_FILE *file;
    vfs_fat32_mount_t *mount = 0;
    if (backend == 0) return K_OK;
    vfs_backend_lock(backend);
    if (backend->directory || backend->detached) {
        vfs_backend_unlock(backend);
        return K_OK;
    }
    if (backend->data != 0 || backend->unlink_prepared) {
        vfs_backend_unlock(backend);
        return K_EBUSY;
    }
    size = backend->size;
    revision = backend->content_revision;
    file = backend->fat_file;
    vfs_backend_unlock(backend);
    if (size > SIZE_MAX) return K_EINVAL;
    if (file == 0 &&
        ((mount = vfs_fat32_mount_for_path(backend->path, relative)) == 0 ||
         relative[0] == 0U)) return K_EINVAL;
    if (size != 0U) {
        snapshot = (uint8_t *)kmalloc((size_t)size, 0);
        if (snapshot == 0) return K_ENOMEM;
        while (offset < size) {
            uint64_t remaining = size - offset;
            UINT32 capacity = remaining > 1024U * 1024U ? 1024U * 1024U :
                              (UINT32)remaining;
            UINT32 transferred = 0U;
            /* 保留已打开 inode 的标识；获取此 vnode 后，命名空间可能已经变化。 */
            BOOLEAN success = file != 0 ?
                fat32_read_file(file, offset,
                                        snapshot + offset, capacity, &transferred) :
                fat32_read_path(mount->filesystem, relative, offset,
                                        snapshot + offset, capacity, &transferred);
            if (!success ||
                transferred != capacity) {
                kfree(snapshot);
                return K_EIO;
            }
            offset += transferred;
        }
    }
    vfs_backend_lock(backend);
    if (backend->size != size || backend->content_revision != revision ||
        backend->fat_file != file || backend->detached || backend->data != 0 ||
        backend->unlink_prepared) {
        vfs_backend_unlock(backend);
        kfree(snapshot);
        return K_EBUSY;
    }
    backend->data = snapshot;
    backend->data_capacity = size;
    backend->snapshot_size = size;
    backend->snapshot_revision = revision;
    backend->unlink_prepared = true;
    vfs_backend_unlock(backend);
    return K_OK;
}

void vfs_fat32_backend_commit_unlink_locked(vfs_backend_node_t *backend) {
    if (backend == 0) return;
    __atomic_store_n(&backend->detached, true, __ATOMIC_RELEASE);
    backend->unlink_prepared = false;
}

void vfs_fat32_backend_abort_unlink(vfs_backend_node_t *backend) {
    uint8_t *snapshot;
    if (backend == 0) return;
    vfs_backend_lock(backend);
    snapshot = backend->data;
    backend->data = 0;
    backend->data_capacity = 0;
    backend->unlink_prepared = false;
    vfs_backend_unlock(backend);
    kfree(snapshot);
}

kstatus_t vfs_register_backend_file_ex(const char *path, uint64_t size, uint32_t mode,
                                       vfs_backend_read_t read,
                                       vfs_backend_write_t write,
                                       vfs_backend_fsync_t fsync,
                                       vfs_backend_truncate_t truncate,
                                       void *context) {
    if (path == 0 || path[0] != '/' || read == 0 ||
        (mode & 0170000U) != 0100000U) return K_EINVAL;
    size_t path_length = vfs_string_length(path);
    if (path_length >= VFS_PATH_LIMIT) return K_EINVAL;
    vfs_backend_node_t *backend =
        (vfs_backend_node_t *)kzalloc(sizeof(*backend), 0);
    if (backend == 0) return K_ENOMEM;
    memcpy(backend->path, path, path_length + 1U);
    backend->size = size;
    backend->mode = mode;
    backend->directory = false;
    backend->mutable_node = false;
    backend->read = read;
    backend->write = write;
    backend->fsync = fsync;
    backend->truncate = truncate;
    backend->context = context;
    io_mutex_init(&backend->io_lock, true);
    return vfs_register_node(backend);
}

kstatus_t vfs_register_backend_file(const char *path, uint64_t size, uint32_t mode,
                                    vfs_backend_read_t read,
                                    vfs_backend_write_t write,
                                    vfs_backend_fsync_t fsync,
                                    void *context) {
    return vfs_register_backend_file_ex(path, size, mode, read, write, fsync,
                                         0, context);
}

#endif
