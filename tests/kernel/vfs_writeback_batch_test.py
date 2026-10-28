"""执行真实回写代码，核对数据、范围、借用地址与失败重试。"""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'kernel/fs/vfs/page_cache.c').read_text()
lookup = source[source.index('static uint32_t vfs_page_cache_bucket('):
                source.index('static void vfs_vm_mapping_retain(')]
flush = source[source.index('static kstatus_t vfs_page_cache_flush_entry('):
               source.index('kstatus_t vfs_page_cache_flush(vnode_t')]
code = r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define PAGE_SIZE 4096U
#define VFS_PAGE_CACHE_BUCKETS 256U
enum { K_OK, K_EINVAL, K_EOVERFLOW, K_EACCES, K_EIO };
typedef int kstatus_t;
typedef struct entry {
 uint64_t index; void *page; bool dirty; struct entry *next,*hash_next;uint64_t writeback_generation;
} vfs_cached_page_t;
typedef struct {
 vfs_cached_page_t *head,*buckets[256],**expanded_buckets;
 uint32_t bucket_count; uint64_t entry_count,growth_retry_at,writeback_generation;
} vfs_page_cache_t;
typedef struct {
 void *context; int (*write)(void*,uint64_t,void*,size_t,uint64_t*);
} vfs_backend_node_t;
typedef struct {void *fs_private,*page_cache;uint64_t size;} vnode_t;
static unsigned char memory[64*PAGE_SIZE],disk[64*PAGE_SIZE],expected[64*PAGE_SIZE];
static vfs_cached_page_t entries[64];
static vfs_page_cache_t cache;
static unsigned calls,depth,short_write,fail_at;
static uint64_t allowed_start,allowed_end;
static void *kzalloc(size_t n,int flags){(void)flags;return calloc(1,n);}
static void kfree(void *p){free(p);}
static void *page_to_phys(void *p){return p;}
static bool vfs_page_cache_still_mapped(vfs_cached_page_t *entry){(void)entry;return false;}
static void *phys_to_direct(void *p){return p;}
static void vfs_backend_lock(void *b){assert(b&&!depth);++depth;}
static void vfs_backend_unlock(void *b){assert(b&&depth==1);--depth;}
static int write_pages(void *ctx,uint64_t off,void *data,size_t n,uint64_t *written){
 (void)ctx;assert(depth==1);++calls;
 assert(n&&n<=16*PAGE_SIZE&&off>=allowed_start&&off+n<=allowed_end);
 /* 直接传入首个缓存页，禁止用拷贝缓冲伪装合并 I/O。 */
 assert(data==entries[off/PAGE_SIZE].page);
 for(size_t j=0;j<n;++j){
  uint64_t p=(off+j)/PAGE_SIZE;
  assert(entries[p].dirty);
  assert(((unsigned char*)data)[j]==((unsigned char*)entries[p].page)[(off+j)%PAGE_SIZE]);
 }
 *written=short_write?n-1:n;
 if(calls==fail_at)return K_EIO;
 memcpy(disk+off,data,*written);
 return K_OK;
}
''' + lookup + flush + r'''
static vfs_backend_node_t backend={.write=write_pages};
static vnode_t vnode={&backend,&cache,sizeof(disk)};
static void reset(unsigned order,unsigned physical,unsigned holes){
 memset(&cache,0,sizeof(cache));memset(entries,0,sizeof(entries));
 memset(disk,0x11,sizeof(disk));memset(expected,0x11,sizeof(expected));
 for(unsigned i=0;i<64;++i){
  unsigned slot=physical==0?i:physical==1?63-i:(i*17)%64;
  entries[i].index=i;entries[i].page=memory+slot*PAGE_SIZE;
  entries[i].dirty=holes!=1||i%5!=2;
  memset(entries[i].page,(int)(i+32),PAGE_SIZE);
 }
 for(unsigned i=0;i<64;++i){
  unsigned index=order==0?i:order==1?63-i:(i*13)%64;
  if(holes==2&&index%5==2)continue;
  vfs_page_cache_insert(&cache,&entries[index]);
 }
 calls=depth=short_write=fail_at=0;backend.write=write_pages;
 vnode.size=sizeof(disk);allowed_start=0;allowed_end=vnode.size;
 /* 小缓存也覆盖索引遍历，不依赖测试用桶容量碰巧选择链表。 */
 vfs_page_cache_grow(&cache);
}
static void oracle(unsigned first,unsigned count){
 allowed_start=(uint64_t)first*PAGE_SIZE;
 uint64_t end=(uint64_t)(first+count)*PAGE_SIZE;
 allowed_end=end<vnode.size?end:vnode.size;
 for(unsigned i=first;i<first+count;++i){
  if(!vfs_page_cache_lookup(&cache,i)||!entries[i].dirty)continue;
  uint64_t off=(uint64_t)i*PAGE_SIZE;
  if(off>=vnode.size)continue;
  size_t n=vnode.size-off<PAGE_SIZE?(size_t)(vnode.size-off):PAGE_SIZE;
  memcpy(expected+off,entries[i].page,n);
 }
}
int main(void){
 unsigned cases=0;
 const unsigned lengths[]={1,2,15,16,17,31,32,64};
 for(unsigned order=0;order<3;++order)for(unsigned phys=0;phys<3;++phys)
 for(unsigned holes=0;holes<3;++holes)for(unsigned first=0;first<2;++first)
 for(unsigned k=0;k<8;++k)for(unsigned tail=0;tail<2;++tail){
  unsigned count=lengths[k];if(first+count>64)continue;
  reset(order,phys,holes);
  if(tail)vnode.size=(uint64_t)(first+count-1)*PAGE_SIZE+17;
  oracle(first,count);
  assert(vfs_page_cache_flush_locked(&vnode,first,count)==K_OK);
  assert(!memcmp(disk,expected,sizeof(disk))&&!depth);
  for(unsigned i=0;i<64;++i)if(vfs_page_cache_lookup(&cache,i)){
   if(i>=first&&i<first+count)assert(!entries[i].dirty);
   else assert(entries[i].dirty==(holes!=1||i%5!=2));
  }
  if(order<2&&!phys&&!holes)assert(calls==(count+15)/16);
  ++cases;
 }
 /* 首组短写或报错时整组保持脏位；重试恢复完整数据。 */
 for(unsigned order=0;order<3;++order)for(unsigned kind=0;kind<2;++kind){
  reset(order,0,0);oracle(0,64);
  short_write=kind;fail_at=kind?0:1;
  assert(vfs_page_cache_flush_locked(&vnode,0,64)==K_EIO&&calls==1&&!depth);
  for(unsigned i=0;i<64;++i)assert(entries[i].dirty);
  short_write=fail_at=0;
  assert(vfs_page_cache_flush_locked(&vnode,0,64)==K_OK);
  assert(!memcmp(disk,expected,sizeof(disk)));
 }
 /* 已完成的组不重写，失败组和未访问组仍可重试。 */
 reset(0,0,0);oracle(0,64);fail_at=2;
 assert(vfs_page_cache_flush_locked(&vnode,0,64)==K_EIO&&calls==2);
 unsigned cleaned=0;for(unsigned i=0;i<64;++i)cleaned+=!entries[i].dirty;
 assert(cleaned==16);fail_at=0;calls=0;
 assert(vfs_page_cache_flush_locked(&vnode,0,64)==K_OK&&calls==3);
 assert(!memcmp(disk,expected,sizeof(disk)));
 reset(1,0,0);oracle(0,64);
 assert(vfs_page_cache_flush_locked(&vnode,0,UINT64_MAX)==K_OK&&calls==4);
 assert(!memcmp(disk,expected,sizeof(disk)));
 reset(0,0,0);oracle(0,64);backend.write=0;
 assert(vfs_page_cache_flush_locked(&vnode,0,64)==K_EACCES&&!calls&&!depth);
 for(unsigned i=0;i<64;++i)assert(entries[i].dirty);
 printf("writeback: %u range/data/order/physical/hole/EOF cases; 64 contiguous pages -> 4 borrowed-buffer writes; failed/short groups retain dirt and retry\n",cases);
}
'''
with tempfile.TemporaryDirectory(prefix='vfs-writeback-batch-') as directory:
    binary = str(Path(directory) / 'test')
    subprocess.run(['cc', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
                    '-x', 'c', '-', '-o', binary], input=code, text=True, check=True)
    subprocess.run([binary], check=True, timeout=30)
