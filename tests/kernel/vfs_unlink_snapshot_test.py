"""Production FAT unlink snapshot and pre-eviction transaction failure paths."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
backend = (ROOT / 'kernel/fs/vfs/backend.c').read_text()
core = (ROOT / 'kernel/fs/vfs/core.c').read_text()

def function(source, signature):
    start = source.index(signature)
    end = source.index('{', start) + 1
    depth = 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]

remove = core[core.index('kstatus_t vfs_remove_typed_kernel('):
              core.index('        /* 删除磁盘目录项后同步摘除规范 vnode')]
# Cache eviction is exercised separately; this harness stops after commit.
remove += '(void)victim; if (mounted_victim != 0) object_put(mounted_victim); return K_OK; } return K_EXDEV; }\n'
remove += function(core, 'kstatus_t vfs_remove_kernel(') + '\n'
code = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define VFS_PATH_LIMIT 256U
#define OS_FILE_TYPE_DIRECTORY 2U
typedef int kstatus_t;
typedef uint32_t UINT32;
typedef bool BOOLEAN;
enum {K_OK,K_EINVAL,K_ENOMEM,K_EIO,K_ENOENT,K_ENOTEMPTY,K_EXDEV,K_EBUSY,K_EOVERFLOW,K_ENOTDIR,K_EISDIR};
typedef struct {unsigned identity;uint64_t Size;} fat_file_t;
typedef fat_file_t OS_FAT32_FILE;
typedef struct {char path[256];bool directory,detached,unlink_prepared;uint64_t size;uint8_t *data;fat_file_t *fat_file;uint64_t data_capacity,content_revision,snapshot_revision,snapshot_size;} vfs_backend_node_t;
typedef struct {void *filesystem;} vfs_fat32_mount_t;
typedef struct {void *fs_private;unsigned refs;} vnode_t;
typedef struct {unsigned type;} os_file_info_t;
static vfs_backend_node_t b;
static vnode_t v;
static vfs_fat32_mount_t mount={(void*)1};
static unsigned locked,backend_locked,live,allocations,releases,routes,removes;
static unsigned path_reads,file_reads,largest_read;
static int resize_during_read;
static bool publish_during_read;
static int mutate_after_prepare;
static bool revise_during_read;
static fat_file_t inode={.identity=42,.Size=8};
static kstatus_t vfs_fat32_backend_write(void *,uint64_t,const void *,size_t,uint64_t *);
static kstatus_t vfs_fat32_backend_truncate(void *,uint64_t);
static bool allocation_fail,read_fail,short_read,remove_fail,bad_prepare_route,exists;
static uint8_t disk[2U*1024U*1024U+1U]={1,2,3,4,5,6,7,8};
static void *kmalloc(size_t n,unsigned flags){
 (void)flags;assert(!locked&&(!backend_locked||b.detached));++allocations;
 if(allocation_fail)return NULL;
 assert(n<=sizeof(disk));void *p=malloc(n);assert(p);++live;return p;
}
static void *kzalloc(size_t n,unsigned flags){void *p=kmalloc(n,flags);if(p)memset(p,0,n);return p;}
static void kfree(void *p){
 assert(!locked&&(!backend_locked||b.detached));if(p){assert(live);--live;++releases;free(p);}
}
static void vfs_backend_lock(vfs_backend_node_t *p){(void)p;assert(!backend_locked);backend_locked=1;}
static bool close_fail;
static unsigned closes;
static bool fat32_close(fat_file_t *f){assert(backend_locked&&f==&inode);++closes;return !close_fail;}
static void vfs_backend_unlock(vfs_backend_node_t *p){
 assert(backend_locked);backend_locked=0;
 if(mutate_after_prepare&&p->unlink_prepared&&!p->detached){
  int mutation=mutate_after_prepare;mutate_after_prepare=0;
  uint64_t bytes=0;vfs_backend_lock(p);
  if(mutation==1)assert(vfs_fat32_backend_write(p,8,"expanded",8,&bytes)==K_OK);
  else if(mutation==-1)assert(vfs_fat32_backend_truncate(p,4)==K_OK);
  else assert(vfs_fat32_backend_write(p,0,"x",1,&bytes)==K_OK);
  vfs_backend_unlock(p);
 }
}
static void vfs_initialize(void){}
static void vfs_lock(void){assert(!locked);locked=1;}
static void vfs_unlock(void){assert(locked);locked=0;}
static size_t vfs_string_length(const char *p){return strlen(p);}
static vfs_fat32_mount_t *vfs_fat32_mount_for_path(const char *p,char *r){
 (void)p;++routes;strcpy(r,"/chrome");return bad_prepare_route&&routes>1?NULL:&mount;
}
static bool fat32_stat_path(void *f,const char *p,os_file_info_t *i){
 (void)f;(void)p;i->type=b.directory?OS_FILE_TYPE_DIRECTORY:1;return exists;
}
static vnode_t *vfs_find_fat_node_locked(const char *p){(void)p;assert(locked);return &v;}
static void object_get(vnode_t *p){assert(locked);++p->refs;}
static void object_put(vnode_t *p){assert(!locked&&p->refs>1);--p->refs;}
static bool fat32_read_path(void *f,const char *p,uint64_t offset,
                                   void *buffer,UINT32 capacity,UINT32 *transferred){
 (void)f;(void)p;assert(!locked&&offset+capacity<=sizeof(disk));++path_reads;
 if(capacity>largest_read)largest_read=capacity;
 if(read_fail)return false;
 *transferred=short_read?capacity-1:capacity;
 memcpy(buffer,disk+offset,*transferred);
 if(resize_during_read){b.size=resize_during_read>0?16:4;resize_during_read=0;}
 if(revise_during_read){++b.content_revision;revise_during_read=false;}
 if(publish_during_read){
  publish_during_read=false;
  uint8_t *winner=kmalloc(8,0);memset(winner,77,8);
  vfs_backend_lock(&b);b.data=winner;vfs_backend_unlock(&b);
 }
 return true;
}
static bool fat32_read_file(fat_file_t *f,uint64_t offset,void *buffer,
                                   UINT32 capacity,UINT32 *transferred){
 assert(f==&inode&&f->identity==42);++file_reads;
 unsigned previous=path_reads;
 bool result=fat32_read_path(NULL,NULL,offset,buffer,capacity,transferred);
 path_reads=previous;return result;
}
static bool fat32_write_path(void *f,const char *p,uint64_t offset,
                                    const void *buffer,UINT32 n,UINT32 *transferred){
 (void)f;(void)p;assert(backend_locked&&offset+n<=sizeof(disk));
 memcpy(disk+offset,buffer,n);*transferred=n;return true;
}
static bool fat32_write_file(fat_file_t *f,uint64_t offset,const void *buffer,
                                    UINT32 n,UINT32 *transferred){
 bool success=fat32_write_path(NULL,NULL,offset,buffer,n,transferred);
 if(offset+n>f->Size)f->Size=offset+n;return success;
}
static bool fat32_truncate_path(void *f,const char *p,uint64_t size){
 (void)f;(void)p;(void)size;assert(backend_locked);return true;
}
static bool fat32_truncate_file(fat_file_t *f,uint64_t size){
 assert(backend_locked);f->Size=size;return true;
}
static bool fat32_remove_path_typed(void *f,const char *p,int directory){
 assert(directory==-1);
 (void)f;(void)p;assert(!locked&&backend_locked&&!b.fat_file);++removes;
 if(remove_fail)return false;
 exists=false;return true;
}
static bool kmalloc_try_grow(void *p,size_t n){(void)p;(void)n;return false;}
static kstatus_t page_memory_get_stats(uint64_t *total,uint64_t *available){
 *total=1000;*available=750;return K_OK;
}
''' + backend[backend.index('struct {\n    uint64_t failures;'):
              backend.index('static kstatus_t vfs_memory_reserve(')] + '\n'.join(function(backend, signature) for signature in (
    'static kstatus_t vfs_memory_reserve(',
    'kstatus_t vfs_memory_backend_read(',
    'kstatus_t vfs_memory_backend_write(',
    'kstatus_t vfs_memory_backend_truncate(',
    'kstatus_t vfs_fat32_backend_write(',
    'kstatus_t vfs_fat32_backend_truncate(',
    'kstatus_t vfs_fat32_backend_prepare_unlink(',
    'void vfs_fat32_backend_commit_unlink_locked(',
    'void vfs_fat32_backend_abort_unlink(')) + remove + r'''
static void reset(void){
 assert(!live&&!locked&&!backend_locked);
 memset(&b,0,sizeof(b));strcpy(b.path,"/apps/chrome");b.size=8;
 inode.Size=8;
 v=(vnode_t){&b,1};allocations=releases=routes=removes=0;
 path_reads=file_reads=largest_read=0;
 closes=0;close_fail=false;
 resize_during_read=0;
 publish_during_read=false;
 mutate_after_prepare=0;revise_during_read=false;
 allocation_fail=read_fail=short_read=remove_fail=bad_prepare_route=false;exists=true;
}
int main(void){
 for(int mutation=-1;mutation<=2;++mutation){
  if(!mutation)continue;
  reset();b.fat_file=&inode;mutate_after_prepare=mutation;
  assert(vfs_remove_kernel(b.path)==K_EBUSY);
  assert(exists&&!removes&&!b.data&&!b.detached&&!b.unlink_prepared&&!live&&v.refs==1);
 }
 reset();b.fat_file=&inode;revise_during_read=true;
 assert(vfs_remove_kernel(b.path)==K_EBUSY);
 assert(exists&&!removes&&!b.data&&!b.detached&&!live&&v.refs==1);
 reset();assert(vfs_fat32_backend_prepare_unlink(&b)==K_OK);
 uint8_t *prepared=b.data;
 assert(vfs_remove_kernel(b.path)==K_EBUSY);
 assert(exists&&!removes&&b.data==prepared&&!b.detached&&live==1&&allocations==1&&v.refs==1);
 vfs_fat32_backend_abort_unlink(&b);assert(!live);
 reset();b.fat_file=&inode;publish_during_read=true;
 assert(vfs_remove_kernel(b.path)==K_EBUSY);
 assert(exists&&!removes&&b.data&&b.data[0]==77&&!b.detached&&live==1&&allocations==2&&releases==1&&v.refs==1);
 vfs_fat32_backend_abort_unlink(&b);assert(!live);
 for(int resize=1;resize>=-1;resize-=2){
  reset();b.fat_file=&inode;resize_during_read=resize;
  assert(vfs_remove_kernel(b.path)==K_EBUSY);
  assert(exists&&!removes&&!b.data&&!b.detached&&!live&&allocations==1&&releases==1&&v.refs==1);
 }
 reset();allocation_fail=true;b.size=512ULL*1024*1024;
 assert(vfs_remove_kernel(b.path)==K_ENOMEM);
 assert(exists&&!removes&&!b.detached&&!b.data&&!live&&v.refs==1);
 reset();bad_prepare_route=true;
 assert(vfs_remove_kernel(b.path)==K_EINVAL);
 assert(exists&&!removes&&!allocations&&v.refs==1);
 for(unsigned persistent=0;persistent<2;++persistent)
 for(unsigned partial=0;partial<2;++partial){
  reset();if(persistent)b.fat_file=&inode;read_fail=!partial;short_read=partial;
  assert(vfs_remove_kernel(b.path)==K_EIO);
  assert(exists&&!removes&&!b.data&&!b.detached&&!live&&allocations==1&&releases==1&&v.refs==1);
 }
 reset();b.fat_file=&inode;close_fail=true;
 assert(vfs_remove_kernel(b.path)==K_EIO);
 assert(closes==1&&!removes&&b.fat_file==&inode&&exists&&!b.data&&!live&&v.refs==1);
 reset();b.fat_file=&inode;remove_fail=true;
 assert(vfs_remove_kernel(b.path)==K_EIO);
 assert(closes==1&&removes==1&&!b.fat_file&&exists&&!b.data&&!live&&v.refs==1);
 remove_fail=false;assert(vfs_remove_kernel(b.path)==K_OK);
 vfs_fat32_backend_abort_unlink(&b);assert(!live);
 reset();remove_fail=true;
 assert(vfs_remove_kernel(b.path)==K_EIO);
 assert(exists&&!b.data&&!b.detached&&!live&&allocations==1&&releases==1&&v.refs==1);
 remove_fail=false;assert(vfs_remove_kernel(b.path)==K_OK);
 assert(!exists&&b.detached&&live==1&&v.refs==1);
 assert(!memcmp(b.data,disk,8));
 uint8_t old[8];memcpy(old,disk,8);memset(disk,99,8);
 uint8_t readback[8];uint64_t count=0;
 assert(vfs_memory_backend_read(&b,0,readback,8,&count)==K_OK&&count==8);
 assert(!memcmp(readback,old,8));
 assert(vfs_memory_backend_read(&b,8,readback,1,&count)==K_OK&&count==0);
 assert(vfs_memory_backend_read(&b,9,readback,1,&count)==K_EINVAL);
 unsigned before_file_reads=file_reads,before_path_reads=path_reads;
 uint8_t extended[16];
 vfs_backend_lock(&b);
 assert(vfs_fat32_backend_write(&b,8,"updated!",8,&count)==K_OK&&count==8);
 vfs_backend_unlock(&b);
 assert(b.size==16&&live==1&&!exists);
 assert(vfs_memory_backend_read(&b,0,extended,16,&count)==K_OK&&count==16);
 assert(!memcmp(extended,old,8)&&!memcmp(extended+8,"updated!",8));
 vfs_backend_lock(&b);
 assert(vfs_fat32_backend_truncate(&b,3)==K_OK);
 vfs_backend_unlock(&b);
 assert(b.size==3&&live==1&&!exists);
 assert(file_reads==before_file_reads&&path_reads==before_path_reads);
 vfs_fat32_backend_abort_unlink(&b);assert(!live&&!b.data);
 reset();b.size=0;
 assert(vfs_remove_kernel(b.path)==K_OK&&!exists&&b.detached&&!b.data&&!allocations&&v.refs==1);
 for(unsigned persistent=0;persistent<2;++persistent){
  reset();b.size=sizeof(disk);
  if(persistent){b.fat_file=&inode;bad_prepare_route=true;}
  assert(vfs_remove_kernel(b.path)==K_OK&&!exists&&b.detached&&live==1);
  assert(largest_read==1024U*1024U);
  assert(file_reads==(persistent?3U:0U)&&path_reads==(persistent?0U:3U));
  assert(routes==(persistent?1U:2U));
  assert(!memcmp(b.data,disk,sizeof(disk)));
  vfs_fat32_backend_abort_unlink(&b);assert(!live&&!b.data);
 }
 assert(!locked&&!backend_locked&&!live);
 puts("FAT unlink: copy/commit growth, shrink and same-size mutations; locked removal, rollback, detached writes/truncates and 1MiB bounds passed");
}
'''
with tempfile.TemporaryDirectory(prefix='vfs-unlink-snapshot-') as directory:
    binary = str(Path(directory) / 'test')
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
                    '-x', 'c', '-', '-o', binary], input=code, text=True, check=True)
    subprocess.run([binary], check=True)
