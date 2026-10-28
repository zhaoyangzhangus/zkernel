"""Production compound marking: section spans, holes and exact metadata state."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root/'kernel/mm/phys/page_db.c').read_text()
lookup = source[source.index('static page_t *section_page('):source.index('static uint8_t page_zone(')]
lookup = lookup.replace('page_section_t *section = &g_sections[i];',
                        '++visits; page_section_t *section = &g_sections[i];')
mark = source[source.index('void mark_compound('):source.index('static bool block_is_free(')]
pool_source = (root/'kernel/mm/phys/percpu_page.c').read_text()
pinned = pool_source[pool_source.index('static uint64_t pool_group_pinned_mask('):
                     pool_source.index('static page_local_pool_t *pool_for_cpu(')]
prepare = pool_source[pool_source.index('static void pool_prepare_group_slots('):
                      pool_source.index('static void pool_partial_push(')]
code = r'''
#include <assert.h>
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#define PAGE_SIZE 4096ULL
#define PAGE_SHIFT 12
#define PAGE_FREE 1
#define PAGE_COMPOUND_HEAD 2
#define PAGE_COMPOUND_TAIL 4
#define PAGE_OWNER_NONE 0
#define PAGE_OWNER_BUDDY 1
#define PAGE_PINNED 8
#define PAGE_POOL_GROUP_SLOTS 64
typedef uint64_t pfn_t;
typedef struct page { atomic_uint refs; int mapcount;
 unsigned flags,owner,order,zone; uintptr_t private_data;
 union { struct { struct page *head; uint64_t index; } compound; } u;
} page_t;
typedef struct {uint64_t base_pfn,present_pages;page_t *pages;} page_section_t;
typedef struct {uint64_t value;} paddr_t;
static page_section_t g_sections[64];static unsigned g_section_count,visits;
static bool g_sections_pfn_ordered;
static paddr_t page_to_phys(page_t *p) {
 uintptr_t address=(uintptr_t)p;
 for(unsigned s=0;s<g_section_count;++s) {
  page_section_t *sec=&g_sections[s];uintptr_t start=(uintptr_t)sec->pages;
  if(address>=start && address-start<sec->present_pages*sizeof(page_t))
   return (paddr_t){(sec->base_pfn+(address-start)/sizeof(page_t))*PAGE_SIZE};
 }
 abort();
}
''' + lookup + mark + pinned + prepare + r'''
static unsigned check_pinned(uint64_t base,unsigned order) {
 visits=0;uint64_t mask=pool_group_pinned_mask(base,order,UINT64_MAX);unsigned scanned=visits;
 visits=0;uint64_t reference=0;
 for(unsigned slot=0;slot<64;++slot)for(unsigned j=0;j<(1U<<order);++j) {
  page_t *p=page_for_pfn(base+((uint64_t)slot<<order)+j);
  if(p && (p->flags&PAGE_PINNED)) {reference|=1ULL<<slot;break;}
 }
 assert(mask==reference && scanned<=visits);return scanned;
}
static void init(void) {
 for(unsigned s=0;s<g_section_count;++s) for(uint64_t j=0;j<g_sections[s].present_pages;++j) {
  page_t *p=&g_sections[s].pages[j];atomic_init(&p->refs,23);
  p->mapcount=-7;p->flags=0x80;p->owner=9;p->order=17;p->zone=s;
  p->private_data=0xabcdef;p->u.compound.head=0;p->u.compound.index=0;
 }
}
static unsigned check_prepare(uint64_t base,unsigned order) {
 init();page_t *head=page_for_pfn(base);assert(head);
 visits=0;pool_prepare_group_slots(head,order);unsigned scanned=visits;
 uint64_t count=64ULL<<order,slot_pages=1ULL<<order;
 for(unsigned s=0;s<g_section_count;++s)for(uint64_t j=0;j<g_sections[s].present_pages;++j) {
  page_t *p=&g_sections[s].pages[j];uint64_t pfn=g_sections[s].base_pfn+j;
  assert(p->zone==s);
  if(pfn>=base && pfn-base<count) {
   assert(p->mapcount==-1 && p->private_data==0);
   uint64_t offset=pfn-base,index=offset&(slot_pages-1);
   assert(p->flags==(index?PAGE_COMPOUND_TAIL:PAGE_COMPOUND_HEAD));
   assert(p->owner==PAGE_OWNER_NONE && p->order==(index?0:order));
   assert(!atomic_load(&p->refs) && p->u.compound.index==index);
   assert(p->u.compound.head==page_for_pfn(base+offset-index));
  } else {
   assert(p->mapcount==-7 && p->private_data==0xabcdef);
   assert(p->flags==0x80 && p->owner==9 && p->order==17 && atomic_load(&p->refs)==23);
   assert(!p->u.compound.head && !p->u.compound.index);
  }
 }
 return scanned;
}
static void check(page_t *head,unsigned order,bool free_page) {
 init();uint64_t base=page_to_phys(head).value/PAGE_SIZE,count=1ULL<<order;
 visits=0;mark_compound(head,order,free_page);
 for(unsigned s=0;s<g_section_count;++s) for(uint64_t j=0;j<g_sections[s].present_pages;++j) {
  page_t *p=&g_sections[s].pages[j];uint64_t pfn=g_sections[s].base_pfn+j;
  bool inside=pfn>=base && pfn-base<count;uint64_t index=pfn-base;
  assert(p->zone==s);
  if(inside) {
   assert(p->mapcount==-1 && p->private_data==0);
   assert(p->flags==((free_page?PAGE_FREE:0)|(index?PAGE_COMPOUND_TAIL:PAGE_COMPOUND_HEAD)));
   assert(p->owner==(free_page?PAGE_OWNER_BUDDY:PAGE_OWNER_NONE));
   assert(p->order==(index?0:order));
   assert(atomic_load(&p->refs)==(!index && !free_page?1U:0U));
   assert(p->u.compound.head==head && p->u.compound.index==index);
  } else {
   assert(p->mapcount==-7 && p->private_data==0xabcdef);
   assert(p->flags==0x80 && p->owner==9 && p->order==17 && atomic_load(&p->refs)==23);
   assert(!p->u.compound.head && !p->u.compound.index);
  }
 }
}
static void release(void) {
 for(unsigned s=0;s<g_section_count;++s)free(g_sections[s].pages);
}
static void section(unsigned s,uint64_t base,uint64_t n) {
 g_sections[s]=(page_section_t){base,n,calloc(n,sizeof(page_t))};
 assert(g_sections[s].pages);
}
int main(void) {
 /* A maximum Buddy block in the last section: no per-page section scan. */
 g_section_count=64;
 for(unsigned s=0;s<63;++s)section(s,1+s*2,1);
 section(63,262144,262144);
 for(unsigned free_page=0;free_page<2;++free_page) {
  check(g_sections[63].pages,18,free_page);assert(visits==64);
 }
 printf("262144-page compound mark: section inspections 16777216 -> %u\n",visits);
 assert(check_pinned(262144,3)==64);
 printf("512-page unpinned group: section inspections 32768 -> 64\n");
 for(unsigned order=0;order<4;++order)assert(check_prepare(262144,order)==64);
 printf("512-page group preparation: section inspections 36864 -> 64\n");
 /* Pin each head/tail position individually, then all slots simultaneously. */
 for(unsigned i=0;i<512;++i) {
  g_sections[63].pages[i].flags|=PAGE_PINNED;
  check_pinned(262144,3);
  g_sections[63].pages[i].flags&=~PAGE_PINNED;
 }
 for(unsigned i=0;i<512;++i)g_sections[63].pages[i].flags|=PAGE_PINNED;
 check_pinned(262144,3);
 assert(page_for_pfn(262144)==g_sections[63].pages);
 assert(page_for_address(262144*PAGE_SIZE)==g_sections[63].pages);
 assert(!page_for_address(262144*PAGE_SIZE+1));assert(!page_for_pfn(UINT64_MAX));
 release();
 /* Adjacent physical ranges, disjoint metadata, deliberately unordered. */
 g_section_count=3;section(0,108,8);section(1,100,8);section(2,119,13);
 for(unsigned o=0;o<=6;++o)for(unsigned f=0;f<2;++f)
  check(&g_sections[1].pages[3],o,f);
 release();
 /* Random non-overlapping layouts include holes and crossing boundaries. */
 unsigned random=0x12345678;
 /* Allocated Buddy blocks have no physical holes; metadata arrays can still
  * be split inside a slot. Preserve its head across every such split. */
 for(unsigned trial=0;trial<1000;++trial) {
  g_section_count=8;uint64_t base=1000;
  for(unsigned s=0;s<8;++s) {
   random=random*1664525U+1013904223U;unsigned n=65+(random%31);
   section(7-s,base,n);base+=n;
  }
  for(unsigned order=0;order<4;++order)check_prepare(1003,order);
  release();
 }
 for(unsigned trial=0;trial<1000;++trial) {
  g_section_count=8;uint64_t base=1000;
  for(unsigned s=0;s<8;++s) {
   random=random*1664525U+1013904223U;unsigned n=1+(random%31);
   section(7-s,base,n);base+=n+((random>>8)%4);
  }
  random=random*1664525U+1013904223U;unsigned s=random%8;
  page_t *head=&g_sections[s].pages[(random>>8)%g_sections[s].present_pages];
  check(head,(random>>16)%9,(random>>24)&1);
  for(unsigned section=0;section<8;++section)
   for(unsigned i=0;i<g_sections[section].present_pages;++i) {
    random=random*1664525U+1013904223U;
    if((random>>28)<3)g_sections[section].pages[i].flags|=PAGE_PINNED;
   }
  for(unsigned order=0;order<4;++order)
   check_pinned(g_sections[s].base_pfn,order);
  release();
 }
 puts("Compound/group preparation and pinned masks: exact state, cross-section slot heads, holes, 2000 layouts and untouched neighbours passed");
}
'''
with tempfile.TemporaryDirectory(prefix='liteos-compound-span-') as directory:
    binary = str(Path(directory)/'test')
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror',
                    '-fsanitize=address,undefined','-fno-pie','-no-pie',
                    '-x','c','-','-o',binary],input=code,text=True,check=True)
    subprocess.run([binary],check=True)
