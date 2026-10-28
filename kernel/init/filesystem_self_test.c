#include <kernel/init_filesystem.h>
#include <kernel/bootinfo.h>
#include <arch/x86_64/cpu.h>
#include <arch/x86_64/paging.h>
#include <kernel/block.h>
#include <kernel/debug_stage.h>
#include <kernel/device.h>
#include <kernel/elf_loader.h>
#include <kernel/io.h>
#include <kernel/journal.h>
#include <kernel/litefs.h>
#include <kernel/mm.h>
#include <kernel/vfs.h>
#include <kernel/vm.h>
#include <kernel/block_device.h>
#include <kernel/fat32.h>
#include <kernel/console.h>
#include "../fs/vfs/internal.h"
#include "filesystem_internal.h"

/* REFACTOR_P3_FILESYSTEM_SELF_TEST_OWNER：FAT32、VFS 和文件映射测试。 */

#define FAT32_TEST_SECTOR_COUNT 128U
static UINT8 g_fat32_test_disk[FAT32_TEST_SECTOR_COUNT * 512U];

static VOID store_u16(UINT8 *destination, UINT16 value) {
    destination[0] = (UINT8)value;
    destination[1] = (UINT8)(value >> 8);
}

static VOID store_u32(UINT8 *destination, UINT32 value) {
    destination[0] = (UINT8)value;
    destination[1] = (UINT8)(value >> 8);
    destination[2] = (UINT8)(value >> 16);
    destination[3] = (UINT8)(value >> 24);
}

static BOOLEAN fat32_test_read(VOID *context, UINT64 lba, UINT32 count, VOID *buffer) {
    UINT8 *disk = (UINT8 *)context;
    if (disk == 0 || buffer == 0 || lba >= FAT32_TEST_SECTOR_COUNT ||
        count > FAT32_TEST_SECTOR_COUNT - lba) return 0;
    for (UINT32 i = 0; i < count * 512U; ++i) {
        ((UINT8 *)buffer)[i] = disk[lba * 512ULL + i];
    }
    return 1;
}

static BOOLEAN fat32_test_write(VOID *context, UINT64 lba, UINT32 count,
                                const VOID *buffer) {
    UINT8 *disk = (UINT8 *)context;
    if (disk == 0 || buffer == 0 || lba >= FAT32_TEST_SECTOR_COUNT ||
        count > FAT32_TEST_SECTOR_COUNT - lba) return 0;
    for (UINT32 i = 0; i < count * 512U; ++i) {
        disk[lba * 512ULL + i] = ((const UINT8 *)buffer)[i];
    }
    return 1;
}

