"""执行真实命名空间索引、注册表扩容和路径重设，校验线性查找模型。"""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'kernel/fs/vfs/core.c').read_text()


def function(signature):
    start = source.index(signature)
    return source[start:source.index('\n}', start) + 2] + '\n'


helpers = source[source.index('static uint32_t vfs_namespace_bucket('):
                 source.index('static bool vfs_fat_relative_path_equal(')]
helpers = helpers.replace('        vfs_backend_node_t *backend = vnode != 0 ?',
                          '        ++visits; vfs_backend_node_t *backend = vnode != 0 ?')
code = r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define VFS_PATH_LIMIT 256U
#define VFS_INITIAL_NODE_CAPACITY 8U
typedef int kstatus_t;
enum {K_OK,K_ENAMETOOLONG};
typedef struct vnode {void *fs_private;} vnode_t;
typedef struct {
 char path[256];vnode_t *namespace_next;uint32_t namespace_slot;
 bool fat32_node,detached;
} vfs_backend_node_t;
typedef struct {void *filesystem;} vfs_fat32_mount_t;
static vnode_t *g_vfs_initial_nodes[8],*g_vfs_initial_buckets[8];
static vnode_t **g_vfs_nodes=g_vfs_initial_nodes,**g_vfs_buckets=g_vfs_initial_buckets;
static uint32_t g_vfs_initial_fat_buckets[8],g_vfs_initial_fat_next[8];
static uint32_t *g_vfs_fat_buckets=g_vfs_initial_fat_buckets,*g_vfs_fat_next=g_vfs_initial_fat_next;
static uint32_t g_vfs_node_capacity=8,g_vfs_next_free_node;
static unsigned visits;static bool fail_alloc;
static void *kzalloc(size_t n,int flags){assert(!flags);return fail_alloc?0:calloc(1,n);}
static void kfree(void *p){free(p);}
static vfs_fat32_mount_t *vfs_fat32_mount_for_path(const char *p,char *r){(void)p;(void)r;return 0;}
''' + function('size_t vfs_string_length(') + helpers
code += function('static bool vfs_path_range_equals(')
code += function('static uint32_t vfs_free_node_slot_locked(')
code += function('static void vfs_set_node_slot_locked(')
code += function('static bool vfs_path_matches_subtree_prefix(')
code += function('static kstatus_t vfs_rekey_cached_subtree_locked(')
code += r'''
static vnode_t *oracle(const char *path){
 for(unsigned i=0;i<g_vfs_node_capacity;++i){
  vnode_t *v=g_vfs_nodes[i];vfs_backend_node_t *b=v?v->fs_private:0;
  if(b&&!strcmp(b->path,path))return v;
 }
 return 0;
}
static void check(const char *path){assert(vfs_find_node_locked(path)==oracle(path));}
static void audit(void){
 unsigned indexed=0,published=0;
 for(unsigned bucket=0;bucket<g_vfs_node_capacity;++bucket){
  unsigned previous=0;bool first=true;
  for(vnode_t *v=g_vfs_buckets[bucket];v;){
   vfs_backend_node_t *b=v->fs_private;
   assert(first||previous<b->namespace_slot);first=false;previous=b->namespace_slot;
   assert(g_vfs_nodes[b->namespace_slot]==v);
   assert(vfs_namespace_bucket(b->path,strlen(b->path))==bucket);
   ++indexed;assert(indexed<=g_vfs_node_capacity);v=b->namespace_next;
  }
 }
 for(unsigned i=0;i<g_vfs_node_capacity;++i)published+=g_vfs_nodes[i]!=0;
 assert(published==indexed);
}
int main(void){
 const unsigned n=32768;
 vfs_backend_node_t *backends=calloc(n,sizeof(*backends));
 vnode_t *nodes=calloc(n,sizeof(*nodes));assert(backends&&nodes);
 for(unsigned i=0;i<n;++i)nodes[i].fs_private=&backends[i];
 /* 强制碰撞和逆序槽位插入，确保桶链仍按原先的最低槽位规则命中。 */
 unsigned found=0;
 for(unsigned candidate=0;found<4;++candidate){
  char name[64];snprintf(name,sizeof(name),"/collision-%u",candidate);
  if(vfs_namespace_bucket(name,strlen(name)))continue;
  strcpy(backends[found++].path,name);
 }
 const unsigned slots[]={6,2,5,0};
 for(unsigned i=0;i<4;++i)vfs_set_node_slot_locked(slots[i],&nodes[i]);
 for(unsigned i=0;i<4;++i)check(backends[i].path);
 strcpy(backends[4].path,backends[0].path);vfs_set_node_slot_locked(1,&nodes[4]);
 assert(vfs_find_node_locked(backends[0].path)==&nodes[4]);
 vfs_set_node_slot_locked(1,0);assert(vfs_find_node_locked(backends[0].path)==&nodes[0]);
 audit();for(unsigned i=0;i<8;++i)vfs_set_node_slot_locked(i,0);
 for(unsigned i=0;i<n;++i){
  snprintf(backends[i].path,256,"/tmp/profile/%05u",i);
  unsigned slot=vfs_free_node_slot_locked();assert(slot==i);
  vfs_set_node_slot_locked(slot,&nodes[i]);
 }
 audit();visits=0;
 assert(vfs_find_node_locked(backends[n-1].path)==&nodes[n-1]);
 unsigned hit=visits;assert(hit<16);visits=0;
 assert(!vfs_find_node_locked("/tmp/profile/missing"));assert(visits<16);
 printf("32768-node exact lookup: last hit visits 32768 -> %u; miss bucket visits %u\n",hit,visits);
 vnode_t **old_nodes=g_vfs_nodes,**old_buckets=g_vfs_buckets;
 fail_alloc=true;assert(vfs_free_node_slot_locked()==UINT32_MAX);
 assert(old_nodes==g_vfs_nodes&&old_buckets==g_vfs_buckets&&g_vfs_node_capacity==n);
 for(unsigned i=0;i<n;i+=127)check(backends[i].path);
 fail_alloc=false;assert(vfs_free_node_slot_locked()==n);
 assert(g_vfs_node_capacity==n*2&&g_vfs_buckets==g_vfs_nodes+g_vfs_node_capacity);
 audit();for(unsigned i=0;i<n;i+=127)check(backends[i].path);
 for(unsigned i=0;i<n;++i)vfs_set_node_slot_locked(i,0);
 /* 发布、替换、摘除和键变更穿插进行；模型允许同名节点以验证首槽语义。 */
 uint32_t random=0x17cd5a91;
 for(unsigned round=0;round<12000;++round){
  random=random*1664525U+1013904223U;unsigned slot=(random>>8)%512;
  random=random*1664525U+1013904223U;unsigned operation=random%3;
  if(operation==0)vfs_set_node_slot_locked(slot,0);
  else{
   vnode_t *old=g_vfs_nodes[slot];
   unsigned index=slot*2+(old==&nodes[slot*2]);
   snprintf(backends[index].path,256,"/tmp/live/%03u",(random>>16)%128);
   vfs_set_node_slot_locked(slot,&nodes[index]);
  }
  char query[64];snprintf(query,sizeof(query),"/tmp/live/%03u",(random>>16)%128);
  check(query);if(round%400==0)audit();
 }
 audit();for(unsigned i=0;i<512;++i)vfs_set_node_slot_locked(i,0);
 strcpy(backends[0].path,"/dir");strcpy(backends[1].path,"/dir/child");
 strcpy(backends[2].path,"/directory");
 for(unsigned i=0;i<3;++i)vfs_set_node_slot_locked(i,&nodes[i]);
 assert(vfs_rekey_cached_subtree_locked("/dir",4,"/new",4,false)==K_OK);
 assert(vfs_find_node_locked("/dir")==&nodes[0]);
 assert(vfs_rekey_cached_subtree_locked("/dir",4,"/new",4,true)==K_OK);
 assert(!vfs_find_node_locked("/dir")&&!vfs_find_node_locked("/dir/child"));
 assert(vfs_find_node_locked("/new")==&nodes[0]);
 assert(vfs_find_node_locked("/new/child")==&nodes[1]);
 assert(vfs_find_node_locked("/directory")==&nodes[2]);audit();
 for(unsigned i=0;i<3;++i)vfs_set_node_slot_locked(i,0);
 audit();kfree(g_vfs_nodes);free(backends);free(nodes);
 puts("namespace index: collisions, first-slot identity, growth/OOM rollback, 12000 publication/removal/replacement model operations and real subtree rekey passed");
}
'''
with tempfile.TemporaryDirectory(prefix='vfs-namespace-index-') as directory:
    binary = str(Path(directory) / 'test')
    subprocess.run(['cc', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
                    '-x', 'c', '-', '-o', binary], input=code, text=True, check=True)
    subprocess.run([binary], check=True, timeout=30)
