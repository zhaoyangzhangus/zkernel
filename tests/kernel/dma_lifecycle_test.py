"""Production DMA table with mocked pages/IOMMU; never touches real hardware."""
from pathlib import Path
import subprocess
import tempfile

root=Path(__file__).resolve().parents[2]
source=(root/'kernel/drivers/core/dma.c').read_text()
assert source.count('if (pages[j] == page) return K_EINVAL;')==1
source=source.replace('if (pages[j] == page) return K_EINVAL;',
    'atomic_fetch_add(&duplicate_visits,1); if (pages[j] == page) return K_EINVAL;')
source=source.replace('for (uint32_t i = g_dma_next_free_slot; i < DMA_MAPPING_LIMIT; ++i) {',
    'for (uint32_t i = g_dma_next_free_slot; i < DMA_MAPPING_LIMIT; ++i) { atomic_fetch_add(&slot_visits,1);')
start=source.index('kstatus_t dma_unmap_checked(')
end=source.index('void dma_unmap(',start)
unmap=source[start:end].replace('for (uint32_t i = 0; i < DMA_MAPPING_LIMIT; ++i) {',
    'for (uint32_t i = 0; i < DMA_MAPPING_LIMIT; ++i) { atomic_fetch_add(&unmap_visits,1);')
source=source[:start]+unmap+source[end:]
code=r'''
#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdatomic.h>
static atomic_uint attempts,unmap_visits;
static atomic_uint slot_visits;
static atomic_uint duplicate_visits;
static _Bool parallel;
static _Thread_local unsigned preempt_depth;
static _Thread_local unsigned caller_depth;
void sched_preempt_disable(void) {++preempt_depth;}
void sched_preempt_enable(void) {assert(preempt_depth);--preempt_depth;}
static _Bool counted_cas(atomic_uint *p,unsigned *expected,unsigned value,
                        memory_order success,memory_order failure) {
    atomic_fetch_add(&attempts,1);
    if(parallel)while(atomic_load(&attempts)<8)sched_yield();
    return atomic_compare_exchange_strong_explicit(p,expected,value,success,failure);
}
''' + source.replace('atomic_compare_exchange_strong_explicit(&g_dma_init_state',
                     'counted_cas(&g_dma_init_state') + r'''
static device_t device;
static page_t pages[256];
static atomic_uint allocations,map_calls,unmap_calls;
static atomic_uint physical_translations;
static atomic_size_t allocation_bytes;
static int fail_after=-1;
static bool use_iommu,map_failure;
static kstatus_t unmap_status;
static dma_mapping_t *unmap_target;
static bool inspect_unmap;
void *kmalloc(size_t n,uint32_t flags) {
    assert(!flags && preempt_depth==caller_depth);
    if(fail_after==0)return 0;
    if(fail_after>0)--fail_after;
    void *p=malloc(n);assert(p);memset(p,0xa5,n);
    atomic_fetch_add(&allocation_bytes,n);
    atomic_fetch_add(&allocations,1);return p;
}
void *kzalloc(size_t n,uint32_t flags) { (void)n;(void)flags;abort(); }
void kfree(void *p) { assert(p && preempt_depth==caller_depth);assert(atomic_fetch_sub(&allocations,1)>0);free(p); }
void object_get(void *p) { assert(preempt_depth==caller_depth && p==&device && atomic_fetch_add(&device.object.refs.value,1)>0); }
void object_put(void *p) { assert(preempt_depth==caller_depth && p==&device && atomic_fetch_sub(&device.object.refs.value,1)>1); }
paddr_t page_to_phys(const page_t *p) {
    atomic_fetch_add(&physical_translations,1);
    assert(p>=pages && p<pages+256); return paddr_make(0x100000ULL+(uint64_t)(p-pages)*PAGE_SIZE);
}
bool iommu_hardware_enabled(void) { return use_iommu; }
kstatus_t iommu_map_pages(device_t *d,iova_t addr,page_t **p,uint32_t n,uint32_t access) {
    assert(preempt_depth==caller_depth+1 && atomic_load(&g_dma_lock.state));
    assert(d==&device && p && n && addr.value>=DMA_IOVA_BASE && access);
    atomic_fetch_add(&map_calls,1);return map_failure?K_EIO:K_OK;
}
kstatus_t iommu_unmap_pages(device_t *d,iova_t addr,uint64_t n) {
    assert(preempt_depth==caller_depth && !atomic_load(&g_dma_lock.state));
    assert(d==&device && addr.value>=DMA_IOVA_BASE && n);atomic_fetch_add(&unmap_calls,1);
    if(inspect_unmap) {
        assert(unmap_target && (unmap_target->flags&DMA_MAPPING_UNMAPPING_FLAG));
        dma_mapping_t destination={0};
        assert(dma_mapping_move(&destination,unmap_target)==K_EBUSY);
        assert(dma_unmap_checked(unmap_target)==K_EBUSY);
        assert(dma_validate_access(d,addr,n,DMA_DEVICE_READ)==K_EACCES);
    }
    return unmap_status;
}
static void *initializer(void *unused) {
    (void)unused;dma_initialize();assert(!atomic_load(&g_dma_lock.state));
    assert(g_dma_next_iova==DMA_IOVA_BASE);
    assert(g_dma_next_free_slot==0);
    for(unsigned i=0;i<DMA_MAPPING_LIMIT;++i)assert(!g_dma_mappings[i]);return 0;
}
static void check_clean(void) {
    assert(preempt_depth==caller_depth);
    assert(!atomic_load(&allocations) && atomic_load(&device.object.refs.value)==1);
    for(unsigned i=0;i<256;++i)
        assert(atomic_load(&pages[i].refs)==1 && !pages[i].private_data && !(pages[i].flags&PAGE_PINNED));
    for(unsigned i=0;i<DMA_MAPPING_LIMIT;++i)assert(!g_dma_mappings[i]);
    assert(!g_dma_next_free_slot);
}
static void *mapper(void *p) {
    unsigned index=(unsigned)(uintptr_t)p;page_t *array[]={&pages[index]};
    for(unsigned i=0;i<1000;++i) {
        dma_mapping_t m={0},moved={0};
        assert(dma_map_pages(&device,array,1,DMA_BIDIRECTIONAL,&m)==K_OK);
        assert(dma_mapping_move(&moved,&m)==K_OK && !m.flags && dma_mapping_active(&moved));
        assert(dma_unmap_checked(&moved)==K_OK && !moved.flags);
        assert(!preempt_depth);
    }
    return 0;
}
int main(void) {
    refcount_init(&device.object.refs,1);atomic_init(&device.state,DEVICE_ACTIVE);
    for(unsigned i=0;i<256;++i)atomic_init(&pages[i].refs,1);
    pthread_t threads[8];parallel=true;
    for(unsigned i=0;i<DMA_MAPPING_LIMIT;++i)g_dma_mappings[i]=(void *)(uintptr_t)1;
    g_dma_next_iova=UINT64_MAX;
    for(unsigned i=0;i<8;++i)assert(!pthread_create(&threads[i],0,initializer,0));
    for(unsigned i=0;i<8;++i)assert(!pthread_join(threads[i],0));parallel=false;
    assert(atomic_load(&attempts)==8);check_clean();
    unsigned move_cases=0;
    for(unsigned flags=0;flags<8;++flags)for(unsigned source_registered=0;source_registered<2;++source_registered)
    for(unsigned destination_registered=0;destination_registered<2;++destination_registered)for(unsigned fields=0;fields<128;++fields) {
        dma_mapping_t source={.device=&device,.pages=(void *)(uintptr_t)0x1000,.page_count=3,
            .direction=DMA_BIDIRECTIONAL,.segments=(void *)(uintptr_t)0x2000,.segment_count=2,
            .mapped_length=3*PAGE_SIZE,.flags=flags};
        dma_mapping_t destination={.direction=DMA_FROM_DEVICE};
        if(fields&1)destination.device=&device;
        if(fields&2)destination.pages=(void *)(uintptr_t)0x3000;
        if(fields&4)destination.page_count=1;
        if(fields&8)destination.segments=(void *)(uintptr_t)0x4000;
        if(fields&16)destination.segment_count=1;
        if(fields&32)destination.mapped_length=PAGE_SIZE;
        if(fields&64)destination.flags=1;
        dma_mapping_t source_before=source,destination_before=destination;
        g_dma_mappings[17]=source_registered?&source:0;
        g_dma_mappings[99]=destination_registered?&destination:0;
        bool active=(flags&DMA_MAPPING_ACTIVE_FLAG)!=0;
        kstatus_t expected;
        if(fields || destination_registered)expected=K_EBUSY;
        else if(!active)expected=source_registered?K_EINVAL:K_OK;
        else if(flags&DMA_MAPPING_UNMAPPING_FLAG)expected=K_EBUSY;
        else expected=source_registered?K_OK:K_EINVAL;
        assert(dma_mapping_move(&destination,&source)==expected);
        if(expected==K_OK) {
            dma_mapping_t zero={0};assert(!memcmp(&source,&zero,sizeof(source)));
            assert(!memcmp(&destination,&source_before,sizeof(destination)));
            assert(g_dma_mappings[17]==(active?&destination:0));
        } else {
            assert(!memcmp(&source,&source_before,sizeof(source)) && !memcmp(&destination,&destination_before,sizeof(destination)));
            assert(g_dma_mappings[17]==(source_registered?&source:0));
        }
        assert(g_dma_mappings[99]==(destination_registered?&destination:0));
        g_dma_mappings[17]=g_dma_mappings[99]=0;
        assert(!atomic_load(&g_dma_lock.state));++move_cases;
    }
    dma_mapping_t alias={.flags=DMA_MAPPING_ACTIVE_FLAG};
    assert(dma_mapping_move(&alias,&alias)==K_OK && alias.flags==DMA_MAPPING_ACTIVE_FLAG);
    assert(dma_mapping_move(0,&alias)==K_EINVAL && dma_mapping_move(&alias,0)==K_EINVAL);
    assert(move_cases==4096);check_clean();
    page_t *array[]={&pages[0],&pages[2],&pages[3]};
    dma_mapping_t m={0};
    for(unsigned failure=0;failure<2;++failure) {
        fail_after=(int)failure;assert(dma_map_pages(&device,array,3,DMA_TO_DEVICE,&m)==K_ENOMEM);
        fail_after=-1;check_clean();
    }
    for(unsigned direction=0;direction<=DMA_BIDIRECTIONAL;++direction) {
        atomic_store(&physical_translations,0);
        assert(dma_map_pages(&device,array,3,(enum dma_direction)direction,&m)==K_OK);
        assert(atomic_load(&physical_translations)==6); /* Validation + one formation pass. */
        assert(m.segment_count==2 && m.segments[0].length==PAGE_SIZE && m.segments[1].length==2*PAGE_SIZE);
        assert(m.pages!=array && m.pages[0]==&pages[0] && m.pages[1]==&pages[2]);
        for(unsigned access=0;access<=DMA_DEVICE_WRITE;++access) {
            bool allowed=direction==DMA_BIDIRECTIONAL || (direction==DMA_TO_DEVICE && access==DMA_DEVICE_READ) || (direction==DMA_FROM_DEVICE && access==DMA_DEVICE_WRITE);
            assert(dma_validate_access(&device,m.segments[0].addr,PAGE_SIZE,(enum dma_device_access)access)==(allowed?K_OK:K_EACCES));
        }
        assert(dma_unmap_checked(&m)==K_OK);check_clean();
    }
    unsigned seed=17;
    for(unsigned test=0;test<4096;++test) {
        unsigned indices[8]={0,1,2,3,4,5,6,7};
        for(unsigned i=7;i>0;--i) {
            seed=seed*1664525U+1013904223U;
            unsigned other=(seed>>8)%(i+1),saved=indices[i];indices[i]=indices[other];indices[other]=saved;
        }
        unsigned n=1+test%8;page_t *input[8];
        for(unsigned i=0;i<n;++i)input[i]=&pages[indices[i]];
        atomic_store(&physical_translations,0);
        size_t bytes_before=atomic_load(&allocation_bytes);
        assert(dma_map_pages(&device,input,n,DMA_BIDIRECTIONAL,&m)==K_OK);
        assert(atomic_load(&physical_translations)==2*n && m.mapped_length==(uint64_t)n*PAGE_SIZE);
        unsigned first=0,segment=0;
        while(first<n) {
            unsigned run=1;
            while(first+run<n && indices[first+run]==indices[first]+run)++run;
            assert(m.segments[segment].addr.value==0x100000ULL+(uint64_t)indices[first]*PAGE_SIZE);
            assert(m.segments[segment].length==(uint64_t)run*PAGE_SIZE);
            first+=run;++segment;
        }
        assert(segment==m.segment_count);
        /* 独立的物理连续区基准也会计算分配大小：合并后的段不应按 page_count 过量分配。 */
        assert(atomic_load(&allocation_bytes)-bytes_before==
               n*sizeof(page_t *)+segment*sizeof(dma_segment_t));
        for(unsigned s=0;s<segment;++s)
            assert(dma_validate_access(&device,m.segments[s].addr,
                   m.segments[s].length,DMA_DEVICE_READ)==K_OK);
        memset(input,0,sizeof(input)); /* Mapping must retain its own stable pointer copy. */
        for(unsigned i=0;i<n;++i)assert(m.pages[i]==&pages[indices[i]]);
        assert(dma_unmap_checked(&m)==K_OK);check_clean();
    }
    page_t *duplicate[]={&pages[0],&pages[0]};
    assert(dma_map_pages(&device,duplicate,2,DMA_TO_DEVICE,&m)==K_EINVAL);check_clean();
    for(unsigned reverse=0;reverse<2;++reverse) {
        page_t *ordered[8];
        for(unsigned i=0;i<8;++i)ordered[i]=&pages[reverse?7-i:i];
        atomic_store(&duplicate_visits,0);
        assert(dma_map_pages(&device,ordered,8,DMA_TO_DEVICE,&m)==K_OK);
        assert(atomic_load(&duplicate_visits)==(reverse?28U:0U));
        assert(dma_unmap_checked(&m)==K_OK);check_clean();
    }
    unsigned duplicate_cases=0;
    for(unsigned reverse=0;reverse<2;++reverse) {
        page_t *input[256];
        for(unsigned i=0;i<256;++i)input[i]=&pages[reverse?255-i:i];
        size_t before=atomic_load(&allocation_bytes);
        atomic_store(&physical_translations,0);
        assert(dma_map_pages(&device,input,256,DMA_TO_DEVICE,&m)==K_OK);
        assert(m.segment_count==(reverse?256U:1U));
        assert(atomic_load(&allocation_bytes)-before==
               sizeof(input)+m.segment_count*sizeof(dma_segment_t));
        assert(atomic_load(&physical_translations)==512);
        assert(dma_unmap_checked(&m)==K_OK);check_clean();
    }
    for(unsigned n=1;n<=6;++n)for(unsigned value=0;value<(1U<<(2*n));++value) {
        page_t *input[8];unsigned seen=0;bool repeated=false;
        for(unsigned i=0;i<n;++i) {
            unsigned index=(value>>(2*i))&3U;
            input[i]=&pages[index];
            if(seen&(1U<<index))repeated=true;
            seen|=1U<<index;
        }
        assert(dma_map_pages(&device,input,n,DMA_TO_DEVICE,&m)==(repeated?K_EINVAL:K_OK));
        if(!repeated)assert(dma_unmap_checked(&m)==K_OK);
        else assert(!dma_mapping_active(&m));
        check_clean();++duplicate_cases;
    }
    assert(duplicate_cases==5460);
    pages[0].flags=PAGE_FREE;
    assert(dma_map_pages(&device,array,3,DMA_TO_DEVICE,&m)==K_EINVAL);
    pages[0].flags=0;check_clean();
    use_iommu=true;map_failure=true;
    assert(dma_map_pages(&device,array,3,DMA_BIDIRECTIONAL,&m)==K_EIO);check_clean();map_failure=false;
    assert(dma_map_pages(&device,array,3,DMA_BIDIRECTIONAL,&m)==K_OK && m.segment_count==1);
    unmap_target=&m;inspect_unmap=true;unmap_status=K_EIO;
    assert(dma_unmap_checked(&m)==K_EIO && dma_mapping_active(&m));
    assert(atomic_load(&pages[0].refs)==2 && pages[0].private_data==1 && atomic_load(&allocations)==2);
    unmap_status=K_ENOENT;assert(dma_unmap_checked(&m)==K_OK);inspect_unmap=false;check_clean();
    use_iommu=false;unmap_status=K_OK;
    static dma_mapping_t mappings[DMA_MAPPING_LIMIT+1];page_t *single[]={&pages[0]};
    atomic_store(&slot_visits,0);
    for(unsigned i=0;i<DMA_MAPPING_LIMIT;++i)assert(dma_map_pages(&device,single,1,DMA_TO_DEVICE,&mappings[i])==K_OK);
    assert(atomic_load(&slot_visits)==DMA_MAPPING_LIMIT && g_dma_next_free_slot==DMA_MAPPING_LIMIT);
    assert(dma_map_pages(&device,single,1,DMA_TO_DEVICE,&mappings[DMA_MAPPING_LIMIT])==K_ENOMEM);
    assert(atomic_load(&slot_visits)==DMA_MAPPING_LIMIT);
    assert(atomic_load(&pages[0].refs)==DMA_MAPPING_LIMIT+1 && pages[0].private_data==DMA_MAPPING_LIMIT);
    atomic_store(&unmap_visits,0);
    assert(dma_unmap_checked(&mappings[DMA_MAPPING_LIMIT-1])==K_OK);
    assert(atomic_load(&unmap_visits)==DMA_MAPPING_LIMIT); /* No second full-table removal scan. */
    dma_mapping_t moved={0};assert(dma_mapping_move(&moved,&mappings[100])==K_OK);
    assert(g_dma_mappings[100]==&moved && !mappings[100].flags);
    for(unsigned i=0;i<DMA_MAPPING_LIMIT;++i)assert(dma_unmap_checked(&mappings[i])==K_OK);
    assert(dma_unmap_checked(&moved)==K_OK);check_clean();
    dma_mapping_t *oracle_slots[DMA_MAPPING_LIMIT]={0};
    for(unsigned step=0;step<20000;++step) {
        seed=seed*1664525U+1013904223U;unsigned index=(seed>>8)%(DMA_MAPPING_LIMIT+1);
        dma_mapping_t *mapping=&mappings[index];
        if(dma_mapping_active(mapping)) {
            unsigned slot=0;while(slot<DMA_MAPPING_LIMIT && oracle_slots[slot]!=mapping)++slot;
            assert(slot<DMA_MAPPING_LIMIT && dma_unmap_checked(mapping)==K_OK);oracle_slots[slot]=0;
        } else {
            unsigned slot=0;while(slot<DMA_MAPPING_LIMIT && oracle_slots[slot])++slot;
            kstatus_t status=dma_map_pages(&device,single,1,DMA_TO_DEVICE,mapping);
            assert(status==(slot<DMA_MAPPING_LIMIT?K_OK:K_ENOMEM));
            if(status==K_OK)oracle_slots[slot]=mapping;
        }
        unsigned lowest=DMA_MAPPING_LIMIT;
        for(unsigned i=0;i<DMA_MAPPING_LIMIT;++i) {
            assert(g_dma_mappings[i]==oracle_slots[i]);
            if(!oracle_slots[i] && lowest==DMA_MAPPING_LIMIT)lowest=i;
        }
        assert(g_dma_next_free_slot<=lowest);
        for(unsigned i=0;i<g_dma_next_free_slot;++i)assert(g_dma_mappings[i]);
    }
    for(unsigned i=0;i<DMA_MAPPING_LIMIT+1;++i)assert(dma_unmap_checked(&mappings[i])==K_OK);
    check_clean();
    for(unsigned nesting=0;nesting<32;++nesting) {
        for(unsigned i=0;i<nesting;++i)sched_preempt_disable();
        caller_depth=nesting;
        dma_lock();assert(preempt_depth==nesting+1);
        dma_unlock();assert(preempt_depth==nesting);
        for(unsigned failure=0;failure<2;++failure) {
            fail_after=(int)failure;
            assert(dma_map_pages(&device,single,1,DMA_TO_DEVICE,&m)==K_ENOMEM);
            fail_after=-1;check_clean();
        }
        for(unsigned hardware=0;hardware<2;++hardware) {
            use_iommu=hardware;
            if(hardware) {
                map_failure=true;
                assert(dma_map_pages(&device,single,1,DMA_TO_DEVICE,&m)==K_EIO);
                map_failure=false;check_clean();
            }
            assert(dma_map_pages(&device,single,1,DMA_TO_DEVICE,&m)==K_OK);
            assert(preempt_depth==nesting);
            dma_mapping_t destination={0};
            assert(dma_mapping_move(&destination,&m)==K_OK && preempt_depth==nesting);
            assert(dma_validate_access(&device,destination.segments[0].addr,
                   PAGE_SIZE,DMA_DEVICE_READ)==K_OK && preempt_depth==nesting);
            assert(dma_validate_access(&device,destination.segments[0].addr,
                   PAGE_SIZE,DMA_DEVICE_WRITE)==K_EACCES && preempt_depth==nesting);
            if(hardware) {
                unmap_target=&destination;inspect_unmap=true;unmap_status=K_EIO;
                assert(dma_unmap_checked(&destination)==K_EIO && preempt_depth==nesting);
                assert(dma_mapping_active(&destination));
            }
            unmap_status=K_OK;
            assert(dma_unmap_checked(&destination)==K_OK && preempt_depth==nesting);
            inspect_unmap=false;check_clean();
        }
        caller_depth=0;
        for(unsigned i=0;i<nesting;++i)sched_preempt_enable();
        assert(!preempt_depth);
    }
    use_iommu=false;
    unsigned before=atomic_load(&attempts);
    for(unsigned i=0;i<10000;++i)dma_initialize();
    for(unsigned i=0;i<4;++i)assert(!pthread_create(&threads[i],0,mapper,(void *)(uintptr_t)i));
    for(unsigned i=0;i<4;++i)assert(!pthread_join(threads[i],0));
    assert(atomic_load(&attempts)==before);check_clean();
    puts("DMA: eight initializers, 4096 move states/segment permutations, 5460 duplicate-list oracle cases, ascending/reverse duplicate reads 0/28, 256 total fill-slot reads, 20000 lowest-free-slot oracle operations, allocation/IOMMU rollback, failed-unmap ownership, 32 nested preemption levels and 4000 concurrent cycles passed");
}
'''
with tempfile.TemporaryDirectory(prefix='liteos-dma-lifecycle-') as directory:
    binary=str(Path(directory)/'test')
    subprocess.run(['cc','-std=c11','-O2','-Wall','-Wextra','-Werror','-pthread',
                    '-I'+str(root/'include'),'-fsanitize=address,undefined',
                    '-fno-pie','-no-pie','-x','c','-','-o',binary],input=code,text=True,check=True)
    subprocess.run([binary],check=True,timeout=30)
