"""Production owned/borrowed allocation, rollback and failed-unmap retirement."""
from pathlib import Path
import subprocess
import tempfile
import resource
root=Path(__file__).resolve().parents[2]
source=(root/'kernel/mm/vmalloc.c').read_text()
area=source[source.index('typedef struct {'):source.index('static uint64_t g_vmalloc_bitmap')]
code=source[source.index('static bool find_virtual_run('):source.index('bool vmalloc_self_test(')]
code=code.replace('for (uint32_t i = g_vmalloc_next_free_area; i < VMALLOC_MAX_AREAS; ++i) {',
                  'for (uint32_t i = g_vmalloc_next_free_area; i < VMALLOC_MAX_AREAS; ++i) { ++area_visits;')
code=code.replace('for (uint32_t i = 0; i < g_vmalloc_area_limit; ++i) {',
                  'for (uint32_t i = 0; i < g_vmalloc_area_limit; ++i) { ++scan_visits;')
release=source[source.index('static bool vmalloc_release_area_locked('):source.index('static vmalloc_area_t *vmalloc_reserve_area_locked(')]
previous=release.replace('vmalloc_release_area_locked(', 'previous_release(')
previous=previous.replace('    uint32_t remaining = 0U;', '    bool complete = true;')
previous=previous.replace('            remaining = i + 1U;', '            complete = false;')
previous=previous.replace('    if (remaining != 0U) {', '    if (!complete) {')
previous=previous.replace('        area->data_pages = remaining;\n', '')
borrowed_release='''            if (area->owns_pages) {
                page_t *page = phys_to_page(physical);
                if (page != 0) page_free(page);
            }'''
assert borrowed_release in previous
previous=previous.replace(borrowed_release, '''            page_t *page = phys_to_page(physical);
            if (area->owns_pages && page != 0) page_free(page);''')
