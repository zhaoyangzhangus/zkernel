#include <arch/x86_64/uaccess.h>
#include <kernel/vfs.h>
#include <kernel/string.h>
#include "internal.h"

#ifdef OS_KERNEL_BUILD

/* REFACTOR_FS_VFS_FILE_IO_OWNER：内核文件位置和数据 I/O 策略。 */

kstatus_t vfs_load_file_kernel(const char *path, size_t minimum, size_t maximum,
                               void **buffer, size_t *length) {
    if (buffer) *buffer = 0;
    if (length) *length = 0U;
    if (!path || !buffer || !length || !minimum || maximum < minimum)
        return K_EINVAL;
    file_t *file = 0;
    kstatus_t status = vfs_open_kernel(path, VFS_OPEN_READ, 0U, &file);
    void *storage = 0;
    uint64_t size = 0U, bytes = 0U;
    if (status != K_OK) goto done;
    if (!file || !file->vnode) { status = K_EINVAL; goto done; }
    vfs_vnode_lock(file->vnode);
    size = file->vnode->size;
    vfs_vnode_unlock(file->vnode);
    if (size < minimum || size > maximum || size > SIZE_MAX) {
        status = K_EINVAL;
        goto done;
    }
    /* 只在读取大小时持有 vnode 锁；分配和实际 I/O 都在锁外。 */
    storage = kmalloc((size_t)size, 0U);
    if (!storage) { status = K_ENOMEM; goto done; }
    status = vfs_read_kernel(file, storage, (size_t)size, &bytes);
    if (status == K_OK && bytes != size) status = K_EIO;
done:
    if (file) vfs_close(file);
    if (status != K_OK) {
        if (storage) kfree(storage);
        return status;
    }
    *buffer = storage;
    *length = (size_t)size;
    return K_OK;
}

/* 整次写入的串行化与 vnode/后端锁分离：用户数据复制时可能获取这些内部锁。
 * 所有运行时 I/O 保护区都会延迟线程退出。 */
static kstatus_t vfs_write_transaction_begin(vfs_backend_node_t *backend,
                                             bool *locked) {
    kstatus_t status = io_mutex_lock(&backend->write_mutex);
    *locked = status == K_OK;
    return status;
}

static void vfs_write_transaction_end(vfs_backend_node_t *backend, bool locked) {
    if (locked) io_mutex_unlock_required(&backend->write_mutex);
}

static kstatus_t vfs_file_write_begin(file_t *file, vfs_backend_node_t *backend,
                                      size_t length, bool *locked) {
    kstatus_t status = vfs_write_transaction_begin(backend, locked);
    if (status != K_OK) return status;
    uint64_t position = file->position;
    if ((file->flags & VFS_OPEN_APPEND) != 0U) {
        vfs_vnode_lock(file->vnode);
        position = file->vnode->size;
        vfs_vnode_unlock(file->vnode);
    }
    if (length > UINT64_MAX - position) {
        vfs_write_transaction_end(backend, *locked);
        return K_EOVERFLOW;
    }
    file->position = position;
    return K_OK;
}

