"""Production pinned scanning considers only reclaimable slots, including tails."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'kernel/mm/phys/percpu_page.c').read_text()
begin = source.index('static uint64_t pool_group_pinned_mask(')
end = source.index('static page_local_pool_t *pool_for_cpu(', begin)
function = source[begin:end].replace('(pages[j].flags & PAGE_PINNED)',
                                    '(read_flags(&pages[j]) & PAGE_PINNED)')
program = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#define PAGE_POOL_GROUP_SLOTS 64U
#define PAGE_PINNED 1U
typedef struct {unsigned flags;} page_t;
static page_t pages[2048];
static bool present[2048];
static unsigned reads,lookups;
static unsigned read_flags(const page_t *page) {++reads;return page->flags;}
static page_t *page_span_for_pfn(uint64_t pfn,uint64_t *span) {
    ++lookups;if(pfn<1000||pfn-1000>=2048||!present[pfn-1000])return 0;
    uint64_t index=pfn-1000,n=1;
    /* 不连续元数据跨度的边界以及缺失页面形成的空洞。 */
    while(n<*span&&index+n<2048&&present[index+n]&&(index+n)%17)++n;
    *span=n;return &pages[index];
}
''' + function + r'''
static unsigned cases;
static void check(unsigned order,uint64_t candidates) {
    uint64_t expected=0;unsigned count=64U<<order;
    for(unsigned n=0;n<count;++n)
        if(present[n]&&(pages[n].flags&PAGE_PINNED))expected|=1ULL<<(n>>order);
    reads=lookups=0;
    uint64_t actual=pool_group_pinned_mask(1000,order,candidates);
    assert(actual==(expected&candidates));
    unsigned slots=(unsigned)__builtin_popcountll(candidates);
    assert(reads<=(slots<<order));
    if(!candidates)assert(!reads&&!lookups);
    /* 即使占用槽位中存在固定页面，排空后得到的空闲掩码和整组回收判定也必须与旧的
     * 全槽扫描一致。 */
    assert((candidates&~actual)==(candidates&~expected));
    assert((candidates==UINT64_MAX&&actual==0)==
           (candidates==UINT64_MAX&&expected==0));
    ++cases;
}
int main(void) {
    for(unsigned n=0;n<2048;++n)present[n]=true;
    for(unsigned order=0;order<=5;++order) {
        for(unsigned slot=0;slot<64;++slot) {
            check(order,1ULL<<slot);assert(reads==(1U<<order));
        }
        check(order,0);check(order,UINT64_MAX);
        for(unsigned n=0;n<(64U<<order);++n) {
            pages[n].flags=PAGE_PINNED;
            check(order,1ULL<<(n>>order));
            check(order,~(1ULL<<(n>>order)));
            check(order,UINT64_MAX);
            pages[n].flags=0;
        }
    }
    uint64_t seed=73;
    for(unsigned trial=0;trial<20000;++trial) {
        unsigned order=trial%6;
        for(unsigned n=0;n<(64U<<order);++n) {
            seed=seed*6364136223846793005ULL+1;
            pages[n].flags=(seed>>32)%31==0?PAGE_PINNED:0;
            present[n]=(seed>>40)%19!=0;
        }
        seed=seed*6364136223846793005ULL+1;
        check(order,seed);check(order,0);check(order,UINT64_MAX);
    }
    printf("Pinned candidates: %u complete-mask oracle cases; every slot/head/tail, holes and random masks; singleton order-5 scan checks at most 32 instead of 2048 pages; empty mask performs no lookup\n",cases);
}
'''
with tempfile.TemporaryDirectory(prefix='liteos-pool-pinned-') as directory:
    binary = str(Path(directory) / 'test')
    subprocess.run(['cc', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
                    '-x', 'c', '-', '-o', binary], input=program, text=True, check=True)
    subprocess.run([binary], check=True, timeout=60)
