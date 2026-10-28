"""Production backend registration versus repeated bounded path scans."""
from pathlib import Path
import subprocess
import tempfile
root=Path(__file__).resolve().parents[2]
source=(root/'kernel/fs/vfs/backend.c').read_text()
code=source[source.index('kstatus_t vfs_register_backend_file_ex('):source.index('kstatus_t vfs_register_backend_file(',source.index('kstatus_t vfs_register_backend_file_ex('))]
old=code.replace('vfs_register_backend_file_ex(', 'previous_register(')
old=old.replace("if (path == 0 || path[0] != '/' || read == 0 ||", "if (path == 0 || path[0] != '/' || vfs_string_length(path) == 0 ||\n        vfs_string_length(path) >= VFS_PATH_LIMIT || read == 0 ||")
old=old.replace('    size_t path_length = vfs_string_length(path);\n    if (path_length >= VFS_PATH_LIMIT) return K_EINVAL;\n','')
old=old.replace('    memcpy(backend->path, path, path_length + 1U);',
                '    size_t path_length = vfs_string_length(path);\n    for(size_t i=0;i<=path_length;++i)backend->path[i]=path[i];')
internal=(root/'kernel/fs/vfs/internal.h').read_text()
layout=internal[internal.index('typedef struct vfs_backend_node {'):internal.index('typedef struct vfs_cached_page {')]
program=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdatomic.h>
#include <string.h>
#include <stdio.h>
#define VFS_PATH_LIMIT 256U
#define K_EINVAL -22
#define K_ENOMEM -12
typedef int kstatus_t;
typedef struct {atomic_uint state;} spinlock_t;
typedef int kmutex_t;
typedef int io_mutex_t;
typedef struct vnode vnode_t;
static void io_mutex_init(io_mutex_t *m,bool pi){assert(pi);*m=0;}
typedef struct fat_file OS_FAT32_FILE;
typedef int (*vfs_backend_read_t)(void);
typedef vfs_backend_read_t vfs_backend_write_t;
typedef vfs_backend_read_t vfs_backend_fsync_t;
typedef vfs_backend_read_t vfs_backend_truncate_t;
''' + layout + r'''
static struct {uint64_t before;vfs_backend_node_t node;uint64_t after;} storage;
static unsigned scans,allocations,registrations;static bool fail_alloc;static int register_status;
static size_t vfs_string_length(const char *s){
 ++scans;size_t n=0;while(n<VFS_PATH_LIMIT&&s[n])++n;return n;
}
static void *kzalloc(size_t n,unsigned flags){
 assert(n==sizeof(storage.node)&&!flags);++allocations;
 return fail_alloc?0:&storage.node;
}
static int vfs_register_node(vfs_backend_node_t *n){assert(n==&storage.node);++registrations;return register_status;}
static int callback(void){return 0;}
''' + old + code + r'''
static void reset(void){
 memset(&storage,0,sizeof(storage));storage.before=17;storage.after=29;
 scans=allocations=registrations=0;
}
int main(void){
 char path[257];unsigned cases=0,reduced=0;
 for(unsigned length=0;length<=256;++length)
 for(unsigned absolute=0;absolute<2;++absolute)
 for(unsigned readable=0;readable<2;++readable)
 for(unsigned regular=0;regular<2;++regular)
 for(unsigned oom=0;oom<2;++oom)for(unsigned reject=0;reject<2;++reject){
  memset(path,'x',sizeof(path));path[length]=0;if(length&&absolute)path[0]='/';
  uint32_t mode=regular?0100644:0040755;fail_alloc=oom;register_status=reject?-17:0;
  reset();int before=previous_register(path,UINT64_MAX,mode,readable?callback:0,
                                      callback,callback,callback,path);
  vfs_backend_node_t expected=storage.node;
  unsigned old_scans=scans,old_alloc=allocations,old_regs=registrations;
  reset();int after=vfs_register_backend_file_ex(path,UINT64_MAX,mode,readable?callback:0,
                                                callback,callback,callback,path);
  assert(before==after&&!memcmp(&expected,&storage.node,sizeof(expected)));
  assert(allocations==old_alloc&&registrations==old_regs&&scans<=1);
  assert(storage.before==17&&storage.after==29);
  if(registrations){assert(old_scans==3&&scans==1);++reduced;}
  ++cases;
 }
 reset();assert(vfs_register_backend_file_ex(0,0,0100644,callback,0,0,0,0)==K_EINVAL);
 assert(!scans&&!allocations&&!registrations);
 assert(cases==8224&&reduced==510);
 printf("Backend registration: %u exact old/new layout/callback/error cases; %u valid paths scan once instead of three times; allocation isolation and canaries preserved\n",cases,reduced);
}
'''
with tempfile.TemporaryDirectory(prefix='liteos-backend-register-') as directory:
    binary=str(Path(directory)/'test')
    subprocess.run(['cc','-std=c11','-O2','-Wall','-Wextra','-Werror',
                    '-fsanitize=address,undefined','-fno-pie','-no-pie','-x','c','-','-o',binary],
                   input=program,text=True,check=True)
    subprocess.run([binary],check=True)
