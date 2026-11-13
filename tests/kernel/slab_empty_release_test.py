#!/usr/bin/env python3
"""Exercise production empty-slab unlink and ownership safeguards."""
import pathlib, subprocess, tempfile
root=pathlib.Path(__file__).resolve().parents[2]
source=(root/'kernel/mm/kmalloc.c').read_text()
start=source.index('static slab_page_t *cache_partial_head(')
end=source.index('/* Caller holds cache->lock;',start)
mock=r'''
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
typedef struct {int value;} page_t;
typedef struct slab {uint64_t magic;struct slab *next;page_t *page;
 unsigned free_count,slot_count;struct slab **prev_link;void *free_list;} slab_page_t;
typedef struct {slab_page_t *slabs;} kmem_cache_t;
static unsigned frees,errors;
static void record_error(void){++errors;}
static void page_free(page_t *p){assert(p);++frees;}
'''
cases=r'''
int main(void){
 page_t page={0};slab_page_t tail={1,0,&page,3,4,0,0};
 slab_page_t middle={1,&tail,&page,4,4,0,0};
 slab_page_t head={1,&middle,&page,4,4,0,0};kmem_cache_t c={&head};
 head.prev_link=&c.slabs;middle.prev_link=&head.next;tail.prev_link=&middle.next;
 cache_release_empty(&c,&tail);assert(!frees&&tail.magic==1);
 cache_release_empty(&c,&middle);assert(frees==1&&head.next==&tail&&!middle.magic&&tail.prev_link==&head.next);
 cache_release_empty(&c,&head);assert(frees==2&&c.slabs==&tail&&!head.magic);
 tail.free_count=4;cache_release_empty(&c,&tail);assert(frees==3&&!c.slabs);
 cache_release_empty(&c,&head);assert(errors==1&&frees==3);
 head.free_list=&page;tail.free_list=&page;
 cache_partial_insert(&c,&tail);cache_partial_insert(&c,&head);
 assert(cache_partial_head(&c)==&head&&tail.prev_link==&head.next);
 head.free_list=0;cache_partial_remove_head(&c,&head);
 assert(c.slabs==&tail&&tail.prev_link==&c.slabs&&!head.prev_link);
 tail.free_list=0;assert(!cache_partial_head(&c)&&!tail.prev_link&&errors==2);
 slab_page_t *many=calloc(10000,sizeof(*many));assert(many);
 for(unsigned i=0;i<10000;++i){
  many[i].magic=1;many[i].page=&page;many[i].free_list=&page;
  many[i].free_count=4;many[i].slot_count=4;cache_partial_insert(&c,&many[i]);
 }
 for(unsigned i=0;i<10000;i+=2)cache_release_empty(&c,&many[i]);
 slab_page_t **link=&c.slabs;unsigned count=0;
 while(*link){assert((*link)->prev_link==link);++count;link=&(*link)->next;}
 assert(count==5000);
 for(unsigned i=1;i<10000;i+=2)cache_release_empty(&c,&many[i]);
 assert(!c.slabs&&frees==10003&&errors==2);free(many);
 /* 对所有部分计数（包括满载/已分离 slab 归还首个槽位的情况），都要保持链表顺序，
  * 直到最后一个槽位释放页面。 */
 for(unsigned slots=1;slots<=64;++slots)
 for(unsigned available=0;available<slots;++available){
  void *payload=0,*prior=0;unsigned before=frees;
  slab_page_t s={1,0,&page,available,slots,0,available?&prior:0};
  if(available)cache_partial_insert(&c,&s);
  cache_put_payload_locked(&c,&s,&payload);
  assert(payload==(available?&prior:0)&&s.free_count==available+1);
  if(available+1==slots){
   assert(frees==before+1&&!c.slabs&&!s.magic&&!s.prev_link&&!s.next);
  }else{
   assert(frees==before&&c.slabs==&s&&s.prev_link==&c.slabs);
   assert(s.free_list==&payload&&s.magic==1);
   s.free_count=slots;cache_release_empty(&c,&s);
   assert(!c.slabs&&frees==before+1);
  }
 }
 for(unsigned slots=2;slots<=64;++slots)
 for(unsigned position=0;position<3;++position){
  void *payload=0,*prior=0;unsigned before=frees;
  slab_page_t nodes[3];
  for(unsigned i=0;i<3;++i){
   nodes[i]=(slab_page_t){1,0,&page,i==position?slots-1:1,
                         i==position?slots:2,0,&prior};
  }
  for(unsigned i=3;i!=0;--i)cache_partial_insert(&c,&nodes[i-1]);
  cache_put_payload_locked(&c,&nodes[position],&payload);
  assert(frees==before+1&&payload==&prior&&!nodes[position].magic);
  slab_page_t **cursor=&c.slabs;unsigned remaining=0;
  for(unsigned i=0;i<3;++i)if(i!=position){
   assert(*cursor==&nodes[i]&&nodes[i].prev_link==cursor);
   cursor=&nodes[i].next;++remaining;
  }
  assert(!*cursor&&remaining==2);
  for(unsigned i=0;i<3;++i)if(i!=position){
   nodes[i].free_count=2;cache_release_empty(&c,&nodes[i]);
  }
  assert(!c.slabs&&frees==before+3);
 }
 assert(errors==2);
}
'''
with tempfile.TemporaryDirectory(prefix='liteos-slab-release-') as d:
 binary=str(pathlib.Path(d)/'test')
 subprocess.run(['cc','-Wall','-Wextra','-Werror','-x','c','-','-o',binary],
                input=mock+source[start:end]+cases,text=True,check=True)
 subprocess.run([binary],check=True)
assert source.count('cache_release_empty(cache, slab);')==1
assert source.count('cache_put_payload_locked(cache, slab,')==2
print('slab: head/middle/tail release, live-slot retention and missing-link protection passed')
assert 'while (' not in source[source.index('static void cache_release_empty('):end]
print('slab: production insert/remove, 10000-node unlink and backlink invariants passed')
print('Shared return: 2080 partial-count states and 189 head/middle/tail last-slot releases passed')
