"""Power-control writeback must release namespace locks before I/O."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'kernel/fs/vfs/core.c').read_text()
start = source.index('kstatus_t vfs_sync_all(void)')
end = source.index('kstatus_t vfs_unmount_fat32', start)
code = r'''
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
typedef int kstatus_t;
#define K_OK 0
#define K_EIO -5
#define VFS_FAT32_MOUNT_LIMIT 3
typedef struct {unsigned refs;bool locked;int error;} vnode_t;
typedef struct {unsigned syncs;bool fail;} OS_FAT32;
static vnode_t nodes[2]={{1,false,0},{1,false,0}};
static vnode_t *g_vfs_nodes[]={&nodes[0],0,&nodes[1]};
static unsigned g_vfs_node_capacity=3;
static struct {bool mounted;OS_FAT32 *filesystem;} g_fat32_mounts[3];
static bool locked;
static unsigned writes;
static void vfs_initialize(void) {}
static void vfs_lock(void) {assert(!locked);locked=true;}
static void vfs_unlock(void) {assert(locked);locked=false;}
static void object_get(vnode_t *v) {assert(locked);++v->refs;}
static void object_put(vnode_t *v) {assert(!locked && !v->locked);--v->refs;}
static int vfs_page_cache_writeback(vnode_t *v) {
    assert(!locked && !v->locked && v->refs==2);++writes;return v->error;
}
static bool fat32_sync(OS_FAT32 *fs) {
    assert(!locked && writes%2==0);++fs->syncs;return !fs->fail;
}
''' + source[start:end] + r'''
int main(void) {
    OS_FAT32 fs[2]={{0,false},{0,false}};
    g_fat32_mounts[0].mounted=true;g_fat32_mounts[0].filesystem=&fs[0];
    g_fat32_mounts[1].filesystem=&fs[1]; /* Detached: not flushed. */
    assert(vfs_sync_all()==K_OK && fs[0].syncs==1 && !fs[1].syncs);
    nodes[0].error=-12;
    assert(vfs_sync_all()==-12 && fs[0].syncs==2);
    fs[0].fail=true;assert(vfs_sync_all()==K_EIO && fs[0].syncs==3);
    assert(nodes[0].refs==1 && nodes[1].refs==1 && writes==6);
}
'''
with tempfile.TemporaryDirectory(prefix='vfs-sync-all-') as directory:
    binary = str(Path(directory) / 'test')
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                    '-x', 'c', '-', '-o', binary], input=code, text=True, check=True)
    subprocess.run([binary], check=True)
print('VFS global sync: file-before-volume ordering, refs/locks, detached mounts and failures PASS')