static BOOLEAN filesystem_vfs_fat_delete_self_test(void) {
    static const CHAR8 path[] = "/vfsdel.asm";
    static const CHAR8 fileman_path[] = "/VFSDEL.ASM";
    static const CHAR8 source[] = "bits 64\n";
    static const CHAR8 replacement_source[] = "bits 32\n";
    static const CHAR8 retained_tail[] = "tail";
    file_t *file = 0;
    file_t *replacement = 0;
    file_t *reader = 0;
    file_t *pressure_open = 0;
    static OS_FAT32_FILE *pressure[OS_FAT32_MAX_OPEN_FILES];
    UINT32 pressure_count = 0U;
    os_file_info_t info = {0};
    UINT64 bytes = 0U;
    CHAR8 result[sizeof(source)] = {0};
    BOOLEAN success = 0;

    char relative[VFS_PATH_LIMIT];
    vfs_fat32_mount_t *mount = vfs_fat32_mount_for_path(path, relative);
    UINT32 seeded = 0U;
    if (mount == 0 || !fat32_create_path(mount->filesystem, relative, 0) ||
        !fat32_write_path(mount->filesystem, relative, 0U, source,
                                 sizeof(source) - 1U, &seeded) ||
        seeded != sizeof(source) - 1U ||
        vfs_open_kernel(path, VFS_OPEN_READ, 0U, &reader) != K_OK || reader == 0) {
        goto cleanup;
    }
    vfs_backend_node_t *shared = (vfs_backend_node_t *)reader->vnode->fs_private;
    if (shared == 0 || shared->fat_file == 0) goto cleanup;
    OS_FAT32_FILE *retained_inode = shared->fat_file;
    if (!fat32_create_path(mount->filesystem, "/vfspress.asm", 0)) goto cleanup;
    /* 缓存命中的 VFS open 不需要备用原生 FAT 槽位。先填满原始表，再验证读/写 open 均保留同一 inode。 */
    while (pressure_count < OS_FAT32_MAX_OPEN_FILES &&
           fat32_open(mount->filesystem, relative, &pressure[pressure_count])) {
        ++pressure_count;
    }
    for (UINT32 index = 0U; index < OS_FAT32_MAX_OPEN_FILES; ++index) {
        if (!mount->filesystem->OpenFiles[index].Used) goto cleanup;
    }
    if (pressure_count == OS_FAT32_MAX_OPEN_FILES ||
        vfs_open_kernel(path, VFS_OPEN_READ | VFS_OPEN_WRITE, 0U,
                         &pressure_open) != K_OK || pressure_open == 0 ||
        pressure_open->vnode != reader->vnode || shared->fat_file != retained_inode) {
        goto cleanup;
    }
    vfs_close(pressure_open);
    pressure_open = 0;
    if (vfs_open_kernel(path, VFS_OPEN_READ, 0U, &pressure_open) != K_OK ||
        pressure_open == 0 || pressure_open->vnode != reader->vnode ||
        shared->fat_file != retained_inode) goto cleanup;
    vfs_close(pressure_open);
    pressure_open = 0;
    /* /sample.txt is an idle canonical inode from the preceding test. A
    * 缓存未命中时必须回收备用句柄，同时保留当前读取者的 inode，即使此时所有原生槽位都已占用。 */
    if (vfs_open_kernel("/vfspress.asm", VFS_OPEN_READ, 0U, &pressure_open) != K_OK ||
        pressure_open == 0 || shared->fat_file != retained_inode ||
        ((vfs_backend_node_t *)pressure_open->vnode->fs_private)->fat_file == 0) goto cleanup;
    vfs_close(pressure_open);
    pressure_open = 0;
    while (pressure_count != 0U) (void)fat32_close(pressure[--pressure_count]);
    if (vfs_remove_kernel("/vfspress.asm") != K_OK) goto cleanup;
    if (vfs_open_kernel("/sample.txt", VFS_OPEN_READ, 0U, &pressure_open) != K_OK ||
        pressure_open == 0 ||
        ((vfs_backend_node_t *)pressure_open->vnode->fs_private)->fat_file != 0 ||
        vfs_read_kernel(pressure_open, result, 5U, &bytes) != K_OK || bytes != 5U ||
        __builtin_memcmp(result, "world", 5U) != 0) goto cleanup;
    vfs_close(pressure_open);
    pressure_open = 0;
    if (vfs_open_kernel(path,
                        VFS_OPEN_READ | VFS_OPEN_WRITE | VFS_OPEN_CREATE |
                            VFS_OPEN_TRUNCATE,
                        0666U, &file) != K_OK || file == 0 ||
        file->vnode != reader->vnode || shared->fat_file != retained_inode ||
        vfs_write_kernel(file, source, sizeof(source) - 1U, &bytes) != K_OK ||
        bytes != sizeof(source) - 1U ||
        vfs_rename_kernel(fileman_path, "/vfsren.asm") != K_OK ||
        vfs_stat_kernel(path, &info) != K_ENOENT ||
        vfs_file_stat(file, &info) != K_OK ||
        __builtin_memcmp(info.name, "vfsren.asm", sizeof("vfsren.asm")) != 0 ||
        vfs_seek(file, 0, OS_FILE_SEEK_SET, &bytes) != K_OK ||
        vfs_write_kernel(file, source, sizeof(source) - 1U, &bytes) != K_OK ||
        bytes != sizeof(source) - 1U ||
        vfs_seek(file, 0, OS_FILE_SEEK_SET, &bytes) != K_OK ||
        vfs_read_kernel(file, result, sizeof(source) - 1U, &bytes) != K_OK ||
        bytes != sizeof(source) - 1U) {
        goto cleanup;
    }
    for (UINT32 index = 0U; index < sizeof(source) - 1U; ++index) {
        if (result[index] != source[index]) goto cleanup;
    }
    if (vfs_rename_kernel("/VFSREN.ASM", path) != K_OK ||
        vfs_remove_kernel(fileman_path) != K_OK ||
        vfs_seek(file, 0, OS_FILE_SEEK_SET, &bytes) != K_OK ||
        vfs_read_kernel(file, result, sizeof(source) - 1U, &bytes) != K_OK ||
        bytes != sizeof(source) - 1U) {
        goto cleanup;
    }
    for (UINT32 index = 0U; index < sizeof(source) - 1U; ++index) {
        if (result[index] != source[index]) goto cleanup;
    }
    /* 复用路径名不能重定向已打开的旧版本。 */
    if (vfs_open_kernel(path,
                        VFS_OPEN_READ | VFS_OPEN_WRITE | VFS_OPEN_CREATE |
                            VFS_OPEN_TRUNCATE,
                        0666U, &replacement) != K_OK || replacement == 0 ||
        vfs_write_kernel(replacement, replacement_source,
                          sizeof(replacement_source) - 1U, &bytes) != K_OK ||
        bytes != sizeof(replacement_source) - 1U ||
        vfs_seek(file, 0, OS_FILE_SEEK_SET, &bytes) != K_OK ||
        vfs_read_kernel(file, result, sizeof(source) - 1U, &bytes) != K_OK ||
        bytes != sizeof(source) - 1U) goto cleanup;
    for (UINT32 index = 0U; index < sizeof(source) - 1U; ++index) {
        if (result[index] != source[index]) goto cleanup;
    }
    if (vfs_seek(replacement, 0, OS_FILE_SEEK_SET, &bytes) != K_OK ||
        vfs_rename_kernel(path, path) != K_OK ||
        vfs_read_kernel(replacement, result,
                         sizeof(replacement_source) - 1U, &bytes) != K_OK ||
        bytes != sizeof(replacement_source) - 1U) goto cleanup;
    for (UINT32 index = 0U; index < sizeof(replacement_source) - 1U; ++index) {
        if (result[index] != replacement_source[index]) goto cleanup;
    }
    vfs_close(replacement);
    replacement = 0;
    if (vfs_remove_kernel(fileman_path) != K_OK) goto cleanup;
    if (vfs_seek(file, sizeof(source) - 1U, OS_FILE_SEEK_SET, &bytes) != K_OK ||
        vfs_write_kernel(file, retained_tail, sizeof(retained_tail) - 1U,
                           &bytes) != K_OK ||
        bytes != sizeof(retained_tail) - 1U ||
        vfs_seek(file, sizeof(source) - 1U, OS_FILE_SEEK_SET, &bytes) != K_OK ||
        vfs_read_kernel(file, result, sizeof(retained_tail) - 1U, &bytes) != K_OK ||
        bytes != sizeof(retained_tail) - 1U) goto cleanup;
    for (UINT32 index = 0U; index < sizeof(retained_tail) - 1U; ++index) {
        if (result[index] != retained_tail[index]) goto cleanup;
    }
    if (vfs_truncate_kernel(file, sizeof(source) - 1U) != K_OK) goto cleanup;
    vfs_close(file);
    file = 0;

    if (vfs_stat_kernel(path, &info) != K_ENOENT ||
        vfs_rename_kernel(path, path) != K_ENOENT ||
        vfs_open_kernel(path, VFS_OPEN_READ, 0U, &file) != K_ENOENT) {
        goto cleanup;
    }
    if (file != 0) goto cleanup;
    if (vfs_open_kernel(path,
                        VFS_OPEN_READ | VFS_OPEN_WRITE | VFS_OPEN_CREATE |
                            VFS_OPEN_TRUNCATE,
                        0666U, &file) != K_OK || file == 0 ||
        vfs_remove_kernel(fileman_path) != K_OK ||
        vfs_stat_kernel(path, &info) != K_ENOENT) {
        goto cleanup;
    }
    vfs_close(file);
    file = 0;
    success = 1;

cleanup:
    if (pressure_open != 0) vfs_close(pressure_open);
    while (pressure_count != 0U) (void)fat32_close(pressure[--pressure_count]);
    if (reader != 0) vfs_close(reader);
    if (replacement != 0) vfs_close(replacement);
    if (file != 0) vfs_close(file);
    if (!success) (void)vfs_remove_kernel(path);
    if (!success) (void)vfs_remove_kernel("/vfsren.asm");
    if (!success) (void)vfs_remove_kernel("/vfspress.asm");
    return success;
}

