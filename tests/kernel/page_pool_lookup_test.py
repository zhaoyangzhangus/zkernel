"""Actual pool lookup: first-owner overlap semantics and binary boundary search."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
def function(source, name):
    start = source.index(name)
    return source[start:source.index('\n}', start) + 2]
source = (root / 'kernel/mm/phys/percpu_page.c').read_text()
lookup = function(source, 'static page_pool_group_t *pool_group_for_base(')
lookup = lookup.replace('const page_pool_section_t *section = &g_pool_sections[middle];',
                        '++visits; const page_pool_section_t *section = &g_pool_sections[middle];')
lookup = lookup.replace('const page_pool_section_t *pool_section = &g_pool_sections[i];',
                        '++visits; const page_pool_section_t *pool_section = &g_pool_sections[i];')
source = (root / 'kernel/mm/phys/page_db.c').read_text()
initializer = function(source, 'static void check_pool_section_order(')
program = r'''
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#define PAGE_POOL_GROUP_SHIFT 6
#define PAGE_POOL_GROUP_SLOTS 64
#define PAGE_POOL_SMALL_MAX_ORDER 5
typedef struct { uint64_t group_base_pfn;uint32_t group_first,group_count; } page_pool_section_t;
typedef struct { unsigned marker; } page_pool_group_t;
static page_pool_section_t g_pool_sections[65536];
static page_pool_group_t groups[65536*6];
static page_pool_group_t *g_pool_groups=groups;
static uint32_t g_section_count,g_pool_group_count,g_pool_base_group_count;
static bool g_pool_sections_ordered;
static _Thread_local unsigned visits;
''' + initializer + lookup + r'''
static page_pool_group_t *oracle(uint64_t pfn,unsigned order) {
 if(order>5 || (pfn&((64ULL<<order)-1)))return 0;
 for(unsigned s=0;s<g_section_count;++s) {
  page_pool_section_t *section=&g_pool_sections[s];
  if(pfn<section->group_base_pfn)continue;
  uint64_t delta=pfn-section->group_base_pfn;
  if(delta&63)continue;
  uint64_t index=delta>>6;
  if(index<section->group_count) {
   uint64_t id=order*(uint64_t)g_pool_base_group_count+section->group_first+index;
   return id<g_pool_group_count?&groups[id]:0;
  }
 }
 return 0;
}
static void check(uint64_t pfn,unsigned order) {
 visits=0;assert(pool_group_for_base(pfn,order)==oracle(pfn,order));
 if(g_pool_sections_ordered)assert(visits<=18);
}
static void *reader(void *raw) {
 unsigned seed=(unsigned)(uintptr_t)raw;
 for(unsigned i=0;i<20000;++i) {
  seed=seed*1664525U+1013904223U;unsigned s=seed&65535;
  visits=0;assert(pool_group_for_base(4096+s*2048ULL,0)==&groups[s]);assert(visits<=18);
 }
 return 0;
}
int main(void) {
 check_pool_section_order();check(0,0);
 g_section_count=g_pool_base_group_count=65536;g_pool_group_count=65536*6;
 for(unsigned s=0;s<65536;++s)g_pool_sections[s]=(page_pool_section_t){4096+s*2048ULL,s,1};
 check_pool_section_order();assert(g_pool_sections_ordered);
 for(unsigned s=0;s<65536;++s)for(unsigned order=0;order<6;++order) {
  visits=0;assert(pool_group_for_base(4096+s*2048ULL,order)==&groups[order*65536+s]);assert(visits<=18);
 }
 visits=0;assert(pool_group_for_base(4096+65535*2048ULL,0)==&groups[65535]);unsigned last=visits;
 pthread_t threads[8];
 for(unsigned i=0;i<8;++i)assert(!pthread_create(&threads[i],0,reader,(void *)(uintptr_t)(i+1)));
 for(unsigned i=0;i<8;++i)assert(!pthread_join(threads[i],0));
 unsigned seed=19;
 for(unsigned layout=0;layout<1000;++layout) {
  g_section_count=1+layout%64;uint64_t base=0,end=0;unsigned first=0;
  for(unsigned s=0;s<g_section_count;++s) {
   seed=seed*1664525U+1013904223U;
   base+=seed%4*64;uint64_t next=base+(1+(seed>>16)%4)*64;
   if(next<end)next=end;
   unsigned count=(unsigned)((next-base)>>6);
   g_pool_sections[s]=(page_pool_section_t){base,first,count};first+=count;end=next;
  }
  g_pool_base_group_count=first;g_pool_group_count=first*6;
  check_pool_section_order();assert(g_pool_sections_ordered);
  for(uint64_t p=0;p<=end+64;p+=64)for(unsigned order=0;order<6;++order)check(p,order);
  if(g_section_count>1) {
   page_pool_section_t temp=g_pool_sections[0];g_pool_sections[0]=g_pool_sections[g_section_count-1];
   g_pool_sections[g_section_count-1]=temp;check_pool_section_order();
   for(uint64_t p=0;p<=end+64;p+=64)for(unsigned order=0;order<6;++order)check(p,order);
  }
 }
 // 起点/终点重复时必须选择第一个所有者，即使其 ID 无效也一样。
 g_section_count=3;g_pool_base_group_count=8;g_pool_group_count=48;
 g_pool_sections[0]=(page_pool_section_t){64,0,3};
 g_pool_sections[1]=(page_pool_section_t){64,3,3};
 g_pool_sections[2]=(page_pool_section_t){192,6,2};
 check_pool_section_order();assert(g_pool_sections_ordered);
 for(unsigned p=0;p<512;++p)for(unsigned order=0;order<6;++order)check(p,order);
 g_pool_sections[0].group_first=99;check(192,0);
 g_pool_sections[0].group_count=0;check_pool_section_order();assert(!g_pool_sections_ordered);check(192,0);
 g_pool_sections[0]=(page_pool_section_t){UINT64_MAX-63,0,1};
 check_pool_section_order();assert(!g_pool_sections_ordered);check(UINT64_MAX-63,0);
 g_pool_sections[0]=(page_pool_section_t){65,0,1};
 check_pool_section_order();assert(!g_pool_sections_ordered);check(64,0);
 check(0,6);check(0,64);check(0,255);
 printf("PASS: pool first-owner/gaps/overlaps/duplicate endpoints/invalid IDs and orders, 1000 oracle maps, 160000 concurrent reads; last-section visits 65536 -> %u\n",last);
}
'''
with tempfile.TemporaryDirectory(prefix='liteos-pool-lookup-') as directory:
    binary = str(Path(directory) / 'test')
    subprocess.run(['cc', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                    '-pthread', '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
                    '-x', 'c', '-', '-o', binary], input=program, text=True, check=True)
    subprocess.run([binary], check=True, timeout=30)
