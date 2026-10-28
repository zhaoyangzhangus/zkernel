"""Actual surface populate: batched accounting, existing pages, partial failure."""
from pathlib import Path
import subprocess
import tempfile

root=Path(__file__).resolve().parents[2]
source=(root/'kernel/mm/fault.c').read_text()
code=source[source.index('static kstatus_t vm_populate_range_internal('):
            source.index('kstatus_t vm_populate_range(',source.index('static kstatus_t vm_populate_range_internal('))]
code=code.replace('atomic_fetch_add_explicit(', 'count_add(')
vma=(root/'kernel/mm/vma.c').read_text()
ranges=vma[vma.index('bool range_size_valid('):vma.index('static bool find_gap_subtree(')]
program=r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <kernel/vm.h>
#define VM_USER_BASE 0x10000ULL
#define VM_USER_END 0x800000000000ULL
#define X86_CACHE_WB 0
static vm_space_t space;static vm_area_t area;static vm_object_t object;
static page_t pages[4096];static bool present[4096],mapped[4096];
static unsigned held,n,kind,fail_index,calls[4];static bool around;
static uint64_t count_add(atomic_uint_fast64_t *value,uint64_t count,memory_order order) {
    unsigned i=value==&space.rss_pages?0:value==&space.tlb_generation?1:
               value==&object.prefaulted_pages?2:3;
    assert(held && count);++calls[i];return atomic_fetch_add_explicit(value,count,order);
}
''' + ranges + r'''
static void map_lock(vm_space_t *s) {assert(s==&space && !held);held=1;}
static void map_unlock(vm_space_t *s) {
    assert(s==&space && held);unsigned count=0;
    for(unsigned i=0;i<n;++i)count+=mapped[i];
    assert(atomic_load(&space.rss_pages)==17+count);
    assert(atomic_load(&space.tlb_generation)==11+(count!=0));
    assert(atomic_load(&object.prefaulted_pages)==29+count);
    assert(atomic_load(&object.fault_around_pages)==41+(around?count:0));
    for(unsigned i=0;i<4;++i)assert(calls[i]==(unsigned)(count!=0 && (i!=3 || around)));
    held=0;
}
static vm_area_t *find_area(const vm_space_t *s,uint64_t address) {
    assert(s==&space && held);return address>=area.start && address<area.end?&area:0;
}
void vm_object_get(vm_object_t *o) {assert(o==&object);atomic_fetch_add(&o->refs.value,1);}
void vm_object_put(vm_object_t *o) {assert(!held && o==&object);atomic_fetch_sub(&o->refs.value,1);}
static uint32_t hardware_flags(uint32_t prot,bool cow) {assert(!cow);return prot;}
static kstatus_t x86_translate_page(paddr_t root,vaddr_t address,paddr_t *out,uint64_t *flags) {
    (void)root;(void)out;(void)flags;assert(held);
    unsigned i=(unsigned)((address-area.start)/PAGE_SIZE);assert(i<n);
    if(kind==1 && i==fail_index)return K_EIO;
    return present[i]?K_OK:K_ENOENT;
}
static kstatus_t anon_page_get(vm_object_t *o,uint64_t index,bool create,bool pin,page_t **out) {
    assert(!pin && held && o==&object && create && index>=5);
    unsigned i=(unsigned)(index-5);assert(i<n && !present[i]);
    if(kind==2 && i==fail_index)return K_ENOMEM;
    *out=&pages[i];return K_OK;
}
paddr_t page_to_phys(const page_t *page) {return (paddr_t){(uint64_t)(page-pages)*PAGE_SIZE};}
static kstatus_t x86_map_page(paddr_t root,vaddr_t address,paddr_t physical,uint32_t flags,uint32_t cache) {
    (void)root;assert(held && flags==area.prot && cache==X86_CACHE_WB);
    unsigned i=(unsigned)((address-area.start)/PAGE_SIZE);assert(i<n && physical.value==i*PAGE_SIZE);
    if(kind==3 && i==fail_index)return K_EIO;
    assert(!mapped[i]);mapped[i]=true;return K_OK;
}
kstatus_t vm_handle_fault(vm_space_t *s,const vm_fault_info_t *fault) {(void)s;(void)fault;assert(0);return K_EIO;}
''' + code + r'''
static void run(unsigned count,unsigned mask,unsigned failure,unsigned at,bool fault_around) {
    memset(&space,0,sizeof(space));memset(&area,0,sizeof(area));memset(&object,0,sizeof(object));
    memset(present,0,sizeof(present));memset(mapped,0,sizeof(mapped));memset(calls,0,sizeof(calls));
    n=count;kind=failure;fail_index=at;around=fault_around;
    object.type=VM_OBJECT_SHARED;object.flags=VM_OBJECT_FLAG_WINDOW_SURFACE;
    object.size=(count+5)*PAGE_SIZE;refcount_init(&object.refs,1);
    area.start=VM_USER_BASE;area.end=area.start+count*PAGE_SIZE;area.object_offset=5*PAGE_SIZE;
    area.object=&object;area.prot=VM_PROT_USER|VM_PROT_READ|((mask&1)?0:VM_PROT_WRITE);
    atomic_init(&space.rss_pages,17);atomic_init(&space.tlb_generation,11);
    atomic_init(&object.prefaulted_pages,29);atomic_init(&object.fault_around_pages,41);
    for(unsigned i=0;i<n;++i)present[i]=(mask&(1U<<(i%8)))!=0;
    unsigned stop=n;kstatus_t expected=K_OK;
    for(unsigned i=0;i<n;++i)if(i==at && (kind==1 || (!present[i] && kind>=2))) {
        stop=i;expected=kind==2?K_ENOMEM:K_EIO;break;
    }
    kstatus_t result=vm_populate_range_internal(&space,area.start,count*PAGE_SIZE-(around?1:0),around);
    assert(result==expected && !held && atomic_load(&object.refs.value)==1);
    for(unsigned i=0;i<n;++i)assert(mapped[i]==(i<stop && !present[i]));
}
int main(void) {
    unsigned cases=0;
    for(unsigned mask=0;mask<256;++mask)for(unsigned kind=0;kind<4;++kind)
      for(unsigned at=0;at<=8;++at)for(unsigned around=0;around<2;++around) {
        run(8,mask,kind,at,around);++cases;
      }
    run(4096,0,0,4096,false);run(4096,0,0,4096,true);run(4096,255,0,4096,true);
    assert(vm_populate_range_internal(0,area.start,PAGE_SIZE,false)==K_EINVAL);
    assert(vm_populate_range_internal(&space,area.start,0,false)==K_EINVAL);
    assert(vm_populate_range_internal(&space,area.start+1,PAGE_SIZE,false)==K_EINVAL);
    assert(vm_populate_range_internal(&space,area.start,SIZE_MAX,false)==K_EINVAL);
    assert(vm_populate_range_internal(&space,area.end,PAGE_SIZE,false)==K_EINVAL);
    assert(!held && atomic_load(&object.refs.value)==1);
    printf("Surface populate: %u complete/partial/existing-page oracle cases, 4096-page batches, zero empty accounting, one update per counter before unlock, offsets/refs/error statuses passed\n",cases+3);
}
'''
with tempfile.TemporaryDirectory(prefix='liteos-surface-populate-') as directory:
    binary=str(Path(directory)/'test')
    subprocess.run(['cc','-std=c11','-O2','-Wall','-Wextra','-Werror','-I'+str(root/'include'),
                    '-fsanitize=address,undefined','-fno-pie','-no-pie','-x','c','-','-o',binary],
                   input=program,text=True,check=True)
    subprocess.run([binary],check=True,timeout=60)