static BOOLEAN filesystem_vfs_fat_rename_limit_self_test(void) {
    static const CHAR8 parent[] = "/vfslimit";
    static const CHAR8 child[] = "/vfslimit/child";
    CHAR8 destination[251];
    file_t *file = 0;
    os_file_info_t info = {0};
    UINT64 bytes = 0U;
    CHAR8 result = 0;
    BOOLEAN success = 0;
    destination[0] = '/';
    for (UINT32 i = 1U; i < 250U; ++i) destination[i] = 'x';
    destination[250] = 0;
    if (vfs_mkdir_kernel(parent, 0755U) != K_OK ||
        vfs_open_kernel(child, VFS_OPEN_CREATE | VFS_OPEN_READ | VFS_OPEN_WRITE,
                         0600U, &file) != K_OK || file == 0 ||
        vfs_write_kernel(file, "x", 1U, &bytes) != K_OK || bytes != 1U ||
        vfs_rename_kernel(parent, destination) != K_ENAMETOOLONG ||
        vfs_stat_kernel(parent, &info) != K_OK ||
        vfs_stat_kernel(child, &info) != K_OK || info.size != 1U ||
        vfs_stat_kernel(destination, &info) != K_ENOENT ||
        vfs_seek(file, 0, OS_FILE_SEEK_SET, &bytes) != K_OK ||
        vfs_read_kernel(file, &result, 1U, &bytes) != K_OK || bytes != 1U || result != 'x') {
        goto cleanup;
    }
    success = 1;
cleanup:
    if (file != 0) vfs_close(file);
    if (vfs_remove_kernel(child) != K_OK) success = 0;
    if (vfs_remove_kernel(parent) != K_OK) success = 0;
    return success;
}

/* 兼容性冒烟测试期间，Chrome 启动 profile 刻意使用内存命名空间。
 * 独立于 FAT 后端覆盖命名空间约定（mkdir/open/write/stat/rename/remove）。 */
static BOOLEAN filesystem_vfs_chrome_profile_overlay_self_test(void) {
    /* 不要将测试 Preferences 放入 Chrome 正在使用的 user-data 目录：桌面启动可能与后续文件系统检查重叠，
     * Chromium 会将下方刻意精简的夹具视为损坏的真实 profile。 */
    static const CHAR8 root[] = "/tmp/vfs-chrome-profile-test";
    static const CHAR8 profile[] = "/tmp/vfs-chrome-profile-test/Default";
    static const CHAR8 preferences[] =
        "/tmp/vfs-chrome-profile-test/Default/Preferences";
    static const CHAR8 local_state[] = "/tmp/vfs-chrome-profile-test/Local State";
    static const CHAR8 data[] = "{\"profile_test\":true}";
    file_t *file = 0;
    os_file_info_t info = {0};
    char relative[VFS_PATH_LIMIT];
    UINT64 bytes = 0U;
    UINT32 failure_stage = 0U;
    kstatus_t status = K_OK;
    BOOLEAN success = 0;

    failure_stage = 1U;
    if (vfs_fat32_relative_path(root, relative)) {
        status = K_EEXIST;
        goto cleanup;
    }
    failure_stage = 2U;
    status = vfs_mkdir_kernel(root, 0700U);
    if (status != K_OK) goto cleanup;
    failure_stage = 3U;
    status = vfs_mkdir_kernel(profile, 0700U);
    if (status != K_OK) goto cleanup;
    failure_stage = 4U;
    status = vfs_open_kernel(preferences,
                             VFS_OPEN_READ | VFS_OPEN_WRITE |
                                 VFS_OPEN_CREATE | VFS_OPEN_TRUNCATE,
                             0600U, &file);
    if (status != K_OK || file == 0) goto cleanup;
    failure_stage = 5U;
    status = vfs_write_kernel(file, data, sizeof(data) - 1U, &bytes);
    if (status != K_OK || bytes != sizeof(data) - 1U) {
        if (status == K_OK) status = K_EIO;
        goto cleanup;
    }
    failure_stage = 6U;
    status = vfs_stat_kernel(preferences, &info);
    if (status != K_OK || info.size != sizeof(data) - 1U) {
        if (status == K_OK) status = K_EIO;
        goto cleanup;
    }
    vfs_close(file);
    file = 0;
    failure_stage = 7U;
    status = vfs_rename_kernel(local_state, local_state);
    if (status != K_ENOENT) goto cleanup;
    status = vfs_rename_kernel(preferences, preferences);
    if (status != K_OK) goto cleanup;
    status = vfs_rename_kernel(preferences, local_state);
    if (status != K_OK) goto cleanup;
    failure_stage = 8U;
    status = vfs_stat_kernel(preferences, &info);
    if (status != K_ENOENT) goto cleanup;
    failure_stage = 9U;
    status = vfs_stat_kernel(local_state, &info);
    if (status != K_OK) goto cleanup;
    failure_stage = 10U;
    status = vfs_remove_kernel(local_state);
    if (status != K_OK) goto cleanup;
    failure_stage = 11U;
    status = vfs_remove_kernel(profile);
    if (status != K_OK) goto cleanup;
    failure_stage = 12U;
    status = vfs_remove_kernel(root);
    if (status != K_OK) goto cleanup;
    success = 1;

cleanup:
    if (file != 0) vfs_close(file);
    if (!success) {
        kernel_serial_write("OS_CHROME_OVERLAY_TEST_FAIL STAGE=");
        kernel_serial_write_u32(failure_stage);
        kernel_serial_write(" STATUS=");
        kernel_serial_write_u32((UINT32)(-status));
        kernel_serial_write("\r\n");
        (void)vfs_remove_kernel(local_state);
        (void)vfs_remove_kernel(preferences);
        (void)vfs_remove_kernel(profile);
        (void)vfs_remove_kernel(root);
    }
    return success;
}

