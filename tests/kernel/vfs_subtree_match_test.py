#!/usr/bin/env python3
"""Production rename predicate must compare equal-length path contents."""
import pathlib, subprocess, tempfile
root=pathlib.Path(__file__).resolve().parents[2]
source=(root/'kernel/fs/vfs/core.c').read_text()
def extract(name,end):
 start=source.index('static bool '+name+'(')
 return source[start:source.index(end,start)]
code='#include <stdbool.h>\n#include <stddef.h>\n#include <stdint.h>\n#include <assert.h>\n#include <string.h>\n#include <stdio.h>\n#include <sanitizer/asan_interface.h>\n#define VFS_PATH_LIMIT 256\n'
code+=extract('vfs_path_range_equals','static uint32_t vfs_free_node_slot_locked(')
code+=extract('vfs_path_is_subtree','static kstatus_t vfs_rekey_cached_subtree_locked')
code+=r'''
typedef struct {char path[256];bool fat32_node,detached;} vfs_backend_node_t;
typedef struct {void *filesystem;} vfs_fat32_mount_t;
typedef int kstatus_t;
enum {K_OK,K_ENAMETOOLONG};
static vfs_fat32_mount_t *vfs_fat32_mount_for_path(const char *p,char *r){(void)p;(void)r;return NULL;}
typedef struct {void *fs_private;} vnode_t;
static void vfs_namespace_insert_locked(vnode_t *v,uint32_t slot){(void)v;(void)slot;}
static void vfs_namespace_remove_locked(vnode_t *v){(void)v;}
static vnode_t *g_vfs_nodes[4];
static uint32_t g_vfs_node_capacity=4;
static unsigned scans;
static size_t vfs_string_length(const char *s){++scans;size_t n=0;while(n<256&&s[n])++n;return n;}
'''
start=source.index('static kstatus_t vfs_rekey_cached_subtree_locked(')
rekey=source[start:source.index('\n/* 替换磁盘文件前',start)]
new_match='''            if (backend == 0 ||
                !vfs_path_matches_subtree_prefix(backend->path, old_path, old_length)) continue;
            candidate_length = vfs_string_length(backend->path);'''
assert new_match in rekey
previous=rekey.replace('vfs_rekey_cached_subtree_locked(', 'previous_rekey(').replace(new_match,'''        if (backend == 0) continue;
        candidate_length = vfs_string_length(backend->path);
        if (!vfs_path_is_subtree(backend->path, candidate_length, old_path, old_length)) continue;''')
code+=previous+rekey
code+=r'''
int main(void){
 unsigned cases=0;
 char path[256],root[256];
 for(unsigned r=0;r<256;++r)for(unsigned p=0;p<=256;++p){
  memset(root,'a',sizeof(root));root[r]=0;
  memset(path,'a',sizeof(path));if(p<256)path[p]=0;
  assert(vfs_path_matches_subtree_prefix(path,root,r)==vfs_path_is_subtree(path,p,root,r));++cases;
  if(r<256&&r<p){path[r]='/';assert(vfs_path_matches_subtree_prefix(path,root,r)==vfs_path_is_subtree(path,p,root,r));++cases;}
 }
 for(unsigned r=1;r<256;++r){
  memset(root,'a',sizeof(root));root[r]=0;
  for(unsigned mismatch=0;mismatch<r;++mismatch){
   memcpy(path,root,sizeof(path));path[mismatch]='b';
   assert(!vfs_path_matches_subtree_prefix(path,root,r));++cases;
  }
 }
 assert(vfs_path_is_subtree("/tmp/a",6,"/tmp/a",6));
 assert(!vfs_path_is_subtree("/tmp/b",6,"/tmp/a",6));
 assert(vfs_path_is_subtree("/tmp/a/file",11,"/tmp/a",6));
 assert(!vfs_path_is_subtree("/tmp/ab/file",12,"/tmp/a",6));
 assert(!vfs_path_is_subtree("/tmp/b/file",11,"/tmp/a",6));
 assert(!vfs_path_is_subtree("/tmp",4,"/tmp/a",6));
 vfs_backend_node_t b[4]={{.path="/tmp/a"},{.path="/tmp/b"},
                         {.path="/tmp/a/file"},{.path="/tmp/ab"}};
 vnode_t v[4];
 for(int i=0;i<4;++i){v[i].fs_private=&b[i];g_vfs_nodes[i]=&v[i];}
 vfs_rekey_cached_subtree_locked("/tmp/a",6,"/tmp/z",6,true);
 assert(!strcmp(b[0].path,"/tmp/z"));
 assert(!strcmp(b[1].path,"/tmp/b"));
 assert(!strcmp(b[2].path,"/tmp/z/file"));
 assert(!strcmp(b[3].path,"/tmp/ab"));
 unsigned avoided=0;
 for(unsigned length=1;length<200;++length){
  char target[256];memset(target,'z',length);target[0]='/';target[length]=0;
  strcpy(b[0].path,"/tmp/a");strcpy(b[1].path,"/tmp/b/deep/file");
  strcpy(b[2].path,"/tmp/a/file");strcpy(b[3].path,"/tmp/ab");
  vfs_backend_node_t initial[4],expected[4];memcpy(initial,b,sizeof(b));
  scans=0;previous_rekey("/tmp/a",6,target,length,true);unsigned old_scans=scans;
  memcpy(expected,b,sizeof(b));memcpy(b,initial,sizeof(b));scans=0;
  vfs_rekey_cached_subtree_locked("/tmp/a",6,target,length,true);
  assert(!memcmp(expected,b,sizeof(b))&&scans==4&&old_scans==8);avoided+=old_scans-scans;
 }
 strcpy(b[0].path,"/tmp/a");strcpy(b[1].path,"/tmp/b/deep/file");
 strcpy(b[2].path,"/tmp/a/file");strcpy(b[3].path,"/tmp/ab");
 __asan_poison_memory_region(b[1].path+7,249);scans=0;
 vfs_rekey_cached_subtree_locked("/tmp/a",6,"/tmp/z",6,true);
 assert(scans==4&&!strcmp(b[0].path,"/tmp/z")&&!strcmp(b[2].path,"/tmp/z/file"));
 __asan_unpoison_memory_region(b[1].path+7,249);
 printf("Subtree prefix: %u bounded length/slash/mismatch oracle cases; 199 complete rekey table comparisons skip %u unrelated length scans; poisoned tails untouched\n",cases,avoided);
}
'''
with tempfile.TemporaryDirectory(prefix='liteos-subtree-') as d:
 binary=str(pathlib.Path(d)/'test')
 subprocess.run(['cc','-O2','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-fno-pie','-no-pie','-x','c','-','-o',binary],
                input=code,text=True,check=True)
 subprocess.run([binary],check=True)
print('VFS rename: identity, unrelated equal-length paths and descendant boundaries passed')
