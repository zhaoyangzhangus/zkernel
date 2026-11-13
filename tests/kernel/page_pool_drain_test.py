#!/usr/bin/env python3
"""Verify production drain reaches free groups behind a partial head."""
import pathlib
import subprocess
import tempfile
root = pathlib.Path(__file__).resolve().parents[2]
source = (root / 'kernel/mm/phys/percpu_page.c').read_text()
begin = source.index('static void pool_drain_one(')
function = source[begin:source.index('void pool_drain_cpu(', begin)]
code = r'''
#include <assert.h>
#include <stdint.h>
#include <stdatomic.h>
#include <stdio.h>
#define PAGE_POOL_GROUP_NONE UINT32_MAX
#define PAGE_POOL_GROUP_SHIFT 6
typedef struct { unsigned dummy; } page_t;
typedef struct { page_t *base;uint64_t free_mask;uint32_t partial_head; } page_local_pool_t;
typedef struct {uint64_t base_pfn,free_mask;atomic_uint_fast64_t remote_mask;
 uint32_t partial_next;uint16_t owner_cpu;uint8_t pool_order,flags;
} page_pool_group_t;
static page_local_pool_t pool;
static page_pool_group_t groups[3];
static page_t pages[3];
static uint64_t pinned[3];
static unsigned freed,locked;
static page_local_pool_t *pool_for_cpu(unsigned c,unsigned z,unsigned o){
 assert(c==0 && z==0 && o==1);return &pool;
}
static void pool_drain_remote(unsigned c,unsigned z,unsigned o,page_local_pool_t*p){
 (void)c;(void)z;(void)o;(void)p;
}
typedef struct {uint64_t value;} phys_t;
static phys_t page_to_phys(page_t*p){return (phys_t){(uint64_t)(p-pages)<<12};}
#define PAGE_SHIFT 12
static page_pool_group_t *pool_group_for_base(uint64_t p,unsigned o){(void)o;return &groups[p];}
static uint32_t pool_group_id(page_pool_group_t*g){return g-groups;}
static page_pool_group_t *pool_group_by_id(uint32_t id){return id<3?&groups[id]:0;}
static uint64_t pool_group_pinned_mask(uint64_t p,unsigned o,uint64_t candidates){
 (void)o;assert(candidates==groups[p].free_mask);return pinned[p]&candidates;
}
static page_t *page_for_pfn(uint64_t p){return &pages[p];}
static void pool_partial_push(page_local_pool_t*p,uint32_t id,page_pool_group_t*g){
 assert(!g->flags);g->partial_next=p->partial_head;p->partial_head=id;g->flags=1;
}
static uint32_t pool_partial_pop(page_local_pool_t*p){
 uint32_t id=p->partial_head;if(id==UINT32_MAX)return id;
 p->partial_head=groups[id].partial_next;groups[id].flags=0;return id;
}
static void lock_pages(void){assert(!locked);locked=1;}
static void unlock_pages(void){assert(locked);locked=0;}
static void free_block_locked(page_t*p,unsigned order){
 assert(locked && order==7 && p==&pages[1]);++freed;
}
''' + function + r'''
int main(void){
 pool.partial_head=0;
 for(unsigned i=0;i<3;++i){groups[i].base_pfn=i;groups[i].flags=1;
 groups[i].partial_next=i==2?UINT32_MAX:i+1;groups[i].owner_cpu=0;groups[i].pool_order=1;}
 groups[0].free_mask=1;groups[1].free_mask=UINT64_MAX;groups[2].free_mask=3;
 pool_drain_one(0,0,1);
 assert(freed==1 && groups[1].owner_cpu==UINT16_MAX && !locked);
 assert(pool.partial_head==2 && groups[2].partial_next==0 && groups[0].partial_next==UINT32_MAX);
 assert(groups[0].flags && groups[2].flags);
 pool_drain_one(0,0,1);assert(freed==1);
 /* 尾部页面仍被固定的完整组不能归还给 Buddy。在非末尾槽位固定页面时，也不能改变
  * 无关部分组的空闲掩码。 */
 pool.partial_head=0;
 for(unsigned i=0;i<3;++i){groups[i].flags=1;groups[i].free_mask=i==1?UINT64_MAX:3;
 groups[i].owner_cpu=0;groups[i].pool_order=1;
 groups[i].partial_next=i==2?UINT32_MAX:i+1;}
 pinned[0]=4;pinned[1]=8;pinned[2]=1;
 pool_drain_one(0,0,1);
 assert(freed==1&&groups[0].free_mask==3&&groups[1].free_mask==(UINT64_MAX&~8ULL));
 assert(groups[2].free_mask==2&&groups[1].owner_cpu==0);
 pool_drain_one(0,0,1);assert(freed==1);
 puts("pool drain: partial survivors retained, free group behind head reclaimed");
}
'''
with tempfile.TemporaryDirectory(prefix='liteos-pool-drain-') as directory:
    binary = str(pathlib.Path(directory) / 'test')
    subprocess.run(['cc', '-Wall', '-Wextra', '-Werror', '-x', 'c', '-', '-o', binary], input=code, text=True, check=True)
    subprocess.run([binary], check=True)