/* Chromium 也会直接在 /tmp 创建大型匿名/共享内存后备文件（例如 .org.chromium.Chromium.*）。
 * 浏览器启动前确保这些文件使用易失内存后端，而不是 FAT。 */
static BOOLEAN filesystem_vfs_tmp_shared_file_self_test(void) {
    static const CHAR8 path[] = "/tmp/.org.chromium.Chromium.000000";
    static const UINT64 shared_size = 4198400U;
    file_t *file = 0;
    os_file_info_t info = {0};
    vfs_backend_node_t *backend;
    UINT32 failure_stage = 0U;
    kstatus_t status = K_OK;
    BOOLEAN success = 0;
    char relative[VFS_PATH_LIMIT];

    failure_stage = 1U;
    if (vfs_fat32_relative_path(path, relative)) {
        status = K_EIO;
        goto cleanup;
    }
    failure_stage = 2U;
    status = vfs_open_kernel(path,
                             VFS_OPEN_READ | VFS_OPEN_WRITE |
                                 VFS_OPEN_CREATE | VFS_OPEN_TRUNCATE,
                             0600U, &file);
    if (status != K_OK || file == 0) goto cleanup;
    backend = (vfs_backend_node_t *)file->vnode->fs_private;
    failure_stage = 3U;
    if (backend == 0 || backend->fat32_node ||
        backend->truncate != vfs_memory_backend_truncate) {
        status = K_EIO;
        goto cleanup;
    }
    failure_stage = 4U;
    status = vfs_truncate_kernel(file, shared_size);
    if (status != K_OK) goto cleanup;
    failure_stage = 5U;
    status = vfs_stat_kernel(path, &info);
    if (status != K_OK || info.size != shared_size) {
        if (status == K_OK) status = K_EIO;
        goto cleanup;
    }
    vfs_close(file);
    file = 0;
    failure_stage = 6U;
    status = vfs_remove_kernel(path);
    if (status != K_OK) goto cleanup;
    success = 1;

cleanup:
    if (file != 0) vfs_close(file);
    if (!success) {
        kernel_serial_write("OS_TMP_SHARED_FILE_TEST_FAIL STAGE=");
        kernel_serial_write_u32(failure_stage);
        kernel_serial_write(" STATUS=");
        kernel_serial_write_u32((UINT32)(-status));
        kernel_serial_write("\r\n");
        (void)vfs_remove_kernel(path);
    }
    return success;
}

static VOID filesystem_test_numbered_path(const CHAR8 *prefix, UINT32 index,
                                          CHAR8 output[VFS_PATH_LIMIT]) {
    size_t length = vfs_string_length(prefix);
    if (length >= VFS_PATH_LIMIT - 3U) return;
    for (size_t position = 0U; position < length; ++position) {
        output[position] = prefix[position];
    }
    output[length] = (CHAR8)('0' + (index / 10U));
    output[length + 1U] = (CHAR8)('0' + (index % 10U));
    output[length + 2U] = '\0';
}

/* Chrome 首个 profile 创建的节点数超过旧版固定 64 节点 VFS 缓存。
 * 将容量要求保留为可执行测试，避免小型命名空间回归在浏览器启动时静默返回 K_ENOMEM。 */
static BOOLEAN filesystem_vfs_chrome_profile_capacity_self_test(void) {
    static const CHAR8 root[] = "/tmp/vfs-chrome-profile-test";
    static const CHAR8 capacity_root[] =
        "/tmp/vfs-chrome-profile-test/capacity";
    static const CHAR8 node_prefix[] =
        "/tmp/vfs-chrome-profile-test/capacity/node-";
    static const UINT32 node_count = 80U;
    file_t *file = 0;
    CHAR8 path[VFS_PATH_LIMIT];
    UINT32 created = 0U;
    UINT32 failure_stage = 0U;
    kstatus_t status = K_OK;
    BOOLEAN success = 0;

    failure_stage = 1U;
    status = vfs_mkdir_kernel(root, 0700U);
    if (status != K_OK) goto cleanup;
    failure_stage = 2U;
    status = vfs_mkdir_kernel(capacity_root, 0700U);
    if (status != K_OK) goto cleanup;
    for (UINT32 index = 0U; index < node_count; ++index) {
        filesystem_test_numbered_path(node_prefix, index, path);
        failure_stage = 3U + index;
        status = vfs_open_kernel(path,
                                 VFS_OPEN_READ | VFS_OPEN_WRITE |
                                     VFS_OPEN_CREATE | VFS_OPEN_TRUNCATE,
                                 0600U, &file);
        if (status != K_OK || file == 0) goto cleanup;
        vfs_close(file);
        file = 0;
        ++created;
    }
    success = 1;

cleanup:
    if (file != 0) vfs_close(file);
    for (UINT32 index = 0U; index < created; ++index) {
        filesystem_test_numbered_path(node_prefix, index, path);
        (void)vfs_remove_kernel(path);
    }
    (void)vfs_remove_kernel(capacity_root);
    (void)vfs_remove_kernel(root);
    if (!success) {
        kernel_serial_write("OS_CHROME_CAPACITY_TEST_FAIL STAGE=");
        kernel_serial_write_u32(failure_stage);
        kernel_serial_write(" STATUS=");
        kernel_serial_write_u32((UINT32)(-status));
        kernel_serial_write("\r\n");
    }
    return success;
}

