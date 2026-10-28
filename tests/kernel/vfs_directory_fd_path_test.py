"""Execute the real pinned directory path snapshot and stale inode checks."""
from pathlib import Path
import subprocess
import tempfile
root=Path(__file__).resolve().parents[2]
source=(root/'kernel/fs/vfs/core.c').read_text()
begin=source.index('kstatus_t vfs_directory_path_kernel(')
helper=source[begin:source.index('static bool vfs_path_is_subtree(',begin)]
code=r'''
#include <assert.h>
#include <string.h>
#include <kernel/base.h>
#define KOBJECT_TYPE_FILE 7
#define KOBJECT_TYPE_VNODE 8
typedef struct {char path[256];bool fat32_node,directory,detached;} vfs_backend_node_t;
typedef struct {struct {int type;} object;void *fs_private;} vnode_t;
typedef struct {struct {int type;} object;vnode_t *vnode;} file_t;
static vnode_t *current;
static bool locked;
static unsigned locks;
static void vfs_initialize(void){}
static void vfs_lock(void){assert(!locked);locked=true;++locks;}
static void vfs_unlock(void){assert(locked);locked=false;}
static vnode_t *vfs_find_node_locked(const char *p){assert(locked && p);return current;}
static vnode_t *vfs_find_fat_node_locked(const char *p){return vfs_find_node_locked(p);}
static size_t vfs_string_length(const char *p){assert(locked);return strlen(p);}
''' + helper + r'''
int main(void){
 vfs_backend_node_t b={.path="/tmp/new-name",.directory=true};
 vnode_t v={{8},&b}, replacement={{8},&b};file_t f={{7},&v};current=&v;
 char out[32];
 for(int fat=0;fat<2;++fat){
  b.fat32_node=fat;memset(out,'X',sizeof(out));
  assert(vfs_directory_path_kernel(&f,out,sizeof(out))==K_OK && !strcmp(out,b.path) && !locked);
  current=&replacement;assert(vfs_directory_path_kernel(&f,out,sizeof(out))==K_ENOENT && !locked);
  current=&v;b.detached=true;assert(vfs_directory_path_kernel(&f,out,sizeof(out))==K_ENOENT && !locked);
  b.detached=false;b.directory=false;
  assert(vfs_directory_path_kernel(&f,out,sizeof(out))==K_ENOTDIR && !locked);
  b.directory=true;memset(out,'X',sizeof(out));
  assert(vfs_directory_path_kernel(&f,out,4)==K_ENAMETOOLONG && out[0]=='X' && !locked);
 }
 unsigned before=locks;
 assert(vfs_directory_path_kernel(0,out,32)==K_EBADF && locks==before);
 assert(vfs_directory_path_kernel(&f,0,32)==K_EINVAL && locks==before);
 f.object.type=0;assert(vfs_directory_path_kernel(&f,out,32)==K_EBADF && locks==before);
 bool deleted=false;
 assert(vfs_vnode_path_kernel(&v,out,sizeof(out),&deleted)==K_OK && !deleted);
 b.detached=true;
 assert(vfs_vnode_path_kernel(&v,out,sizeof(out),&deleted)==K_OK && deleted);
 v.object.type=0;
 assert(vfs_vnode_path_kernel(&v,out,sizeof(out),&deleted)==K_EBADF);
}
'''
with tempfile.TemporaryDirectory() as directory:
    path=Path(directory)/'path.c';path.write_text(code)
    binary=Path(directory)/'path'
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-I'+str(root/'include'),str(path),'-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True)
print('VFS directory-fd path: current inode identity, renamed path, detached/replaced/non-directory rejection and lock/error balance passed')
