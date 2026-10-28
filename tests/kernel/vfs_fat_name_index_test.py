"""校验真实 FAT 缓存索引、挂载别名、重命名及扩容语义。"""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'kernel/fs/vfs/core.c').read_text()


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
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define VFS_PATH_LIMIT 256U
#define VFS_INITIAL_NODE_CAPACITY 8U
#define VFS_FAT32_MOUNT_LIMIT 8U
typedef int kstatus_t;
enum {K_OK,K_ENAMETOOLONG};
typedef struct vnode { void *fs_private; } vnode_t;
typedef struct { char path[256]; vnode_t *namespace_next; uint32_t namespace_slot;
 bool fat32_node,detached; } vfs_backend_node_t;
typedef struct { char prefix[256]; void *filesystem; bool mounted; } vfs_fat32_mount_t;
static vnode_t *g_vfs_initial_nodes[8], *g_vfs_initial_buckets[8];
static vnode_t **g_vfs_nodes=g_vfs_initial_nodes, **g_vfs_buckets=g_vfs_initial_buckets;
static uint32_t g_vfs_initial_fat_buckets[8],g_vfs_initial_fat_next[8];
static uint32_t *g_vfs_fat_buckets=g_vfs_initial_fat_buckets,*g_vfs_fat_next=g_vfs_initial_fat_next;
static uint32_t g_vfs_node_capacity=8,g_vfs_next_free_node;
static vfs_fat32_mount_t g_fat32_mounts[8];
static unsigned long comparisons;
static bool fail_alloc;
static void *kzalloc(size_t n,int flags){assert(!flags);return fail_alloc?0:calloc(1,n);}
static void kfree(void *p){free(p);}
'''
program += function('size_t vfs_string_length(')
program += function('static uint32_t vfs_namespace_bucket(')
if 'static uint32_t vfs_fat_name_bucket(' in source:
    for name in ('static uint32_t vfs_fat_name_bucket(',
                 'static void vfs_fat_index_insert_locked(',
                 'static void vfs_fat_index_remove_locked('):
        program += function(name)
for name in ('static void vfs_namespace_insert_locked(',
             'static void vfs_namespace_remove_locked(',
             'static vnode_t *vfs_find_node_locked(',
             'static bool vfs_path_range_equals(',
             'static uint32_t vfs_free_node_slot_locked(',
             'static void vfs_set_node_slot_locked(',
             'static bool vfs_volatile_path(',
             'static bool vfs_mount_matches(',
             'vfs_fat32_mount_t *vfs_fat32_mount_for_path('):
    program += function(name)
program += function('static bool vfs_fat_relative_path_equal(').replace(
    '    if (right == 0 || length >= VFS_PATH_LIMIT) return false;',
    '    ++comparisons; if (right == 0 || length >= VFS_PATH_LIMIT) return false;')
for name in ('static vnode_t *vfs_find_fat_node_locked(',
             'static bool vfs_path_matches_subtree_prefix(',
             'static kstatus_t vfs_rekey_cached_subtree_locked('):
    program += function(name)
program += r'''
static vnode_t *oracle(const char *path){
 char r[256];vfs_fat32_mount_t *mount=vfs_fat32_mount_for_path(path,r);
 if(!mount)return 0;
 for(uint32_t i=0;i<g_vfs_node_capacity;++i){
  vnode_t *v=g_vfs_nodes[i];vfs_backend_node_t *b=v?v->fs_private:0;
  if(b&&b->fat32_node&&!b->detached&&
     vfs_fat_relative_path_equal(r,vfs_string_length(r),mount,b->path))return v;
 }
 return 0;
}
static void check(const char *path){vnode_t *expected=oracle(path);assert(vfs_find_fat_node_locked(path)==expected);}
static void audit(void){
 unsigned indexed=0,published=0;
 for(uint32_t bucket=0;bucket<g_vfs_node_capacity;++bucket){
  uint32_t previous=0;bool first=true;
  for(uint32_t link=g_vfs_fat_buckets[bucket];link;link=g_vfs_fat_next[link-1]){
   uint32_t slot=link-1;assert(slot<g_vfs_node_capacity);
   vnode_t *v=g_vfs_nodes[slot];vfs_backend_node_t *b=v?v->fs_private:0;
   assert(b&&b->fat32_node&&b->namespace_slot==slot);
   assert(first||previous<slot);first=false;previous=slot;
   assert(vfs_fat_name_bucket(b->path)==bucket);
   assert(++indexed<=g_vfs_node_capacity);
  }
 }
 for(uint32_t i=0;i<g_vfs_node_capacity;++i){
  vnode_t *v=g_vfs_nodes[i];vfs_backend_node_t *b=v?v->fs_private:0;
  published+=b&&b->fat32_node;
 }
 assert(indexed==published);
}
int main(void){
 const unsigned n=32768;
 vnode_t *nodes=calloc(n+8,sizeof(*nodes));vfs_backend_node_t *b=calloc(n+8,sizeof(*b));assert(nodes&&b);
 g_fat32_mounts[0]=(vfs_fat32_mount_t){"/",(void*)1,true};
 g_fat32_mounts[1]=(vfs_fat32_mount_t){"/alias",(void*)1,true};
 g_fat32_mounts[2]=(vfs_fat32_mount_t){"/other",(void*)2,true};
 /* 固定随机序列对照完整扫描，覆盖碰撞、槽位复用和运行时挂载变化。 */
 for(unsigned i=0;i<512;++i){
  nodes[i].fs_private=&b[i];b[i].fat32_node=true;
  snprintf(b[i].path,256,"/dir%u/item%u",i/16,i%16);
  vfs_set_node_slot_locked(vfs_free_node_slot_locked(),&nodes[i]);
 }
 uint32_t random=0x63a92b17U;
 for(unsigned round=0;round<5000;++round){
  random=random*1664525U+1013904223U;unsigned slot=(random>>8)%512;
  vfs_set_node_slot_locked(slot,0);
  random=random*1664525U+1013904223U;
  if(random%5U){
   const char *prefix=(random&16U)?"/alias":((random&32U)?"/other":"");
   snprintf(b[slot].path,256,"%s/dir%u/item%u",prefix,(random>>12)%48,(random>>20)%16);
   b[slot].fat32_node=(random&128U)!=0;b[slot].detached=(random&256U)!=0;
   vfs_set_node_slot_locked(slot,&nodes[slot]);
  }
  if(round%19U==0){
   g_fat32_mounts[1].filesystem=(void*)(uintptr_t)(1U+(random&1U));
   g_fat32_mounts[1].mounted=(random&2U)!=0;
  }
  char query[256];snprintf(query,sizeof(query),"/alias/DIR%u/ITEM%u",(random>>12)%48,(random>>20)%16);
  check(query);check(b[slot].path);
  if(round%500U==0)audit();
 }
 audit();
 for(unsigned i=0;i<512;++i)vfs_set_node_slot_locked(i,0);
 g_fat32_mounts[1]=(vfs_fat32_mount_t){"/alias",(void*)1,true};
 for(unsigned i=0;i<n;++i){
  nodes[i].fs_private=&b[i];b[i].fat32_node=true;b[i].detached=false;
  snprintf(b[i].path,sizeof(b[i].path),"/profile/f%05u.bin",i);
  unsigned slot=vfs_free_node_slot_locked();assert(slot==i);vfs_set_node_slot_locked(slot,&nodes[i]);
 }
 comparisons=0;assert(vfs_find_fat_node_locked("/PROFILE/F32767.BIN")==&nodes[n-1]);
 unsigned long hit=comparisons;comparisons=0;
 assert(!vfs_find_fat_node_locked("/profile/missing.bin"));unsigned long miss=comparisons;
 printf("32768 FAT nodes: hit comparisons=%lu, missing comparisons=%lu\n",hit,miss);
 assert(hit<128 && miss<128);
 vnode_t **old_nodes=g_vfs_nodes;uint32_t old_capacity=g_vfs_node_capacity;
 fail_alloc=true;assert(vfs_free_node_slot_locked()==UINT32_MAX);
 assert(g_vfs_nodes==old_nodes&&g_vfs_node_capacity==old_capacity);
 assert(vfs_find_fat_node_locked("/profile/f32767.bin")==&nodes[n-1]);
 fail_alloc=false;
 check("/profile/f00000.bin");check("/alias/PROFILE/F32767.BIN");
 check("/other/profile/f32767.bin");
 /* 同名不同目录与跨文件系统不能混淆；相同卷别名和大小写命中同一 inode。 */
 for(unsigned i=0;i<4;++i){
  unsigned j=n+i;nodes[j].fs_private=&b[j];b[j].fat32_node=true;
  const char *paths[]={"/one/shared", "/two/shared", "/other/one/shared", "/alias/ONE/SHARED"};
  strcpy(b[j].path,paths[i]);unsigned slot=vfs_free_node_slot_locked();vfs_set_node_slot_locked(slot,&nodes[j]);
 }
 assert(vfs_find_fat_node_locked("/alias/one/SHARED")==&nodes[n]);
 b[n].detached=true;assert(vfs_find_fat_node_locked("/one/shared")==&nodes[n+3]);
 b[n].detached=false;check("/two/shared");check("/other/one/shared");
 /* 挂载变化会改变相对路径，但不会改变文件末段；无需重建索引。 */
 g_fat32_mounts[2].filesystem=(void*)1;check("/one/shared");check("/other/one/shared");
 g_fat32_mounts[1].filesystem=(void*)2;check("/alias/profile/f32767.bin");
 g_fat32_mounts[1].filesystem=(void*)1;g_fat32_mounts[2].filesystem=(void*)2;
 /* 挂载根的相对路径为空，允许通过完整扫描处理这一少见情况。 */
 unsigned j=n+4;nodes[j].fs_private=&b[j];b[j].fat32_node=true;strcpy(b[j].path,"/alias");
 vfs_set_node_slot_locked(vfs_free_node_slot_locked(),&nodes[j]);
 check("/");check("/alias");
 /* 重命名父目录必须同时更新文件末段索引；摘除与复用槽位也要一致。 */
 assert(vfs_rekey_cached_subtree_locked("/one",4,"/renamed",8,true)==K_OK);
 assert(!vfs_find_fat_node_locked("/one/shared"));
 assert(vfs_find_fat_node_locked("/renamed/shared")==&nodes[n]);
 strcpy(b[n+5].path,"/renamed/newname");nodes[n+5].fs_private=&b[n+5];b[n+5].fat32_node=true;
 vfs_set_node_slot_locked(n,&nodes[n+5]);
 assert(vfs_find_fat_node_locked("/renamed/shared")==&nodes[n+3]);
 vfs_set_node_slot_locked(n+3,0);
 assert(!vfs_find_fat_node_locked("/renamed/shared"));
 assert(vfs_find_fat_node_locked("/renamed/newname")==&nodes[n+5]);
 for(unsigned i=0;i<n;i+=79){char path[80];snprintf(path,sizeof(path),"/ALIAS/profile/F%05u.BIN",i);check(path);}
 audit();
 puts("FAT filename index: alias/case, first slot, detach/replacement, live remount, mount roots, directory rename and growth passed");
 free(g_vfs_nodes==g_vfs_initial_nodes?0:g_vfs_nodes);free(nodes);free(b);
}
'''
with tempfile.TemporaryDirectory(prefix='vfs-fat-index-') as directory:
    path = Path(directory)
    for variant in ('current', 'linear'):
        code = program if variant == 'current' else program.replace(
            'bool indexed = relative_length != 0U && g_vfs_node_capacity != 0U;',
            'bool indexed = false;')
        assert variant == 'current' or code != program
        source_path, binary = path / f'{variant}.c', path / variant
        source_path.write_text(code)
        subprocess.run(['cc', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                        '-Wno-unused-function', '-Wno-unused-variable',
                        '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
                        str(source_path), '-o', str(binary)], check=True)
        result = subprocess.run([str(binary)], cwd=path, capture_output=True,
                                text=True, timeout=30)
        if variant == 'current':
            assert result.returncode == 0, result.stderr
            print(result.stdout.strip())
        else:
            assert result.returncode != 0 and 'hit<128 && miss<128' in result.stderr
            print('负对照通过：退回全表扫描时，候选数量断言会失败')