BOOLEAN filesystem_vfs_file_api_self_test(void) {
    file_t *file = 0;
    os_file_info_t info = {0};
    uint64_t bytes = 0U;
    uint64_t position = 0U;
    char result[4] = {0};
    bool success = false;
    if (!filesystem_vfs_chrome_profile_overlay_self_test()) return false;
    if (!filesystem_vfs_tmp_shared_file_self_test()) return false;
    if (!filesystem_vfs_chrome_profile_capacity_self_test()) return false;
    kstatus_t status = vfs_mkdir_kernel("/vfs-api", 0755U);
    if (status != K_OK) {
        kernel_serial_write("OS_VFS_API_FAIL_STAGE=1 STATUS=");
        kernel_serial_write_u32((uint32_t)(-status));
        kernel_serial_write("\r\n");
        goto cleanup;
    }
    if (
        vfs_open_kernel("/vfs-api/file", VFS_OPEN_READ | VFS_OPEN_WRITE |
                        VFS_OPEN_CREATE | VFS_OPEN_TRUNCATE, 0666U,
                        &file) != K_OK ||
        vfs_write_kernel(file, "abc", 3U, &bytes) != K_OK || bytes != 3U ||
        vfs_seek(file, 0, OS_FILE_SEEK_SET, &position) != K_OK || position != 0U ||
        vfs_read_kernel(file, result, 3U, &bytes) != K_OK || bytes != 3U ||
        result[0] != 'a' || result[1] != 'b' || result[2] != 'c' ||
        vfs_truncate_kernel(file, 1U) != K_OK ||
        vfs_stat_kernel("/vfs-api/file", &info) != K_OK || info.size != 1U ||
        info.type != OS_FILE_TYPE_REGULAR ||
        vfs_rename_kernel("/vfs-api/file", "/vfs-api/renamed") != K_OK ||
        vfs_stat_kernel("/vfs-api/file", &info) != K_ENOENT ||
        vfs_stat_kernel("/vfs-api/renamed", &info) != K_OK ||
        info.size != 1U || info.type != OS_FILE_TYPE_REGULAR ||
        vfs_seek(file, 0, OS_FILE_SEEK_SET, &position) != K_OK ||
        vfs_read_kernel(file, result, 1U, &bytes) != K_OK || bytes != 1U ||
        result[0] != 'a') goto cleanup;
    vfs_close(file);
    file = 0;
    if (vfs_stat_kernel("/vfs-api", &info) != K_OK ||
        info.type != OS_FILE_TYPE_DIRECTORY ||
        vfs_remove_kernel("/vfs-api/renamed") != K_OK ||
        vfs_remove_kernel("/vfs-api") != K_OK) goto cleanup;
    success = true;
cleanup:
    if (file != 0) vfs_close(file);
    if (!success) {
        (void)vfs_remove_kernel("/vfs-api/file");
        (void)vfs_remove_kernel("/vfs-api/renamed");
        (void)vfs_remove_kernel("/vfs-api");
    }
    return success;
}

