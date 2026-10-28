#include <kernel/kmem.h>
#include <kernel/object.h>
#include <kernel/vfs.h>
#include <kernel/fat32.h>
#include <kernel/string.h>
#include <kernel/mm.h>
#include "internal.h"

#ifdef OS_KERNEL_BUILD
#define VFS_DIRECTORY_ENTRY_LIMIT 256U
#define VFS_INITIAL_NODE_CAPACITY 256U

static vnode_t *g_vfs_initial_nodes[VFS_INITIAL_NODE_CAPACITY];
static vnode_t **g_vfs_nodes = g_vfs_initial_nodes;
static vnode_t *g_vfs_initial_buckets[VFS_INITIAL_NODE_CAPACITY];
static vnode_t **g_vfs_buckets = g_vfs_initial_buckets;
static uint32_t g_vfs_initial_fat_buckets[VFS_INITIAL_NODE_CAPACITY];
static uint32_t g_vfs_initial_fat_next[VFS_INITIAL_NODE_CAPACITY];
static uint32_t *g_vfs_fat_buckets = g_vfs_initial_fat_buckets;
static uint32_t *g_vfs_fat_next = g_vfs_initial_fat_next;
static uint32_t g_vfs_node_capacity = VFS_INITIAL_NODE_CAPACITY;
static uint32_t g_vfs_next_free_node;
static spinlock_t g_vfs_lock;
static atomic_uint g_vfs_init_state;
/* 加载多个 DSO 时，Linux fstat 需要不同的文件标识。 */
static atomic_uint_fast64_t g_vfs_next_inode_id;
vfs_fat32_mount_t g_fat32_mounts[VFS_FAT32_MOUNT_LIMIT];

static void vfs_lock(void) {
    vfs_state_lock(&g_vfs_lock.state);
}

static void vfs_unlock(void) {
    vfs_state_unlock(&g_vfs_lock.state);
}

size_t vfs_string_length(const char *text) {
    size_t length = 0;
    if (text == 0) return 0;
    while (length < VFS_PATH_LIMIT && text[length] != 0) ++length;
    return length;
}

static void vnode_destroy(void *object) {
    vnode_t *vnode = (vnode_t *)object;
    vfs_backend_node_t *backend = vnode != 0 ?
                                  (vfs_backend_node_t *)vnode->fs_private : 0;
    if (vnode != 0) {
        /* 最后一个 vnode 引用释放时仍要尽力写回 mmap 脏页。 */
        vfs_vnode_lock(vnode);
        (void)vfs_page_cache_flush(vnode);
        vfs_vnode_unlock(vnode);
        vfs_page_cache_destroy(vnode);
    }
    if (backend != 0) {
        kfree(backend->data);
        if (backend->fat_file != 0) {
            (void)fat32_close(backend->fat_file);
            backend->fat_file = 0;
        }
        kfree(backend);
    }
    kfree(vnode);
}

static void file_destroy(void *object) {
    file_t *file = (file_t *)object;
    if (file != 0) {
        if (file->vnode != 0) object_put(file->vnode);
        kfree(file);
    }
}

static const object_ops_t g_vnode_object_ops = {
    .destroy = vnode_destroy,
    .type_name = "Vnode",
    .is_signaled = 0,
    .wait_value = 0,
};

static const object_ops_t g_file_object_ops = {
    .destroy = file_destroy,
    .type_name = "File",
    .is_signaled = 0,
    .wait_value = 0,
};

static void vfs_initialize(void) {
    if (atomic_load_explicit(&g_vfs_init_state, memory_order_acquire) == 2U) return;
    unsigned expected = 0U;
    if (atomic_compare_exchange_strong_explicit(&g_vfs_init_state, &expected, 1U,
                                                 memory_order_acq_rel,
                                                 memory_order_acquire)) {
        atomic_init(&g_vfs_lock.state, 0U);
        for (uint32_t i = 0; i < g_vfs_node_capacity; ++i) {
            g_vfs_nodes[i] = 0;
            g_vfs_buckets[i] = 0;
            g_vfs_fat_buckets[i] = 0U;
            g_vfs_fat_next[i] = 0U;
        }
        g_vfs_next_free_node = 0U;
        atomic_store_explicit(&g_vfs_init_state, 2U, memory_order_release);
        return;
    }
    while (atomic_load_explicit(&g_vfs_init_state, memory_order_acquire) != 2U) {
        __asm__ volatile ("pause");
    }
}

static uint32_t vfs_namespace_bucket(const char *path, size_t length) {
    uint32_t hash = 2166136261U;
    for (size_t index = 0U; index < length; ++index)
        hash = (hash ^ (uint8_t)path[index]) * 16777619U;
    return hash & (g_vfs_node_capacity - 1U);
}

/* 只索引末段名称：挂载点改变时，相对路径可能变化，但非根路径的末段保持不变。 */
static uint32_t vfs_fat_name_bucket(const char *path) {
    size_t length = vfs_string_length(path), first = 0U;
    for (size_t i = 0U; i < length; ++i)
        if (path[i] == '/') first = i + 1U;
    uint32_t hash = 2166136261U;
    for (size_t i = first; i < length; ++i) {
        uint8_t ch = (uint8_t)path[i];
        if (ch >= 'A' && ch <= 'Z') ch = (uint8_t)(ch - 'A' + 'a');
        hash = (hash ^ ch) * 16777619U;
    }
    return hash & (g_vfs_node_capacity - 1U);
}

/* 桶中保存槽位加一，按槽位排序以保留线性查找的首项语义。 */
static void vfs_fat_index_insert_locked(vnode_t *vnode, uint32_t slot) {
    vfs_backend_node_t *backend = vnode != 0 ? vnode->fs_private : 0;
    if (backend == 0 || !backend->fat32_node) return;
    uint32_t *link = &g_vfs_fat_buckets[vfs_fat_name_bucket(backend->path)];
    while (*link != 0U && *link - 1U < slot)
        link = &g_vfs_fat_next[*link - 1U];
    g_vfs_fat_next[slot] = *link;
    *link = slot + 1U;
}

static void vfs_fat_index_remove_locked(vnode_t *vnode) {
    vfs_backend_node_t *backend = vnode != 0 ? vnode->fs_private : 0;
    if (backend == 0 || !backend->fat32_node) return;
    uint32_t slot = backend->namespace_slot;
    uint32_t *link = &g_vfs_fat_buckets[vfs_fat_name_bucket(backend->path)];
    while (*link != 0U && *link != slot + 1U)
        link = &g_vfs_fat_next[*link - 1U];
    if (*link != 0U) *link = g_vfs_fat_next[slot];
    g_vfs_fat_next[slot] = 0U;
}

/* 索引借用注册表持有的引用。桶链按槽位排序，保留存在同名节点时原先的首槽命中语义。 */
static void vfs_namespace_insert_locked(vnode_t *vnode, uint32_t slot) {
    vfs_backend_node_t *backend = vnode != 0 ? vnode->fs_private : 0;
    if (backend == 0) return;
    uint32_t bucket = vfs_namespace_bucket(backend->path, vfs_string_length(backend->path));
    vnode_t **link = &g_vfs_buckets[bucket];
    while (*link != 0) {
        vfs_backend_node_t *current = (*link)->fs_private;
        if (current->namespace_slot >= slot) break;
        link = &current->namespace_next;
    }
    backend->namespace_slot = slot;
    backend->namespace_next = *link;
    *link = vnode;
    vfs_fat_index_insert_locked(vnode, slot);
}

static void vfs_namespace_remove_locked(vnode_t *vnode) {
    vfs_backend_node_t *backend = vnode != 0 ? vnode->fs_private : 0;
    if (backend == 0) return;
    uint32_t bucket = vfs_namespace_bucket(backend->path, vfs_string_length(backend->path));
    vnode_t **link = &g_vfs_buckets[bucket];
    while (*link != 0 && *link != vnode)
        link = &((vfs_backend_node_t *)(*link)->fs_private)->namespace_next;
    if (*link != 0) *link = backend->namespace_next;
    backend->namespace_next = 0;
    vfs_fat_index_remove_locked(vnode);
}

static vnode_t *vfs_find_node_locked(const char *path) {
    size_t path_length = vfs_string_length(path);
    if (g_vfs_node_capacity == 0U) return 0;
    for (vnode_t *vnode = g_vfs_buckets[vfs_namespace_bucket(path, path_length)];
         vnode != 0; vnode = ((vfs_backend_node_t *)vnode->fs_private)->namespace_next) {
        vfs_backend_node_t *backend = vnode != 0 ?
                                      (vfs_backend_node_t *)vnode->fs_private : 0;
        if (backend == 0) continue;
        size_t j = 0;
        while (j < path_length && path[j] == backend->path[j]) ++j;
        /* 在不重复扫描候选项的情况下拒绝前缀匹配。达到上限时保留原有的
         * 非 NUL 结尾路径比较语义。 */
        if (j == path_length && (path_length == VFS_PATH_LIMIT ||
                                 backend->path[path_length] == 0)) return vnode;
    }
    return 0;
}

/* 扫描注册表时，调用方可复用已规范化的请求路径。 */
static bool vfs_fat_relative_path_equal(const char *left_relative,
                                         size_t length,
                                         const vfs_fat32_mount_t *left_mount,
                                         const char *right) {
    if (right == 0 || length >= VFS_PATH_LIMIT) return false;
    size_t right_length = vfs_string_length(right);
    if (right_length >= VFS_PATH_LIMIT || right_length < length) return false;
    /* 挂载路由只移除绝对路径前缀。相对名称必须是候选路径的后缀；
     * 先排除名称不同的节点，避免为每个缓存节点遍历全部挂载点并复制路径。 */
    size_t suffix = right_length - length;
    /* 同目录节点通常共享长前缀，从末尾比较可优先排除文件名不同的节点。 */
    for (size_t remaining = length; remaining != 0U; ) {
        size_t index = --remaining;
        char a = left_relative[index], b = right[suffix + index];
        if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
        if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
        if (a != b) return false;
    }
    char right_relative[VFS_PATH_LIMIT];
    vfs_fat32_mount_t *right_mount = vfs_fat32_mount_for_path(right, right_relative);
    /* 相对名称只在同一文件系统内具有标识意义。该文件系统的别名挂载仍可共享规范页缓存。 */
    if (right_mount == 0 || right_mount->filesystem != left_mount->filesystem)
        return false;
    return vfs_string_length(right_relative) == length;
}

static vnode_t *vfs_find_fat_node_locked(const char *path) {
    char relative[VFS_PATH_LIMIT];
    vfs_fat32_mount_t *mount = vfs_fat32_mount_for_path(path, relative);
    if (mount == 0) return 0;
    size_t relative_length = vfs_string_length(relative);
    /* 根路径在不同挂载别名下的末段可能不同；其他路径只访问同名桶。 */
    bool indexed = relative_length != 0U && g_vfs_node_capacity != 0U;
    uint32_t slot = indexed ? g_vfs_fat_buckets[vfs_fat_name_bucket(relative)] :
        (g_vfs_node_capacity != 0U ? 1U : 0U);
    while (slot != 0U) {
        uint32_t index = slot - 1U;
        slot = indexed ? g_vfs_fat_next[index] :
            (index + 1U < g_vfs_node_capacity ? slot + 1U : 0U);
        vnode_t *vnode = g_vfs_nodes[index];
        vfs_backend_node_t *backend = vnode != 0 ?
                                      (vfs_backend_node_t *)vnode->fs_private : 0;
        /* 注册表淘汰可能晚于 unlink 提交。旧句柄仍持有已分离的 vnode，
         * 但新 open 不得复用其页缓存。 */
        if (backend != 0 && backend->fat32_node &&
            !__atomic_load_n(&backend->detached, __ATOMIC_ACQUIRE) &&
            vfs_fat_relative_path_equal(relative, relative_length,
                                         mount, backend->path)) return vnode;
    }
    return 0;
}

