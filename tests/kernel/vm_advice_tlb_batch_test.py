"""Actual madvise + sparse TLB unmapper across VMA boundaries and failures."""
from pathlib import Path
import subprocess
import tempfile
import resource

root=Path(__file__).resolve().parents[2]
source=(root/'kernel/mm/protection.c').read_text()
checks=source[source.index('static bool range_fully_mapped('):source.index('bool vm_range_is_mapped(')]
advice=source[source.index('kstatus_t vm_advise('):source.index('static kstatus_t vm_protect_impl(')]
tlb=(root/'kernel/mm/tlb.c').read_text()
tlb=tlb[tlb.index('static void rss_decrement('):]
fixture=r'''
#include <assert.h>
#include <string.h>
#include <stdio.h>
#include <kernel/vm.h>
#include <arch/x86_64/paging.h>
#include <uapi/mm.h>
#define BASE 0x10000ULL
static vm_space_t space;
static vm_area_t areas[512];static vm_object_t objects[512];
static bool present[512];
static unsigned locked,batches,discarded,fail_batch;
typedef struct anon_page_node { struct anon_page_node *next; } anon_page_node_t;
static anon_page_node_t detached[512];
static unsigned released,fail_discard;
static void map_lock(vm_space_t *s) { assert(s==&space&&!locked);locked=1; }
static void map_unlock(vm_space_t *s) { assert(s==&space&&locked);locked=0; }
static bool range_valid(uint64_t address,size_t size,uint64_t *end) {
    if(!size || address%PAGE_SIZE || size%PAGE_SIZE || size>UINT64_MAX-address)return false;
    *end=address+size;return true;
}
static vm_area_t *area_from_list(list_head_t *node) { return list_entry(node,vm_area_t,ordered_node); }
static vm_area_t *find_area(const vm_space_t *s,uint64_t address) {
    for(list_head_t *n=s->area_list.next;n!=&s->area_list;n=n->next) {
        vm_area_t *a=area_from_list(n);
        if(a->start<=address&&address<a->end)return a;
    }
    return 0;
}
kstatus_t x86_next_present_page(paddr_t table,vaddr_t first,vaddr_t end,vaddr_t *result) {
    assert(table.value==1&&locked);
    for(uint64_t address=first;address<end;address+=PAGE_SIZE) {
        unsigned i=(address-BASE)/PAGE_SIZE;assert(i<512);
        if(present[i]) { *result=address;return K_OK; }
    }
    return K_ENOENT;
}
kstatus_t x86_unmap_user_page_batch(paddr_t table,const vaddr_t *addresses,size_t count,size_t *unmapped) {
    assert(table.value==1&&locked&&count&&count<=X86_USER_UNMAP_BATCH_MAX);
    ++batches;*unmapped=0;
    if(batches==fail_batch)return K_EIO;
    for(size_t i=0;i<count;i++) {
        unsigned index=(addresses[i]-BASE)/PAGE_SIZE;assert(index<512&&present[index]);
        present[index]=false;++*unmapped;
    }
    return K_OK;
}
static kstatus_t anon_page_retire_range(vm_object_t *object,uint64_t first,uint64_t end,
                                        anon_page_node_t **out) {
    assert(locked&&object&&first==0&&end==1);
    if(discarded+1==fail_discard)return K_EIO;
    assert(discarded<512);detached[discarded].next=*out;
    *out=&detached[discarded++];return K_OK;
}
static void anon_page_release_retired(anon_page_node_t *pages) {
    assert(!locked);
    while(pages){++released;pages=pages->next;}
}
'''
program=r'''
static void prepare(unsigned count) {
    memset(&space,0,sizeof(space));memset(areas,0,sizeof(areas));memset(objects,0,sizeof(objects));
    memset(present,0,sizeof(present));space.root_table=paddr_make(1);list_init(&space.area_list);
    for(unsigned i=0;i<count;i++) {
        areas[i].start=BASE+i*PAGE_SIZE;areas[i].end=areas[i].start+PAGE_SIZE;
        areas[i].flags=VM_MAP_PRIVATE;areas[i].object=&objects[i];objects[i].type=VM_OBJECT_ANON;
        list_add_tail(&space.area_list,&areas[i].ordered_node);present[i]=true;
    }
    atomic_store(&space.rss_pages,count);locked=batches=discarded=fail_batch=0;
    released=fail_discard=0;
}
static kstatus_t advise(unsigned pages) { return vm_advise(&space,BASE,pages*PAGE_SIZE,OS_VM_ADVICE_DONTNEED); }
int main(void) {
    prepare(64);
    assert(vm_advise(&space,BASE,PAGE_SIZE,UINT32_MAX)==K_EINVAL);
    assert(vm_advise(&space,BASE,PAGE_SIZE,18)==K_EINVAL);
    assert(!locked&&!batches&&!discarded); /* BoringSSL must see unsupported probes fail. */
    prepare(64);assert(advise(64)==K_OK&&batches==1&&discarded==64&&!locked);
    assert(released==64);
    assert(!atomic_load(&space.rss_pages)&&atomic_load(&space.advice_generation)==1);
    printf("64 adjacent VMAs / 64 resident pages: %u TLB batch, not 64\n",batches);
    prepare(65);assert(advise(65)==K_OK&&batches==2&&discarded==65);
    prepare(512);memset(present,0,sizeof(present));present[0]=present[511]=true;
    atomic_store(&space.rss_pages,2);assert(advise(512)==K_OK&&batches==1&&discarded==512);
    assert(!atomic_load(&space.rss_pages));
    prepare(64);areas[25].flags=VM_MAP_SHARED;
    assert(advise(64)==K_OK&&batches==1&&discarded==63);
    prepare(64);areas[63].flags|=VM_MAP_GUARD;
    assert(advise(64)==K_EINVAL&&!batches&&!discarded&&!locked);
    prepare(64);list_del(&areas[32].ordered_node);
    assert(advise(64)==K_ENOENT&&!batches&&!discarded&&!locked);
    prepare(65);fail_batch=2;
    assert(advise(65)==K_EIO&&batches==2&&!discarded&&!locked);
    assert(atomic_load(&space.rss_pages)==1&&!atomic_load(&space.advice_generation));
    for(unsigned i=0;i<64;i++)assert(!present[i]);
    assert(present[64]);
    prepare(64);memset(present,0,sizeof(present));atomic_store(&space.rss_pages,0);
    assert(advise(64)==K_OK&&!batches&&discarded==64&&!locked);
    assert(released==64);
    for(unsigned failure=1;failure<=64;++failure) {
        prepare(64);fail_discard=failure;
        assert(advise(64)==K_EIO && !locked);
        assert(discarded==failure-1 && released==discarded);
    }
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='vm-advice-tlb-') as directory:
    base=Path(directory);c=base/'test.c';binary=base/'test'
    c.write_text(fixture+checks+tlb+advice+program)
    command=['cc','-std=gnu11','-O1','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-fno-pie','-no-pie','-I'+str(root/'include'),str(c),'-o',str(binary)]
    subprocess.run(command,check=True)
    subprocess.run([str(binary)],check=True,timeout=10)
    start=advice.index('    kstatus_t unmap_status = vm_tlb_unmap_range(')
    end=advice.index('\n    atomic_fetch_add_explicit(&space->advice_generation',start)
    legacy=r'''
    for(list_head_t *n=&first_area->ordered_node;n!=range_end;n=n->next) {
        vm_area_t *a=area_from_list(n);
        uint64_t first=a->start>address?a->start:address,last=a->end<end?a->end:end;
        kstatus_t status=vm_tlb_unmap_range(space,first,last);
        if(status!=K_OK) { map_unlock(space);return status; }
    }
'''
    c.write_text(fixture+checks+tlb+advice[:start]+legacy+advice[end:]+program)
    subprocess.run(command,check=True)
    def no_core():resource.setrlimit(resource.RLIMIT_CORE,(0,0))
    negative=subprocess.run([str(binary)],capture_output=True,timeout=10,preexec_fn=no_core)
    assert negative.returncode!=0 and b'batches==1' in negative.stderr
    # Both success and partial-retirement failures must release after unlock.
    reordered=advice.replace(
        'map_unlock(space);\n    anon_page_release_retired(retired);',
        'anon_page_release_retired(retired);\n    map_unlock(space);')
    assert reordered!=advice
    c.write_text(fixture+checks+tlb+reordered+program)
    subprocess.run(command,check=True)
    negative=subprocess.run([str(binary)],capture_output=True,timeout=10,preexec_fn=no_core)
    assert negative.returncode!=0 and b'!locked' in negative.stderr
print('VM advice batching: boundaries, sparse pages, shared backing, guard/hole preflight, failed shootdown/RSS and negative control passed')