program=r'''
#include <kernel/bitmap.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>
#define VMALLOC_MANAGED_SIZE (1ULL<<30)
#define VMALLOC_PAGE_COUNT (VMALLOC_MANAGED_SIZE/PAGE_SIZE)
#define VMALLOC_BITMAP_WORDS (VMALLOC_PAGE_COUNT/64)
#define VMALLOC_MAX_AREAS 1024U
#define X86_64_VMALLOC_BASE 0x100000000ULL
#define PAGE_ALLOC_ZERO 1U
#define PAGE_FREE 1U
#define X86_PAGE_WRITE 2U
#define X86_PAGE_GLOBAL 256U
enum x86_cache_mode { X86_CACHE_WB, X86_CACHE_UC };
typedef struct { unsigned flags; bool live; } page_t;
''' + area + r'''
static uint64_t g_vmalloc_bitmap[VMALLOC_BITMAP_WORDS];
static vmalloc_area_t g_vmalloc_areas[VMALLOC_MAX_AREAS];
static uint32_t g_vmalloc_failure,g_vmalloc_pending_cleanup;
static uint32_t g_vmalloc_next_free_area,g_vmalloc_area_limit,area_visits,scan_visits;
static page_t pool[4096];
static struct { paddr_t root; page_t *page; } mappings[VMALLOC_PAGE_COUNT];
static unsigned held,live,alloc_calls,map_calls,fail_alloc,fail_map,physical_lookups,unmap_calls;
static uint64_t current_root=1,fail_unmap_va,fail_unmap_second_va;
static void vmalloc_initialize(void) {}
static uint64_t vmalloc_lock_irqsave(void) { assert(!held);held=1;return 42; }
static void vmalloc_unlock_irqrestore(uint64_t f) { assert(held && f==42);held=0; }
static paddr_t x86_current_root_table(void) { return paddr_make(current_root); }
static page_t *page_alloc(unsigned order,unsigned flags) {
    assert(!order && flags==PAGE_ALLOC_ZERO);
    if (++alloc_calls==fail_alloc) return 0;
    for(unsigned i=0;i<4096;++i) if(!pool[i].live) {
        pool[i].live=true; pool[i].flags=0; ++live; return &pool[i];
    }
    return 0;
}
static void page_free(page_t *p) { assert(p->live);p->live=false;p->flags=PAGE_FREE;--live; }
static paddr_t page_to_phys(page_t *p) { return paddr_make((p-pool+1)*PAGE_SIZE); }
static page_t *mapped_page(paddr_t p) { assert(p.value && p.value/PAGE_SIZE<=4096);return &pool[p.value/PAGE_SIZE-1]; }
static page_t *phys_to_page(paddr_t p) { ++physical_lookups;return mapped_page(p); }
static unsigned slot(vaddr_t va) {
    assert(va>=X86_64_VMALLOC_BASE && !(va%PAGE_SIZE));
    unsigned i=(va-X86_64_VMALLOC_BASE)/PAGE_SIZE;
    assert(i<VMALLOC_PAGE_COUNT); return i;
}
static kstatus_t x86_map_page(paddr_t root,vaddr_t va,paddr_t phys,unsigned flags,enum x86_cache_mode cache) {
    assert(held && flags==(X86_PAGE_WRITE|X86_PAGE_GLOBAL) && cache<=X86_CACHE_UC);
    if (++map_calls==fail_map) return K_EIO;
    unsigned i=slot(va); assert(!mappings[i].page);
    mappings[i].page=mapped_page(phys);mappings[i].root=root;return K_OK;
}
static kstatus_t x86_unmap_page(paddr_t root,vaddr_t va,paddr_t *phys) {
    assert(held);++unmap_calls;unsigned i=slot(va);
    if (!mappings[i].page) return K_ENOENT;
    assert(root.value==mappings[i].root.value);
    if (va==fail_unmap_va||va==fail_unmap_second_va) return K_EIO;
    *phys=page_to_phys(mappings[i].page);mappings[i].page=0;return K_OK;
}
''' + code + previous + r'''
static void guards(void *p,unsigned pages) {
    unsigned i=slot((uintptr_t)p);
    assert(!mappings[i-1].page && !mappings[i+pages].page);
    for(unsigned n=0;n<pages;++n) assert(mappings[i+n].page);
}
static void empty(void) {
    assert(!g_vmalloc_next_free_area);
    assert(!g_vmalloc_area_limit);
    assert(!held && !g_vmalloc_pending_cleanup);
    for(unsigned i=0;i<VMALLOC_MAX_AREAS;++i) assert(!g_vmalloc_areas[i].used);
    for(unsigned i=0;i<VMALLOC_BITMAP_WORDS;++i) assert(!g_vmalloc_bitmap[i]);
    for(unsigned i=0;i<VMALLOC_PAGE_COUNT;++i) assert(!mappings[i].page);
}
int main(void) {
    assert(!vmalloc(0) && !vmalloc(SIZE_MAX));
    void *p=vmalloc(PAGE_SIZE*65+1); assert(p && live==66);guards(p,66);
    current_root=2;vfree(p);assert(!live&&physical_lookups==66);empty();
    scan_visits=0;vfree((void *)123);assert(!scan_visits);
    p=vmalloc(PAGE_SIZE);assert(p&&g_vmalloc_area_limit==1);
    vfree((void *)123);assert(scan_visits==1);
    scan_visits=0;vfree(p);assert(scan_visits==1);empty();
    void *bounded[128];
    for(unsigned i=0;i<128;++i)bounded[i]=vmalloc(PAGE_SIZE);
    assert(g_vmalloc_area_limit==128);
    for(unsigned i=128;i>64;--i)vfree(bounded[i-1]);
    assert(g_vmalloc_area_limit==64);
    scan_visits=0;vfree((void *)123);assert(scan_visits==64);
    for(unsigned i=0;i<64;++i)vfree(bounded[i]);
    empty();
    /* Last pending cleanup at the first slot must not scan the live suffix.
     * Failed cleanup still traverses the suffix looking for other retries. */
    for(unsigned i=0;i<128;++i)bounded[i]=vmalloc(PAGE_SIZE);
    fail_unmap_va=(uintptr_t)bounded[0];vfree(bounded[0]);
    assert(g_vmalloc_pending_cleanup==1&&g_vmalloc_area_limit==128);
    scan_visits=0;p=vmalloc(PAGE_SIZE);assert(p&&scan_visits==128);
    assert(g_vmalloc_pending_cleanup==1);vfree(p);
    fail_unmap_va=0;scan_visits=0;p=vmalloc(PAGE_SIZE);
    assert(p==bounded[0]&&scan_visits==1&&!g_vmalloc_pending_cleanup);
    assert(g_vmalloc_area_limit==128&&live==128);vfree(p);
    for(unsigned i=1;i<128;++i)vfree(bounded[i]);
    empty();
    for(unsigned pending=0;pending<128;++pending) {
        for(unsigned i=0;i<128;++i)bounded[i]=vmalloc(PAGE_SIZE);
        fail_unmap_va=(uintptr_t)bounded[pending];vfree(bounded[pending]);
        assert(g_vmalloc_pending_cleanup==1&&live==128);
        fail_unmap_va=0;scan_visits=0;p=vmalloc(PAGE_SIZE);
        assert(p==bounded[pending]&&scan_visits==pending+1);
        assert(!g_vmalloc_pending_cleanup&&g_vmalloc_area_limit==128&&live==128);
        for(unsigned i=0;i<128;++i)vfree(bounded[i]);
        empty();
    }
    fail_alloc=alloc_calls+2;assert(!vmalloc(PAGE_SIZE*3) && !live);empty();fail_alloc=0;
    fail_map=map_calls+3;fail_unmap_va=X86_64_VMALLOC_BASE+PAGE_SIZE;
    assert(!vmalloc(PAGE_SIZE*3) && live==1 && g_vmalloc_pending_cleanup==1);
    assert(g_vmalloc_areas[0].used && g_vmalloc_areas[0].data_pages==1);
    assert(g_vmalloc_area_limit==1);
    assert(bitmap_test_bit(g_vmalloc_bitmap,0) && bitmap_test_bit(g_vmalloc_bitmap,4));
    current_root=3;fail_map=0;
    p=vmalloc(PAGE_SIZE);assert(p && slot((uintptr_t)p)==6 && live==2);
    assert(g_vmalloc_pending_cleanup==1);vfree(p);assert(live==1);
    assert(g_vmalloc_area_limit==1);
    fail_unmap_va=0;current_root=4;
    p=vmalloc(PAGE_SIZE);assert(p && slot((uintptr_t)p)==1 && live==1);
    assert(!g_vmalloc_pending_cleanup);vfree(p);assert(!live);empty();
    for(unsigned keep_first=0;keep_first<2;++keep_first) {
        void *first=vmalloc(PAGE_SIZE);assert(first);
        fail_unmap_va=(uintptr_t)first;vfree(first);
        void *second=vmalloc(PAGE_SIZE);assert(second);
        fail_unmap_va=(uintptr_t)second;vfree(second);
        assert(g_vmalloc_pending_cleanup==2&&g_vmalloc_area_limit==2&&live==2);
        fail_unmap_va=keep_first?(uintptr_t)first:0;
        scan_visits=0;void *replacement=vmalloc(PAGE_SIZE);assert(replacement);
        assert(scan_visits==2&&g_vmalloc_pending_cleanup==keep_first);
        assert(g_vmalloc_area_limit==(keep_first?2U:1U));
        vfree(replacement);
        if(keep_first) {
            assert(g_vmalloc_area_limit==1&&live==1);
            fail_unmap_va=0;replacement=vmalloc(PAGE_SIZE);assert(replacement);
            vfree(replacement);
        }
        assert(!live);empty();
    }
    /* The old path rescans every retired suffix page after a head failure. */
    p=vmalloc(PAGE_SIZE*4096);assert(p&&live==4096);
    fail_unmap_va=(uintptr_t)p;
    uint64_t old_irq=vmalloc_lock_irqsave();
    assert(!previous_release(&g_vmalloc_areas[0]));
    unsigned unmap_before=unmap_calls;
    assert(!previous_release(&g_vmalloc_areas[0])&&unmap_calls-unmap_before==4096);
    assert(live==1&&g_vmalloc_areas[0].data_pages==4096);
    fail_unmap_va=0;assert(previous_release(&g_vmalloc_areas[0]));
    vmalloc_unlock_irqrestore(old_irq);assert(!live);empty();
    p=vmalloc(PAGE_SIZE*4096);assert(p&&live==4096);
    fail_unmap_va=(uintptr_t)p;vfree(p);
    assert(live==1&&g_vmalloc_areas[0].data_pages==1);
    unmap_before=unmap_calls;vfree(p);
    assert(unmap_calls-unmap_before==1&&live==1);
    /* Full virtual reservation and saved root remain intact until success. */
    assert(g_vmalloc_areas[0].total_pages==4098&&bitmap_test_bit(g_vmalloc_bitmap,4097));
    fail_unmap_va=0;unmap_before=unmap_calls;vfree(p);
    assert(unmap_calls-unmap_before==1&&!live);empty();
    for(unsigned failed=0;failed<64;++failed) {
        p=vmalloc(PAGE_SIZE*64);assert(p&&live==64);
        fail_unmap_va=(uintptr_t)p+failed*PAGE_SIZE;vfree(p);
        assert(live==1&&g_vmalloc_areas[0].data_pages==failed+1);
        unmap_before=unmap_calls;vfree(p);
        assert(unmap_calls-unmap_before==failed+1&&live==1);
        fail_unmap_va=0;vfree(p);assert(!live);empty();
    }
    p=vmalloc(PAGE_SIZE*64);assert(p&&live==64);
    fail_unmap_va=(uintptr_t)p;fail_unmap_second_va=(uintptr_t)p+47*PAGE_SIZE;
    ++current_root;vfree(p);
    assert(live==2&&g_vmalloc_areas[0].data_pages==48&&g_vmalloc_pending_cleanup==1);
    fail_unmap_second_va=0;unmap_before=unmap_calls;vfree(p);
    assert(unmap_calls-unmap_before==48&&live==1&&g_vmalloc_areas[0].data_pages==1);
    assert(g_vmalloc_areas[0].total_pages==66&&bitmap_test_bit(g_vmalloc_bitmap,65));
    unmap_before=unmap_calls;vfree(p);assert(unmap_calls-unmap_before==1&&live==1);
    fail_unmap_va=0;vfree(p);assert(!live);empty();
    page_t *borrowed[3];for(unsigned i=0;i<3;++i) borrowed[i]=page_alloc(0,PAGE_ALLOC_ZERO);
    void *out=(void*)123;
    assert(vmalloc_map_pages(borrowed,3,X86_CACHE_UC,&out)==K_OK);guards(out,3);
    unsigned before=physical_lookups;
    uint64_t irq_flags=vmalloc_lock_irqsave();
    assert(previous_release(&g_vmalloc_areas[0]));
    vmalloc_unlock_irqrestore(irq_flags);
    assert(physical_lookups==before+3&&live==3);empty();
    assert(vmalloc_map_pages(borrowed,3,X86_CACHE_UC,&out)==K_OK);guards(out,3);
    before=physical_lookups;
    fail_unmap_va=(uintptr_t)out;vfree(out);assert(live==3 && g_vmalloc_pending_cleanup==1);
    assert(physical_lookups==before);
    vfree(out);assert(live==3 && g_vmalloc_pending_cleanup==1);
    assert(physical_lookups==before);
    assert(vmalloc_last_failure()==(0x400U | (unsigned)-K_EIO));
    fail_unmap_va=0;p=vmalloc(PAGE_SIZE);assert(p && live==4&&physical_lookups==before);
    vfree(p);assert(live==3&&physical_lookups==before+1);empty();
    out=(void*)123;fail_map=map_calls+3;fail_unmap_va=X86_64_VMALLOC_BASE+PAGE_SIZE;
    before=physical_lookups;
    assert(vmalloc_map_pages(borrowed,3,X86_CACHE_WB,&out)==K_EIO && out==(void*)123);
    assert(live==3 && g_vmalloc_pending_cleanup==1);
    assert(physical_lookups==before);
    fail_map=0;fail_unmap_va=0;
    p=vmalloc(PAGE_SIZE);assert(p && !g_vmalloc_pending_cleanup);vfree(p);empty();
    for(unsigned i=0;i<3;++i) { page_free(borrowed[i]); }
    assert(!live);
    /* 公共事务在每个映射位置失败时都须保持所有权与输出发布规则。 */
    unsigned failure_cases=0;
    for(unsigned count=1;count<=8;++count)for(unsigned failed=1;failed<=count;++failed) {
        fail_alloc=alloc_calls+failed;
        assert(!vmalloc(count*PAGE_SIZE)&&!live);empty();fail_alloc=0;
        fail_map=map_calls+failed;
        assert(!vmalloc(count*PAGE_SIZE)&&!live);empty();fail_map=0;
        page_t *borrowed_failure[8];
        for(unsigned i=0;i<count;++i)borrowed_failure[i]=page_alloc(0,PAGE_ALLOC_ZERO);
        out=(void*)123;fail_map=map_calls+failed;
        assert(vmalloc_map_pages(borrowed_failure,count,X86_CACHE_UC,&out)==K_EIO);
        assert(out==(void*)123&&live==count);empty();fail_map=0;
        for(unsigned i=0;i<count;++i)page_free(borrowed_failure[i]);
        assert(!live);failure_cases+=3;
    }
    printf("Unified mapping rollback: %u allocation/owned-map/borrowed-map failure positions PASS\n",failure_cases);
    static const unsigned batch_sizes[]={1,2,64,1024,4096};
    page_t *batch[4096];
    for(unsigned b=0;b<sizeof(batch_sizes)/sizeof(batch_sizes[0]);++b) {
        unsigned count=batch_sizes[b];
        for(unsigned i=0;i<count;++i) {batch[i]=page_alloc(0,PAGE_ALLOC_ZERO);assert(batch[i]);}
        assert(vmalloc_map_pages(batch,count,X86_CACHE_WB,&out)==K_OK);guards(out,count);
        before=physical_lookups;irq_flags=vmalloc_lock_irqsave();
        assert(previous_release(&g_vmalloc_areas[0]));vmalloc_unlock_irqrestore(irq_flags);
        assert(physical_lookups==before+count&&live==count);empty();
        assert(vmalloc_map_pages(batch,count,X86_CACHE_WB,&out)==K_OK);guards(out,count);
        before=physical_lookups;vfree(out);
        assert(physical_lookups==before&&live==count);empty();
        for(unsigned i=0;i<count;++i)page_free(batch[i]);
        assert(!live);
    }
    void *slots[VMALLOC_MAX_AREAS];
    area_visits=0;
    for(unsigned i=0;i<VMALLOC_MAX_AREAS;++i) { slots[i]=vmalloc(PAGE_SIZE);assert(slots[i]); }
    assert(area_visits==VMALLOC_MAX_AREAS);
    assert(!vmalloc(PAGE_SIZE));
    assert(area_visits==VMALLOC_MAX_AREAS);
    /* Holes must be reused in record-index order, not release order. */
    vfree(slots[700]);vfree(slots[123]);
    void *low=vmalloc(PAGE_SIZE),*high=vmalloc(PAGE_SIZE);
    assert(low==slots[123] && high==slots[700]);
    assert(g_vmalloc_areas[123].address==(vaddr_t)(uintptr_t)low);
    assert(g_vmalloc_areas[700].address==(vaddr_t)(uintptr_t)high);
    for(unsigned i=0;i<VMALLOC_MAX_AREAS;++i) { vfree(slots[i]); }
    assert(!live);empty();
    void *model[VMALLOC_MAX_AREAS]={0};unsigned seed=19;
    for(unsigned operation=0;operation<20000;++operation) {
        seed=seed*1664525U+1013904223U;
        unsigned index=(seed>>8)%VMALLOC_MAX_AREAS;
        if(model[index]) {vfree(model[index]);model[index]=0;}
        else {
            unsigned first=0;while(first<VMALLOC_MAX_AREAS && model[first])++first;
            void *p=vmalloc(PAGE_SIZE);
            if(first==VMALLOC_MAX_AREAS)assert(!p);
            else {
                assert(p && g_vmalloc_areas[first].address==(vaddr_t)(uintptr_t)p);
                model[first]=p;
            }
        }
        unsigned first=0;while(first<VMALLOC_MAX_AREAS && model[first])++first;
        assert(g_vmalloc_next_free_area<=first);
        for(unsigned i=0;i<VMALLOC_MAX_AREAS;++i)
            assert(g_vmalloc_areas[i].used==(model[i]!=0));
        unsigned limit=VMALLOC_MAX_AREAS;
        while(limit&&!model[limit-1])--limit;
        assert(g_vmalloc_area_limit==limit);
    }
    for(unsigned i=0;i<VMALLOC_MAX_AREAS;++i)if(model[i])vfree(model[i]);
    assert(!live);empty();
    puts("vmalloc lifecycle: owned/borrowed pages, guards, 1024 fill-slot reads, ordered hole reuse, area limit, map/allocation failures, retained rollback, saved root and cleanup retry passed");
    puts("Borrowed release: 1/2/3/64/1024/4096 pages use zero rather than one metadata query per page, including partial-unmap failures/rollback/retries; owned release still queries and frees each successfully unmapped page");
    puts("Area scan bound: empty/one-live misses inspect 0/1 instead of 1024 records; trailing releases shrink 128 -> 64; 20000 occupancy states retain exact last-used boundary and failed-cleanup reservations");
    assert(sizeof(g_vmalloc_areas)==32768U);
    puts("Pending cleanup scan: last retry at slot zero visits 1 instead of 128 records; failed retry preserves the full scan and live suffix");
    puts("Area metadata: actual 32-byte records occupy 32 KiB, removing 8 KiB of unused request-size bookkeeping");
    puts("Cleanup retries: 4096-page head failure retries 1 instead of 4096 pages; every failed position in a 64-page area preserves the complete reservation, references and saved-root cleanup until success");
}
'''
with tempfile.TemporaryDirectory(prefix='liteos-vmalloc-lifecycle-') as directory:
    path=Path(directory)
    def compile_test(actual):
        subprocess.run(['cc','-std=c11','-O2','-Wall','-Wextra','-Werror',
                    '-I',str(root/'include'),'-fsanitize=address,undefined',
                    '-fno-pie','-no-pie','-x','c','-','-o',str(path/'test')],
                       input=actual,text=True,check=True)
    compile_test(program)
    subprocess.run([str(path/'test')],check=True,timeout=20)
    borrowed_free=program.replace('if (area->owns_pages) {\n                g_vmalloc_failure',
                                  'if (true) {\n                g_vmalloc_failure',1)
    assert borrowed_free != program
    compile_test(borrowed_free)
    failed=subprocess.run([str(path/'test')],capture_output=True,text=True,timeout=20,
                          preexec_fn=lambda:resource.setrlimit(resource.RLIMIT_CORE,(0,0)))
    assert failed.returncode and 'live==3' in failed.stderr,failed.stderr
    print('Negative control rejected: mapping failure frees a borrowed page')