static bool vfs_path_range_equals(const char *left, size_t left_length,
                                  const char *right, size_t right_length) {
    if (left == 0 || right == 0 || left_length != right_length) return false;
    return memcmp(left, right, left_length) == 0;
}

/* 内存后备的 /tmp 不能因 profile 文件数量达到固定上限而失败。
 * 所有注册表读写（包括扩容）都持有 g_vfs_lock。 */
static uint32_t vfs_free_node_slot_locked(void) {
    for (uint32_t index = g_vfs_next_free_node; index < g_vfs_node_capacity; ++index) {
        if (g_vfs_nodes[index] == 0) {
            g_vfs_next_free_node = index;
            return index;
        }
    }
    g_vfs_next_free_node = g_vfs_node_capacity;
    if (g_vfs_node_capacity > UINT32_MAX / 2U) return UINT32_MAX;
    uint32_t old_capacity = g_vfs_node_capacity;
    uint32_t new_capacity = old_capacity * 2U;
    /* 注册表和两套索引共用一次分配，失败时完整保留旧状态。 */
    vnode_t **replacement = (vnode_t **)kzalloc(
        (size_t)new_capacity * (2U * sizeof(*replacement) + 2U * sizeof(uint32_t)), 0);
    if (replacement == 0) return UINT32_MAX;
    memcpy(replacement, g_vfs_nodes, (size_t)old_capacity * sizeof(*replacement));
    vnode_t **old_nodes = g_vfs_nodes;
    g_vfs_nodes = replacement;
    g_vfs_node_capacity = new_capacity;
    g_vfs_buckets = replacement + new_capacity;
    g_vfs_fat_buckets = (uint32_t *)(replacement + (size_t)new_capacity * 2U);
    g_vfs_fat_next = g_vfs_fat_buckets + new_capacity;
    /* 倒序插入可直接接到每个桶头，碰撞集中时也不重复遍历已有桶链。 */
    for (uint32_t index = old_capacity; index != 0U; --index)
        vfs_namespace_insert_locked(g_vfs_nodes[index - 1U], index - 1U);
    if (old_nodes != g_vfs_initial_nodes) kfree(old_nodes);
    return old_capacity;
}

/* 调用方持有命名空间锁。该提示是下界，不是预留：发布前重复查询空槽时必须返回同一槽位。
 * 所有插入/移除路径都在此更新提示。 */
static void vfs_set_node_slot_locked(uint32_t index, vnode_t *vnode) {
    vfs_namespace_remove_locked(g_vfs_nodes[index]);
    g_vfs_nodes[index] = vnode;
    vfs_namespace_insert_locked(vnode, index);
    if (vnode == 0) {
        if (index < g_vfs_next_free_node) g_vfs_next_free_node = index;
    } else if (index == g_vfs_next_free_node) {
        g_vfs_next_free_node = index + 1U;
    }
}

/* 临时状态使用内存后备，避免 FAT 不支持符号链接和同步簇分配。
 * 应用的 run 子目录属于应用自身；data 等其他子目录仍保存在磁盘上。 */
