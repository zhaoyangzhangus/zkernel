"""Actual cached pool slot selection across separate metadata arrays."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'kernel/mm/phys/percpu_page.c').read_text()
helpers = source[source.index('static void pool_install_base('):source.index('static void pool_partial_push(')]
alloc = source[source.index('page_t *pool_alloc_small('):source.index('static PAGE_NOINLINE bool pool_free_slow(')]
slot = source[source.index('static bool pool_page_in_local_group('):source.index('bool pool_free_small(')]
# Independent physical-address path retained as the pre-cache oracle.
previous = slot[:slot.index('    uintptr_t metadata_delta =')]
previous += slot[slot.index('    uint64_t page_physical'):]
previous = previous.replace('pool_page_in_local_group(', 'previous_slot(')
free_small = source[source.index('bool pool_free_small('):source.index('static PAGE_NOINLINE void pool_free_o0_slow(')]
free_o0 = source[source.index('static PAGE_HOT_NOINLINE void pool_free_o0('):source.index('static void pool_drain_one(')]
program = '#include "' + str(root / 'kernel/mm/phys/internal.h') + '"\n' + r'''
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
page_o0_cache_t g_o0_cache[MAX_CPUS];
static page_local_pool_t small;
static page_t *first,*second;
static unsigned split,total,lookups,slow_calls,physical_lookups;
static const uint64_t base_pfn=4096;
page_t *page_for_pfn(pfn_t pfn) {
    ++lookups;
    if(pfn<base_pfn||pfn-base_pfn>=total)return 0;
    unsigned n=(unsigned)(pfn-base_pfn);
    return n<split?first+n:second+n-split;
}
page_t *page_span_for_pfn(pfn_t pfn,uint64_t *span) {
    page_t *page=page_for_pfn(pfn);if(!page)return 0;
    unsigned n=(unsigned)(pfn-base_pfn),available=n<split?split-n:total-n;
    if(*span>available)*span=available;
    return page;
}
paddr_t page_to_phys(const page_t *page) {
    ++physical_lookups;
    uintptr_t p=(uintptr_t)page,a=(uintptr_t)first,b=(uintptr_t)second;
    if(p>=a&&p-a<(size_t)split*sizeof(page_t)&&(p-a)%sizeof(page_t)==0)
        return (paddr_t){(base_pfn+(p-a)/sizeof(page_t))*PAGE_SIZE};
    if(p>=b&&p-b<(size_t)(total-split)*sizeof(page_t)&&(p-b)%sizeof(page_t)==0)
        return (paddr_t){(base_pfn+split+(p-b)/sizeof(page_t))*PAGE_SIZE};
    return (paddr_t){UINT64_MAX};
}
static page_local_pool_t *pool_for_cpu(uint32_t cpu,uint8_t zone,uint8_t order) {
    assert(cpu==0&&zone==PAGE_ZONE_NORMAL);
    return order==0?&g_o0_cache[0].normal:&small;
}
static page_t *pool_alloc_slow(uint8_t zone,uint8_t order,uint32_t cpu) {
    (void)zone;(void)order;(void)cpu;++slow_calls;return 0;
}
static bool pool_free_slow(page_t *page,uint8_t order,uint32_t cpu) {
    (void)page;(void)order;(void)cpu;++slow_calls;return false;
}
static void pool_free_o0_slow(page_t *page,uint32_t cpu) {
    (void)page;(void)cpu;++slow_calls;
}
''' + helpers + alloc + slot + previous + free_small + free_o0 + r'''
int main(void) {
    _Static_assert(sizeof(page_local_pool_t)==24,"Cached span reuses existing padding field");
    unsigned cases=0,negative_cases=0,free_cases=0;
    for(unsigned order=0;order<=5;++order) {
        total=64U<<order;
        for(split=1;split<=total;++split) {
            first=calloc(split,sizeof(*first));
            second=calloc(total-split+1U,sizeof(*second));assert(first&&second);
            page_local_pool_t *pool=pool_for_cpu(0,PAGE_ZONE_NORMAL,order);
            pool_prepare_group_slots(first,order);
            pool_install_base(pool,base_pfn,order);
            assert(pool->base==first&&pool->contiguous_pages==split);
            for(unsigned n=0;n<total;++n) {
                page_t *page=page_for_pfn(base_pfn+n);
                unsigned actual=0xdeadbeef,expected=actual;
                physical_lookups=0;
                bool a=pool_page_in_local_group(page,pool,order,&actual);
                assert(physical_lookups==(n<split?0U:2U));
                physical_lookups=0;
                bool b=previous_slot(page,pool,order,&expected);
                assert(physical_lookups==2U&&a==b&&actual==expected);
                assert(a==(n%(1U<<order)==0));
                if(a)assert(actual==(n>>order)&&actual<64);
                else assert(actual==0xdeadbeef);
                ++free_cases;
            }
            /* One-past metadata and physical group must not become slot 64. */
            unsigned outside=91;
            assert(!pool_page_in_local_group(second+total-split,pool,order,&outside));
            assert(outside==91);
            page_t *misaligned=(page_t *)((uintptr_t)first+1U);
            physical_lookups=0;
            assert(!pool_page_in_local_group(misaligned,pool,order,&outside));
            assert(outside==91&&!physical_lookups);
            assert(!pool_page_in_local_group(0,pool,order,&outside));
            assert(!pool_page_in_local_group(first,0,order,&outside));
            assert(!pool_page_in_local_group(first,pool,order,0));
            for(unsigned bit=0;bit<64;++bit) {
                unsigned offset=bit<<order;page_t *expected=page_for_pfn(base_pfn+offset);
                lookups=0;page_t *actual=pool_slot_page(pool,bit,order);
                assert(actual==expected&&lookups==(offset<split?0U:1U));
                assert(actual->u.compound.head==actual&&actual->u.compound.index==0);
                if(offset>=split) {
                    /* Integer-address control avoids forming an out-of-array
                     * pointer: the old arithmetic points into the wrong array. */
                    assert((uintptr_t)first+(size_t)offset*sizeof(*first)!=(uintptr_t)actual);
                    ++negative_cases;
                }
                ++cases;
            }
            pool->free_mask=UINT64_MAX;slow_calls=0;
            for(unsigned bit=0;bit<64;++bit) {
                page_t *expected=page_for_pfn(base_pfn+((uint64_t)bit<<order));
                expected->owner=PAGE_OWNER_PAGETABLE;
                expected->private_data=74;
                expected->flags|=PAGE_DIRTY|PAGE_UPTODATE|PAGE_WRITEBACK;
                atomic_store(&expected->mapcount,73);
                page_t *actual=order==0?pool_alloc_o0_normal(0):pool_alloc_small(PAGE_ZONE_NORMAL,order,0);
                assert(actual==expected&&atomic_load(&actual->refs)==1&&!slow_calls);
                assert(actual->owner==PAGE_OWNER_NONE && !actual->private_data);
                assert(actual->flags==PAGE_COMPOUND_HEAD && actual->order==order);
                assert(atomic_load(&actual->mapcount)==-1);
                assert(actual->u.compound.head==actual && !actual->u.compound.index);
            }
            assert(!pool->free_mask);
            assert(!pool_alloc_small(PAGE_ZONE_NORMAL,order,0)&&slow_calls==1);
            slow_calls=0;
            for(unsigned bit=0;bit<64;++bit) {
                page_t *page=page_for_pfn(base_pfn+((uint64_t)bit<<order));
                page->zone=PAGE_ZONE_NORMAL;
                physical_lookups=0;
                if(order==0)pool_free_o0(page,0);
                else assert(pool_free_small(page,order,0));
                assert(!slow_calls&&atomic_load(&page->refs)==0);
                assert(physical_lookups==((bit<<order)<split?0U:2U));
                assert(pool->free_mask==(bit==63?UINT64_MAX:(1ULL<<(bit+1))-1));
            }
            page_t *pinned=page_for_pfn(base_pfn+(11ULL<<order));pinned->flags|=PAGE_PINNED;
            pool->free_mask=(1ULL<<11)|(1ULL<<12);
            assert(pool_alloc_small(PAGE_ZONE_NORMAL,order,0)==page_for_pfn(base_pfn+(12ULL<<order)));
            assert(!pool->free_mask);
            pool_install_base(pool,base_pfn+total,order);
            assert(!pool->base&&!pool->contiguous_pages);
            free(first);free(second);
        }
    }
    assert(negative_cases>0);
    printf("Metadata spans: %u exact slot pointers, every split/order/head and actual 64-slot allocation; %u old cross-array addresses rejected; contiguous cache hits perform zero PFN queries, pin skipping/exhaustion preserved\n",cases,negative_cases);
    printf("Local free: %u head/tail and split-boundary cases match physical oracle; actual small/order-0 frees restore all 64 slots and references; cached array uses zero rather than two physical queries, cross-array fallback preserved\n",free_cases);
}
'''
with tempfile.TemporaryDirectory(prefix='liteos-pool-metadata-') as directory:
    binary = str(Path(directory) / 'test')
    subprocess.run(['cc', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
                    '-I' + str(root / 'include'), '-x', 'c', '-', '-o', binary],
                   input=program, text=True, check=True)
    subprocess.run([binary], check=True, timeout=60)
    old = program.replace('pool_prepare_allocated_page(page, order);',
        'atomic_store_explicit(&page->refs, 1U, memory_order_relaxed);')
    old = old.replace('pool_prepare_allocated_page(page, 0U);',
        'atomic_store_explicit(&page->refs, 1U, memory_order_relaxed);')
    assert old != program
    subprocess.run(['cc', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                    '-I' + str(root / 'include'), '-x', 'c', '-', '-o', binary],
                   input=old, text=True, check=True)
    result = subprocess.run([binary], capture_output=True)
    assert result.returncode != 0, 'stale page-table metadata escaped detection'
print('Negative control rejected: pooled allocation retains page-table owner/mapcount/private pin count')
