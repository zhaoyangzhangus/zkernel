"""执行实际内存节点创建，检查父目录竞态、原子发布和失败清理。"""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'kernel/fs/vfs/core.c').read_text()
code = source[source.index('static kstatus_t vfs_publish_memory_node('):
              source.index('/* 不可变链接内容与命名空间项一起发布。')]
program = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <kernel/base.h>
#define VFS_PATH_LIMIT 256U
typedef int io_mutex_t;
static void io_mutex_init(io_mutex_t *m,bool pi){assert(pi);*m=1;}
typedef struct {
 char path[256];void *data;uint64_t size;uint32_t mode;
 bool directory,mutable_node;void *context;io_mutex_t io_lock;
 int (*read)(void),(*write)(void),(*fsync)(void),(*truncate)(void);
} vfs_backend_node_t;
typedef struct {void *fs_private;} vnode_t;
typedef struct {void *filesystem;} vfs_fat32_mount_t;
typedef struct {unsigned type;} os_file_info_t;
static vnode_t *published,collision;
static unsigned locked,locks,allocations,live,fail_at,lookups,parents,publishes;
static bool parent_exists=true,remove_on_allocation,collision_on_allocation,full;
static bool mounted,fat_exists,fat_create=true;
static unsigned fat_stats,fat_creates;
static size_t vfs_string_length(const char *p){return p?strnlen(p,256):0;}
static void *kzalloc(size_t n,int flags){
 assert(!locked&&!flags);
 if(remove_on_allocation)parent_exists=false;
 if(collision_on_allocation)published=&collision;
 if(++allocations==fail_at)return 0;
 void *p=calloc(1,n);assert(p);++live;return p;
}
static void kfree(void *p){assert(!locked);if(p){assert(live);--live;free(p);}}
static vnode_t *vfs_create_vnode_object(vfs_backend_node_t *b){
 vnode_t *v=kzalloc(sizeof(*v),0);if(v)v->fs_private=b;return v;
}
static void object_put(vnode_t *v){
 assert(!locked&&v&&v!=&collision);
 kfree(v->fs_private);kfree(v);
}
static void vfs_initialize(void){}
static void vfs_lock(void){assert(!locked);locked=1;++locks;}
static void vfs_unlock(void){assert(locked);locked=0;}
static vnode_t *vfs_find_node_locked(const char *p){
 assert(locked&&!strcmp(p,"/tmp/parent/item"));++lookups;return published;
}
static bool vfs_parent_directory_exists_locked(const char *p,size_t n){
 assert(locked&&n==strlen(p));++parents;return parent_exists;
}
static uint32_t vfs_free_node_slot_locked(void){assert(locked);return full?UINT32_MAX:0;}
static void vfs_set_node_slot_locked(uint32_t slot,vnode_t *v){
 assert(locked&&!slot&&!published&&parent_exists);published=v;++publishes;
}
static vfs_fat32_mount_t *vfs_fat32_mount_for_path(const char *p,char *r){
 static vfs_fat32_mount_t mount;
 if(!mounted)return 0;
 strcpy(r,!strcmp(p,"/disk")?"":"item");return &mount;
}
static bool fat32_stat_path(void *fs,const char *p,os_file_info_t *i){
 (void)fs;(void)i;assert(!locked&&!strcmp(p,"item"));++fat_stats;return fat_exists;
}
static bool fat32_create_path(void *fs,const char *p,bool dir){
 (void)fs;(void)dir;assert(!locked&&!strcmp(p,"item"));++fat_creates;return fat_create;
}
static int vfs_memory_backend_read(void){return 0;}
static int vfs_memory_backend_write(void){return 0;}
static int vfs_memory_backend_truncate(void){return 0;}
''' + code + r'''
static void reset(void){
 if(published&&published!=&collision)object_put(published);
 assert(!live&&!locked);published=0;
 locks=allocations=fail_at=lookups=parents=publishes=fat_stats=fat_creates=0;
 parent_exists=fat_create=true;
 remove_on_allocation=collision_on_allocation=full=mounted=fat_exists=false;
}
int main(void){
 const char *path="/tmp/parent/item";
 for(unsigned dir=0;dir<2;++dir){
  assert(vfs_create_kernel(path,0,dir)==K_OK);
  vfs_backend_node_t *b=published->fs_private;
  assert(!strcmp(b->path,path)&&b->directory==dir&&b->mutable_node);
  assert(b->mode==(dir?0040755U:0100666U)&&!b->size&&!b->data);
  assert(b->context==b&&b->io_lock==1);
  assert(b->read==(dir?0:vfs_memory_backend_read));
  assert(b->write==(dir?0:vfs_memory_backend_write));
  assert(b->truncate==(dir?0:vfs_memory_backend_truncate)&&!b->fsync);
  assert(locks==1&&lookups==1&&parents==1&&publishes==1&&live==2);
  reset();
  for(unsigned failure=1;failure<=2;++failure){
   fail_at=failure;assert(vfs_create_kernel(path,0600,dir)==K_ENOMEM);
   assert(!live&&!published&&!locks);reset();
  }
  remove_on_allocation=true;
  assert(vfs_create_kernel(path,0600,dir)==K_ENOTDIR);
  assert(!published&&!live&&locks==1&&parents==1&&!publishes);reset();
  collision_on_allocation=true;
  assert(vfs_create_kernel(path,0600,dir)==K_EEXIST);
  assert(published==&collision&&!live&&locks==1&&lookups==1&&!parents);reset();
  full=true;assert(vfs_create_kernel(path,0600,dir)==K_ENOMEM);
  assert(!published&&!live&&!publishes&&parents==1);reset();
 }
 assert(vfs_mkdir_kernel(path,0700)==K_OK);
 assert(((vfs_backend_node_t*)published->fs_private)->mode==0040700);reset();
 const char *bad[]={0,"relative","/","/tmp/"};
 for(unsigned i=0;i<4;++i)assert(vfs_create_kernel(bad[i],0,false)==K_EINVAL);
 char long_path[257];memset(long_path,'x',256);long_path[0]='/';long_path[256]=0;
 assert(vfs_create_kernel(long_path,0,false)==K_EINVAL&&!allocations);
 mounted=true;assert(vfs_create_kernel("/disk",0,false)==K_EBUSY);
 fat_exists=true;assert(vfs_create_kernel("/disk/item",0,false)==K_EEXIST);
 fat_exists=false;fat_create=false;
 assert(vfs_create_kernel("/disk/item",0,false)==K_EIO);
 fat_create=true;assert(vfs_create_kernel("/disk/item",0,false)==K_OK);
 assert(fat_stats==3&&fat_creates==2&&!locks&&!allocations);
 puts("PASS: memory file/directory atomic publication, one lookup/lock, parent removal and duplicate races, OOM rollback, modes and unchanged FAT delegation");
}
'''
with tempfile.TemporaryDirectory(prefix='vfs-memory-publication-') as directory:
    binary = str(Path(directory) / 'test')
    subprocess.run(['cc', '-D_GNU_SOURCE', '-std=c11', '-O2', '-Wall', '-Wextra',
                    '-Werror', '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
                    '-I' + str(root / 'include'), '-x', 'c', '-', '-o', binary],
                   input=program, text=True, check=True)
    subprocess.run([binary], check=True)