BOOLEAN filesystem_fat32_self_test(void) {
    static OS_BLOCK_MANAGER block_manager;
    static OS_BLOCK_DEVICE *device;
    static OS_FAT32 filesystem;
    CHAR8 buffer[6] = {0};
    UINT32 size = 0;
    OS_FAT32_FILE *held_file = 0;
    UINT8 *fat1;
    UINT8 *fat2;
    UINT8 *root;
    UINT8 *data;
    file_t *mounted_file = 0;
    for (UINT32 i = 0; i < sizeof(g_fat32_test_disk); ++i) g_fat32_test_disk[i] = 0;
    g_fat32_test_disk[510] = 0x55U;
    g_fat32_test_disk[511] = 0xAAU;
    store_u16(g_fat32_test_disk + 11U, 512U);
    g_fat32_test_disk[13] = 1U;
    store_u16(g_fat32_test_disk + 14U, 1U);
    g_fat32_test_disk[16] = 2U;
    store_u16(g_fat32_test_disk + 19U, FAT32_TEST_SECTOR_COUNT);
    store_u32(g_fat32_test_disk + 36U, 1U);
    store_u32(g_fat32_test_disk + 44U, 2U);
    fat1 = g_fat32_test_disk + 512U;
    fat2 = fat1 + 512U;
    store_u32(fat1 + 0U, 0x0FFFFFF8U);
    store_u32(fat1 + 4U, 0xFFFFFFFFU);
    store_u32(fat1 + 8U, 0x0FFFFFFFU);
    store_u32(fat1 + 12U, 0x0FFFFFFFU);
    for (UINT32 i = 0; i < 512U; ++i) fat2[i] = fat1[i];
    root = g_fat32_test_disk + 3U * 512U;
    for (UINT32 i = 0; i < 11U; ++i) root[i] = (UINT8)"SAMPLE  TXT"[i];
    root[11] = 0x20U;
    store_u16(root + 26U, 3U);
    store_u32(root + 28U, 5U);
    data = g_fat32_test_disk + 4U * 512U;
    data[0] = 'h'; data[1] = 'e'; data[2] = 'l'; data[3] = 'l'; data[4] = 'o';
    if (!block_manager_init(&block_manager)) return 0;
    if (!block_register(&block_manager, "mem0", 512U, FAT32_TEST_SECTOR_COUNT,
                               fat32_test_read, fat32_test_write, 0,
                               g_fat32_test_disk, &device)) return 0;
    if (!fat32_init(&filesystem, device)) return 0;
    if (!fat32_read_path(&filesystem, "sample.txt", 0U, buffer, 5U, &size) ||
        size != 5U ||
        buffer[0] != 'h' || buffer[4] != 'o') return 0;
    if (!fat32_write_path(&filesystem, "sample.txt", 0U,
                                 "world", 5U, &size) || size != 5U ||
        !fat32_create_path(&filesystem, "new.txt", 0) ||
        fat32_create_path(&filesystem, "new.txt", 0) ||
        !fat32_create_path(&filesystem, "Long Created File.txt", 0) ||
        !fat32_write_path(&filesystem, "Long Created File.txt", 0U,
                                 "lfn", 3U, &size) || size != 3U ||
        !fat32_remove_path(&filesystem, "Long Created File.txt") ||
        !fat32_create_path(&filesystem, "Long Created File.txt", 0) ||
        !fat32_open(&filesystem, "Long Created File.txt", &held_file) ||
        fat32_remove_path(&filesystem, "Long Created File.txt") ||
        !fat32_close(held_file) ||
        !fat32_remove_path(&filesystem, "Long Created File.txt") ||
        !fat32_create_path(&filesystem, "Long Folder Name", 1) ||
        !fat32_create_path(&filesystem, "Long Folder Name/Long Child Name.txt", 0) ||
        !fat32_remove_path(&filesystem, "Long Folder Name/Long Child Name.txt") ||
        !fat32_remove_path(&filesystem, "Long Folder Name") ||
        !fat32_create_path(&filesystem, "subdir", 1) ||
        !fat32_create_path(&filesystem, "subdir/inner.txt", 0) ||
        fat32_remove_path(&filesystem, "subdir") ||
        !fat32_remove_path(&filesystem, "subdir/inner.txt") ||
        !fat32_remove_path(&filesystem, "subdir") ||
        !fat32_remove_path(&filesystem, "new.txt") ||
        !fat32_sync(&filesystem) ||
        g_fat32_test_disk[4U * 512U] != 'w' ||
        g_fat32_test_disk[4U * 512U + 4U] != 'd' ||
        vfs_mount_fat32("/", &filesystem) != K_OK) return 0;
    os_file_info_t mounted_info = {0};
    if (vfs_stat_kernel("/sample.txt", &mounted_info) != K_OK ||
        mounted_info.type != OS_FILE_TYPE_REGULAR || mounted_info.size != 5U ||
        vfs_enumerate_kernel("/", 0U, &mounted_info) != K_OK ||
        mounted_info.type != OS_FILE_TYPE_REGULAR) {
        (void)vfs_unmount_fat32();
        return 0;
    }
    kernel_serial_write("OS_FAT32_PERSIST_BEGIN\r\n");
    if (vfs_open_kernel("/sample.txt", VFS_OPEN_READ, 0U, &mounted_file) != K_OK ||
        mounted_file == 0 ||
        ((vfs_backend_node_t *)mounted_file->vnode->fs_private)->fat_file == 0) {
        kernel_serial_write("OS_FAT32_PERSIST_FAIL\r\n");
        if (mounted_file != 0) vfs_close(mounted_file);
        (void)vfs_unmount_fat32();
        return 0;
    }
    vfs_close(mounted_file);
    mounted_file = 0;
    kernel_serial_write("OS_FAT32_PERSIST_OK\r\n");
    if (!filesystem_vfs_fat_delete_self_test() ||
        !filesystem_vfs_fat_rename_limit_self_test()) {
        (void)vfs_unmount_fat32();
        return 0;
    }
    if (vfs_unmount_fat32() != K_OK) return 0;
    return 1;
}

typedef struct vm_fault_lock_test_context {
    vm_space_t *space;
    page_t *page;
    BOOLEAN callback_called;
    BOOLEAN observed_unlocked;
} vm_fault_lock_test_context_t;

static void vm_fault_lock_test_retain(void *mapping) {
    (void)mapping;
}

static void vm_fault_lock_test_release(void *mapping) {
    (void)mapping;
}

static kstatus_t vm_fault_lock_test_page_get(void *mapping,
                                             uint64_t page_index,
                                             page_t **out) {
    vm_fault_lock_test_context_t *context =
        (vm_fault_lock_test_context_t *)mapping;
    if (context == 0 || out == 0 || page_index != 0U) return K_EINVAL;
    context->callback_called = 1;
    context->observed_unlocked =
        atomic_load_explicit(&context->space->map_lock.state,
                             memory_order_acquire) == 0U;
    if (!context->observed_unlocked) return K_EBUSY;
    if (context->page == 0) {
        context->page = page_alloc(0U, PAGE_ALLOC_ZERO);
        if (context->page == 0) return K_ENOMEM;
    }
    *out = context->page;
    return K_OK;
}

static const vm_file_ops_t g_vm_fault_lock_test_file_ops = {
    .retain = vm_fault_lock_test_retain,
    .release = vm_fault_lock_test_release,
    .page_get = vm_fault_lock_test_page_get,
};

