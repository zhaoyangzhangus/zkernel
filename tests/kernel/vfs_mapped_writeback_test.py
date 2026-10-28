"""真实映射计数和回写代码：已映射页重复写入不能因上次同步丢失脏位。"""
from pathlib import Path
import resource
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'kernel/fs/vfs/page_cache.c').read_text()
lookup = source[source.index('static uint32_t vfs_page_cache_bucket('):
                source.index('static void vfs_vm_mapping_retain(')]
flush = source[source.index('static bool vfs_page_cache_still_mapped('):
               source.index('kstatus_t vfs_page_cache_flush(vnode_t')]
start = source.index('static void vfs_page_cache_copy_bytes(')
copy_bytes = source[start:source.index('static bool vfs_page_cache_source_alias(', start)]
code = r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define PAGE_SIZE 4096U
#define VFS_PAGE_CACHE_BUCKETS 256U
enum {K_OK,K_EINVAL,K_EOVERFLOW,K_EACCES,K_EIO};
typedef int kstatus_t;
typedef struct {atomic_int mapcount;unsigned char *bytes;} page_t;
typedef struct entry {
 uint64_t index;page_t *page;bool dirty;struct entry *next,*hash_next;uint64_t writeback_generation;
} vfs_cached_page_t;
typedef struct {
 vfs_cached_page_t *head,*buckets[256],**expanded_buckets;
 uint32_t bucket_count;uint64_t entry_count,growth_retry_at,writeback_generation;
} vfs_page_cache_t;
typedef struct {
 void *context;int (*write)(void*,uint64_t,void*,size_t,uint64_t*);
} vfs_backend_node_t;
typedef struct {void *fs_private,*page_cache;uint64_t size;} vnode_t;
static unsigned char memory[3*PAGE_SIZE],disk[3*PAGE_SIZE],input[PAGE_SIZE];
static unsigned locks,writes;static bool fail;
static void *kzalloc(size_t n,int flags){(void)flags;return calloc(1,n);}
static void kfree(void *p){free(p);}
static void *page_to_phys(page_t *p){return p->bytes;}
static void *phys_to_direct(void *p){return p;}
static void vfs_backend_lock(void *b){assert(b&&!locks);++locks;}
static void vfs_backend_unlock(void *b){assert(b&&locks==1);--locks;}
static int write_pages(void *ctx,uint64_t off,void *data,size_t n,uint64_t *written){
 (void)ctx;assert(locks==1&&off+n<=sizeof(disk));++writes;
 *written=n;if(fail)return K_EIO;memcpy(disk+off,data,n);return K_OK;
}
''' + lookup + flush + copy_bytes + r'''
int main(void){
 page_t pages[3];vfs_cached_page_t entries[3]={0};vfs_page_cache_t cache={0};
 vfs_backend_node_t backend={.write=write_pages};vnode_t vnode={&backend,&cache,sizeof(disk)};
 for(unsigned i=0;i<3;++i){
  atomic_init(&pages[i].mapcount,(int)i-1);pages[i].bytes=memory+i*PAGE_SIZE;
  memset(pages[i].bytes,32+i,PAGE_SIZE);
  entries[i]=(vfs_cached_page_t){.index=i,.page=&pages[i],.dirty=true};
  vfs_page_cache_insert(&cache,&entries[i]);
 }
 vfs_page_cache_grow(&cache);
 assert(vfs_page_cache_flush_locked(&vnode,0,3)==K_OK&&writes==1);
 assert(!entries[0].dirty&&entries[1].dirty&&entries[2].dirty);
 for(unsigned round=0;round<20;++round){
  for(unsigned i=1;i<3;++i)memset(pages[i].bytes,64+round+i,PAGE_SIZE);
  assert(vfs_page_cache_flush_locked(&vnode,0,3)==K_OK);
  assert(!memcmp(memory,disk,sizeof(disk))&&entries[1].dirty&&entries[2].dirty);
 }
 /* 同步期间另一个映射仍存在，以及最后一份映射已撤销两种边界。 */
 atomic_fetch_sub(&pages[1].mapcount,1);atomic_fetch_sub(&pages[2].mapcount,1);
 assert(vfs_page_cache_flush_locked(&vnode,0,3)==K_OK);
 assert(!entries[1].dirty&&entries[2].dirty);
 memset(input,0xab,sizeof(input));
 vfs_page_cache_copy_bytes(&entries[2],0,input,sizeof(input));
 assert(entries[2].dirty);
 assert(vfs_page_cache_flush_locked(&vnode,0,3)==K_OK);
 assert(!memcmp(memory,disk,sizeof(disk))&&entries[2].dirty);
 atomic_fetch_sub(&pages[2].mapcount,1);
 memset(pages[2].bytes,0x77,PAGE_SIZE);
 fail=true;assert(vfs_page_cache_flush_locked(&vnode,0,3)==K_EIO&&entries[2].dirty);
 fail=false;assert(vfs_page_cache_flush_locked(&vnode,0,3)==K_OK&&!entries[2].dirty);
 assert(!memcmp(memory,disk,sizeof(disk))&&!locks);
 unsigned before=writes;assert(vfs_page_cache_flush_locked(&vnode,0,3)==K_OK&&writes==before);
 /* 代数回绕时清除旧标记，不能把旧轮次的标记误认为本次已经完成。 */
 cache.writeback_generation=UINT64_MAX;entries[0].writeback_generation=1;
 entries[0].dirty=true;
 assert(vfs_page_cache_flush_locked(&vnode,0,3)==K_OK&&writes==before+1);
 assert(cache.writeback_generation==1&&!entries[0].dirty);
 entries[0].dirty=true;vfs_page_cache_copy_bytes(&entries[0],0,input,sizeof(input));
 assert(!entries[0].dirty);
 puts("mapped writeback: repeated stores/sync, shared aliases, full-page direct updates, final unmap, errors and clean-cache no-op passed");
}
'''
with tempfile.TemporaryDirectory(prefix='vfs-mapped-writeback-') as directory:
    binary = str(Path(directory) / 'test')
    variants = (
        ('current', code, None),
        ('mapped dirt cleared', code.replace(
            'entries[index]->dirty = vfs_page_cache_still_mapped(entries[index]);',
            'entries[index]->dirty = false;'), 'entries[1].dirty'),
        ('same run submitted again', code.replace(
            'entry->writeback_generation == cache->writeback_generation) return K_OK;',
            'false) return K_OK;'), 'writes==1'),
    )
    for name, program, expected_error in variants:
        subprocess.run(['cc', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                        '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
                        '-x', 'c', '-', '-o', binary], input=program, text=True, check=True)
        result = subprocess.run([binary], capture_output=True, text=True, timeout=20,
                                preexec_fn=lambda: resource.setrlimit(resource.RLIMIT_CORE, (0, 0)))
        if expected_error:
            assert result.returncode and expected_error in result.stderr, (name, result)
            print('Negative control rejected:', name)
        else:
            assert result.returncode == 0, result.stderr
            print(result.stdout, end='')