/* 直接 I/O 写入会更新页缓存视图，使 mmap 能看到新数据。 */
kstatus_t vfs_truncate_node(vnode_t *vnode, uint64_t size) {
    vfs_backend_node_t *backend;
    kstatus_t status;
    if (vnode == 0 || vnode->object.type != KOBJECT_TYPE_VNODE) return K_EINVAL;
    backend = (vfs_backend_node_t *)vnode->fs_private;
    if (backend == 0 || backend->directory) return K_EISDIR;
    if (backend->truncate == 0) return K_EACCES;
    bool locked;
    status = vfs_write_transaction_begin(backend, &locked);
    if (status != K_OK) return status;
    /* 清空文件会丢弃所有脏字节。先回写这些页面会浪费 I/O，还可能先分配 RAM 后备存储
     * 再立即释放；内存紧张时甚至会因此无法执行 truncate(0)。后端截断成功前保留缓存内容
     * 和脏位。 */
    if (size != 0U) {
        vfs_vnode_lock(vnode);
        /* EOF 不变时既不会丢弃也不会暴露字节。保留 mmap 脏页，不要把 ftruncate 当成 fsync；
         * 仍需在下方调用后端，以执行验证并返回元数据/错误。 */
        /* 完全位于新 EOF 之后的脏页仅在后端成功后丢弃。它们无需回写，且截断失败时必须保留
         * 脏数据。保留边界页：更改 EOF 前仍需持久化其保留的前缀。 */
        status = size == vnode->size ? K_OK :
            vfs_page_cache_flush_prefix(vnode, size);
        vfs_vnode_unlock(vnode);
        if (status != K_OK) {
            vfs_write_transaction_end(backend, locked);
            return status;
        }
    }
    vfs_backend_lock(backend);
    status = backend->truncate(backend->context, size);
    vfs_backend_unlock(backend);
    if (status == K_OK) {
        vfs_page_cache_resize(vnode, size);
    }
    vfs_write_transaction_end(backend, locked);
    return status;
}

kstatus_t vfs_seek(file_t *file, int64_t offset, uint32_t whence,
                   uint64_t *position) {
    uint64_t base;
    uint64_t result;
    if (file == 0 || file->object.type != KOBJECT_TYPE_FILE ||
        file->vnode == 0 || position == 0 || file->vnode->fs_private == 0) {
        return K_EBADF;
    }
    vfs_backend_node_t *backend = (vfs_backend_node_t *)file->vnode->fs_private;
    if (backend->directory) return K_EISDIR;
    if (whence != OS_FILE_SEEK_SET && whence != OS_FILE_SEEK_CURRENT &&
        whence != OS_FILE_SEEK_END) return K_EINVAL;
    kstatus_t status = vfs_file_position_lock(file);
    if (status != K_OK) return status;
    if (whence == OS_FILE_SEEK_SET) base = 0U;
    else if (whence == OS_FILE_SEEK_CURRENT) base = file->position;
    else {
        vfs_vnode_lock(file->vnode);
        base = file->vnode->size;
        vfs_vnode_unlock(file->vnode);
    }
    if (offset < 0) {
        uint64_t magnitude = (uint64_t)(-(offset + 1)) + 1U;
        if (magnitude > base) { status = K_EINVAL; goto done; }
        result = base - magnitude;
    } else {
        if ((uint64_t)offset > UINT64_MAX - base) { status = K_EOVERFLOW; goto done; }
        result = base + (uint64_t)offset;
    }
    file->position = result;
    *position = result;
done:
    vfs_file_position_unlock(file);
    return status;
}

kstatus_t vfs_truncate_kernel(file_t *file, uint64_t size) {
    if (file == 0 || file->object.type != KOBJECT_TYPE_FILE ||
        (file->rights & VFS_OPEN_WRITE) == 0U) return K_EBADF;
    return vfs_truncate_node(file->vnode, size);
}

kstatus_t vfs_truncate(file_t *file, uint64_t size) {
    return vfs_truncate_kernel(file, size);
}

static void vfs_read_kernel_yield(void) {
    /* 后端所有权已释放，进度也已提交。不得切换早期启动/空闲续执行路径，
     * 也不得切出调用方的外层临界区。仅释放本函数的抢占保护并不会消费 need_resched。 */
    if (sched_preempt_disabled()) return;
    thread_t *current = sched_current_thread();
    if (current == 0 || current->sched_class == SCHED_CLASS_IDLE) return;
    uint64_t flags;
    __asm__ volatile ("pushfq; popq %0" : "=r"(flags));
    if ((flags & (1ULL << 9)) != 0U) schedule();
}

/* 两种读取 API 共用后端加锁、EOF 重新验证和计数边界检查。每个分块都要重新验证：
 * 并发 truncate 可能在读取取得进展后改变 EOF。 */