static BOOLEAN filesystem_vfs_lock_preemption_self_test(void) {
    vnode_t vnode = {0};
    vfs_backend_node_t backend = {0};
    if (sched_preempt_disabled()) return 0;
    io_mutex_init(&vnode.lock, true);
    io_mutex_init(&backend.io_lock, true);

    vfs_vnode_lock(&vnode);
    BOOLEAN vnode_preemptible = !sched_preempt_disabled();
    vfs_vnode_unlock(&vnode);
    if (!vnode_preemptible || sched_preempt_disabled()) return 0;

    vfs_backend_lock(&backend);
    BOOLEAN backend_preemptible = !sched_preempt_disabled();
    vfs_backend_unlock(&backend);
    return backend_preemptible && !sched_preempt_disabled();
}

/* 持有地址空间锁时绝不能调用文件后备缺页回调。 */
static BOOLEAN filesystem_vm_fault_lock_self_test(void) {
    vm_fault_lock_test_context_t context = {0};
    vm_object_t *object = 0;
    vm_space_t *space = 0;
    vaddr_t address = 0x0000000040000000ULL;
    BOOLEAN success = 0;

    if (vm_space_create(&space) != K_OK) goto cleanup;
    context.space = space;
    if (vm_object_create_file(&context, &g_vm_fault_lock_test_file_ops,
                              PAGE_SIZE, 0U, PAGE_SIZE, &object) != K_OK ||
        vm_map_object(space, object, &address, 0U, PAGE_SIZE,
                      VM_PROT_READ | VM_PROT_USER,
                      VM_MAP_PRIVATE | VM_MAP_FIXED) != K_OK ||
        vm_handle_fault(space,
                        &(vm_fault_info_t){address, VM_PROT_READ, 0U}) != K_OK ||
        !context.callback_called || !context.observed_unlocked) {
        goto cleanup;
    }
    success = 1;

cleanup:
    if (space != 0) vm_space_put(space);
    if (object != 0) vm_object_put(object);
    if (context.page != 0) page_free(context.page);
    return success;
}

