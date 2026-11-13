"""Actual boot free-list builder versus its original full-candidate oracle."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root/'kernel/mm/phys/page_db.c').read_text()
builder = source[source.index('static bool pages_are_free('):source.index('void page_stats_note_alloc(')]
builder = builder.replace('if ((pages[i].flags & PAGE_FREE)',
                          '++checks; if ((pages[i].flags & PAGE_FREE)')
code = r'''
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#define PAGE_SHIFT 12
#define PAGE_FREE 1
#define BUDDY_MAX_ORDER 18
typedef struct {unsigned flags;} page_t;
typedef struct {uint64_t base_pfn,present_pages;page_t *pages;} page_section_t;
static page_section_t g_sections[8];static unsigned g_section_count;
typedef struct {uint64_t pfn;unsigned order;} block_t;
static block_t blocks[262144],expected[262144];
static unsigned used,expected_used,checks,old_checks;
/* 真实的 insert_block 会修改复合页面元数据，但会保留 PAGE_FREE；构建器只检查该位。
 * 在此记录 insert_block 的实际调用。 */
static void insert_block(uint64_t physical,unsigned order) {
 assert(!(physical&4095) && used<262144);
 blocks[used++]=(block_t){physical>>PAGE_SHIFT,order};
}
''' + builder + r'''
static void oracle(void) {
 expected_used=old_checks=0;
 for(unsigned s=0;s<g_section_count;++s) {
  page_section_t *sec=&g_sections[s];uint64_t index=0;
  while(index<sec->present_pages) {
   if(!(sec->pages[index].flags&PAGE_FREE)) {++index;continue;}
   unsigned order=0;
   while(order<BUDDY_MAX_ORDER) {
    uint64_t n=1ULL<<(order+1);
    if(index+n>sec->present_pages || ((sec->base_pfn+index)&(n-1)))break;
    bool free=true;
    for(uint64_t i=0;i<n;++i) {
     ++old_checks;
     if(!(sec->pages[index+i].flags&PAGE_FREE)) {free=false;break;}
    }
    if(!free)break;++order;
   }
   expected[expected_used++]=(block_t){sec->base_pfn+index,order};
   index+=1ULL<<order;
  }
 }
}
static void check(void) {
 oracle();used=checks=0;build_free_lists();assert(used==expected_used);
 for(unsigned i=0;i<used;++i)
  assert(blocks[i].pfn==expected[i].pfn && blocks[i].order==expected[i].order);
 assert(checks<=old_checks);
}
static void section(unsigned s,uint64_t base,unsigned n) {
 g_sections[s]=(page_section_t){base,n,calloc(n,sizeof(page_t))};assert(g_sections[s].pages);
 for(unsigned i=0;i<n;++i)g_sections[s].pages[i].flags=PAGE_FREE|0x80;
}
static void release(void) {
 for(unsigned s=0;s<g_section_count;++s)free(g_sections[s].pages);
}
int main(void) {
 g_section_count=1;section(0,262144,262144);check();
 assert(used==1 && blocks[0].order==18 && checks==262143 && old_checks==524286);
 printf("262144 free pages: candidate checks %u -> %u; per-page section lookups removed\n",old_checks,checks);
 /* 增长边界上的保留页面必须阻止生成过大的内存块。 */
 for(unsigned i=0;i<=18;++i) {
  unsigned at=(1U<<i)-1;g_sections[0].pages[at].flags=0;check();
  g_sections[0].pages[at].flags=PAGE_FREE;
 }
 release();
 /* 遍历所有小型标志布局，包括未对齐的区段起始位置。 */
 for(unsigned base=0;base<16;++base)for(unsigned mask=0;mask<256;++mask) {
  section(0,base,8);
  for(unsigned i=0;i<8;++i)g_sections[0].pages[i].flags=(mask>>i)&1;
  check();release();
 }
 unsigned random=1234567;
 for(unsigned trial=0;trial<1000;++trial) {
  g_section_count=8;uint64_t base=17;
  for(unsigned s=0;s<8;++s) {
   random=random*1664525U+1013904223U;unsigned n=1+(random%127);
   section(7-s,base,n);base+=n+3;
   for(unsigned i=0;i<n;++i) {
    random=random*1664525U+1013904223U;
    g_sections[7-s].pages[i].flags=(random>>28)?PAGE_FREE:0;
   }
  }
  check();release();
 }
 puts("Boot Buddy partition: exact old-oracle blocks, reserved boundaries, 4096 small layouts, 1000 sparse multi-section layouts passed");
}
'''
with tempfile.TemporaryDirectory(prefix='liteos-buddy-boot-') as directory:
    binary = str(Path(directory)/'test')
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror',
                    '-fsanitize=address,undefined','-fno-pie','-no-pie',
                    '-x','c','-','-o',binary],input=code,text=True,check=True)
    subprocess.run([binary],check=True)