static kstatus_t vfs_read_backend_chunk(vnode_t *vnode, vfs_backend_node_t *backend,
                                       uint64_t position, void *buffer,
                                       size_t length, uint64_t *bytes,
                                       bool kernel_buffer) {
    *bytes = 0U;
    void *staging = 0;
    void *destination = buffer;
    if (kernel_buffer && vfs_page_cache_present(vnode) &&
        vfs_page_cache_buffer_alias(vnode, buffer, length)) {
        /* 先保存缓存视图，再写入别名目的页，不能让后端覆盖尚未回写的源页。 */
        staging = kmalloc(length, 0U);
        if (staging == 0) return K_ENOMEM;
        destination = staging;
    }
    bool transaction = vfs_page_cache_present(vnode);
    for (;;) {
        *bytes = 0U;
        /* 不跨用户态复制持有事务锁；后端读取与缓存合并必须避开直接写入的中间状态。 */
        kstatus_t status = transaction ? vfs_backend_consistent_read_lock(backend) : K_OK;
        if (status != K_OK) {
            if (staging != 0) kfree(staging);
            return status;
        }
        vfs_backend_lock(backend);
        status = position >= backend->size ? K_OK :
            backend->read(backend->context, position, destination, length, bytes);
        vfs_backend_unlock(backend);
        if (status == K_OK && (*bytes > length || *bytes > UINT64_MAX - position))
            status = K_EIO;
        /* 无缓存时保留单次后端读取。若读取期间首次建立映射，重新读取并合并，
         * 避免直接写入恰好处于后端更新和页缓存更新之间。 */
        if (!transaction && status == K_OK && *bytes != 0U &&
            vfs_page_cache_present(vnode)) {
            transaction = true;
            continue;
        }
        if (transaction && status == K_OK && *bytes != 0U)
            status = vfs_page_cache_overlay_read(vnode, position, destination, (size_t)*bytes);
        if (transaction) vfs_backend_consistent_read_unlock(backend);
        if (staging != 0) {
            if (status == K_OK && *bytes != 0U) memmove(buffer, staging, (size_t)*bytes);
            kfree(staging);
        }
        return status;
    }
}

kstatus_t vfs_read(file_t *file, void __user *buf, size_t len, uint64_t *bytes) {
    if (file == 0 || file->object.type != KOBJECT_TYPE_FILE || file->vnode == 0 ||
        bytes == 0 || (file->rights & VFS_OPEN_READ) == 0U) return K_EINVAL;
    if (len != 0 && !x86_user_range_valid(buf, len)) return K_EINVAL;
    vfs_backend_node_t *backend =
        (vfs_backend_node_t *)file->vnode->fs_private;
    if (backend == 0 || backend->read == 0) return K_EIO;
    /* 当文件偏移位于 EOF 或其后时，POSIX read() 返回 EOF，而不是 EIO。
     * SQLite 探测/截断日志和数据库页面时依赖此行为。 */
    *bytes = 0U;
    if (len == 0U) return K_OK;
    kstatus_t lock_status = vfs_file_position_lock(file);
    if (lock_status != K_OK) return lock_status;
    size_t total = 0;
    kstatus_t result = K_OK;
    uint8_t fallback[PAGE_SIZE];
    uint8_t *temporary = fallback;
    size_t capacity = sizeof(fallback);
    /* 大块用户读取必须能进入 FAT 连续扇区路径，不能被逐页中转拆碎。
     * 小请求不分配；内存不足时沿用单页路径，每次系统调用最多暂存 64 KiB。 */
    if (len >= 4U * PAGE_SIZE) {
        /* 文件大小由后端锁保护；避免截断竞争导致无符号下溢或无谓的大缓冲分配。 */
        vfs_backend_lock(backend);
        uint64_t size = backend->size;
        vfs_backend_unlock(backend);
        if (file->position >= size) {
            vfs_file_position_unlock(file);
            return K_OK;
        }
        uint64_t available = size - file->position;
        size_t requested = available < len ? (size_t)available : len;
        if (requested >= 4U * PAGE_SIZE) {
            size_t batch = requested > 64U * 1024U ? 64U * 1024U : requested;
            uint8_t *allocated = kmalloc(batch, 0U);
            if (allocated) { temporary = allocated; capacity = batch; }
        }
    }
    while (total < len) {
        size_t chunk = len - total;
        if (chunk > capacity) chunk = capacity;
        uint64_t got = 0;
        kstatus_t status = vfs_read_backend_chunk(file->vnode, backend, file->position,
                                                  temporary, chunk, &got, false);
        if (status != K_OK) {
            result = status;
            break;
        }
        if (got == 0) break;
        if (copy_to_user((uint8_t __user *)buf + total, temporary, (size_t)got) != K_OK) {
            result = K_EACCES;
            break;
        }
        total += (size_t)got;
        file->position += got;
        if (got < chunk) break;
        if (total < len) vfs_read_kernel_yield();
    }
    if (temporary != fallback) kfree(temporary);
    *bytes = total;
    vfs_file_position_unlock(file);
    return result;
}