/* 验证同一个 vnode 的文件页在多个地址空间之间共享统一页缓存。 */
BOOLEAN filesystem_file_mapping_self_test(void) {
    /*
    * 启动映像预置了此 8.3 文件。QEMU 10 的 vvfat 后端会在来宾创建/删除目录项时触发断言，
    * 因此测试复用已有 inode，仅测试 vnode/页缓存映射。
     */
    static const CHAR8 test_path[] = "/EFI/OS/vfsmap.tst";
    static const UINT8 initial[] = {'w', 'o', 'r', 'l', 'd'};
    file_t *file = 0;
    file_t *write_file = 0;
    vm_object_t *object = 0;
    vm_space_t *first = 0;
    vm_space_t *second = 0;
    vm_space_t *shared = 0;
    vm_space_t *private_space = 0;
    vm_space_t *private_child = 0;
    vaddr_t first_address = 0x0000000050000000ULL;
    vaddr_t second_address = 0x0000000060000000ULL;
    vaddr_t shared_address = 0x0000000070000000ULL;
    vaddr_t private_address = 0x0000000080000000ULL;
    paddr_t first_physical = paddr_make(0);
    paddr_t second_physical = paddr_make(0);
    paddr_t shared_physical = paddr_make(0);
    paddr_t private_physical = paddr_make(0);
    paddr_t private_child_physical = paddr_make(0);
    UINT32 failure_stage = 0;
    UINT64 initial_written = 0;
    BOOLEAN success = 0;
    if (!filesystem_vfs_lock_preemption_self_test()) {
        failure_stage = 20;
        goto cleanup;
    }
    if (!filesystem_vm_fault_lock_self_test()) {
        failure_stage = 19;
        goto cleanup;
    }
    if (vfs_open_kernel(test_path,
                        VFS_OPEN_READ | VFS_OPEN_WRITE, 0U, &file) != K_OK ||
        file == 0) {
        failure_stage = 101;
        goto cleanup;
    }
    kstatus_t write_status = vfs_write_kernel(file, initial, sizeof(initial),
                                              &initial_written);
    if (write_status != K_OK || initial_written != sizeof(initial)) {
        failure_stage = 103;
        goto cleanup;
    }
    file->position = 0;
    if (vm_object_create_file(file->vnode, vfs_vm_file_ops(),
                              file->vnode->size, 0, PAGE_SIZE, &object) != K_OK) {
        failure_stage = 2;
        goto cleanup;
    }
    if (vm_space_create(&first) != K_OK || vm_space_create(&second) != K_OK ||
        vm_space_create(&shared) != K_OK || vm_space_create(&private_space) != K_OK) {
        failure_stage = 3;
        goto cleanup;
    }
    if (vm_map_object(first, object, &first_address, 0, PAGE_SIZE,
                      VM_PROT_READ | VM_PROT_USER,
                      VM_MAP_PRIVATE | VM_MAP_FIXED) != K_OK ||
        vm_map_object(second, object, &second_address, 0, PAGE_SIZE,
                      VM_PROT_READ | VM_PROT_USER,
                      VM_MAP_PRIVATE | VM_MAP_FIXED) != K_OK ||
        vm_map_object(shared, object, &shared_address, 0, PAGE_SIZE,
                      VM_PROT_READ | VM_PROT_WRITE | VM_PROT_USER,
                      VM_MAP_SHARED | VM_MAP_FIXED) != K_OK) {
        failure_stage = 4;
        goto cleanup;
    }
    if (vm_handle_fault(first, &(vm_fault_info_t){first_address, VM_PROT_READ, 0}) != K_OK ||
        vm_handle_fault(second, &(vm_fault_info_t){second_address, VM_PROT_READ, 0}) != K_OK ||
        vm_handle_fault(shared, &(vm_fault_info_t){shared_address, VM_PROT_WRITE, 0}) != K_OK) {
        failure_stage = 5;
        goto cleanup;
    }
    if (x86_translate_page(first->root_table, first_address, &first_physical, 0) != K_OK ||
        x86_translate_page(second->root_table, second_address, &second_physical, 0) != K_OK ||
        x86_translate_page(shared->root_table, shared_address, &shared_physical, 0) != K_OK) {
        failure_stage = 6;
        goto cleanup;
    }
    if (first_physical.value != second_physical.value ||
        first_physical.value != shared_physical.value) {
        failure_stage = 7;
        goto cleanup;
    }
    UINT8 *bytes = (UINT8 *)phys_to_direct(first_physical);
    success = bytes != 0 && bytes[0] == 'w' && bytes[1] == 'o' &&
              bytes[2] == 'r' && bytes[3] == 'l' && bytes[4] == 'd';
    if (!success) failure_stage = 8;
    if (success &&
        (vm_map_object(private_space, object, &private_address, 0, PAGE_SIZE,
                       VM_PROT_READ | VM_PROT_WRITE | VM_PROT_USER,
                       VM_MAP_PRIVATE | VM_MAP_FIXED) != K_OK ||
         vm_handle_fault(private_space,
                         &(vm_fault_info_t){private_address, VM_PROT_READ, 0}) != K_OK ||
         vm_handle_fault(private_space,
                         &(vm_fault_info_t){private_address, VM_PROT_WRITE, 0}) != K_OK ||
         x86_translate_page(private_space->root_table, private_address,
                            &private_physical, 0) != K_OK)) {
        failure_stage = 14;
        success = false;
    }
    if (success) {
        UINT8 *private_bytes = (UINT8 *)phys_to_direct(private_physical);
        if (private_physical.value == shared_physical.value || private_bytes == 0 ||
            private_bytes[0] != 'w' || private_bytes[4] != 'd') {
            failure_stage = 15;
            success = false;
        } else {
            /* 私有映射写入只修改 shadow，文件页和共享映射仍保持原内容。 */
            private_bytes[0] = 'X';
            if (vm_space_clone_cow(private_space, &private_child) != K_OK ||
                vm_handle_fault(private_child,
                                &(vm_fault_info_t){private_address, VM_PROT_READ, 0}) != K_OK ||
                x86_translate_page(private_child->root_table, private_address,
                                   &private_child_physical, 0) != K_OK ||
                private_child_physical.value != private_physical.value ||
                vm_handle_fault(private_child,
                                &(vm_fault_info_t){private_address, VM_PROT_WRITE, 3U}) != K_OK ||
                x86_translate_page(private_child->root_table, private_address,
                                   &private_child_physical, 0) != K_OK ||
                private_child_physical.value == private_physical.value) {
                failure_stage = 16;
                success = false;
            } else {
                UINT8 *child_bytes = (UINT8 *)phys_to_direct(private_child_physical);
                if (child_bytes == 0 || child_bytes[0] != 'X') {
                    failure_stage = 17;
                    success = false;
                } else {
                    child_bytes[0] = 'Y';
                }
            }
            if (success && (private_bytes[0] != 'X' || bytes[0] != 'w')) {
                failure_stage = 18;
                success = false;
            }
        }
    }
    if (success) {
        static const UINT8 upper[] = {'W', 'O', 'R', 'L', 'D'};
        static const UINT8 lower[] = {'w', 'o', 'r', 'l', 'd'};
        UINT64 written = 0;
        if (vfs_open_kernel(test_path,
                            VFS_OPEN_READ | VFS_OPEN_WRITE,
                            0U, &write_file) != K_OK) {
            failure_stage = 8;
            success = false;
        } else if (vfs_write_kernel(write_file, upper, sizeof(upper), &written) != K_OK ||
                   written != sizeof(upper) || bytes[0] != 'W' || bytes[4] != 'D') {
            failure_stage = 9;
            success = false;
        } else {
            write_file->position = 0;
            if (vfs_write_kernel(write_file, lower, sizeof(lower), &written) != K_OK ||
                written != sizeof(lower)) {
                failure_stage = 10;
                success = false;
            } else {
                bytes[0] = 'm';
                bytes[1] = 'm';
                bytes[2] = 'a';
                bytes[3] = 'p';
                bytes[4] = '!';
                /* 模拟页回收器从硬件 PTE 脏位发现该共享映射已被修改。 */
                vfs_file_page_mark_dirty(write_file->vnode, 0);
                kstatus_t sync_status = vfs_fsync(write_file);
                if (sync_status != K_OK) {
                    failure_stage = 11;
                    kernel_serial_write("OS_VFS_FILEMAP_DEBUG_STATUS=");
                    kernel_serial_write_u32((UINT32)(-sync_status));
                    kernel_serial_write("\r\n");
                    success = false;
                } else {
                    write_file->position = 0;
                    if (vfs_write_kernel(write_file, lower, sizeof(lower), &written) != K_OK ||
                        written != sizeof(lower) || vfs_fsync(write_file) != K_OK) {
                        failure_stage = 12;
                        success = false;
                    }
                }
            }
        }
    }

cleanup:
    if (!success && failure_stage != 0) {
        kernel_serial_write("OS_VFS_FILEMAP_DEBUG_STAGE=");
        kernel_serial_write_u32(failure_stage);
        kernel_serial_write("\r\n");
    }
    if (write_file != 0) vfs_close(write_file);
    if (private_child != 0) vm_space_put(private_child);
    if (private_space != 0) vm_space_put(private_space);
    if (shared != 0) vm_space_put(shared);
    if (second != 0) vm_space_put(second);
    if (first != 0) vm_space_put(first);
    if (object != 0) vm_object_put(object);
    if (file != 0) vfs_close(file);
    return success;
}
