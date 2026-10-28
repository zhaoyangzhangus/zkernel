"""执行真实 stat、卷路由及 FAT 查找：缓存命中不得读取旧磁盘长度。"""
from pathlib import Path
import re
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'kernel/fs/vfs/core.c').read_text()
limit = re.search(r'#define VFS_FAT32_MOUNT_LIMIT (\d+)U',
                  (root / 'kernel/fs/vfs/internal.h').read_text()).group(1)

def function(signature):
    start = source.index(signature)
    end = source.index('{', start) + 1
    depth = 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end] + '\n'

program = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <uapi/file.h>
#define VFS_PATH_LIMIT 256U
#define VFS_FAT32_MOUNT_LIMIT MOUNT_LIMIT_VALUE
typedef int kstatus_t;
enum {K_OK, K_EINVAL, K_EIO, K_ENOENT};
typedef struct {bool mounted; void *filesystem; char prefix[256];} vfs_fat32_mount_t;
typedef struct {bool fat32_node, detached, directory; unsigned mode, namespace_slot; char path[256];} vfs_backend_node_t;
typedef struct {void *fs_private; uint64_t size;} vnode_t;
static vfs_fat32_mount_t g_fat32_mounts[VFS_FAT32_MOUNT_LIMIT];
static vnode_t *g_vfs_nodes[4];
static uint32_t g_vfs_node_capacity=4,g_vfs_fat_buckets[4],g_vfs_fat_next[4];
static unsigned locked, stats;
static bool disk_exists=true, parent_exists=false, memory_directory=false;
static void vfs_initialize(void) {}
static void vfs_lock(void) {assert(!locked); locked=1;}
static void vfs_unlock(void) {assert(locked); locked=0;}
static bool fat32_stat_path(void *fs, const char *path, os_file_info_t *out) {
    assert(!locked && fs && path); ++stats;
    *out=(os_file_info_t){.type=OS_FILE_TYPE_REGULAR,.mode=0100666,.size=7};
    return disk_exists;
}
static bool vfs_mount_parent_exists(const char *path, size_t length) {
    assert(!locked && path && length); return parent_exists;
}
static bool vfs_directory_exists_locked(const char *path, size_t length) {
    assert(locked && path && length); return memory_directory;
}
static vnode_t *vfs_find_node_locked(const char *path) {
    assert(locked);
    for(unsigned i=0;i<4;++i) if(g_vfs_nodes[i]) {
        vfs_backend_node_t *backend=g_vfs_nodes[i]->fs_private;
        if(backend && !backend->fat32_node && !strcmp(path,backend->path))
            return g_vfs_nodes[i];
    }
    return 0;
}
'''.replace('MOUNT_LIMIT_VALUE', limit)
for signature in ('size_t vfs_string_length(', 'static uint32_t vfs_fat_name_bucket(',
                  'static void vfs_fat_index_insert_locked(',
                  'static void vfs_fat_index_remove_locked(',
                  'static bool vfs_path_range_equals(',
                  'static bool vfs_volatile_path(', 'static bool vfs_mount_matches(',
                  'vfs_fat32_mount_t *vfs_fat32_mount_for_path(',
                  'static bool vfs_fat_relative_path_equal(',
                  'static vnode_t *vfs_find_fat_node_locked(',
                  'static void vfs_set_leaf_name(', 'static void vfs_fill_info(',
                  'kstatus_t vfs_stat_kernel('):
    program += function(signature)
program += r'''
int main(void) {
    g_fat32_mounts[0]=(vfs_fat32_mount_t){true,(void*)1,"/"};
    g_fat32_mounts[1]=(vfs_fat32_mount_t){true,(void*)1,"/alias"};
    g_fat32_mounts[2]=(vfs_fat32_mount_t){true,(void*)2,"/other"};
    vfs_backend_node_t backend={.fat32_node=true,.mode=0100666,.path="/file"};
    vnode_t node={&backend,12345}; g_vfs_nodes[0]=&node;
    vfs_fat_index_insert_locked(&node,0);
    os_file_info_t info;
    /* 已打开文件的可见长度可以领先于磁盘目录项；普通路径和别名卷必须一致。 */
    for(unsigned i=0;i<2000;++i) {
        node.size=i+100;
        const char *path=i%2?"/alias/FILE":"/file";
        assert(vfs_stat_kernel(path,&info)==K_OK && info.size==node.size);
        assert(info.type==OS_FILE_TYPE_REGULAR && !stats && !locked);
        assert(!strcmp(info.name,i%2?"FILE":"file"));
    }
    assert(vfs_stat_kernel("/other/file",&info)==K_OK && info.size==7 && stats==1);
    backend.detached=true;
    assert(vfs_stat_kernel("/file",&info)==K_OK && info.size==7 && stats==2);
    disk_exists=false;
    assert(vfs_stat_kernel("/file",&info)==K_ENOENT && stats==3 && !locked);
    /* 重命名后旧路径不得命中，新的路径保留同一 inode 的长度。 */
    backend.detached=false; vfs_fat_index_remove_locked(&node);
    strcpy(backend.path,"/renamed");vfs_fat_index_insert_locked(&node,0);
    assert(vfs_stat_kernel("/file",&info)==K_ENOENT && stats==4);
    assert(vfs_stat_kernel("/alias/RENAMED",&info)==K_OK && info.size==node.size && stats==4);
    backend.directory=true; backend.mode=0040755;
    assert(vfs_stat_kernel("/renamed",&info)==K_OK && info.size==0 && info.type==OS_FILE_TYPE_DIRECTORY);
    unsigned before=stats;
    assert(vfs_stat_kernel("/alias",&info)==K_OK && info.type==OS_FILE_TYPE_DIRECTORY && stats==before);
    parent_exists=true;
    assert(vfs_stat_kernel("/synthetic",&info)==K_OK && info.type==OS_FILE_TYPE_DIRECTORY && stats==before+1);
    /* 内存文件、隐式目录和无效参数保持原有语义。 */
    vfs_fat_index_remove_locked(&node);
    backend.fat32_node=false; backend.directory=false; backend.mode=0100600;
    strcpy(backend.path,"/tmp/file");
    assert(vfs_stat_kernel("/tmp/file",&info)==K_OK && info.size==node.size && info.mode==0100600);
    memory_directory=true;
    assert(vfs_stat_kernel("/tmp/dir",&info)==K_OK && info.type==OS_FILE_TYPE_DIRECTORY);
    memory_directory=false;
    assert(vfs_stat_kernel("/tmp/absent",&info)==K_ENOENT);
    assert(vfs_stat_kernel(0,&info)==K_EINVAL && vfs_stat_kernel("relative",&info)==K_EINVAL);
    assert(vfs_stat_kernel("/",0)==K_EINVAL && !locked);
    puts("Cached stat: 2000 fresh-size/alias hits without disk access; volume identity, detach, rename, directory and RAM fallback passed");
}
'''
with tempfile.TemporaryDirectory(prefix='vfs-cached-stat-') as directory:
    path = Path(directory)
    for variant in ('current', 'disk_first'):
        code = program if variant == 'current' else program.replace(
            'mount != 0 ? vfs_find_fat_node_locked(path) :', 'mount != 0 ? 0 :')
        (path / 'test.c').write_text(code)
        binary = path / variant
        subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                        '-Wno-unused-function', '-fsanitize=address,undefined',
                        '-fno-pie', '-no-pie', '-I' + str(root / 'include'),
                        str(path / 'test.c'), '-o', str(binary)], check=True)
        result = subprocess.run([str(binary)], capture_output=True, text=True)
        if variant == 'current':
            assert result.returncode == 0, result.stderr
            print(result.stdout.strip())
        else:
            assert result.returncode != 0, '绕过缓存的旧路径未被检出'
            print('负对照通过：重新读取磁盘旧长度会触发回归断言')
