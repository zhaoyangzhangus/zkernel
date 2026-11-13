"""Actual PFN lookup: binary ordered ranges, first-match unordered fallback."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'kernel/mm/phys/page_db.c').read_text()
start = source.index('static void check_section_pfn_order(')
lookup = source[start:source.index('static uint8_t page_zone(', start)]
lookup = lookup.replace('page_section_t *section = &g_sections[middle];',
                        '++visits; page_section_t *section = &g_sections[middle];')
lookup = lookup.replace('\n        page_section_t *section = &g_sections[i];',
                        '\n        ++visits; page_section_t *section = &g_sections[i];')
program = r'''
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <pthread.h>
#include <kernel/mm.h>
static page_section_t g_sections[65536];
static uint32_t g_section_count;
static bool g_sections_pfn_ordered;
static _Thread_local unsigned visits;
''' + lookup + r'''
static page_section_t *oracle(pfn_t pfn) {
 for(unsigned i=0;i<g_section_count;++i)
  if(pfn>=g_sections[i].base_pfn && pfn-g_sections[i].base_pfn<g_sections[i].present_pages)
   return &g_sections[i];
 return 0;
}
static void check(pfn_t pfn) {
 page_section_t *reference=oracle(pfn);visits=0;
 assert(section_for_pfn(pfn)==reference);
 if(g_sections_pfn_ordered)assert(visits<=17);
 page_t *expected=reference?reference->pages+(pfn-reference->base_pfn):0;
 assert(page_for_pfn(pfn)==expected);
 uint64_t count=UINT64_MAX;
 assert(page_span_for_pfn(pfn,&count)==expected);
 assert(count==(reference?reference->present_pages-(pfn-reference->base_pfn):UINT64_MAX));
}
static void *reader(void *raw) {
 unsigned seed=(unsigned)(uintptr_t)raw;
 for(unsigned i=0;i<20000;++i) {
  seed=seed*1664525U+1013904223U;unsigned s=seed&65535;
  visits=0;assert(page_for_pfn(256+s*8ULL+3)==&g_sections[s].pages[3]);assert(visits<=17);
 }
 return 0;
}
int main(void) {
 check_section_pfn_order();check(0);check(UINT64_MAX);
 page_t *metadata=calloc(65536U*4,sizeof(page_t));assert(metadata);
 g_section_count=65536;
 for(unsigned i=0;i<g_section_count;++i)
  g_sections[i]=(page_section_t){.base_pfn=256+i*8ULL,.present_pages=4,.pages=metadata+i*4};
 check_section_pfn_order();assert(g_sections_pfn_ordered);
 // 直接给出预期结果，避免在大型映射上执行 O(n²) 基准计算。
 for(unsigned i=0;i<g_section_count;++i)for(unsigned j=0;j<8;++j) {
  visits=0;assert(page_for_pfn(256+i*8ULL+j)==(j<4?&metadata[i*4+j]:0));assert(visits<=17);
 }
 visits=0;assert(page_for_pfn(256+65535U*8ULL)==&metadata[65535U*4]);unsigned last=visits;
 pthread_t threads[8];
 for(unsigned i=0;i<8;++i)assert(!pthread_create(&threads[i],0,reader,(void *)(uintptr_t)(i+1)));
 for(unsigned i=0;i<8;++i)assert(!pthread_join(threads[i],0));
 unsigned seed=19;
 for(unsigned layout=0;layout<1000;++layout) {
  g_section_count=1+layout%64;
  uint64_t base=layout%2?UINT64_MAX-4096:0;
  for(unsigned i=0;i<g_section_count;++i) {
   seed=seed*1664525U+1013904223U;unsigned n=1+(seed>>16)%8;
   g_sections[i]=(page_section_t){.base_pfn=base,.present_pages=n,.pages=metadata+i*16};
   base+=n+(seed&3);
  }
  check_section_pfn_order();assert(g_sections_pfn_ordered);
  for(unsigned i=0;i<g_section_count;++i) {
   pfn_t p=g_sections[i].base_pfn;
   check(p);check(p+g_sections[i].present_pages-1);check(p+g_sections[i].present_pages);
   if(p)check(p-1);
  }
  check(UINT64_MAX);
  if(g_section_count>1) {
   page_section_t saved=g_sections[0];g_sections[0]=g_sections[g_section_count-1];
   g_sections[g_section_count-1]=saved;
   check_section_pfn_order();assert(!g_sections_pfn_ordered);
   for(unsigned i=0;i<g_section_count;++i)check(g_sections[i].base_pfn);
  }
 }
 // 对重叠范围、相同基址和空跨度，仍采用首次匹配结果。
 g_section_count=3;
 g_sections[0]=(page_section_t){.base_pfn=100,.present_pages=10,.pages=metadata};
 g_sections[1]=(page_section_t){.base_pfn=105,.present_pages=12,.pages=metadata+16};
 g_sections[2]=(page_section_t){.base_pfn=105,.present_pages=0,.pages=metadata+32};
 check_section_pfn_order();assert(!g_sections_pfn_ordered);
 for(unsigned p=90;p<130;++p)check(p);
 assert(!page_for_address(100*PAGE_SIZE+1));
 assert(page_for_address(100*PAGE_SIZE)==metadata);
 free(metadata);
 printf("PASS: PFN ordered/gap lookup through 65536 sections, 160000 concurrent queries, 1000 unordered/boundary oracle maps; last-section visits 65536 -> %u\n",last);
}
'''
with tempfile.TemporaryDirectory(prefix='liteos-pfn-lookup-') as directory:
    binary = str(Path(directory) / 'test')
    subprocess.run(['cc', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                    '-pthread', '-I' + str(root / 'include'), '-fsanitize=address,undefined',
                    '-fno-pie', '-no-pie', '-x', 'c', '-', '-o', binary],
                   input=program, text=True, check=True)
    subprocess.run([binary], check=True, timeout=30)
