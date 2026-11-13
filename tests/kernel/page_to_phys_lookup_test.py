"""Production reverse-page lookup: ordered metadata, unordered physical ranges."""
from pathlib import Path
import subprocess
import tempfile

root=Path(__file__).resolve().parents[2]
source=(root/'kernel/mm/phys/page_db.c').read_text()
start=source.index('paddr_t page_to_phys(')
function=source[start:source.index('\n}',start)+2]
function=function.replace('const page_section_t *section = &g_sections[middle];',
                         '++visits; const page_section_t *section = &g_sections[middle];')
assert '++visits' in function
code=r'''
#include <kernel/mm.h>
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <pthread.h>
static page_section_t g_sections[65536];
static uint32_t g_section_count;
static bool g_initialized;
static _Thread_local unsigned visits;
''' + function + r'''
static void check(const page_t *page,uint64_t expected) {
    visits=0;assert(page_to_phys(page).value==expected);assert(visits<=17);
}
static uint64_t oracle(uintptr_t address) {
    for(unsigned s=0;s<g_section_count;++s) {
        uintptr_t start=(uintptr_t)g_sections[s].pages;
        for(unsigned i=0;i<g_sections[s].present_pages;++i)
            if(address==start+i*sizeof(page_t))
                return (g_sections[s].base_pfn+i)<<PAGE_SHIFT;
    }
    return UINT64_MAX;
}
static void *reader(void *argument) {
    unsigned seed=(unsigned)(uintptr_t)argument;
    for(unsigned i=0;i<20000;++i) {
        seed=seed*1664525U+1013904223U;
        unsigned section=seed&65535U,index=(seed>>16)&3U;
        check(&g_sections[section].pages[index],
              (g_sections[section].base_pfn+index)<<PAGE_SHIFT);
    }
    return NULL;
}
int main(void) {
    page_t dummy={0};check(NULL,UINT64_MAX);check(&dummy,UINT64_MAX);
    g_initialized=true;check(&dummy,UINT64_MAX);
    page_t *metadata=malloc(65536U*5*sizeof(page_t));assert(metadata);
    g_section_count=65536;
    for(unsigned s=0;s<g_section_count;++s) {
        g_sections[s]=(page_section_t){.base_pfn=(65535U-s)*8ULL+256,
            .pages=metadata+s*5,.present_pages=4};
    }
    /* 覆盖每个有效元素和段间空隙；物理 PFN 按反向排列，以证明只需按元数据地址排序。 */
    for(unsigned s=0;s<g_section_count;++s) {
        for(unsigned i=0;i<4;++i)
            check(&g_sections[s].pages[i],(g_sections[s].base_pfn+i)<<PAGE_SHIFT);
        check(g_sections[s].pages+4,UINT64_MAX);
    }
    check((const page_t *)((uintptr_t)metadata-1),UINT64_MAX);
    check(metadata+65536U*5,UINT64_MAX);
    check((const page_t *)UINTPTR_MAX,UINT64_MAX);
    check(NULL,UINT64_MAX);
    pthread_t threads[8];
    for(unsigned i=0;i<8;++i)assert(!pthread_create(&threads[i],NULL,reader,(void *)(uintptr_t)(i+1)));
    for(unsigned i=0;i<8;++i)assert(!pthread_join(threads[i],NULL));
    /* 使用独立的精确地址基准检查随机跨度长度和空隙。 */
    unsigned seed=19;
    for(unsigned layout=0;layout<64;++layout) {
        g_section_count=1+layout;unsigned offset=0;
        for(unsigned s=0;s<g_section_count;++s) {
            seed=seed*1664525U+1013904223U;
            unsigned count=1+(seed>>16)%8;
            g_sections[s]=(page_section_t){.base_pfn=(seed>>8)*16ULL,
                .pages=metadata+offset,.present_pages=count};
            offset+=count+(seed&3U);
        }
        for(unsigned byte=0;byte<offset*sizeof(page_t);++byte) {
            uintptr_t address=(uintptr_t)metadata+byte;
            check((const page_t *)address,oracle(address));
        }
        g_initialized=false;check(metadata,UINT64_MAX);g_initialized=true;
    }
    free(metadata);
    puts("page_to_phys: 65536 reverse-PFN sections, all elements/gaps, byte-offset oracle over 64 layouts, 160000 concurrent queries; at most 17 section visits PASS");
}
'''
with tempfile.TemporaryDirectory(prefix='liteos-page-lookup-') as directory:
    binary=str(Path(directory)/'test')
    subprocess.run(['cc','-std=c11','-O2','-Wall','-Wextra','-Werror','-pthread',
                    '-I'+str(root/'include'),'-fsanitize=address,undefined',
                    '-fno-pie','-no-pie','-x','c','-','-o',binary],
                   input=code,text=True,check=True)
    subprocess.run([binary],check=True,timeout=30)