kstatus_t vfs_read_kernel(file_t *file, void *buf, size_t len, uint64_t *bytes) {
    if (file == 0 || file->object.type != KOBJECT_TYPE_FILE || file->vnode == 0 ||
        buf == 0 || bytes == 0 || (file->rights & VFS_OPEN_READ) == 0U) return K_EINVAL;
    vfs_backend_node_t *backend =
        (vfs_backend_node_t *)file->vnode->fs_private;
    if (backend == 0 || backend->read == 0) return K_EIO;
    /* EOF 处的内核 API 行为与用户态读取路径保持一致。 */
    *bytes = 0U;
    if (len == 0U) return K_OK;
    kstatus_t lock_status = vfs_file_position_lock(file);
    if (lock_status != K_OK) return lock_status;
    size_t total = 0;
    kstatus_t result = K_OK;
    while (total < len) {
        size_t chunk = len - total;
        /* 保留连续直接读取，并在有界批次之间释放后端所有权，给其他 I/O 和调度机会。 */
        if (chunk > 1024U * 1024U) chunk = 1024U * 1024U;
        uint64_t got = 0;
        kstatus_t status = vfs_read_backend_chunk(file->vnode, backend, file->position,
                                                  (uint8_t *)buf + total, chunk, &got, true);
        if (status != K_OK) {
            result = status;
            break;
        }
        if (got == 0) break;
        total += (size_t)got;
        file->position += got;
        if (got < chunk) break;
        if (total < len) vfs_read_kernel_yield();
    }
    *bytes = total;
    vfs_file_position_unlock(file);
    return result;
}

/* 仅提交经过验证的后端计数；复制缓存内容时不能持有后端锁。 */
static kstatus_t vfs_write_chunk(file_t *file, vfs_backend_node_t *backend,
                                const void *buffer, size_t length,
                                uint64_t *bytes) {
    *bytes = 0U;
    vfs_backend_lock(backend);
    kstatus_t status = backend->write(backend->context, file->position,
                                      buffer, length, bytes);
    vfs_backend_unlock(backend);
    if (status != K_OK || *bytes > length) return status == K_OK ? K_EIO : status;
    if (*bytes != 0U) {
        vfs_page_cache_copy_in(file->vnode, file->position, buffer, (size_t)*bytes);
        file->position += *bytes;
    }
    return K_OK;
}

