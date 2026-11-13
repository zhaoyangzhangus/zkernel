"""Actual Buddy split/merge versus pre-refactor lookup routing."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'kernel/mm/phys/page_db.c').read_text()
mark = source[source.index('void mark_compound('):source.index('static bool block_is_free(')]
insert = source[source.index('static bool block_is_free('):source.index('/* Boot construction')]
runtime = source[source.index('page_t *take_from_zone_locked('):source.index('bool mm_boot_init(')]
previous_take = runtime[:runtime.index('void free_block_locked(')].replace(
    'take_from_zone_locked(', 'previous_take(').replace(
    '        mark_compound(block, order, false);',
    '        block = page_for_address(physical);\n        mark_compound(block, order, false);')
program = '#include <kernel/mm.h>\n' + r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
static page_t pages[256],saved_pages[256];
static bool present[256];
static list_head_t g_free_lists[PAGE_ZONE_COUNT][BUDDY_MAX_ORDER+1],saved_lists[PAGE_ZONE_COUNT][BUDDY_MAX_ORDER+1];
static unsigned lookups;
static unsigned expected_outcomes[10000];
paddr_t page_to_phys(const page_t *page) {
    uintptr_t address=(uintptr_t)page,start=(uintptr_t)pages;
    if(address<start||address-start>=sizeof(pages)||(address-start)%sizeof(page_t))
        return (paddr_t){UINT64_MAX};
    return (paddr_t){((address-start)/sizeof(page_t))*PAGE_SIZE};
}
static page_t *page_for_address(uint64_t physical) {
    ++lookups;
    if(physical%PAGE_SIZE||physical/PAGE_SIZE>=256||!present[physical/PAGE_SIZE])return 0;
    return &pages[physical/PAGE_SIZE];
}
static page_t *page_span_for_pfn(pfn_t pfn,uint64_t *span) {
    if(pfn>=256||!present[pfn])return 0;
    uint64_t n=1;
    /* 在保留实际标记的同时，强制覆盖元数据跨度边界。 */
    while(n<*span&&pfn+n<256&&present[pfn+n]&&(pfn+n)%17)n++;
    *span=n;return &pages[pfn];
}
''' + mark + insert + runtime + previous_take + r'''
static void previous_free(page_t *head,uint8_t order) {
    uint64_t physical=page_to_phys(head).value;
    if(physical==UINT64_MAX||order>BUDDY_MAX_ORDER)return;
    atomic_store_explicit(&head->refs,0U,memory_order_relaxed);
    while(order<BUDDY_MAX_ORDER) {
        uint64_t buddy_physical=physical^((1ULL<<order)*PAGE_SIZE);
        page_t *buddy=page_for_address(buddy_physical);
        if(buddy==0||!block_is_free(page_for_address(buddy_physical),order))break;
        list_del(&buddy->u.free_node);
        if(buddy_physical<physical)physical=buddy_physical;
        ++order;
    }
    insert_block(physical,order);
}
static void reset(unsigned layout) {
    memset(pages,0,sizeof(pages));memset(present,1,sizeof(present));
    for(unsigned z=0;z<PAGE_ZONE_COUNT;++z)
    for(unsigned o=0;o<=BUDDY_MAX_ORDER;++o)list_init(&g_free_lists[z][o]);
    for(unsigned i=0;i<256;++i) {
        pages[i].zone=layout==1?(i>=128):0;
        atomic_init(&pages[i].refs,0);atomic_init(&pages[i].mapcount,-1);
    }
    if(layout==2) {
        for(unsigned i=64;i<128;++i)present[i]=false;
        insert_block(0,6);insert_block(128*PAGE_SIZE,7);
    } else if(layout==1) {
        insert_block(0,7);insert_block(128*PAGE_SIZE,7);
    } else insert_block(0,8);
    lookups=0;
}
static unsigned trace(bool old,unsigned layout,unsigned seed) {
    page_t *allocated[128]={0};unsigned orders[128]={0};
    unsigned results=0;
    for(unsigned step=0;step<10000;++step) {
        seed=seed*1664525U+1013904223U;unsigned slot=seed%128;
        if(allocated[slot]) {
            if(old)previous_free(allocated[slot],orders[slot]);
            else free_block_locked(allocated[slot],orders[slot]);
            allocated[slot]=0;
        } else {
            unsigned order=(seed>>8)%9,zone=layout==1?(seed>>17)%2:0;
            page_t *block=old?previous_take(zone,order):take_from_zone_locked(zone,order);
            allocated[slot]=block;orders[slot]=order;
            unsigned outcome=block?(unsigned)(block-pages)+1U:0U;
            if(old)expected_outcomes[step]=outcome;
            else assert(outcome==expected_outcomes[step]);
            results=results*33U+outcome;
            if(block)assert(block->order==order&&!(block->flags&PAGE_FREE)&&atomic_load(&block->refs)==1);
        }
    }
    for(unsigned slot=0;slot<128;++slot)if(allocated[slot]) {
        if(old)previous_free(allocated[slot],orders[slot]);
        else free_block_locked(allocated[slot],orders[slot]);
    }
    return results;
}
int main(void) {
    unsigned old_total=0,new_total=0;
    for(unsigned layout=0;layout<3;++layout)
    for(unsigned trial=0;trial<100;++trial) {
        reset(layout);unsigned expected=trace(true,layout,trial+73);
        old_total+=lookups;
        memcpy(saved_pages,pages,sizeof(pages));memcpy(saved_lists,g_free_lists,sizeof(g_free_lists));
        reset(layout);assert(trace(false,layout,trial+73)==expected);new_total+=lookups;
        assert(!memcmp(saved_pages,pages,sizeof(pages)));
        assert(!memcmp(saved_lists,g_free_lists,sizeof(g_free_lists)));
    }
    reset(0);page_t *head=take_from_zone_locked(0,0);assert(head==pages);
    lookups=0;free_block_locked(head,0);assert(lookups==9);
    reset(0);head=previous_take(0,0);lookups=0;
    previous_free(head,0);assert(lookups==18);
    /* 元数据指针或顺序无效时，所有链表和页面都必须保持不变。 */
    memcpy(saved_pages,pages,sizeof(pages));memcpy(saved_lists,g_free_lists,sizeof(g_free_lists));
    free_block_locked(0,0);free_block_locked(pages,BUDDY_MAX_ORDER+1);
    insert_known_block(0,0);insert_known_block(pages,BUDDY_MAX_ORDER+1);
    assert(!memcmp(saved_pages,pages,sizeof(pages))&&!memcmp(saved_lists,g_free_lists,sizeof(g_free_lists)));
    assert(new_total<old_total);
    printf("Buddy runtime: 3 million split/merge operations across holes/zones/span boundaries; exact results/metadata/free lists; address lookups %u -> %u; eight-merge release uses 9 instead of 18 lookups\n",old_total,new_total);
}
'''
with tempfile.TemporaryDirectory(prefix='liteos-buddy-runtime-') as directory:
    binary = str(Path(directory) / 'test')
    subprocess.run(['cc', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
                    '-I' + str(root / 'include'), '-x', 'c', '-', '-o', binary],
                   input=program, text=True, check=True)
    subprocess.run([binary], check=True, timeout=60)