static bool vfs_volatile_path(const char *path) {
    if (path == 0) return false;
    static const char temporary[] = "/tmp";
    size_t index = 0U;
    while (index < sizeof(temporary) - 1U && path[index] == temporary[index]) ++index;
    if (index == sizeof(temporary) - 1U &&
        (path[index] == 0 || path[index] == '/')) return true;
    static const char applications[] = "/usr/apps/";
    for (index = 0U; index < sizeof(applications) - 1U; ++index)
        if (path[index] != applications[index]) return false;
    size_t first = index;
    for (; index < VFS_PATH_LIMIT; ++index) {
        char value = path[index];
        if (value == '/') break;
        if (!((value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z') ||
              (value >= '0' && value <= '9') || value == '-' || value == '_')) return false;
    }
    if (index == first || index >= VFS_PATH_LIMIT - 5U) return false;
    static const char runtime[] = "/run";
    for (size_t offset = 0U; offset < sizeof(runtime) - 1U; ++offset)
        if (path[index + offset] != runtime[offset]) return false;
    char boundary = path[index + sizeof(runtime) - 1U];
    return boundary == 0 || boundary == '/';
}

static bool vfs_mount_matches(const vfs_fat32_mount_t *mount,
                              const char *path, size_t path_length,
                              size_t prefix_length) {
    if (mount == 0 || !mount->mounted || mount->filesystem == 0 ||
        path == 0 || path_length == 0U || path[0] != '/') return false;
    if (prefix_length == 0U || path_length < prefix_length ||
        !vfs_path_range_equals(path, prefix_length,
                               mount->prefix, prefix_length)) return false;
    return prefix_length == 1U || path_length == prefix_length ||
           path[prefix_length] == '/';
}

vfs_fat32_mount_t *vfs_fat32_mount_for_path(const char *path,
                                             char relative[VFS_PATH_LIMIT]) {
    vfs_fat32_mount_t *selected = 0;
    size_t path_length;
    size_t selected_length = 0U;

    if (path == 0 || relative == 0 || path[0] != '/') return 0;
    path_length = vfs_string_length(path);
    if (path_length == 0U || path_length >= VFS_PATH_LIMIT) return 0;
    if (vfs_volatile_path(path)) {
        relative[0] = 0;
        return 0;
    }
    for (uint32_t index = 0U; index < VFS_FAT32_MOUNT_LIMIT; ++index) {
        vfs_fat32_mount_t *candidate = &g_fat32_mounts[index];
        if (!candidate->mounted || candidate->filesystem == 0) continue;
        size_t prefix_length = vfs_string_length(candidate->prefix);
        if (prefix_length <= selected_length ||
            !vfs_mount_matches(candidate, path, path_length, prefix_length)) continue;
        selected = candidate;
        selected_length = prefix_length;
    }
    if (selected == 0) return 0;

    if (path_length == selected_length) {
        relative[0] = 0;
    } else {
        size_t offset = selected_length == 1U ? 1U : selected_length + 1U;
        size_t length = path_length - offset;
        if (length >= VFS_PATH_LIMIT) return 0;
        for (size_t index = 0U; index < length; ++index) {
            relative[index] = path[offset + index];
        }
        relative[length] = 0;
    }
    return selected;
}

bool vfs_fat32_relative_path(const char *path, char relative[VFS_PATH_LIMIT]) {
    return vfs_fat32_mount_for_path(path, relative) != 0;
}

static bool vfs_fat32_stat_absolute(const char *path, os_file_info_t *info,
                                    char relative[VFS_PATH_LIMIT]) {
    vfs_fat32_mount_t *mount = vfs_fat32_mount_for_path(path, relative);
    return mount != 0 && fat32_stat_path(mount->filesystem, relative, info);
}

kstatus_t vfs_mount_fat32(const char *prefix, OS_FAT32 *filesystem) {
    size_t length;
    if (prefix == 0 || filesystem == 0 || !filesystem->Mounted || prefix[0] != '/') {
        return K_EINVAL;
    }
    length = vfs_string_length(prefix);
    if (length == 0U || length >= VFS_PATH_LIMIT ||
        (length > 1U && prefix[length - 1U] == '/')) {
        return K_EINVAL;
    }
    vfs_initialize();
    vfs_lock();
    bool root_mount = length == 1U;
    uint32_t slot = root_mount ? 0U : VFS_FAT32_MOUNT_LIMIT;
    uint32_t first_slot = root_mount ? 0U : 1U;
    for (uint32_t index = first_slot; index < VFS_FAT32_MOUNT_LIMIT; ++index) {
        if (g_fat32_mounts[index].mounted &&
            g_fat32_mounts[index].prefix[length] == 0 &&
            vfs_path_range_equals(g_fat32_mounts[index].prefix,
                                  length, prefix, length)) {
            vfs_unlock();
            return K_EBUSY;
        }
        if (slot == VFS_FAT32_MOUNT_LIMIT &&
            !g_fat32_mounts[index].mounted) slot = index;
    }
    if (slot == VFS_FAT32_MOUNT_LIMIT) {
        vfs_unlock();
        return K_ENOMEM;
    }
    for (size_t index = 0U; index <= length; ++index) {
        g_fat32_mounts[slot].prefix[index] = prefix[index];
    }
    g_fat32_mounts[slot].filesystem = filesystem;
    g_fat32_mounts[slot].mounted = true;
    vfs_unlock();
    return K_OK;
}

kstatus_t vfs_sync_all(void) {
    kstatus_t result = K_OK;
    vfs_initialize();
    vfs_lock();
    uint32_t limit = g_vfs_node_capacity;
    vfs_unlock();
    for (uint32_t i = 0U; i < limit; ++i) {
        vfs_lock();
        vnode_t *vnode = g_vfs_nodes[i];
        if (vnode != 0) object_get(vnode);
        vfs_unlock();
        if (vnode == 0) continue;
        kstatus_t status = vfs_page_cache_writeback(vnode);
        object_put(vnode);
        if (status != K_OK) result = status;
    }
    /* 存储 I/O 期间绝不持有命名空间锁。挂载对象在整个启动期间保持存活；卸载只会分离其路径前缀。 */
    for (uint32_t i = 0U; i < VFS_FAT32_MOUNT_LIMIT; ++i) {
        vfs_lock();
        OS_FAT32 *filesystem = g_fat32_mounts[i].mounted ?
                              g_fat32_mounts[i].filesystem : 0;
        vfs_unlock();
        if (filesystem != 0 && !fat32_sync(filesystem)) result = K_EIO;
    }
    return result;
}

kstatus_t vfs_unmount_fat32(void) {
    vnode_t *retired = 0;
    char relative[VFS_PATH_LIMIT];
    vfs_initialize();
    vfs_lock();
    if (!g_fat32_mounts[0].mounted) {
        vfs_unlock();
        return K_ENOENT;
    }
    /* 卸载与缓存摘除必须原子完成。同一路径随后挂载另一卷时，不能复用旧卷的大小和页缓存。
     * 活动文件或映射仍引用旧卷时拒绝卸载，避免它们按路径访问新卷。 */
    for (uint32_t index = 0U; index < g_vfs_node_capacity; ++index) {
        vnode_t *node = g_vfs_nodes[index];
        vfs_backend_node_t *backend = node != 0 ? node->fs_private : 0;
        if (backend == 0 || !backend->fat32_node ||
            vfs_fat32_mount_for_path(backend->path, relative) != &g_fat32_mounts[0]) continue;
        if (atomic_load_explicit(&node->object.refs.value, memory_order_acquire) != 1U) {
            vfs_unlock();
            return K_EBUSY;
        }
    }
    for (uint32_t index = 0U; index < g_vfs_node_capacity; ++index) {
        vnode_t *node = g_vfs_nodes[index];
        vfs_backend_node_t *backend = node != 0 ? node->fs_private : 0;
        if (backend == 0 || !backend->fat32_node ||
            vfs_fat32_mount_for_path(backend->path, relative) != &g_fat32_mounts[0]) continue;
        vfs_set_node_slot_locked(index, 0);
        /* 摘除后的索引链接临时串起待释放对象，避免锁内释放页缓存或执行磁盘 I/O。 */
        backend->namespace_next = retired;
        retired = node;
    }
    g_fat32_mounts[0].prefix[0] = 0;
    g_fat32_mounts[0].filesystem = 0;
    g_fat32_mounts[0].mounted = false;
    vfs_unlock();
    while (retired != 0) {
        vnode_t *node = retired;
        vfs_backend_node_t *backend = node->fs_private;
        retired = backend->namespace_next;
        backend->namespace_next = 0;
        object_put(node);
    }
    return K_OK;
}

static bool vfs_directory_exists_locked(const char *path, size_t path_length) {
    if (path == 0 || path_length == 0U || path_length > VFS_PATH_LIMIT) return false;
    if (path_length == 1U && path[0] == '/') return true;
    /* 即使根映像中没有持久目录项，/tmp 仍是易失兼容目录。 */
    if (path_length == 4U && vfs_path_range_equals(path, path_length,
                                                   "/tmp", 4U)) {
        return true;
    }
    char relative[VFS_PATH_LIMIT];
    char directory_path[VFS_PATH_LIMIT];
    os_file_info_t mounted_info;
    /* 调用方可能只提供父目录的前缀长度；后端查询必须使用独立终止的目录路径。
     * 达到路径上限的输入无法补终止符，只保留下面的有界内存节点查找。 */
    if (path_length < VFS_PATH_LIMIT) {
        memcpy(directory_path, path, path_length);
        directory_path[path_length] = 0;
    }
    if (path_length < VFS_PATH_LIMIT &&
        vfs_fat32_stat_absolute(directory_path, &mounted_info, relative)) {
        return mounted_info.type == OS_FILE_TYPE_DIRECTORY;
    }
    for (uint32_t node_index = 0U; node_index < g_vfs_node_capacity; ++node_index) {
        vnode_t *vnode = g_vfs_nodes[node_index];
        vfs_backend_node_t *backend = vnode != 0 ?
                                      (vfs_backend_node_t *)vnode->fs_private : 0;
        if (backend == 0) continue;
        size_t matched = 0U;
        while (matched < path_length && matched < VFS_PATH_LIMIT &&
               backend->path[matched] != 0 &&
               backend->path[matched] == path[matched]) ++matched;
        if (matched != path_length) continue;
        /* 精确匹配只比较有界路径范围内的字节。 */
        if (path_length == VFS_PATH_LIMIT || backend->path[path_length] == 0)
            return backend->directory;
        if (backend->path[path_length] == '/') return true;
    }
    return false;
}

static bool vfs_parent_directory_exists_locked(const char *path, size_t path_length) {
    size_t slash = path_length;
    if (path == 0 || path_length <= 1U || path[path_length - 1U] == '/') return false;
    while (slash > 0U && path[slash - 1U] != '/') --slash;
    if (slash <= 1U) return true;
    return vfs_directory_exists_locked(path, slash - 1U);
}

static void vfs_set_leaf_name(const char *path, char name[OS_FILE_NAME_MAX]) {
    size_t length = 0U;
    size_t start = 0U;
    if (path != 0) {
        while (length < VFS_PATH_LIMIT && path[length] != 0) {
            if (path[length] == '/') start = length + 1U;
            ++length;
        }
    }
    if (length == 1U && path[0] == '/') {
        name[0] = '/';
        name[1] = 0;
        return;
    }
    size_t count = length - start;
    if (count >= OS_FILE_NAME_MAX) count = OS_FILE_NAME_MAX - 1U;
    for (size_t index = 0U; index < count; ++index) name[index] = path[start + index];
    name[count] = 0;
}

static void vfs_fill_info(const char *path, const vnode_t *vnode,
                          const vfs_backend_node_t *backend,
                          os_file_info_t *out) {
    *out = (os_file_info_t){0};
    out->type = backend != 0 && (backend->mode & 0170000U) == 0120000U ?
        OS_FILE_TYPE_SYMLINK :
        (backend != 0 && backend->directory ? OS_FILE_TYPE_DIRECTORY :
                                             OS_FILE_TYPE_REGULAR);
    out->mode = backend != 0 ? backend->mode : 0040555U;
    out->size = vnode != 0 && backend != 0 && !backend->directory ? vnode->size : 0U;
    vfs_set_leaf_name(path, out->name);
}

kstatus_t vfs_file_stat(file_t *file, os_file_info_t *out) {
    vnode_t *vnode;
    vfs_backend_node_t *backend;
    if (file == 0 || out == 0 || file->object.type != KOBJECT_TYPE_FILE ||
        file->vnode == 0) return K_EBADF;
    vnode = file->vnode;
    backend = (vfs_backend_node_t *)vnode->fs_private;
    if (backend == 0) return K_EIO;
    vfs_initialize();
    vfs_lock();
    vfs_fill_info(backend->path, vnode, backend, out);
    vfs_unlock();
    return K_OK;
}

static bool vfs_backend_memory_space(const vfs_backend_node_t *backend) {
    return backend != 0 && !backend->fat32_node && backend->read == 0 &&
           backend->write == 0 && backend->truncate == 0 && backend->fsync == 0;
}

static kstatus_t vfs_memory_space(vfs_space_info_t *out) {
    uint64_t total, available;
    kstatus_t status = page_memory_get_stats(&total, &available);
    if (status != K_OK) return status;
    if (available > total || total > UINT64_MAX / PAGE_SIZE)
        return K_EOVERFLOW;
    *out = (vfs_space_info_t){
        .block_size = PAGE_SIZE, .blocks = total, .blocks_free = available,
        .type = 0x01021994U, .filesystem_id = 1U,
        .name_max = VFS_PATH_LIMIT - 1U
    };
    return K_OK;
}

static kstatus_t vfs_disk_space(OS_FAT32 *filesystem, vfs_space_info_t *out) {
    uint64_t blocks, available, block_size;
    if (!fat32_space(filesystem, &blocks, &available, &block_size)) return K_EIO;
    *out = (vfs_space_info_t){
        .block_size = block_size, .blocks = blocks, .blocks_free = available,
        .type = 0x4D44U, .filesystem_id = (uintptr_t)filesystem,
        .name_max = VFS_PATH_LIMIT - 1U,
    };
    return K_OK;
}

kstatus_t vfs_file_space(file_t *file, vfs_space_info_t *out) {
    if (file == 0 || file->object.type != KOBJECT_TYPE_FILE || file->vnode == 0)
        return K_EBADF;
    if (out == 0) return K_EINVAL;
    vfs_initialize();
    vfs_lock();
    vfs_backend_node_t *backend = file->vnode->fs_private;
    bool memory = vfs_backend_memory_space(backend);
    OS_FAT32 *disk = backend != 0 && backend->fat32_node ? backend->fat_filesystem : 0;
    vfs_unlock();
    /* 在命名空间锁外查询。已打开但 unlink 的 RAM 文件仍持有后备文件系统；
     * fstatfs 无需查找路径名。 */
    return disk != 0 ? vfs_disk_space(disk, out) :
           memory ? vfs_memory_space(out) : K_ENOSYS;
}

kstatus_t vfs_directory_path_kernel(file_t *file, char *path, size_t capacity) {
    if (file == 0 || file->object.type != KOBJECT_TYPE_FILE || file->vnode == 0)
        return K_EBADF;
    if (path == 0 || capacity == 0U) return K_EINVAL;
    vfs_backend_node_t *backend = file->vnode->fs_private;
    if (backend == 0) return K_EIO;
    vfs_initialize();
    vfs_lock();
    kstatus_t status = K_OK;
    vnode_t *current = backend->fat32_node ? vfs_find_fat_node_locked(backend->path) :
                                           vfs_find_node_locked(backend->path);
    if (!backend->directory) status = K_ENOTDIR;
    else if (backend->detached || current != file->vnode) status = K_ENOENT;
    else {
        size_t length = vfs_string_length(backend->path);
        if (length >= capacity) status = K_ENAMETOOLONG;
        else {
            for (size_t index = 0U; index <= length; ++index)
                path[index] = backend->path[index];
        }
    }
    vfs_unlock();
    return status;
}

kstatus_t vfs_vnode_path_kernel(vnode_t *vnode, char *path, size_t capacity,
                                bool *deleted) {
    if (vnode == 0 || vnode->object.type != KOBJECT_TYPE_VNODE) return K_EBADF;
    if (path == 0 || capacity == 0U || deleted == 0) return K_EINVAL;
    vfs_initialize();
    vfs_lock();
    vfs_backend_node_t *backend = vnode->fs_private;
    kstatus_t status = K_EIO;
    if (backend != 0 && backend->path[0] == '/') {
        size_t length = vfs_string_length(backend->path);
        if (length >= capacity) status = K_ENAMETOOLONG;
        else {
            memcpy(path, backend->path, length + 1U);
            *deleted = backend->detached;
            status = K_OK;
        }
    }
    vfs_unlock();
    return status;
}

static bool vfs_path_is_subtree(const char *path, size_t path_length,
                                const char *root, size_t root_length) {
    return (path_length == root_length &&
            vfs_path_range_equals(path, path_length, root, root_length)) ||
           (path_length > root_length && path[root_length] == '/' &&
            vfs_path_range_equals(path, root_length, root, root_length));
}

/* 注册表路径是定长数组；root 已完成验证。 */
static bool vfs_path_matches_subtree_prefix(const char *path, const char *root,
                                            size_t root_length) {
    return root_length < VFS_PATH_LIMIT &&
           (path[root_length] == 0 || path[root_length] == '/') &&
           vfs_path_range_equals(path, root_length, root, root_length);
}

static kstatus_t vfs_rekey_cached_subtree_locked(const char *old_path,
                                            size_t old_length,
                                            const char *new_path,
                                            size_t new_length, bool publish) {
    char old_relative[VFS_PATH_LIMIT];
    vfs_fat32_mount_t *old_mount = vfs_fat32_mount_for_path(old_path, old_relative);
    size_t relative_length = old_mount != 0 ? vfs_string_length(old_relative) : 0U;
    /* 发布任何路径前先验证整个集合。命名空间锁保证两轮遍历一致，包括挂载别名。 */
    for (unsigned pass = 0U; pass < (publish ? 2U : 1U); ++pass) {
    for (uint32_t index = 0U; index < g_vfs_node_capacity; ++index) {
        vnode_t *candidate = g_vfs_nodes[index];
        vfs_backend_node_t *backend = candidate != 0 ?
            (vfs_backend_node_t *)candidate->fs_private : 0;
        size_t candidate_length;
        size_t suffix_length;
        char replacement[VFS_PATH_LIMIT];
        char candidate_relative[VFS_PATH_LIMIT];
        const char *suffix;
        if (old_mount != 0) {
            if (backend == 0 || !backend->fat32_node ||
                __atomic_load_n(&backend->detached, __ATOMIC_ACQUIRE)) continue;
            vfs_fat32_mount_t *candidate_mount =
                vfs_fat32_mount_for_path(backend->path, candidate_relative);
            if (candidate_mount == 0 ||
                candidate_mount->filesystem != old_mount->filesystem) continue;
            size_t matched = 0U;
            while (matched < relative_length) {
                char a = old_relative[matched];
                char b = candidate_relative[matched];
                if (b == 0) break;
                if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
                if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
                if (a != b) break;
                ++matched;
            }
            if (matched != relative_length ||
                (candidate_relative[matched] != 0 &&
                 candidate_relative[matched] != '/')) continue;
            suffix = candidate_relative + matched;
            suffix_length = vfs_string_length(suffix);
        } else {
            if (backend == 0 ||
                !vfs_path_matches_subtree_prefix(backend->path, old_path, old_length)) continue;
            candidate_length = vfs_string_length(backend->path);
            suffix_length = candidate_length - old_length;
            suffix = backend->path + old_length;
        }
        if (new_length >= VFS_PATH_LIMIT ||
            suffix_length >= VFS_PATH_LIMIT - new_length) return K_ENAMETOOLONG;
        if (pass == 0U) continue;
        __builtin_memcpy(replacement, new_path, new_length);
        __builtin_memcpy(replacement + new_length, suffix, suffix_length);
        replacement[new_length + suffix_length] = '\0';
        vfs_namespace_remove_locked(candidate);
        __builtin_memcpy(backend->path, replacement,
                         new_length + suffix_length + 1U);
        vfs_namespace_insert_locked(candidate, index);
    }
    }
    return K_OK;
}

/* 替换磁盘文件前保留旧 inode 内容并关闭其原生句柄；缓存不是额外的目录项。 */
static kstatus_t vfs_rename_fat32_cached(const char *old_path, size_t old_length,
                                        const char *new_path, size_t new_length,
                                        vfs_fat32_mount_t *mount,
                                        const char *old_relative, const char *new_relative) {
    vnode_t *retired = 0;
    vfs_backend_node_t *backend = 0;
    vfs_initialize();
    vfs_lock();
    kstatus_t status = vfs_rekey_cached_subtree_locked(
        old_path, old_length, new_path, new_length, false);
    if (status == K_OK) {
        vnode_t *source = vfs_find_fat_node_locked(old_path);
        vnode_t *target = vfs_find_fat_node_locked(new_path);
        /* 大小写或挂载别名可能指向同一个 inode，不能退役源文件自身。 */
        if (target != 0 && target != source) {
            retired = target;
            object_get(retired);
            backend = retired->fs_private;
        }
    }
    vfs_unlock();
    if (status != K_OK) return status;
    if (backend != 0) {
        status = vfs_fat32_backend_prepare_unlink(backend);
        if (status != K_OK) goto release;
        vfs_backend_lock(backend);
        if (!backend->directory &&
            (!backend->unlink_prepared || backend->detached ||
             backend->size != backend->snapshot_size ||
             backend->content_revision != backend->snapshot_revision)) {
            status = K_EBUSY;
            goto abort;
        }
        if (backend->fat_file != 0) {
            if (!fat32_close(backend->fat_file)) {
                status = K_EIO;
                goto abort;
            }
            backend->fat_file = 0;
        }
    }
    if (!fat32_rename_path(mount->filesystem, old_relative, new_relative)) {
        status = K_EIO;
        if (backend != 0) goto abort;
        goto release;
    }
    if (backend != 0) {
        vfs_fat32_backend_commit_unlink_locked(backend);
        vfs_backend_unlock(backend);
    }
    vfs_lock();
    bool removed = backend != 0 && backend->namespace_slot < g_vfs_node_capacity &&
        g_vfs_nodes[backend->namespace_slot] == retired;
    if (removed) vfs_set_node_slot_locked(backend->namespace_slot, 0);
    status = vfs_rekey_cached_subtree_locked(old_path, old_length, new_path, new_length, true);
    vfs_unlock();
    if (removed) object_put(retired);
    goto release;
abort:
    vfs_backend_unlock(backend);
    vfs_fat32_backend_abort_unlink(backend);
release:
    if (retired != 0) object_put(retired);
    return status;
}

kstatus_t vfs_rename_kernel(const char *old_path, const char *new_path) {
    char old_relative[VFS_PATH_LIMIT];
    char new_relative[VFS_PATH_LIMIT];
    size_t old_length;
    size_t new_length;
    bool old_mounted;
    bool new_mounted;
    vfs_fat32_mount_t *old_mount;
    vfs_fat32_mount_t *new_mount;

    if (old_path == 0 || new_path == 0 || old_path[0] != '/' ||
        new_path[0] != '/') return K_EINVAL;
    old_length = vfs_string_length(old_path);
    new_length = vfs_string_length(new_path);
    if (old_length <= 1U || new_length <= 1U ||
        old_length >= VFS_PATH_LIMIT || new_length >= VFS_PATH_LIMIT ||
        old_path[old_length - 1U] == '/' || new_path[new_length - 1U] == '/') {
        return K_EINVAL;
    }
    if (vfs_path_range_equals(old_path, old_length, new_path, new_length)) {
        os_file_info_t info;
        return vfs_stat_kernel(old_path, &info);
    }

    old_mount = vfs_fat32_mount_for_path(old_path, old_relative);
    new_mount = vfs_fat32_mount_for_path(new_path, new_relative);
    old_mounted = old_mount != 0;
    new_mounted = new_mount != 0;
    if (old_mounted || new_mounted) {
        if (!old_mounted || !new_mounted || old_mount != new_mount ||
            old_relative[0] == 0 ||
            new_relative[0] == 0) return K_EXDEV;
        return vfs_rename_fat32_cached(old_path, old_length, new_path, new_length,
                                      old_mount, old_relative, new_relative);
    }

    vfs_initialize();
    vfs_lock();
    vnode_t *source = vfs_find_node_locked(old_path);
    vfs_backend_node_t *source_backend = source != 0 ?
        (vfs_backend_node_t *)source->fs_private : 0;
    if (source_backend == 0) {
        vfs_unlock();
        return K_ENOENT;
    }
    if (!source_backend->mutable_node) {
        vfs_unlock();
        return K_EACCES;
    }
    if (!vfs_parent_directory_exists_locked(new_path, new_length)) {
        vfs_unlock();
        return K_ENOTDIR;
    }
    vnode_t *replaced = vfs_find_node_locked(new_path);
    vfs_backend_node_t *replaced_backend = replaced != 0 ?
        (vfs_backend_node_t *)replaced->fs_private : 0;
    if (replaced_backend != 0) {
        if (!replaced_backend->mutable_node) {
            vfs_unlock();
            return K_EACCES;
        }
        if (source_backend->directory != replaced_backend->directory) {
            vfs_unlock();
            return replaced_backend->directory ? K_EISDIR : K_ENOTDIR;
        }
        if (replaced_backend->directory) {
            for (uint32_t index = 0U; index < g_vfs_node_capacity; ++index) {
                vnode_t *candidate = g_vfs_nodes[index];
                vfs_backend_node_t *backend = candidate != 0 ?
                    (vfs_backend_node_t *)candidate->fs_private : 0;
                if (backend != 0 && candidate != replaced &&
                    vfs_path_matches_subtree_prefix(backend->path, new_path, new_length)) {
                    vfs_unlock();
                    return K_ENOTEMPTY;
                }
            }
        }
    } else if (vfs_directory_exists_locked(new_path, new_length)) {
        vfs_unlock();
        return K_EEXIST;
    }
    if (source_backend->directory &&
        vfs_path_is_subtree(new_path, new_length, old_path, old_length)) {
        vfs_unlock();
        return K_EINVAL;
    }

    /* 更改路径前验证所有后代，避免重命名失败后内存目录树只移动了一部分。 */
    for (uint32_t index = 0U; index < g_vfs_node_capacity; ++index) {
        vnode_t *candidate = g_vfs_nodes[index];
        vfs_backend_node_t *backend = candidate != 0 ?
            (vfs_backend_node_t *)candidate->fs_private : 0;
        size_t candidate_length;
        size_t suffix_length;
        if (backend == 0 ||
            !vfs_path_matches_subtree_prefix(backend->path, old_path, old_length)) continue;
        candidate_length = vfs_string_length(backend->path);
        suffix_length = candidate_length - old_length;
        if (new_length > VFS_PATH_LIMIT - 1U - suffix_length) {
            vfs_unlock();
            return K_ENAMETOOLONG;
        }
        if (!backend->mutable_node) {
            vfs_unlock();
            return K_EACCES;
        }
    }
    /* 在同一命名空间锁下发布替换项并重新设置源路径键。已有 open 仍保留 vnode 引用和旧内容。 */
    if (replaced != 0) {
        for (uint32_t index = 0U; index < g_vfs_node_capacity; ++index) {
            if (g_vfs_nodes[index] == replaced) {
                vfs_set_node_slot_locked(index, 0);
                break;
            }
        }
    }
    for (uint32_t index = 0U; index < g_vfs_node_capacity; ++index) {
        vnode_t *candidate = g_vfs_nodes[index];
        vfs_backend_node_t *backend = candidate != 0 ?
            (vfs_backend_node_t *)candidate->fs_private : 0;
        size_t candidate_length;
        size_t suffix_length;
        char replacement[VFS_PATH_LIMIT];
        if (backend == 0 ||
            !vfs_path_matches_subtree_prefix(backend->path, old_path, old_length)) continue;
        candidate_length = vfs_string_length(backend->path);
        suffix_length = candidate_length - old_length;
        __builtin_memcpy(replacement, new_path, new_length);
        __builtin_memcpy(replacement + new_length, backend->path + old_length,
                         suffix_length);
        replacement[new_length + suffix_length] = '\0';
        vfs_namespace_remove_locked(candidate);
        __builtin_memcpy(backend->path, replacement,
                         new_length + suffix_length + 1U);
        vfs_namespace_insert_locked(candidate, index);
    }
    vfs_unlock();
    if (replaced != 0) object_put(replaced);
    return K_OK;
}

static size_t vfs_file_name_length(const char *name) {
    size_t length = 0U;
    while (length < OS_FILE_NAME_MAX && name[length] != 0) ++length;
    return length;
}

static bool vfs_append_entry(os_file_info_t entries[VFS_DIRECTORY_ENTRY_LIMIT],
                             uint32_t *entry_count,
                             const os_file_info_t *entry) {
    if (entries == 0 || entry_count == 0 || entry == 0 || entry->name[0] == 0) {
        return false;
    }
    size_t name_length = vfs_file_name_length(entry->name);
    if (name_length >= OS_FILE_NAME_MAX) return false;
    for (uint32_t index = 0U; index < *entry_count; ++index) {
        /* 输入长度受定长名称数组约束。直接检查候选名称在该位置的终止符，
         * 无需重新扫描长度。 */
        if (entries[index].name[name_length] != 0 ||
            !vfs_path_range_equals(entries[index].name, name_length,
                                   entry->name, name_length)) {
            continue;
        }
        if (entry->type == OS_FILE_TYPE_DIRECTORY) entries[index] = *entry;
        return true;
    }
    if (*entry_count >= VFS_DIRECTORY_ENTRY_LIMIT) return false;
    entries[*entry_count] = *entry;
    ++*entry_count;
    return true;
}

typedef struct vfs_directory_collection {
    os_file_info_t *entries;
    uint32_t *count;
    bool failed;
} vfs_directory_collection_t;

static BOOLEAN vfs_collect_fat_directory_entry(VOID *context, const os_file_info_t *entry) {
    vfs_directory_collection_t *collection = context;
    if (!vfs_append_entry(collection->entries, collection->count, entry)) {
        collection->failed = true;
        return 0;
    }
    return 1;
}

/* 合成目录查找和目录列表共用同一条直接子项判断规则。 */
static const char *vfs_mount_child_name(const vfs_fat32_mount_t *mount,
                                        const char *path, size_t path_length,
                                        size_t *name_length) {
    if (!mount->mounted) return 0;
    size_t prefix_length = vfs_string_length(mount->prefix);
    if (prefix_length <= 1U) return 0;
    size_t leaf_start = prefix_length;
    while (leaf_start != 0U && mount->prefix[leaf_start - 1U] != '/') --leaf_start;
    size_t parent_length = leaf_start <= 1U ? 1U : leaf_start - 1U;
    if (parent_length != path_length ||
        !vfs_path_range_equals(path, path_length, mount->prefix, parent_length)) return 0;
    *name_length = prefix_length - leaf_start;
    return mount->prefix + leaf_start;
}

static bool vfs_mount_parent_exists(const char *path, size_t path_length) {
    for (uint32_t index = 0U; index < VFS_FAT32_MOUNT_LIMIT; ++index) {
        vfs_fat32_mount_t *mount = &g_fat32_mounts[index];
        size_t name_length;
        if (vfs_mount_child_name(mount, path, path_length, &name_length) != 0) return true;
    }
    return false;
}

static bool vfs_append_mount_children(const char *path, size_t path_length,
                                      os_file_info_t entries[VFS_DIRECTORY_ENTRY_LIMIT],
                                      uint32_t *entry_count,
                                      bool *directory_exists) {
    for (uint32_t index = 0U; index < VFS_FAT32_MOUNT_LIMIT; ++index) {
        vfs_fat32_mount_t *mount = &g_fat32_mounts[index];
        size_t name_length;
        const char *name = vfs_mount_child_name(mount, path, path_length, &name_length);
        if (name == 0) continue;
        if (directory_exists != 0) *directory_exists = true;
        if (name_length >= OS_FILE_NAME_MAX) return false;
        os_file_info_t entry = {0};
        entry.type = OS_FILE_TYPE_DIRECTORY;
        entry.mode = 0040755U;
        memcpy(entry.name, name, name_length);
        if (!vfs_append_entry(entries, entry_count, &entry)) return false;
    }
    return true;
}

/* 当前 VFS 按绝对路径保存文件。在此根据路径生成目录项，使所有 VFS 使用方看到相同命名空间。 */
kstatus_t vfs_enumerate_kernel_batch(const char *path, uint32_t index,
                                            os_file_info_t *out, uint32_t capacity,
                                            uint32_t *count) {
    os_file_info_t entries[VFS_DIRECTORY_ENTRY_LIMIT];
    size_t directory_length;
    uint32_t entry_count = 0U;
    bool directory_exists = false;
    bool exact_regular_file = false;
    char relative[VFS_PATH_LIMIT];
    vfs_fat32_mount_t *mount;
    os_file_info_t mounted_info;

    if (count == 0) return K_EINVAL;
    *count = 0U;
    if (path == 0 || out == 0 || capacity == 0U || path[0] != '/') return K_EINVAL;
    directory_length = vfs_string_length(path);
    if (directory_length == 0U || directory_length >= VFS_PATH_LIMIT) return K_EINVAL;
    while (directory_length > 1U && path[directory_length - 1U] == '/') --directory_length;
    /* 仅读取 [0, entry_count)；追加操作会完整发布目录项。 */

    mount = vfs_fat32_mount_for_path(path, relative);
    bool mounted_directory = false;
    if (mount != 0) {
        if (relative[0] == 0) {
            mounted_info = (os_file_info_t){
                .type = OS_FILE_TYPE_DIRECTORY, .mode = 0040755U
            };
            mounted_directory = true;
        } else {
            mounted_directory =
                fat32_stat_path(mount->filesystem, relative,
                                        &mounted_info);
        }
    }
    if (mounted_directory) {
        if (mounted_info.type != OS_FILE_TYPE_DIRECTORY) return K_EINVAL;
        directory_exists = true;
        vfs_directory_collection_t collection = {entries, &entry_count, false};
        /* 由收集器判断容量耗尽，不能让底层在第 256 项静默停止，否则后续批次会误报 EOF。
         * 重名项不占新槽，因此必须继续访问到真正的末尾或首次追加失败。 */
        fat32_visit_directory(mount->filesystem, relative,
                                     UINT32_MAX,
                                     vfs_collect_fat_directory_entry, &collection);
        if (collection.failed) return K_ENOMEM;
    }
    if (!vfs_append_mount_children(path, directory_length, entries, &entry_count,
                                   &directory_exists)) {
        return K_ENOMEM;
    }
    if (directory_length == 1U) directory_exists = true;

    vfs_initialize();
    vfs_lock();
    for (uint32_t node_index = 0U; node_index < g_vfs_node_capacity; ++node_index) {
        vnode_t *vnode = g_vfs_nodes[node_index];
        vfs_backend_node_t *backend = vnode != 0 ?
                                      (vfs_backend_node_t *)vnode->fs_private : 0;
        size_t remainder_index;
        const char *remainder;
        /* FAT 缓存只保存打开 inode 的状态；磁盘枚举已经提供真实名称。
         * 重复合并会产生大小写别名，并让已删除或已替换的 inode 重新出现在列表中。 */
        if (backend == 0 || backend->fat32_node) continue;
        size_t matched = 0U;
        while (matched < directory_length && backend->path[matched] != 0 &&
               backend->path[matched] == path[matched]) ++matched;
        if (matched != directory_length) continue;
        if (backend->path[directory_length] == 0) {
            if (backend->directory) directory_exists = true;
            else exact_regular_file = true;
            continue;
        }
        if (directory_length != 1U && backend->path[directory_length] != '/') continue;
        remainder_index = directory_length == 1U ? 1U : directory_length + 1U;
        if (remainder_index >= VFS_PATH_LIMIT || backend->path[remainder_index] == 0) continue;
        directory_exists = true;
        remainder = backend->path + remainder_index;
        size_t name_length = 0U;
        while (name_length < OS_FILE_NAME_MAX &&
               remainder_index + name_length < VFS_PATH_LIMIT &&
               remainder[name_length] != '\0' && remainder[name_length] != '/') ++name_length;
        if (name_length == 0U || name_length >= OS_FILE_NAME_MAX ||
            remainder_index + name_length >= VFS_PATH_LIMIT) {
            vfs_unlock();
            return K_EINVAL;
        }
        os_file_info_t entry = {0};
        for (size_t character = 0U; character < name_length; ++character) {
            entry.name[character] = remainder[character];
        }
        entry.name[name_length] = 0;
        bool child_directory = backend->directory || remainder[name_length] == '/';
        entry.type = child_directory ? OS_FILE_TYPE_DIRECTORY : OS_FILE_TYPE_REGULAR;
        entry.mode = child_directory ?
            (backend->directory ? vnode->mode : 0040555U) : vnode->mode;
        entry.size = child_directory ? 0U : vnode->size;
        if (!vfs_append_entry(entries, &entry_count, &entry)) {
            vfs_unlock();
            return K_ENOMEM;
        }
    }
    vfs_unlock();
    if (!directory_exists) return exact_regular_file ? K_EINVAL : K_ENOENT;
    if (index >= entry_count) return K_ENOENT;
    uint32_t copied = entry_count - index;
    if (copied > capacity) copied = capacity;
    memcpy(out, entries + index, (size_t)copied * sizeof(*out));
    *count = copied;
    return K_OK;
}

kstatus_t vfs_enumerate_kernel(const char *path, uint32_t index,
                               os_file_info_t *out) {
    uint32_t count;
    return vfs_enumerate_kernel_batch(path, index, out, 1U, &count);
}

kstatus_t vfs_enumerate_file(file_t *file, uint32_t index,
                             os_file_info_t *out) {
    vfs_backend_node_t *backend;
    if (file == 0 || file->object.type != KOBJECT_TYPE_FILE ||
        file->vnode == 0 || out == 0) return K_EBADF;
    backend = (vfs_backend_node_t *)file->vnode->fs_private;
    if (backend == 0 || !backend->directory) return K_ENOTDIR;
    return vfs_enumerate_kernel(backend->path, index, out);
}

kstatus_t vfs_enumerate_file_batch(file_t *file, uint32_t index,
                                   os_file_info_t *out, uint32_t capacity,
                                   uint32_t *count) {
    if (count == 0) return K_EINVAL;
    *count = 0U;
    if (file == 0 || file->object.type != KOBJECT_TYPE_FILE ||
        file->vnode == 0 || out == 0) return K_EBADF;
    vfs_backend_node_t *backend = file->vnode->fs_private;
    if (backend == 0 || !backend->directory) return K_ENOTDIR;
    return vfs_enumerate_kernel_batch(backend->path, index, out, capacity, count);
}

static vnode_t *vfs_create_vnode_object(vfs_backend_node_t *backend) {
    vnode_t *vnode;
    uint64_t inode_id;
    if (backend == 0) return 0;
    vnode = (vnode_t *)kzalloc(sizeof(*vnode), 0);
    if (vnode == 0) return 0;
    inode_id = atomic_fetch_add_explicit(&g_vfs_next_inode_id, 1U,
                                         memory_order_relaxed) + 1U;
    if (inode_id == 0U) {
        kfree(vnode);
        return 0;
    }
    refcount_init(&vnode->object.refs, 1U);
    vnode->object.type = KOBJECT_TYPE_VNODE;
    vnode->object.flags = 0;
    vnode->object.ops = &g_vnode_object_ops;
    vnode->inode_id = inode_id;
    vnode->mode = backend->mode;
    vnode->flags = 0;
    vnode->size = backend->size;
    vnode->ops = 0;
    vnode->fs_private = backend;
    io_mutex_init(&backend->write_mutex, true);
    io_mutex_init(&vnode->lock, true);
    vnode->page_cache = 0;
    wait_queue_init(&vnode->record_lock_waitq);
    atomic_init(&vnode->record_lock_sequence, 0U);
    return vnode;
}

/* 内存压力下释放一个空闲原生句柄，同时保留 VFS 页缓存。仅由注册表持有的 inode
 * 没有活动文件或映射引用。 */
static bool vfs_release_idle_fat_handle(OS_FAT32 *filesystem) {
    for (uint32_t index = 0U;; ++index) {
        vnode_t *candidate = 0;
        vfs_lock();
        if (index >= g_vfs_node_capacity) {
            vfs_unlock();
            return false;
        }
        candidate = g_vfs_nodes[index];
        if (candidate != 0 &&
            atomic_load_explicit(&candidate->object.refs.value, memory_order_acquire) == 1U) {
            object_get(candidate);
        } else {
            candidate = 0;
        }
        vfs_unlock();
        if (candidate == 0) continue;
        vfs_backend_node_t *backend = (vfs_backend_node_t *)candidate->fs_private;
        bool released = false;
        if (backend != 0) {
            vfs_backend_lock(backend);
            /* 固定引用后重新检查：并发 open 此时可能已经持有它。 */
            if (atomic_load_explicit(&candidate->object.refs.value, memory_order_acquire) == 2U &&
                backend->fat_file != 0 && !backend->detached &&
                backend->fat_file->FileSystem == filesystem &&
                fat32_close(backend->fat_file)) {
                backend->fat_file = 0;
                released = true;
            }
            vfs_backend_unlock(backend);
        }
        object_put(candidate);
        if (released) return true;
    }
}

static kstatus_t vfs_create_fat32_vnode(const char *path,
                                        bool keep_fat_file,
                                        const os_file_info_t *known_info,
                                        vnode_t **out) {
    os_file_info_t info;
    char relative[VFS_PATH_LIMIT];
    vfs_fat32_mount_t *mount;
    vfs_backend_node_t *backend;
    vnode_t *vnode;
    if (out == 0 || (mount = vfs_fat32_mount_for_path(path, relative)) == 0) {
        return K_ENOENT;
    }
    if (relative[0] == 0) {
        info = (os_file_info_t){
            .type = OS_FILE_TYPE_DIRECTORY, .mode = 0040755U
        };
    } else if (known_info != 0) {
        /* open 路径已查询此条目，避免再次遍历目录。创建新条目的调用方不得传入快照。 */
        info = *known_info;
    } else if (!fat32_stat_path(mount->filesystem, relative, &info) ||
               info.type == OS_FILE_TYPE_UNKNOWN) {
        if (!vfs_mount_parent_exists(path, vfs_string_length(path))) {
            return K_ENOENT;
        }
        info = (os_file_info_t){
            .type = OS_FILE_TYPE_DIRECTORY, .mode = 0040755U
        };
    }
    /* 缓存命中快速路径：无需临时分配或原生 FAT 句柄。下方分配后仍需重新检查，
     * 以解决并发未命中。 */
    vfs_initialize();
    vfs_lock();
    vnode_t *cached = vfs_find_fat_node_locked(path);
    if (cached != 0) {
        object_get(cached);
        vfs_unlock();
        *out = cached;
        return K_OK;
    }
    vfs_unlock();
    /* 缓存命中快速路径结束。 */
    backend = (vfs_backend_node_t *)kzalloc(sizeof(*backend), 0);
    if (backend == 0) return K_ENOMEM;
    size_t path_length = vfs_string_length(path);
    if (path_length >= VFS_PATH_LIMIT) {
        kfree(backend);
        return K_EINVAL;
    }
    for (size_t index = 0U; index <= path_length; ++index) backend->path[index] = path[index];
    backend->size = info.size;
    backend->mode = info.mode;
    backend->directory = info.type == OS_FILE_TYPE_DIRECTORY;
    backend->mutable_node = true;
    backend->fat32_node = true;
    backend->fat_filesystem = mount->filesystem;
    backend->fat_file = 0;
    backend->read = backend->directory ? 0 : vfs_fat32_backend_read;
    backend->write = backend->directory ? 0 : vfs_fat32_backend_write;
    backend->fsync = backend->directory ? 0 : vfs_fat32_backend_fsync;
    backend->truncate = backend->directory ? 0 : vfs_fat32_backend_truncate;
    backend->context = backend;
    io_mutex_init(&backend->io_lock, true);
    vnode = vfs_create_vnode_object(backend);
    if (vnode == 0) {
        kfree(backend);
        return K_ENOMEM;
    }
    /* 读写 inode 也保留原生句柄，避免数据库每次 I/O 都重新扫描路径并丢弃簇链索引。
     * 可写打开原先允许不占用原生槽位；资源紧张时继续保留此行为。 */
    if (!backend->directory &&
        !fat32_open(mount->filesystem, relative,
                           &backend->fat_file)) {
        bool opened = vfs_release_idle_fat_handle(mount->filesystem) &&
            fat32_open(mount->filesystem, relative, &backend->fat_file);
        if (!opened && keep_fat_file) {
            object_put(vnode);
            return K_EIO;
        }
    }
    /*
    * 优先使用规范 FAT vnode，使 mmap 和直接 I/O 共用一个页缓存。但注册表本身只是缓存：
    * 固定槽位耗尽绝不能导致原本有效的磁盘文件无法打开。
     */
    vfs_initialize();
    vfs_lock();
    vnode_t *existing = vfs_find_fat_node_locked(path);
    if (existing != 0) {
        object_get(existing);
        vfs_unlock();
        /* 规范 inode 已支持一致的直接读写。可写 open 不能丢弃其句柄和查找缓存。 */
        object_put(vnode);
        *out = existing;
        return K_OK;
    }
    uint32_t slot = vfs_free_node_slot_locked();
    if (slot == UINT32_MAX) {
        /*
         * 所有规范槽位都已占用。将此 vnode 作为未缓存对象返回，不要让第 65 个不同 FAT 路径变成 K_ENOMEM。
         * 已有缓存路径仍保持规范；此临时 vnode 会在最后一个文件/映射引用释放时销毁。
         */
        vfs_unlock();
        *out = vnode;
        return K_OK;
    }
    vfs_set_node_slot_locked(slot, vnode);
    object_get(vnode);
    vfs_unlock();
    *out = vnode;
    return K_OK;
}

kstatus_t vfs_register_node(vfs_backend_node_t *backend) {
    if (backend == 0) return K_EINVAL;
    vfs_initialize();
    vnode_t *vnode = vfs_create_vnode_object(backend);
    if (vnode == 0) {
        kfree(backend->data);
        kfree(backend);
        return K_ENOMEM;
    }

    vfs_lock();
    if (vfs_find_node_locked(backend->path) != 0) {
        vfs_unlock();
        object_put(vnode);
        return K_EBUSY;
    }
    uint32_t slot = vfs_free_node_slot_locked();
    if (slot == UINT32_MAX) {
        vfs_unlock();
        object_put(vnode);
        return K_ENOMEM;
    }
    vfs_set_node_slot_locked(slot, vnode);
    vfs_unlock();
    return K_OK;
}

/* 分配在锁外完成；冲突、父目录和索引发布必须属于同一个命名空间事务。
 * 失败时在解锁后释放未发布对象，避免析构和页缓存处理进入命名空间临界区。 */
static kstatus_t vfs_publish_memory_node(vnode_t *vnode, size_t path_length,
                                        kstatus_t missing_parent) {
    vfs_backend_node_t *backend = vnode->fs_private;
    vfs_initialize();
    vfs_lock();
    kstatus_t status;
    if (vfs_find_node_locked(backend->path) != 0) status = K_EEXIST;
    else if (!vfs_parent_directory_exists_locked(backend->path, path_length))
        status = missing_parent;
    else {
        uint32_t slot = vfs_free_node_slot_locked();
        status = slot == UINT32_MAX ? K_ENOMEM : K_OK;
        if (status == K_OK) vfs_set_node_slot_locked(slot, vnode);
    }
    vfs_unlock();
    if (status != K_OK) object_put(vnode);
    return status;
}

static kstatus_t vfs_create_memory_node(const char *path, uint32_t mode,
                                        bool directory) {
    vfs_backend_node_t *backend;
    size_t path_length;
    char relative[VFS_PATH_LIMIT];
    os_file_info_t existing_info;
    vfs_fat32_mount_t *mount;
    if (path == 0 || path[0] != '/') return K_EINVAL;
    path_length = vfs_string_length(path);
    if (path_length <= 1U || path_length >= VFS_PATH_LIMIT ||
        path[path_length - 1U] == '/') return K_EINVAL;
    mount = vfs_fat32_mount_for_path(path, relative);
    if (mount != 0) {
        if (relative[0] == 0) return K_EBUSY;
        if (fat32_stat_path(mount->filesystem, relative, &existing_info)) {
            return K_EEXIST;
        }
        if (!fat32_create_path(mount->filesystem, relative, directory)) {
            return K_EIO;
        }
        return K_OK;
    }
    backend = (vfs_backend_node_t *)kzalloc(sizeof(*backend), 0);
    if (backend == 0) return K_ENOMEM;
    for (size_t index = 0U; index <= path_length; ++index) {
        backend->path[index] = path[index];
    }
    backend->size = 0U;
    backend->mode = (directory ? 0040000U : 0100000U) |
                    (mode == 0U ? (directory ? 0755U : 0666U) : (mode & 0777U));
    backend->directory = directory;
    backend->mutable_node = true;
    backend->read = directory ? 0 : vfs_memory_backend_read;
    backend->write = directory ? 0 : vfs_memory_backend_write;
    backend->fsync = 0;
    backend->truncate = directory ? 0 : vfs_memory_backend_truncate;
    backend->context = backend;
    io_mutex_init(&backend->io_lock, true);
    vnode_t *vnode = vfs_create_vnode_object(backend);
    if (vnode == 0) {
        kfree(backend);
        return K_ENOMEM;
    }
    return vfs_publish_memory_node(vnode, path_length, K_ENOTDIR);
}

kstatus_t vfs_create_kernel(const char *path, uint32_t mode, bool directory) {
    return vfs_create_memory_node(path, mode, directory);
}

kstatus_t vfs_mkdir_kernel(const char *path, uint32_t mode) {
    return vfs_create_memory_node(path, mode, true);
}

/* 不可变链接内容与命名空间项一起发布。父目录验证和冲突检测共用命名空间锁：
 * 创建失败或发生竞争时都不会暴露只初始化了一部分的链接。 */
kstatus_t vfs_symlink_kernel(const char *target, const char *path) {
    char relative[VFS_PATH_LIMIT];
    size_t length = vfs_string_length(path);
    size_t target_length = vfs_string_length(target);
    if (path == 0 || target == 0 || path[0] != '/') return K_EINVAL;
    if (target_length == 0U) return K_ENOENT;
    if (length >= VFS_PATH_LIMIT || target_length >= VFS_PATH_LIMIT)
        return K_ENAMETOOLONG;
    if (length <= 1U || path[length - 1U] == '/') return K_EINVAL;
    /* FAT 没有原生符号链接表示。不要在持久文件上发布易失影子 inode；/tmp 已由内存后备。 */
    if (vfs_fat32_mount_for_path(path, relative) != 0) return K_EPERM;
    vfs_backend_node_t *backend = kzalloc(sizeof(*backend), 0);
    if (backend == 0) return K_ENOMEM;
    backend->data = kzalloc(target_length + 1U, 0);
    if (backend->data == 0) { kfree(backend); return K_ENOMEM; }
    memcpy(backend->path, path, length + 1U);
    memcpy(backend->data, target, target_length);
    backend->size = target_length;
    backend->data_capacity = target_length + 1U;
    backend->mode = 0120777U;
    backend->mutable_node = true;
    io_mutex_init(&backend->io_lock, true);
    vnode_t *vnode = vfs_create_vnode_object(backend);
    if (vnode == 0) {
        kfree(backend->data); kfree(backend); return K_ENOMEM;
    }
    return vfs_publish_memory_node(vnode, length, K_ENOENT);
}

kstatus_t vfs_readlink_kernel(const char *path, char *buffer, size_t capacity,
                             size_t *bytes) {
    if (path == 0 || path[0] != '/' || buffer == 0 || bytes == 0 || capacity == 0U)
        return K_EINVAL;
    *bytes = 0U;
    if (vfs_string_length(path) >= VFS_PATH_LIMIT) return K_ENAMETOOLONG;
    vfs_initialize();
    vfs_lock();
    vnode_t *vnode = vfs_find_node_locked(path);
    if (vnode == 0) {
        vfs_unlock();
        os_file_info_t info;
        kstatus_t status = vfs_stat_kernel(path, &info);
        return status == K_OK ? K_EINVAL : status;
    }
    vfs_backend_node_t *backend = vnode->fs_private;
    if (backend == 0 || (backend->mode & 0170000U) != 0120000U) {
        vfs_unlock(); return K_EINVAL;
    }
    if (backend->data == 0 || backend->size == 0U || backend->size >= VFS_PATH_LIMIT) {
        vfs_unlock(); return K_EIO;
    }
    size_t count = backend->size < capacity ? (size_t)backend->size : capacity;
    memcpy(buffer, backend->data, count);
    *bytes = count; // readlink 按约定不追加字符串终止符。
    vfs_unlock();
    return K_OK;
}

/* FAT 不保存符号链接，普通磁盘路径可直接查找；内存后备路径可能包含链接，必须逐段解析。 */
bool vfs_path_needs_resolution(const char *path) {
    if (path == 0 || path[0] != '/') return true;
    for (size_t i = 0U; path[i] != '\0'; ++i) {
        if (path[i] != '/') continue;
        const char *part = path + i + 1U;
        if (part[0] == '/' || (i != 0U && part[0] == '\0')) return true;
        if (part[0] != '.') continue;
        if (part[1] == '\0' || part[1] == '/') return true;
        if (part[1] == '.' && (part[2] == '\0' || part[2] == '/')) return true;
    }
    char relative[VFS_PATH_LIMIT];
    return vfs_fat32_mount_for_path(path, relative) == 0;
}

/* 按顺序解析路径分量：先展开前面的链接，再折叠 '..'。元数据/readlink 操作分别短暂获取命名空间锁；
 * 持有命名空间锁时不执行后备存储查询或内存分配。 */
kstatus_t vfs_resolve_path_kernel(const char *path, bool follow_final,
                                bool allow_missing_leaf, char output[256]) {
    char pending[VFS_PATH_LIMIT], candidate[VFS_PATH_LIMIT];
    char target[VFS_PATH_LIMIT], replacement[VFS_PATH_LIMIT];
    size_t length = vfs_string_length(path);
    if (path == 0 || output == 0 || path[0] != '/') return K_EINVAL;
    if (length >= VFS_PATH_LIMIT) return K_ENAMETOOLONG;
    bool trailing = length > 0U && path[length - 1U] == '/';
    memcpy(pending, path, length + 1U);
    output[0] = '/'; output[1] = 0;
    size_t prefix = 1U, cursor = 0U;
    unsigned links = 0U;
    for (;;) {
        while (pending[cursor] == '/') ++cursor;
        if (pending[cursor] == 0) return K_OK;
        size_t start = cursor;
        while (pending[cursor] != 0 && pending[cursor] != '/') ++cursor;
        size_t component = cursor - start;
        bool separator = pending[cursor] == '/';
        size_t tail = cursor;
        while (pending[tail] == '/') ++tail;
        bool last = pending[tail] == 0;
        if (component == 1U && pending[start] == '.') continue;
        if (component == 2U && pending[start] == '.' && pending[start + 1U] == '.') {
            while (prefix > 1U && output[prefix - 1U] != '/') --prefix;
            if (prefix > 1U) --prefix;
            output[prefix] = 0;
            continue;
        }
        size_t base = prefix + (prefix > 1U ? 1U : 0U);
        if (base + component >= VFS_PATH_LIMIT) return K_ENAMETOOLONG;
        memcpy(candidate, output, prefix);
        if (prefix > 1U) candidate[prefix] = '/';
        memcpy(candidate + base, pending + start, component);
        candidate[base + component] = 0;
        os_file_info_t info;
        kstatus_t status = vfs_stat_kernel(candidate, &info);
        if (status == K_ENOENT && last && allow_missing_leaf && !separator && !trailing) {
            memcpy(output, candidate, base + component + 1U);
            return K_OK;
        }
        if (status != K_OK) return status;
        bool directory_required = !last || separator || trailing;
        if (info.type == OS_FILE_TYPE_SYMLINK &&
            (!last || follow_final || directory_required)) {
            if (++links > 40U) return K_ELOOP;
            size_t bytes = 0U;
            status = vfs_readlink_kernel(candidate, target, sizeof(target) - 1U, &bytes);
            if (status != K_OK) return status;
            if (bytes == 0U || bytes >= VFS_PATH_LIMIT) return K_EIO;
            target[bytes] = 0;
            size_t remaining = vfs_string_length(pending + cursor);
            if (bytes + remaining >= VFS_PATH_LIMIT) return K_ENAMETOOLONG;
            memcpy(replacement, target, bytes);
            memcpy(replacement + bytes, pending + cursor, remaining + 1U);
            memcpy(pending, replacement, bytes + remaining + 1U);
            cursor = 0U;
            if (target[0] == '/') { prefix = 1U; output[1] = 0; }
            continue;
        }
        if (directory_required && info.type != OS_FILE_TYPE_DIRECTORY) return K_ENOTDIR;
        prefix = base + component;
        memcpy(output, candidate, prefix + 1U);
    }
}

kstatus_t vfs_stat_kernel(const char *path, os_file_info_t *out) {
    size_t path_length;
    char relative[VFS_PATH_LIMIT];
    vfs_fat32_mount_t *mount;
    if (path == 0 || out == 0 || path[0] != '/') return K_EINVAL;
    path_length = vfs_string_length(path);
    if (path_length == 0U || path_length >= VFS_PATH_LIMIT) return K_EINVAL;
    while (path_length > 1U && path[path_length - 1U] == '/') --path_length;
    mount = vfs_fat32_mount_for_path(path, relative);
    if (mount != 0 && relative[0] == 0) {
        *out = (os_file_info_t){
            .type = OS_FILE_TYPE_DIRECTORY, .mode = 0040755U
        };
        vfs_set_leaf_name(path, out->name);
        return K_OK;
    }
    vfs_initialize();
    vfs_lock();
    /* 已发布的 FAT inode 与 fstat 使用同一份元数据。命中时不再扫描磁盘目录，
     * 也不会读到尚未写回目录项的旧长度。查询沿用卷标识和 detached 检查。 */
    vnode_t *vnode = mount != 0 ? vfs_find_fat_node_locked(path) :
                                vfs_find_node_locked(path);
    if (vnode != 0) {
        vfs_backend_node_t *backend = (vfs_backend_node_t *)vnode->fs_private;
        if (backend == 0) {
            vfs_unlock();
            return K_EIO;
        }
        vfs_fill_info(path, vnode, backend, out);
        vfs_unlock();
        return K_OK;
    }
    if (mount == 0 && vfs_directory_exists_locked(path, path_length)) {
        *out = (os_file_info_t){0};
        out->type = OS_FILE_TYPE_DIRECTORY;
        out->mode = 0040555U;
        vfs_set_leaf_name(path, out->name);
        vfs_unlock();
        return K_OK;
    }
    vfs_unlock();
    if (mount != 0) {
        bool found = fat32_stat_path(mount->filesystem, relative, out);
        if (found) return K_OK;
        if (!vfs_mount_parent_exists(path, path_length)) return K_ENOENT;
        *out = (os_file_info_t){
            .type = OS_FILE_TYPE_DIRECTORY, .mode = 0040755U
        };
        vfs_set_leaf_name(path, out->name);
        return K_OK;
    }
    return K_ENOENT;
}

kstatus_t vfs_space_kernel(const char *path, vfs_space_info_t *out) {
    char resolved[VFS_PATH_LIMIT], relative[VFS_PATH_LIMIT];
    os_file_info_t info;
    if (out == 0) return K_EINVAL;
    kstatus_t status = vfs_resolve_path_kernel(path, true, false, resolved);
    if (status != K_OK) return status;
    status = vfs_stat_kernel(resolved, &info);
    if (status != K_OK) return status;
    vfs_fat32_mount_t *mount = vfs_fat32_mount_for_path(resolved, relative);
    if (mount != 0) return vfs_disk_space(mount->filesystem, out);
    vfs_initialize();
    vfs_lock();
    vnode_t *vnode = vfs_find_node_locked(resolved);
    bool memory = vnode != 0 ? vfs_backend_memory_space(vnode->fs_private) :
        vfs_volatile_path(resolved);
    vfs_unlock();
    return memory ? vfs_memory_space(out) : K_ENOSYS;
}

kstatus_t vfs_remove_typed_kernel(const char *path, int directory) {
    size_t path_length;
    vnode_t *victim = 0;
    char relative[VFS_PATH_LIMIT];
    vfs_fat32_mount_t *mount;
    os_file_info_t mounted_info;
    if (path == 0 || path[0] != '/' || directory < -1 || directory > 1)
        return K_EINVAL;
    path_length = vfs_string_length(path);
    if (path_length <= 1U || path_length >= VFS_PATH_LIMIT) return K_EINVAL;
    while (path_length > 1U && path[path_length - 1U] == '/') --path_length;
    if (path_length <= 1U) return K_EINVAL;
    mount = vfs_fat32_mount_for_path(path, relative);
    if (mount != 0) {
        vnode_t *mounted_victim = 0;
        vfs_backend_node_t *mounted_backend = 0;
        if (relative[0] == 0 ||
            !fat32_stat_path(mount->filesystem, relative,
                                    &mounted_info)) {
            return K_ENOENT;
        }
        vfs_initialize();
        if (directory >= 0 &&
            ((mounted_info.type == OS_FILE_TYPE_DIRECTORY) != (directory != 0)))
            return directory != 0 ? K_ENOTDIR : K_EISDIR;
        vfs_lock();
        mounted_victim = vfs_find_fat_node_locked(path);
        if (mounted_victim != 0) {
            object_get(mounted_victim);
            mounted_backend = (vfs_backend_node_t *)mounted_victim->fs_private;
        }
        vfs_unlock();
        if (mounted_backend != 0) {
            kstatus_t status = vfs_fat32_backend_prepare_unlink(mounted_backend);
            if (status != K_OK) {
                object_put(mounted_victim);
                return status;
            }
        }
        if (mounted_backend != 0) {
            vfs_backend_lock(mounted_backend);
            if (!mounted_backend->directory &&
                (!mounted_backend->unlink_prepared || mounted_backend->detached ||
                 mounted_backend->size != mounted_backend->snapshot_size ||
                 mounted_backend->content_revision != mounted_backend->snapshot_revision)) {
                vfs_backend_unlock(mounted_backend);
                vfs_fat32_backend_abort_unlink(mounted_backend);
                object_put(mounted_victim);
                return K_EBUSY;
            }
        }
        /* 原生 FAT 在原始句柄打开时会拒绝 unlink。快照现在负责旧文件读取，因此只在 unlink 边界释放句柄。
         * 若删除失败，后续 I/O 仍可重新打开未变化的路径。 */
        if (mounted_backend != 0 && mounted_backend->fat_file != 0) {
            if (!fat32_close(mounted_backend->fat_file)) {
                vfs_backend_unlock(mounted_backend);
                vfs_fat32_backend_abort_unlink(mounted_backend);
                object_put(mounted_victim);
                return K_EIO;
            }
            mounted_backend->fat_file = 0;
        }
        if (!fat32_remove_path_typed(mount->filesystem, relative, directory)) {
            if (mounted_backend != 0) {
                vfs_backend_unlock(mounted_backend);
                vfs_fat32_backend_abort_unlink(mounted_backend);
            }
            if (mounted_victim != 0) object_put(mounted_victim);
            return mounted_info.type == OS_FILE_TYPE_DIRECTORY ? K_ENOTEMPTY : K_EIO;
        }
        if (mounted_backend != 0) {
            vfs_fat32_backend_commit_unlink_locked(mounted_backend);
            vfs_backend_unlock(mounted_backend);
        }
        /* 删除磁盘目录项后同步摘除规范 vnode；仍被打开的 file 引用会让
         * vnode 保持有效。同名重建的 vnode 必须保留。 */
        if (mounted_victim != 0) {
            vnode_t *detached = 0;
            vfs_lock();
            /* 淘汰被固定的 inode，而不是同名替代项。即使两轮操作之间挂载发生变化，
             * 指针标识仍保持有效。 */
            for (uint32_t node_index = 0U;
                 node_index < g_vfs_node_capacity; ++node_index) {
                vnode_t *candidate = g_vfs_nodes[node_index];
                if (candidate == 0 || candidate != mounted_victim) continue;
                vfs_set_node_slot_locked(node_index, 0);
                detached = candidate;
                break;
            }
            vfs_unlock();
            /* 发布节点时已保证每个 vnode 只占一个槽位。摘除后无需再次全表查找；
             * 锁外释放可能触发同名重建，但新的 vnode 不能作为旧目标继续淘汰。 */
            if (detached != 0) object_put(detached);
        }
        if (mounted_victim != 0) object_put(mounted_victim);
        return K_OK;
    }
    vfs_initialize();
    vfs_lock();
    for (uint32_t node_index = 0U; node_index < g_vfs_node_capacity; ++node_index) {
        vnode_t *candidate = g_vfs_nodes[node_index];
        vfs_backend_node_t *backend = candidate != 0 ?
                                      (vfs_backend_node_t *)candidate->fs_private : 0;
        if (backend == 0) continue;
        if (backend->path[path_length] != 0 ||
            !vfs_path_range_equals(backend->path, path_length, path, path_length)) continue;
        if (!backend->mutable_node) {
            vfs_unlock();
            return K_EACCES;
        }
        /* 在执行删除的同一命名空间锁下验证目标项。 */
        if (directory >= 0 && backend->directory != (directory != 0)) {
            vfs_unlock();
            return directory != 0 ? K_ENOTDIR : K_EISDIR;
        }
        if (backend->directory) {
            for (uint32_t child_index = 0U; child_index < g_vfs_node_capacity;
                 ++child_index) {
                vnode_t *child = g_vfs_nodes[child_index];
                vfs_backend_node_t *child_backend = child != 0 ?
                    (vfs_backend_node_t *)child->fs_private : 0;
                if (child_backend == 0 || child == candidate) continue;
                if (child_backend->path[path_length] == '/' &&
                    vfs_path_range_equals(child_backend->path, path_length,
                                          path, path_length)) {
                    vfs_unlock();
                    return K_ENOTEMPTY;
                }
            }
        }
        vfs_set_node_slot_locked(node_index, 0);
        victim = candidate;
        break;
    }
    vfs_unlock();
    if (victim == 0) return K_ENOENT;
    object_put(victim);
    return K_OK;
}

kstatus_t vfs_remove_kernel(const char *path) {
    return vfs_remove_typed_kernel(path, -1);
}

kstatus_t vfs_open_kernel(const char *path, uint32_t flags, uint32_t mode,
                          file_t **out) {
    const uint32_t known_flags = VFS_OPEN_READ | VFS_OPEN_WRITE |
                                 VFS_OPEN_CREATE | VFS_OPEN_EXCLUSIVE |
                                 VFS_OPEN_TRUNCATE | VFS_OPEN_APPEND |
                                 VFS_OPEN_DIRECTORY;
    bool existed = false;
    if (path == 0 || out == 0 || path[0] != '/' ||
        (flags & ~known_flags) != 0U ||
        (flags & (VFS_OPEN_READ | VFS_OPEN_WRITE)) == 0U ||
        ((flags & VFS_OPEN_TRUNCATE) != 0U && (flags & VFS_OPEN_WRITE) == 0U) ||
        ((flags & VFS_OPEN_APPEND) != 0U && (flags & VFS_OPEN_DIRECTORY) != 0U)) {
        return K_EINVAL;
    }
    vfs_initialize();
    vnode_t *vnode = 0;
    os_file_info_t mounted_info;
    char relative[VFS_PATH_LIMIT];
    vfs_fat32_mount_t *mount = vfs_fat32_mount_for_path(path, relative);
    bool mounted_path = mount != 0;
    /* 普通文件直接固定已发布的 inode，再走统一的权限、独占和截断检查。
     * 目录可能是挂载点的隐式父目录，仍沿用后面的存在性判断。 */
    if (mounted_path) {
        vfs_lock();
        vnode_t *cached = vfs_find_fat_node_locked(path);
        if (cached != 0) {
            vfs_backend_node_t *cached_backend = cached->fs_private;
            if (!cached_backend->directory) {
                object_get(cached);
                vnode = cached;
            }
        }
        vfs_unlock();
    }
    bool mounted_exists = vnode != 0 || (mounted_path &&
        (relative[0] == 0 ||
         fat32_stat_path(mount->filesystem, relative, &mounted_info)));
    bool mounted_parent = mounted_path && !mounted_exists &&
                          vfs_mount_parent_exists(path, vfs_string_length(path));
    if (mounted_path && relative[0] == 0) {
        mounted_info = (os_file_info_t){
            .type = OS_FILE_TYPE_DIRECTORY, .mode = 0040755U
        };
    }
    existed = mounted_exists;
    if (vnode == 0 && (mounted_exists || mounted_parent)) {
        kstatus_t mount_status = vfs_create_fat32_vnode(
            path, (flags & VFS_OPEN_WRITE) == 0U,
            mounted_exists ? &mounted_info : 0, &vnode);
        if (mount_status != K_OK) return mount_status;
    }
    /* 根 FAT 挂载覆盖磁盘路径，但其下精确匹配的内存后端
     * /native remain valid namespace entries.  Look them up when the
     * mounted filesystem did not provide the requested path. */
    if (vnode == 0) {
        vfs_lock();
        vnode = vfs_find_node_locked(path);
        if (vnode != 0) {
            vfs_backend_node_t *backend =
                (vfs_backend_node_t *)vnode->fs_private;
            if (mounted_path && !mounted_exists && backend != 0 &&
                backend->fat32_node) {
                vnode = 0;
            } else {
                existed = true;
                object_get(vnode);
            }
        }
        vfs_unlock();
    }
    /* /tmp is an implicit volatile directory so that callers can create
    * 可提供没有根映像持久目录项的临时文件。首次以 O_DIRECTORY 打开时创建其目录 vnode；
    * 否则 stat 和父目录检查会认为它存在，而 open("/tmp") 却返回 ENOENT。 */
    if (vnode == 0 && (flags & VFS_OPEN_DIRECTORY) != 0U &&
        vfs_string_length(path) == 4U &&
        vfs_path_range_equals(path, 4U, "/tmp", 4U)) {
        kstatus_t directory_status = vfs_create_memory_node(path, 0755U, true);
        if (directory_status != K_OK && directory_status != K_EEXIST) {
            return directory_status;
        }
        vfs_lock();
        vnode = vfs_find_node_locked(path);
        if (vnode != 0) {
            object_get(vnode);
            existed = true;
        }
        vfs_unlock();
    }
    if (vnode == 0 && (flags & VFS_OPEN_CREATE) != 0U) {
        kstatus_t create_status = vfs_create_memory_node(
            path, mode, (flags & VFS_OPEN_DIRECTORY) != 0U);
        if (create_status != K_OK && create_status != K_EEXIST) return create_status;
        if (!mounted_path) {
            vfs_lock();
            vnode = vfs_find_node_locked(path);
            if (vnode != 0) object_get(vnode);
            vfs_unlock();
        }
        if (vnode == 0 && mounted_path) {
            kstatus_t mount_status = vfs_create_fat32_vnode(
                path, (flags & VFS_OPEN_WRITE) == 0U, 0, &vnode);
            if (mount_status != K_OK) return mount_status;
        }
        existed = create_status == K_EEXIST;
    }
    if (vnode == 0) return K_ENOENT;
    if ((flags & VFS_OPEN_EXCLUSIVE) != 0U &&
        (flags & VFS_OPEN_CREATE) != 0U && existed) {
        object_put(vnode);
        return K_EEXIST;
    }
    vfs_backend_node_t *backend = (vfs_backend_node_t *)vnode->fs_private;
    if (backend == 0 ||
        (backend->directory && (flags & VFS_OPEN_DIRECTORY) == 0U) ||
        (!backend->directory && (flags & VFS_OPEN_DIRECTORY) != 0U) ||
        ((!backend->directory) && (flags & VFS_OPEN_READ) != 0U && backend->read == 0) ||
        ((flags & VFS_OPEN_WRITE) != 0U && backend->write == 0)) {
        /* vnode 的最后一个引用可能在 object_put 后销毁后端；先保存错误类型。 */
        kstatus_t failure = backend != 0 && backend->directory ? K_EISDIR :
            (backend != 0 && (flags & VFS_OPEN_DIRECTORY) != 0U ? K_ENOTDIR : K_EACCES);
        object_put(vnode);
        return failure;
    }
    if ((flags & VFS_OPEN_TRUNCATE) != 0U) {
        kstatus_t truncate_status = vfs_truncate_node(vnode, 0U);
        if (truncate_status != K_OK) {
            object_put(vnode);
            return truncate_status;
        }
    }
    file_t *file = (file_t *)kzalloc(sizeof(*file), 0);
    if (file == 0) {
        object_put(vnode);
        return K_ENOMEM;
    }
    refcount_init(&file->object.refs, 1U);
    file->object.type = KOBJECT_TYPE_FILE;
    file->object.flags = 0;
    file->object.ops = &g_file_object_ops;
    file->vnode = vnode;
    file->ops = 0;
    file->position = (flags & VFS_OPEN_APPEND) != 0U ? vnode->size : 0U;
    file->flags = flags;
    file->rights = flags;
    file->private_data = 0;
    vfs_file_position_init(file);
    *out = file;
    return K_OK;
}

#endif
