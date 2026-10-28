"""Execute production symlink publication, rollback and readlink semantics."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root/'kernel/fs/vfs/core.c').read_text()
begin = source.index('kstatus_t vfs_symlink_kernel(')
end = source.index('kstatus_t vfs_resolve_path_kernel(', begin)
publish = source[source.index('static kstatus_t vfs_publish_memory_node('):
                 source.index('static kstatus_t vfs_create_memory_node(')]
program = r'''
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <kernel/base.h>
#define VFS_PATH_LIMIT 256
typedef struct {atomic_uint state;} io_mutex_t;
static void io_mutex_init(io_mutex_t *mutex, bool pi) {
    assert(pi); atomic_init(&mutex->state, 0U);
}
typedef struct {
    char path[256]; unsigned char *data; uint64_t size, data_capacity;
    uint32_t mode; bool mutable_node; io_mutex_t io_lock;
} vfs_backend_node_t;
typedef struct {void *fs_private;} vnode_t;
typedef int vfs_fat32_mount_t;
typedef struct {unsigned type;} os_file_info_t;
static vnode_t *node;
static int locked, allocations, live, fail_at, puts, stats;
static bool parent = true, mounted, full, race;
static vnode_t collision;
static void *kzalloc(size_t size, unsigned flags) {
    assert(!locked && !flags);
    if (++allocations == fail_at) return 0;
    void *result = calloc(1, size); assert(result); ++live; return result;
}
static void kfree(void *memory) { assert(!locked); if(memory) {--live; free(memory);} }
static size_t vfs_string_length(const char *text) {return text?strnlen(text,256):0;}
static vfs_fat32_mount_t *vfs_fat32_mount_for_path(const char *path,char *relative) {
    (void)path; (void)relative; static int mount; return mounted?&mount:0;
}
static vnode_t *vfs_create_vnode_object(vfs_backend_node_t *backend) {
    vnode_t *vnode=kzalloc(sizeof(*vnode),0);
    if(vnode) vnode->fs_private=backend;
    return vnode;
}
static void object_put(vnode_t *vnode) {
    assert(!locked && vnode && vnode!=&collision); ++puts;
    vfs_backend_node_t *backend=vnode->fs_private;
    kfree(backend->data); kfree(backend); kfree(vnode);
}
static void vfs_initialize(void) {}
static void vfs_lock(void) {assert(!locked);locked=1;if(race){node=&collision;race=false;}}
static void vfs_unlock(void) {assert(locked);locked=0;}
static vnode_t *vfs_find_node_locked(const char *path) {(void)path;assert(locked);return node;}
static bool vfs_parent_directory_exists_locked(const char *path,size_t size) {
    assert(locked && path && size); return parent;
}
static uint32_t vfs_free_node_slot_locked(void) {assert(locked);return full?UINT32_MAX:0;}
static void vfs_set_node_slot_locked(uint32_t slot,vnode_t *vnode) {
    assert(locked && !slot && !node); node=vnode;
}
static kstatus_t vfs_stat_kernel(const char *path,os_file_info_t *info) {
    assert(!locked); (void)info; ++stats;
    return !strcmp(path,"/tmp/directory")?K_OK:K_ENOENT;
}
''' + publish + source[begin:end] + r'''
static void clear(void) {
    if(node && node!=&collision) object_put(node);
    node=0;assert(!live && !locked);
    allocations=fail_at=puts=stats=0;parent=true;mounted=full=race=false;
}
int main(void) {
    char out[32];size_t bytes=99;
    assert(vfs_symlink_kernel(0,"/tmp/link")==K_EINVAL);
    assert(vfs_symlink_kernel("target",0)==K_EINVAL);
    assert(vfs_symlink_kernel("","/tmp/link")==K_ENOENT);
    assert(vfs_symlink_kernel("target","relative")==K_EINVAL);
    char long_target[300];memset(long_target,'x',299);long_target[299]=0;
    assert(vfs_symlink_kernel(long_target,"/tmp/link")==K_ENAMETOOLONG);
    assert(!allocations);
    mounted=true;assert(vfs_symlink_kernel("target","/fat/link")==K_EPERM);clear();
    for(int failure=1;failure<=3;++failure) {
        fail_at=failure;
        assert(vfs_symlink_kernel("target","/tmp/link")==K_ENOMEM);
        assert(!node && !live && !locked);clear();
    }
    parent=false;assert(vfs_symlink_kernel("target","/tmp/link")==K_ENOENT);
    assert(puts==1 && !live);clear();
    full=true;assert(vfs_symlink_kernel("target","/tmp/link")==K_ENOMEM);
    assert(puts==1 && !live);clear();
    race=true;assert(vfs_symlink_kernel("target","/tmp/link")==K_EEXIST);
    assert(node==&collision && puts==1 && !live);clear();
    const char *target="localhost-199";
    assert(vfs_symlink_kernel(target,"/tmp/link")==K_OK && live==3 && !puts);
    vfs_backend_node_t *backend=node->fs_private;
    assert(backend->mode==0120777 && backend->size==strlen(target));
    assert(!strcmp((char*)backend->data,target));
    assert(vfs_symlink_kernel("replacement","/tmp/link")==K_EEXIST);
    assert(live==3 && !strcmp((char*)backend->data,target));
    memset(out,'X',sizeof(out));
    assert(vfs_readlink_kernel("/tmp/link",out,3,&bytes)==K_OK && bytes==3);
    assert(!memcmp(out,"loc",3) && out[3]=='X');
    assert(vfs_readlink_kernel("/tmp/link",out,sizeof(out),&bytes)==K_OK);
    assert(bytes==strlen(target) && !memcmp(out,target,bytes) && out[bytes]=='X');
    assert(vfs_readlink_kernel("/tmp/link",out,0,&bytes)==K_EINVAL);
    assert(vfs_readlink_kernel("/tmp/link",0,3,&bytes)==K_EINVAL);
    assert(vfs_readlink_kernel("/tmp/link",out,3,0)==K_EINVAL);
    backend->mode=0100600;
    assert(vfs_readlink_kernel("/tmp/link",out,3,&bytes)==K_EINVAL && !bytes);
    clear();
    assert(vfs_readlink_kernel("/tmp/directory",out,3,&bytes)==K_EINVAL);
    assert(vfs_readlink_kernel("/tmp/absent",out,3,&bytes)==K_ENOENT);
    assert(stats==2 && !live && !locked);
}
'''
with tempfile.TemporaryDirectory(prefix='vfs-symlink-') as directory:
    base = Path(directory)
    (base/'test.c').write_text(program)
    subprocess.run(['cc', '-D_GNU_SOURCE', '-std=c11', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
                    '-I'+str(root/'include'), str(base/'test.c'), '-o', str(base/'test')],
                   check=True)
    subprocess.run([str(base/'test')], check=True)
print('VFS symlink: atomic publication, racing duplicate, immutable target, '
      'allocation/parent/namespace rollback, FAT rejection and bounded readlink passed')
