"""Production remove: non-FAT namespace effects versus full-length-scan oracle."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'kernel/fs/vfs/core.c').read_text()
remove = source[source.index('kstatus_t vfs_remove_typed_kernel('):source.index('kstatus_t vfs_open_kernel(')]
equals = source[source.index('static bool vfs_path_range_equals('):source.index('static uint32_t vfs_free_node_slot_locked(')]
slots = source[source.index('static void vfs_set_node_slot_locked('):source.index('static bool vfs_volatile_path(')]
exact = '''        if (backend->path[path_length] != 0 ||
            !vfs_path_range_equals(backend->path, path_length, path, path_length)) continue;'''
child = "                if (child_backend->path[path_length] == '/' &&"
assert exact in remove and child in remove
old = remove.replace('vfs_remove_typed_kernel(', 'previous_remove_typed(').replace('vfs_remove_kernel(', 'previous_remove(').replace(exact, '''        size_t node_length = vfs_string_length(backend->path);
        if (!vfs_path_range_equals(backend->path, node_length, path, path_length)) continue;''').replace(child, '''                size_t child_length = vfs_string_length(child_backend->path);
                if (child_length > path_length && child_backend->path[path_length] == '/' &&''')
code = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <sanitizer/asan_interface.h>
#include <sys/wait.h>
#include <unistd.h>
#define VFS_PATH_LIMIT 256U
typedef int kstatus_t;
enum {K_OK,K_EINVAL,K_ENOENT,K_EACCES,K_ENOTEMPTY,K_EIO,K_EBUSY,K_ENOTDIR,K_EISDIR};
enum {OS_FILE_TYPE_DIRECTORY=2};
typedef struct {char path[256];bool directory,mutable_node,fat32_node,detached,unlink_prepared;uint64_t size,snapshot_size,content_revision,snapshot_revision;void *fat_file;} vfs_backend_node_t;
typedef struct {void *fs_private;unsigned refs;} vnode_t;
typedef struct {void *filesystem;} vfs_fat32_mount_t;
typedef struct {unsigned type;} os_file_info_t;
static vnode_t *g_vfs_nodes[16];
static uint32_t g_vfs_node_capacity=16,g_vfs_next_free_node;
static void vfs_namespace_insert_locked(vnode_t *v,uint32_t slot){(void)v;(void)slot;}
static void vfs_namespace_remove_locked(vnode_t *v){(void)v;}
static unsigned depth,locks,drops,gets,scans,cases,avoided;
static const char *request;
static size_t vfs_string_length(const char *p){
 if(p!=request)++scans;
 size_t n=0;if(p)while(n<256&&p[n])++n;return n;
}
static void vfs_initialize(void){}
static void vfs_lock(void){assert(!depth);++depth;++locks;}
static void vfs_unlock(void){assert(depth==1);--depth;}
static void object_get(vnode_t *v){assert(depth&&v->refs);++v->refs;++gets;}
static void object_put(vnode_t *v){assert(!depth&&v->refs);--v->refs;++drops;}
/* All requests in this fixture select the in-memory backend. FAT removal is
 * independently exercised by the existing FAT identity/deletion regressions. */
static vfs_fat32_mount_t *vfs_fat32_mount_for_path(const char *p,char *r){(void)p;(void)r;return 0;}
static bool fat32_stat_path(void *f,const char *p,os_file_info_t *o){(void)f;(void)p;(void)o;assert(0);return false;}
static vnode_t *vfs_find_fat_node_locked(const char *p){(void)p;assert(0);return 0;}
static kstatus_t vfs_fat32_backend_prepare_unlink(vfs_backend_node_t *b){(void)b;assert(0);return K_EIO;}
static void vfs_fat32_backend_abort_unlink(vfs_backend_node_t *b){(void)b;assert(0);}
static void vfs_fat32_backend_commit_unlink_locked(vfs_backend_node_t *b){(void)b;assert(0);}
static void vfs_backend_lock(vfs_backend_node_t *b){(void)b;assert(0);}
static bool fat32_close(void *f){(void)f;assert(0);return false;}
static void vfs_backend_unlock(vfs_backend_node_t *b){(void)b;assert(0);}
static bool fat32_remove_path_typed(void *f,const char *p,int d){(void)f;(void)p;(void)d;assert(0);return false;}
''' + equals + slots + old + remove + r'''
static vfs_backend_node_t b[16];static vnode_t v[16];
static void reset(void){
 memset(b,0,sizeof(b));memset(v,0,sizeof(v));
 for(unsigned i=0;i<16;++i){
  snprintf(b[i].path,256,"/unrelated/%u/long/deep/path",i);
  b[i].mutable_node=true;v[i].fs_private=&b[i];v[i].refs=2;g_vfs_nodes[i]=&v[i];
 }
 g_vfs_next_free_node=16;g_vfs_node_capacity=16;
}
static void compare(const char *path){
 vnode_t *initial[16],*expected[16];vnode_t saved[16],result[16];
 memcpy(initial,g_vfs_nodes,sizeof(initial));memcpy(saved,v,sizeof(saved));
 uint32_t hint=g_vfs_next_free_node;
 depth=locks=drops=gets=scans=0;request=path;
 kstatus_t status=previous_remove(path);
 assert(!depth);unsigned l=locks,p=drops,g=gets,s=scans;uint32_t h=g_vfs_next_free_node;
 memcpy(expected,g_vfs_nodes,sizeof(expected));memcpy(result,v,sizeof(result));
 memcpy(g_vfs_nodes,initial,sizeof(initial));memcpy(v,saved,sizeof(saved));g_vfs_next_free_node=hint;
 depth=locks=drops=gets=scans=0;
 assert(vfs_remove_kernel(path)==status&&!depth);
 assert(locks==l&&drops==p&&gets==g&&!scans&&g_vfs_next_free_node==h);
 assert(!memcmp(g_vfs_nodes,expected,sizeof(expected))&&!memcmp(v,result,sizeof(result)));
 avoided+=s;++cases;
}
int main(void){
 const char *children[]={"/target/child","/target/","/targetish/child","/other/child","/target",""};
 const char *requests[]={0,"","relative","/","///","/target","/target/","/target///","/missing"};
 for(unsigned slot=0;slot<16;++slot)for(unsigned dir=0;dir<2;++dir)
 for(unsigned writable=0;writable<2;++writable)for(unsigned child=0;child<6;++child)
 for(unsigned hole=0;hole<4;++hole)for(unsigned p=0;p<9;++p){
  reset();strcpy(b[slot].path,"/target");b[slot].directory=dir;b[slot].mutable_node=writable;
  unsigned other=(slot+1)%16;strcpy(b[other].path,children[child]);
  if(hole==1)g_vfs_nodes[other]=0;
  if(hole==2)v[other].fs_private=0;
  if(hole==3)g_vfs_nodes[other]=&v[slot];
  compare(requests[p]);
 }
 for(unsigned length=2;length<256;++length){
  char path[256];memset(path,'a',length);path[0]='/';path[length]=0;
  reset();strcpy(b[0].path,path);compare(path);
  reset();strcpy(b[0].path,path);b[0].directory=true;
  strcpy(b[1].path,path);
  if(length<255){b[1].path[length]='/';b[1].path[length+1]=0;}
  compare(path);
  for(unsigned position=0;position<length;++position){
   reset();strcpy(b[0].path,path);b[0].path[position]='z';compare(path);
  }
 }
 reset();char invalid[256];memset(invalid,'x',256);invalid[0]='/';compare(invalid);
 memset(b[0].path,'x',256);compare("/target");
 reset();g_vfs_node_capacity=0;compare("/target");
 for(unsigned slot=0;slot<16;++slot){
  reset();strcpy(b[slot].path,"/target");compare(b[slot].path);
 }
 /* A successful exact remove must not touch the matched name's tail. */
 reset();strcpy(b[0].path,"/target");__asan_poison_memory_region(b[0].path+8,248);
 request="/target";depth=drops=scans=0;
 assert(vfs_remove_kernel(request)==K_OK&&!depth&&drops==1&&!scans&&!g_vfs_nodes[0]);
 __asan_unpoison_memory_region(b[0].path+8,248);
 /* A slash alone proves a descendant: even its first leaf byte is unused. */
 reset();strcpy(b[0].path,"/target");b[0].directory=true;strcpy(b[1].path,"/target/deep/file");
 __asan_poison_memory_region(b[1].path+8,248);depth=drops=scans=0;
 assert(vfs_remove_kernel(request)==K_ENOTEMPTY&&!depth&&!drops&&!scans&&g_vfs_nodes[0]==&v[0]);
 pid_t pid=fork();assert(pid>=0);
 if(!pid){assert(freopen("/dev/null","w",stderr));previous_remove(request);_exit(0);}
 int status;assert(waitpid(pid,&status,0)==pid&&WIFEXITED(status)&&WEXITSTATUS(status)==1);
 __asan_unpoison_memory_region(b[1].path+8,248);
 /* The type test and removal share the namespace lock. */
 drops=depth=0;
 reset();strcpy(b[0].path,"/target");b[0].directory=true;
 assert(vfs_remove_typed_kernel("/target",0)==K_EISDIR && !drops && !depth && g_vfs_nodes[0]);
 reset();strcpy(b[0].path,"/target");
 assert(vfs_remove_typed_kernel("/target",1)==K_ENOTDIR && !drops && !depth && g_vfs_nodes[0]);
 assert(vfs_remove_typed_kernel("/target",0)==K_OK && drops==1 && !depth && !g_vfs_nodes[0]);
 drops=0;
 reset();strcpy(b[0].path,"/target");b[0].directory=true;
 assert(vfs_remove_typed_kernel("/target",1)==K_OK && drops==1 && !depth && !g_vfs_nodes[0]);
 assert(avoided);printf("Remove scan: %u exact status/registry/refcount/hint/lock cases; %u candidate/child length scans eliminated; poisoned tails untouched; old child scan fails\n",cases,avoided);
}
'''
with tempfile.TemporaryDirectory(prefix='liteos-remove-scan-') as directory:
    binary = str(Path(directory) / 'test')
    subprocess.run(['cc', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
                    '-x', 'c', '-', '-o', binary], input=code, text=True, check=True)
    subprocess.run([binary], check=True)
