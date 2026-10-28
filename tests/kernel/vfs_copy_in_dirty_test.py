"""Production cache update/writeback: partial writes must retain mmap dirt."""
from pathlib import Path
import resource
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root/'kernel/fs/vfs/page_cache.c').read_text()
lookup = source[source.index('static uint32_t vfs_page_cache_bucket('):
                source.index('static void vfs_vm_mapping_retain(')]
flush = source[source.index('static kstatus_t vfs_page_cache_flush_entry('):
               source.index('static kstatus_t vfs_page_cache_flush_locked(')]
copy_in = source[source.index('static void vfs_page_cache_copy_bytes('):]
lookup = lookup.replace('    if (cache == 0) return 0;',
                        '    ++lookups; if (cache == 0) return 0;')
assert '++lookups;' in lookup
code = r'''
#include <assert.h>
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define PAGE_SIZE 4096U
#define VFS_PAGE_CACHE_BUCKETS 256U
typedef int kstatus_t;
enum {K_OK, K_EINVAL, K_EACCES, K_EIO};
typedef struct entry {uint64_t index;void *page;bool dirty;
 struct entry *next,*hash_next;uint64_t writeback_generation;} vfs_cached_page_t;
typedef struct {vfs_cached_page_t *head,*buckets[256],**expanded_buckets;
 uint32_t bucket_count;uint64_t entry_count,growth_retry_at,writeback_generation;} vfs_page_cache_t;
typedef struct {void *context;int (*write)(void*,uint64_t,void*,size_t,uint64_t*);} vfs_backend_node_t;
typedef struct {void *fs_private,*page_cache;uint64_t size;} vnode_t;
static unsigned vnode_depth,backend_depth,fail,short_write,writes,lookups;
static uint8_t disk[3*PAGE_SIZE];
static void *kzalloc(size_t n,int flags) {(void)flags;return calloc(1,n);}
static void kfree(void *p) {free(p);}
static void vfs_vnode_lock(vnode_t *v) {(void)v;assert(!vnode_depth);++vnode_depth;}
static void vfs_vnode_unlock(vnode_t *v) {(void)v;assert(vnode_depth==1);--vnode_depth;}
static void vfs_backend_lock(void *v) {(void)v;assert(vnode_depth==1 && !backend_depth);++backend_depth;}
static void vfs_backend_unlock(void *v) {(void)v;assert(backend_depth==1);--backend_depth;}
static void *page_to_phys(void *p) {return p;}
static bool vfs_page_cache_still_mapped(vfs_cached_page_t *entry) {(void)entry;return false;}
static void *phys_to_direct(void *p) {assert(vnode_depth==1);return p;}
static int write_page(void *ctx,uint64_t off,void *p,size_t n,uint64_t *written) {
 (void)ctx;assert(backend_depth==1 && off+n<=sizeof(disk));++writes;
 *written=short_write?n-1:n;
 if(fail)return K_EIO;
 memcpy(disk+off,p,*written);return K_OK;
}
''' + lookup + flush + copy_in + r'''
int main(void) {
 uint8_t memory[sizeof(disk)],expected[sizeof(disk)],input[sizeof(disk)];
 memset(input,0x77,sizeof(input));
 vfs_cached_page_t entries[3]={0};vfs_page_cache_t cache={0};
 vfs_backend_node_t backend={.write=write_page};
 vnode_t vnode={&backend,&cache,sizeof(disk)};
 for(unsigned i=0;i<3;++i) {
  entries[i].index=i;entries[i].page=memory+i*PAGE_SIZE;
  vfs_page_cache_insert(&cache,&entries[i]);vfs_page_cache_grow(&cache);
 }
 size_t lengths[]={1,17,PAGE_SIZE-1,PAGE_SIZE,PAGE_SIZE+1,2*PAGE_SIZE};
 unsigned cases=0;
 for(unsigned clean=0;clean<2;++clean) {
  for(size_t off=0;off<=PAGE_SIZE;++off) {
   for(unsigned l=0;l<sizeof(lengths)/sizeof(lengths[0]);++l) {
    size_t n=lengths[l];memset(disk,0x11,sizeof(disk));
    memset(memory,clean?0x11:0x22,sizeof(memory));memcpy(expected,memory,sizeof(memory));
    memset(expected+off,0x77,n);memcpy(disk+off,input,n);
    for(unsigned i=0;i<3;++i)entries[i].dirty=!clean;
    vfs_page_cache_copy_in(&vnode,off,input,n);assert(!vnode_depth);
    assert(!memcmp(memory,expected,sizeof(memory)));
    for(unsigned i=0;i<3;++i) {
     size_t base=i*PAGE_SIZE;
     bool full=off<=base && off+n>=base+PAGE_SIZE;
     assert(entries[i].dirty==(!clean && !full));
     vfs_vnode_lock(&vnode);
     assert(vfs_page_cache_flush_entry(&vnode,&backend,&entries[i],i,i)==K_OK);
     vfs_vnode_unlock(&vnode);
    }
    assert(!memcmp(disk,expected,sizeof(disk)));++cases;
   }
  }
 }
 /* Failed and short writeback cannot consume the retained dirty mark. */
 entries[0].dirty=true;vfs_page_cache_copy_in(&vnode,5,input,1);
 vfs_vnode_lock(&vnode);fail=1;
 assert(vfs_page_cache_flush_entry(&vnode,&backend,&entries[0],0,0)==K_EIO && entries[0].dirty);
 fail=0;short_write=1;
 assert(vfs_page_cache_flush_entry(&vnode,&backend,&entries[0],0,0)==K_EIO && entries[0].dirty);
 short_write=0;
 assert(vfs_page_cache_flush_entry(&vnode,&backend,&entries[0],0,0)==K_OK && !entries[0].dirty);
 vfs_vnode_unlock(&vnode);
 /* No destination means no successful full-page replacement. */
 entries[0].page=0;entries[0].dirty=true;
 vfs_page_cache_copy_in(&vnode,0,input,PAGE_SIZE);assert(entries[0].dirty);
 /* Covering the valid EOF prefix is not a full-page replacement. Keeping
  * the mark is conservative; writeback still persists only valid bytes. */
 entries[0].page=memory;vnode.size=17;
 memset(disk,0x11,sizeof(disk));memset(memory,0x22,sizeof(memory));
 memcpy(disk,input,17);vfs_page_cache_copy_in(&vnode,0,input,17);
 assert(entries[0].dirty && vnode.size==17);
 vfs_vnode_lock(&vnode);
 assert(vfs_page_cache_flush_entry(&vnode,&backend,&entries[0],0,0)==K_OK);
 vfs_vnode_unlock(&vnode);
 assert(!entries[0].dirty && !memcmp(disk,input,17));
 for(unsigned i=17;i<sizeof(disk);++i)assert(disk[i]==0x11);
 vnode.size=7;entries[1].dirty=true;
 vfs_page_cache_copy_in(&vnode,PAGE_SIZE+3,input,17);
 assert(vnode.size==PAGE_SIZE+20 && entries[1].dirty);
 uint64_t size=vnode.size;vfs_page_cache_copy_in(&vnode,UINT64_MAX-3,input,8);
 assert(vnode.size==size && !vnode_depth && !backend_depth);
 free(cache.expanded_buckets);
 /* Large independent writes visit only resident entries, including holes,
  * partial edge pages, absent destinations and indices outside the range. */
 const unsigned resident[]={0,1,31,511,2047,2048};
 const size_t wide=8U*1024*1024;
 uint8_t *large=malloc(wide+PAGE_SIZE);assert(large);
 for(size_t i=0;i<wide+PAGE_SIZE;++i)large[i]=(uint8_t)(i*17+i/PAGE_SIZE);
 uint8_t sparse_data[6][PAGE_SIZE];vfs_cached_page_t sparse_entries[6];
 unsigned sparse_cases=0;
 for(unsigned absent=0;absent<2;++absent)
 for(unsigned start=0;start<3;++start)for(unsigned tail=0;tail<3;++tail){
  size_t off=start==0?0:start==1?3:PAGE_SIZE+7;
  size_t n=wide-off-tail;vfs_page_cache_t sparse={0};
  for(unsigned i=0;i<6;++i){
   memset(sparse_data[i],0x35,PAGE_SIZE);
   sparse_entries[i]=(vfs_cached_page_t){.index=resident[i],
      .page=absent && i==1?NULL:sparse_data[i],.dirty=true};
   vfs_page_cache_insert(&sparse,&sparse_entries[i]);
  }
  vnode_t sv={&backend,&sparse,0};lookups=0;
  vfs_page_cache_copy_in(&sv,off,large,n);
  assert(sv.size==off+n && !lookups && !vnode_depth);
  for(unsigned i=0;i<6;++i){
   uint64_t base=(uint64_t)resident[i]*PAGE_SIZE;
   bool full=off<=base && off+n>=base+PAGE_SIZE;
   assert(sparse_entries[i].dirty==(!full || (absent && i==1)));
   for(unsigned j=0;j<PAGE_SIZE;++j){
    uint64_t pos=base+j;
    uint8_t value=(!(absent && i==1) && pos>=off && pos<off+n)?large[pos-off]:0x35;
    assert(sparse_data[i][j]==value);
   }
  }
  ++sparse_cases;
 }
 /* Preserve ascending per-page memmove semantics when source bytes are
  * themselves cached destination bytes. Reverse resident-list order would
  * silently change which bytes later pages consume. */
 uint8_t alias[40*PAGE_SIZE],oracle[sizeof(alias)];
 const unsigned indices[]={2,3,20},slots[]={1,2,19};
 for(unsigned shift=0;shift<3;++shift){
  for(unsigned i=0;i<sizeof(alias);++i)alias[i]=(uint8_t)(i*11+i/PAGE_SIZE);
  memcpy(oracle,alias,sizeof(alias));vfs_page_cache_t ac={0};
  vfs_cached_page_t ae[3];
  for(unsigned i=0;i<3;++i){
   ae[i]=(vfs_cached_page_t){.index=indices[i],.page=alias+slots[i]*PAGE_SIZE,.dirty=true};
   vfs_page_cache_insert(&ac,&ae[i]);
  }
  size_t off=2*PAGE_SIZE+shift,n=20*PAGE_SIZE;
  for(size_t copied=0;copied<n;){
   size_t current=off+copied,in=current%PAGE_SIZE,chunk=PAGE_SIZE-in;
   if(chunk>n-copied)chunk=n-copied;
   for(unsigned i=0;i<3;++i)if(indices[i]==current/PAGE_SIZE)
    memmove(oracle+slots[i]*PAGE_SIZE+in,oracle+copied,chunk);
   copied+=chunk;
  }
  vnode_t av={&backend,&ac,0};lookups=0;
  vfs_page_cache_copy_in(&av,off,alias,n);
  assert(!memcmp(alias,oracle,sizeof(alias)) && lookups==20U+(shift!=0));
  assert(av.size==off+n && !vnode_depth);
 }
 vfs_page_cache_t empty={0};vnode_t ev={&backend,&empty,0};lookups=0;
 vfs_page_cache_copy_in(&ev,0,large,wide);
 assert(ev.size==wide && !lookups && !vnode_depth);
 /* Exercise both adaptive branches around 16 pages, the density cutoff,
  * inline/expanded hash buckets and large file offsets. The oracle uses
  * byte intervals independently of the production traversal. */
 const unsigned counts[]={0,1,6,68,1025},spans[]={1,16,17,256,257};
 const unsigned shifts[]={0,3,PAGE_SIZE-1};unsigned adaptive_cases=0;
 for(unsigned c=0;c<5;++c)for(unsigned high=0;high<2;++high){
  unsigned count=counts[c];vfs_page_cache_t dc={0};
  vfs_cached_page_t *de=calloc(count+1,sizeof(*de));
  uint8_t *dm=malloc((count+1)*PAGE_SIZE);assert(de && dm);
  uint64_t base_index=high?UINT64_MAX/PAGE_SIZE-4096:0;
  for(unsigned i=0;i<count;++i){
   de[i]=(vfs_cached_page_t){.index=base_index+(uint64_t)i*3,.page=dm+i*PAGE_SIZE};
   vfs_page_cache_insert(&dc,&de[i]);vfs_page_cache_grow(&dc);
  }
  if(count==1025)assert(dc.expanded_buckets && dc.bucket_count==512);
  for(unsigned span=0;span<5;++span)for(unsigned shift=0;shift<3;++shift){
   uint64_t off=base_index*PAGE_SIZE+shifts[shift];size_t n=spans[span]*PAGE_SIZE;
   uint64_t end=off+n,pages=(end-1)/PAGE_SIZE-off/PAGE_SIZE+1;
   bool indexed=pages<=16 || (pages<=(dc.bucket_count?dc.bucket_count:256) && pages<=count/4);
   memset(dm,0x35,(count+1)*PAGE_SIZE);
   for(unsigned i=0;i<count;++i)de[i].dirty=true;
   vnode_t dv={&backend,&dc,end+17};lookups=0;
   vfs_page_cache_copy_in(&dv,off,large,n);
   assert(dv.size==end+17 && lookups==(indexed?pages:0) && !vnode_depth);
   for(unsigned i=0;i<count;++i){
    uint64_t base=de[i].index*PAGE_SIZE;
    assert(de[i].dirty==!(off<=base && base<end && end-base>=PAGE_SIZE));
    for(unsigned j=0;j<PAGE_SIZE;++j){
     uint64_t pos=base+j;uint8_t expected=pos>=off && pos<end?large[pos-off]:0x35;
     assert(dm[i*PAGE_SIZE+j]==expected);
    }
   }
   ++adaptive_cases;
  }
  free(dc.expanded_buckets);free(de);free(dm);
 }
 /* Last valid file page: end == UINT64_MAX leaves its final byte outside
  * the write. Even invalid cache indices must not wrap to file offset 0. */
 for(unsigned edge=0;edge<2;++edge){
  uint8_t top_data[2][PAGE_SIZE];memset(top_data,0x35,sizeof(top_data));
  vfs_page_cache_t tc={0};vfs_cached_page_t te[2];
  for(unsigned i=0;i<2;++i){
   te[i]=(vfs_cached_page_t){.index=UINT64_MAX/PAGE_SIZE+i,.page=top_data[i],.dirty=true};
   vfs_page_cache_insert(&tc,&te[i]);
  }
  size_t n=(edge?17:1)*PAGE_SIZE;uint64_t off=UINT64_MAX-n;
  vnode_t tv={&backend,&tc,0};lookups=0;
  vfs_page_cache_copy_in(&tv,off,large,n);
  assert(tv.size==UINT64_MAX && !vnode_depth);
  for(unsigned j=0;j<PAGE_SIZE;++j){
   assert(top_data[0][j]==(j<PAGE_SIZE-1?large[n-(PAGE_SIZE-1)+j]:0x35));
   assert(top_data[1][j]==0x35);
  }
  assert(te[0].dirty && te[1].dirty);
 }
 /* Independent interval oracle for both sides of the alias subtraction,
  * including exact adjacency (not overlap) and a missing destination. */
 const unsigned memory_offsets[]={0,PAGE_SIZE,10*PAGE_SIZE,30*PAGE_SIZE};
 const unsigned source_offsets[]={0,1,PAGE_SIZE-1,PAGE_SIZE,PAGE_SIZE+1,
                                   10*PAGE_SIZE-1,10*PAGE_SIZE,31*PAGE_SIZE};
 const unsigned source_lengths[]={1,PAGE_SIZE-1,PAGE_SIZE,PAGE_SIZE+1,4*PAGE_SIZE};
 unsigned alias_cases=0;
 for(unsigned missing=0;missing<2;++missing)
 for(unsigned m=0;m<4;++m)for(unsigned s=0;s<8;++s)for(unsigned l=0;l<5;++l){
  unsigned mem=memory_offsets[m],src=source_offsets[s],n=source_lengths[l];
  vfs_cached_page_t entry={.page=missing?NULL:large+mem};
  vfs_page_cache_t single={.head=&entry};vnode_t view={&backend,&single,0};
  bool expected=!missing && mem<src+n && src<mem+PAGE_SIZE;
  vfs_vnode_lock(&view);
  assert(vfs_page_cache_source_alias(&single,large+src,n)==expected);
  vfs_vnode_unlock(&view);++alias_cases;
 }
 free(large);
 printf("Alias intervals: %u source-before/inside/after, adjacent and absent-page oracle cases passed\n",alias_cases);
 printf("Adaptive copy-in: %u index/list boundary and high-offset byte-oracle cases; expanded buckets and final-page overflow boundaries passed\n",adaptive_cases);
 printf("Sparse copy-in: %u 8MiB edge/hole/missing-page cases, zero hash probes instead of 2048; aliased writes retain ascending oracle order\n",sparse_cases);
 printf("Cache copy-in: %u partial/full/multipage clean+dirty cases, writeback data, failure retry and missing destination passed\n",cases);
}
'''
with tempfile.TemporaryDirectory(prefix='liteos-copy-in-dirty-') as directory:
    binary = str(Path(directory)/'test')
    subprocess.run(['cc','-std=c11','-O2','-Wall','-Wextra','-Werror',
                    '-fsanitize=address,undefined','-fno-pie','-no-pie',
                    '-x','c','-','-o',binary],input=code,text=True,check=True)
    subprocess.run([binary],check=True,timeout=60)
    for name,old,new in (
        ('wide writes probe every absent page',
         'if (!vfs_page_cache_use_range_index(cache, count) &&',
         'if (false && !vfs_page_cache_use_range_index(cache, count) &&'),
        ('alias guard missing',
         '!vfs_page_cache_source_alias(cache, source, length)',
         '(!vfs_page_cache_source_alias(cache, source, length) || true)'),
    ):
        negative=code.replace(old,new);assert negative!=code
        subprocess.run(['cc','-std=c11','-O2','-Wall','-Wextra','-Werror',
                        '-fsanitize=address,undefined','-fno-pie','-no-pie',
                        '-x','c','-','-o',binary],input=negative,text=True,check=True)
        result=subprocess.run([binary],capture_output=True,text=True,timeout=60,
                              preexec_fn=lambda: resource.setrlimit(resource.RLIMIT_CORE,(0,0)))
        assert result.returncode!=0 and 'Assertion' in result.stderr,(name,result)
        print('Negative control rejected:',name)
