"""执行实际 FAT 缓存替换事务，覆盖身份、引用和每条失败回滚路径。"""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'kernel/fs/vfs/core.c').read_text()
start = source.index('static kstatus_t vfs_rename_fat32_cached(')
helper = source[start:source.index('\nkstatus_t vfs_rename_kernel(', start)]
program = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#define VFS_PATH_LIMIT 256U
typedef int kstatus_t;
enum {K_OK,K_EIO,K_EBUSY,K_ENOMEM,K_ENAMETOOLONG};
typedef struct {
 bool directory,detached,unlink_prepared;uint64_t size,snapshot_size,content_revision,snapshot_revision;
 void *fat_file;uint32_t namespace_slot;
} vfs_backend_node_t;
typedef struct {void *fs_private;unsigned refs;} vnode_t;
typedef struct {void *filesystem;} vfs_fat32_mount_t;
static vfs_backend_node_t backend;
static vnode_t source_node,target;
static vnode_t *g_vfs_nodes[4];static uint32_t g_vfs_node_capacity=4;
static unsigned locked,backend_locked,prepares,closes,renames,commits,aborts,publications,drops;
static bool target_present=true,same_inode,fail_close,fail_rename,concurrent_write;
static int prepare_status,rekey_status;
static void vfs_initialize(void){}
static void vfs_lock(void){assert(!locked);locked=1;}
static void vfs_unlock(void){assert(locked);locked=0;}
static void object_get(vnode_t *v){assert(locked&&v==&target);++v->refs;}
static void object_put(vnode_t *v){assert(!locked&&!backend_locked&&v==&target&&v->refs);--v->refs;++drops;}
static vnode_t *vfs_find_fat_node_locked(const char *p){
 assert(locked);return !strcmp(p,"/temp")?&source_node:target_present?(same_inode?&source_node:&target):0;
}
static kstatus_t vfs_rekey_cached_subtree_locked(const char *a,size_t b,const char *c,size_t d,bool publish){
 assert(locked&&!strcmp(a,"/temp")&&b==5&&!strcmp(c,"/sample.txt")&&d==11);
 if(publish){++publications;assert(!target_present||same_inode||backend.detached);return K_OK;}
 return rekey_status;
}
static void vfs_backend_lock(vfs_backend_node_t *b){assert(b==&backend&&!locked&&!backend_locked);backend_locked=1;}
static void vfs_backend_unlock(vfs_backend_node_t *b){assert(b==&backend&&backend_locked);backend_locked=0;}
static kstatus_t vfs_fat32_backend_prepare_unlink(vfs_backend_node_t *b){
 assert(b==&backend&&!locked&&!backend_locked);++prepares;
 if(prepare_status)return prepare_status;
 b->unlink_prepared=true;b->snapshot_size=b->size;b->snapshot_revision=b->content_revision;
 if(concurrent_write)++b->content_revision;
 return K_OK;
}
static bool fat32_close(void *file){assert(backend_locked&&file==(void*)1);++closes;return !fail_close;}
static bool fat32_rename_path(void *fs,const char *a,const char *b){
 assert(fs==(void*)2&&!strcmp(a,"temp")&&!strcmp(b,"sample.txt")&&!locked);++renames;
 if(target_present&&!same_inode)assert(backend_locked&&!backend.fat_file);
 return !fail_rename;
}
static void vfs_fat32_backend_commit_unlink_locked(vfs_backend_node_t *b){
 assert(b==&backend&&backend_locked&&!locked);++commits;b->detached=true;b->unlink_prepared=false;
}
static void vfs_fat32_backend_abort_unlink(vfs_backend_node_t *b){
 assert(b==&backend&&!locked&&!backend_locked);++aborts;b->unlink_prepared=false;
}
static void vfs_set_node_slot_locked(uint32_t slot,vnode_t *v){
 assert(locked&&slot==0&&!v&&g_vfs_nodes[0]==&target&&backend.detached);g_vfs_nodes[0]=v;
}
''' + helper + r'''
static void reset(void){
 assert(!locked&&!backend_locked);
 backend=(vfs_backend_node_t){.size=8,.fat_file=(void*)1};
 target=(vnode_t){.fs_private=&backend,.refs=2};g_vfs_nodes[0]=&target;
 prepares=closes=renames=commits=aborts=publications=drops=0;
 same_inode=fail_close=fail_rename=concurrent_write=false;target_present=true;
 prepare_status=rekey_status=0;
}
static int run(void){
 vfs_fat32_mount_t mount={(void*)2};
 return vfs_rename_fat32_cached("/temp",5,"/sample.txt",11,&mount,"temp","sample.txt");
}
int main(void){
 reset();assert(run()==K_OK);
 assert(prepares==1&&closes==1&&renames==1&&commits==1&&publications==1&&!aborts);
 assert(backend.detached&&!g_vfs_nodes[0]&&target.refs==1&&drops==2);
 reset();prepare_status=K_ENOMEM;assert(run()==K_ENOMEM);
 assert(prepares==1&&!closes&&!renames&&!aborts&&target.refs==2&&g_vfs_nodes[0]==&target);
 reset();concurrent_write=true;assert(run()==K_EBUSY);
 assert(aborts==1&&!closes&&!renames&&!backend.detached&&target.refs==2);
 reset();fail_close=true;assert(run()==K_EIO);
 assert(aborts==1&&backend.fat_file&&!renames&&!backend.detached&&target.refs==2);
 reset();fail_rename=true;assert(run()==K_EIO);
 assert(aborts==1&&!backend.fat_file&&!backend.detached&&g_vfs_nodes[0]==&target&&target.refs==2);
 reset();rekey_status=K_ENAMETOOLONG;assert(run()==K_ENAMETOOLONG);
 assert(!prepares&&!renames&&!drops&&target.refs==2);
 reset();same_inode=true;assert(run()==K_OK);
 assert(!prepares&&!closes&&!drops&&!backend.detached&&renames==1&&publications==1);
 reset();target_present=false;assert(run()==K_OK);
 assert(!prepares&&!closes&&!drops&&renames==1&&publications==1);
 puts("PASS: FAT replacement retirement before rename, old-open reference, same-inode aliases, uncached target, dry-run/OOM/revision/close/rename rollback");
}
'''
with tempfile.TemporaryDirectory(prefix='vfs-fat-replace-') as directory:
    binary = str(Path(directory) / 'test')
    subprocess.run(['cc', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
                    '-x', 'c', '-', '-o', binary], input=program, text=True, check=True)
    subprocess.run([binary], check=True)