kstatus_t vfs_write(file_t *file, const void __user *buf, size_t len, uint64_t *bytes) {
    if (file == 0 || file->object.type != KOBJECT_TYPE_FILE || file->vnode == 0 ||
        bytes == 0 || (file->rights & VFS_OPEN_WRITE) == 0U) return K_EACCES;
    vfs_backend_node_t *backend =
        (vfs_backend_node_t *)file->vnode->fs_private;
    if (backend == 0 || backend->write == 0) return K_EACCES;
    if (len != 0 && !x86_user_range_valid(buf, len)) return K_EINVAL;
    *bytes = 0U;
    if (len == 0U) return K_OK;

    uint8_t fallback[PAGE_SIZE];
    uint8_t *temporary = fallback;
    size_t capacity = sizeof(fallback);
    /* 大块写入使用有界缓冲区；分配失败时保留单页路径。分配放在事务锁外。 */
    if (len >= 4U * PAGE_SIZE) {
        size_t batch = len > 64U * 1024U ? 64U * 1024U : len;
        uint8_t *allocated = kmalloc(batch, 0U);
        if (allocated != 0) { temporary = allocated; capacity = batch; }
    }

    kstatus_t status = vfs_file_position_lock(file);
    if (status != K_OK) {
        if (temporary != fallback) kfree(temporary);
        return status;
    }
    bool locked;
    status = vfs_file_write_begin(file, backend, len, &locked);
    if (status != K_OK) {
        vfs_file_position_unlock(file);
        if (temporary != fallback) kfree(temporary);
        return status;
    }
    size_t total = 0;
    kstatus_t result = K_OK;
    while (total < len) {
        size_t chunk = len - total;
        if (chunk > capacity) chunk = capacity;
        if (copy_from_user(temporary, (const uint8_t __user *)buf + total, chunk) != K_OK) {
            /* 大块跨越无效页面时以原有粒度重试，提交故障之前可完整读取的页面。 */
            if (chunk > sizeof(fallback)) {
                capacity = sizeof(fallback);
                continue;
            }
            result = K_EACCES;
            break;
        }
        uint64_t written = 0;
        kstatus_t status = vfs_write_chunk(file, backend, temporary, chunk, &written);
        if (status != K_OK) {
            result = status;
            break;
        }
        if (written == 0) break;
        total += (size_t)written;
        if (written < chunk) break;
    }
    *bytes = total;
    vfs_write_transaction_end(backend, locked);
    vfs_file_position_unlock(file);
    if (temporary != fallback) kfree(temporary);
    return result;
}

kstatus_t vfs_write_kernel(file_t *file, const void *buf, size_t len,
                           uint64_t *bytes) {
    if (file == 0 || file->object.type != KOBJECT_TYPE_FILE || file->vnode == 0 ||
        (buf == 0 && len != 0) || bytes == 0 ||
        (file->rights & VFS_OPEN_WRITE) == 0U) return K_EACCES;
    vfs_backend_node_t *backend =
        (vfs_backend_node_t *)file->vnode->fs_private;
    if (backend == 0 || backend->write == 0) return K_EACCES;
    *bytes = 0U;
    if (len == 0U) return K_OK;
    kstatus_t status = vfs_file_position_lock(file);
    if (status != K_OK) return status;
    bool locked;
    status = vfs_file_write_begin(file, backend, len, &locked);
    if (status != K_OK) {
        vfs_file_position_unlock(file);
        return status;
    }
    size_t total = 0;
    kstatus_t result = K_OK;
    while (total < len) {
        size_t chunk = len - total;
        /* 内核缓冲区无需用户复制中转；将连续数据交给后端批量写入，而非逐页调用。
         * 批次保持有界，整次追加写的串行化仍由外层 write_mutex 保证。 */
        if (chunk > 1024U * 1024U) chunk = 1024U * 1024U;
        uint64_t written = 0;
        kstatus_t status = vfs_write_chunk(file, backend,
                                          (const uint8_t *)buf + total, chunk, &written);
        if (status != K_OK) {
            result = status;
            break;
        }
        if (written == 0) break;
        total += (size_t)written;
        if (written < chunk) break;
    }
    *bytes = total;
    vfs_write_transaction_end(backend, locked);
    vfs_file_position_unlock(file);
    return result;
}

kstatus_t vfs_fsync(file_t *file) {
    if (file == 0 || file->object.type != KOBJECT_TYPE_FILE || file->vnode == 0) {
        return K_EINVAL;
    }
    vfs_backend_node_t *backend =
        (vfs_backend_node_t *)file->vnode->fs_private;
    if (backend == 0) return K_EIO;
    return vfs_page_cache_sync(file->vnode, 0U, UINT64_MAX);
}

void vfs_close(file_t *file) {
    object_put(file);
}

#endif
