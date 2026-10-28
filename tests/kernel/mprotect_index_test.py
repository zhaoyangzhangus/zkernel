"""Real mprotect and VMA split/tree indexes on success and every failure exit."""
from pathlib import Path
import subprocess
import tempfile

root=Path(__file__).resolve().parents[2]
source=(root/'kernel/mm/protection.c').read_text()
helpers=source[source.index('static kstatus_t protect_resident_pages('):
               source.index('static bool vm_advice_supported(')]
transaction=source[source.index('static kstatus_t vm_protect_impl('):]
vma=(root/'kernel/mm/vma.c').read_text()
tree=vma[vma.index('vm_area_t *area_from_list('):vma.index('vm_area_t *area_allocate(')]
tree=tree.replace('void rebuild_area_tree(vm_space_t *space) {',
                  'void rebuild_area_tree(vm_space_t *space) { ++rebuilds;')
tree=tree.replace('vm_area_t *area_from_list(list_head_t *node) {',
                  'vm_area_t *area_from_list(list_head_t *node) { ++visits;')
tree=tree.replace('vm_area_t *find_area(const vm_space_t *space, uint64_t address) {',
                  'vm_area_t *find_area(const vm_space_t *space, uint64_t address) { ++boundary_seeks;')
split=vma[vma.index('vm_area_t *area_allocate('):]
program=r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <kernel/vm.h>
#include <arch/x86_64/paging.h>
#define VM_USER_BASE 0x10000ULL
#define VM_USER_END 0x800000000000ULL
#define VM_AREA_COW (1U<<31)
#define VM_AREA_CLONE_OLD_COW (1U<<30)
static vm_space_t space;
static vm_area_t areas[128];static vm_object_t backing[4],shadows[128];
static unsigned allocated,shadow_count,alloc_calls,shadow_calls,protect_calls,flushes;
static unsigned fail_alloc,fail_shadow,fail_protect,fail_flush,rebuilds,locked,bumps;
static unsigned visits,transaction_visits,boundary_seeks;
static uint64_t mock_base=VM_USER_BASE;
static uint32_t ptes[32],initial_flags;
static bool resident[32];
''' + tree + r'''
void *kzalloc(size_t size,unsigned flags) {
    (void)flags;assert(locked && size==sizeof(vm_area_t));
    if(++alloc_calls==fail_alloc)return 0;
    assert(allocated<128);return &areas[allocated++];
}
void vm_object_get(vm_object_t *o) {if(o)atomic_fetch_add(&o->refs.value,1);}
void vm_object_put(vm_object_t *o) {if(o)assert(atomic_fetch_sub(&o->refs.value,1)>0);}
static void area_release(vm_area_t *a) {
    assert(!locked);
    vm_object_put(a->object);vm_object_put(a->private_object);
}
kstatus_t vm_object_create_anon(size_t size,vm_object_t **out) {
    assert(locked);if(++shadow_calls==fail_shadow)return K_ENOMEM;
    assert(shadow_count<128);*out=&shadows[shadow_count++];(*out)->size=size;
    refcount_init(&(*out)->refs,1);return K_OK;
}
''' + split + r'''
static void map_lock(vm_space_t *s) {assert(s==&space && !locked);boundary_seeks=0;locked=1;}
static void map_unlock(vm_space_t *s) {
    assert(s==&space && locked);
    assert(boundary_seeks==1 && "mprotect must reuse both bounds from its initial seek");
    transaction_visits=visits;
    unsigned count=0;
    for(list_head_t *item=s->area_list.next;item!=&s->area_list;item=item->next) {
        vm_area_t *a=area_from_list(item);++count;
        assert(a->start<a->end && a->ordered_node.next->prev==&a->ordered_node);
        for(uint64_t va=a->start;va<a->end;va+=PAGE_SIZE)
            assert(find_area(s,va)==a && "split list/tree mismatch on unlock");
    }
    unsigned indexed=0;
    for(rb_node_t *node=rb_tree_first(&s->areas);node;node=rb_tree_next(node))++indexed;
    assert(count==indexed);
    for(unsigned i=0;i<32;++i)for(unsigned pages=1;pages<=8;++pages) {
        uint64_t hint=mock_base+i*PAGE_SIZE,size=pages*PAGE_SIZE,cursor=hint;
        if(cursor>=VM_USER_END || size>VM_USER_END-cursor)cursor=VM_USER_BASE;
        for(list_head_t *item=s->area_list.next;item!=&s->area_list;item=item->next) {
            vm_area_t *a=area_from_list(item);
            if(a->start>=cursor && size<=a->start-cursor)break;
            if(cursor<a->end)cursor=a->end;
        }
        uint64_t expected=size<=VM_USER_END-cursor?cursor:0;
        assert(find_gap(s,hint,size)==expected);
    }
    locked=0;
}
static uint32_t vm_area_page_flags(const vm_area_t *a,uint64_t va,uint32_t prot) {
    assert(va>=a->start && va<a->end);
    uint32_t flags=(prot&VM_PROT_USER)?X86_PAGE_USER:0;
    if(prot&VM_PROT_WRITE)flags|=X86_PAGE_WRITE;
    if(prot&VM_PROT_EXEC)flags|=X86_PAGE_EXEC;
    return flags;
}
bool x86_page_range_has_present_pages(paddr_t root,vaddr_t start,uint64_t end) {
    (void)root;assert(locked);
    for(uint64_t va=start;va<end;va+=PAGE_SIZE)if(resident[(va-mock_base)/PAGE_SIZE])return true;
    return false;
}
kstatus_t x86_next_present_page(paddr_t root,vaddr_t start,uint64_t end,vaddr_t *out) {
    (void)root;assert(locked);
    for(uint64_t va=start;va<end;va+=PAGE_SIZE)if(resident[(va-mock_base)/PAGE_SIZE]) {*out=va;return K_OK;}
    return K_ENOENT;
}
kstatus_t x86_protect_user_page_deferred(paddr_t root,vaddr_t va,uint32_t flags,
                                       enum x86_cache_mode mode,bool *changed) {
    (void)root;(void)mode;assert(locked);*changed=false;
    if(++protect_calls==fail_protect)return K_EINVAL;
    unsigned i=(unsigned)((va-mock_base)/PAGE_SIZE);
    *changed=ptes[i]!=flags;ptes[i]=flags;return K_OK;
}
kstatus_t x86_protect_page(paddr_t root,vaddr_t va,uint32_t flags,enum x86_cache_mode mode) {
    bool changed;return x86_protect_user_page_deferred(root,va,flags,mode,&changed);
}
bool x86_tlb_shootdown_root(paddr_t root) {(void)root;assert(locked);return ++flushes!=fail_flush;}
static void vm_tlb_bump_generation(vm_space_t *s) {(void)s;assert(locked);++bumps;}
''' + helpers + transaction + r'''
static void prepare(unsigned count,unsigned mode,bool file) {
    mock_base=VM_USER_BASE;
    memset(&space,0,sizeof(space));memset(areas,0,sizeof(areas));
    memset(backing,0,sizeof(backing));memset(shadows,0,sizeof(shadows));list_init(&space.area_list);
    allocated=count;shadow_count=alloc_calls=shadow_calls=protect_calls=flushes=bumps=0;
    fail_alloc=fail_shadow=fail_protect=fail_flush=0;
    initial_flags=X86_PAGE_USER|X86_PAGE_WRITE;
    for(unsigned i=0;i<32;++i) {ptes[i]=initial_flags;resident[i]=mode==1 || (mode==2 && i%2==0);}
    for(unsigned i=0;i<count;++i) {
        areas[i].start=VM_USER_BASE+i*(32/count)*PAGE_SIZE;
        areas[i].end=areas[i].start+(32/count)*PAGE_SIZE;
        areas[i].prot=VM_PROT_READ|VM_PROT_WRITE|VM_PROT_USER;
        areas[i].flags=VM_MAP_PRIVATE;areas[i].object=&backing[i];
        backing[i].type=file?VM_OBJECT_FILE:VM_OBJECT_ANON;backing[i].size=32*PAGE_SIZE;
        refcount_init(&backing[i].refs,1);list_add_tail(&space.area_list,&areas[i].ordered_node);
    }
    rebuild_area_tree(&space);rebuilds=visits=boundary_seeks=0;
}
int main(void) {
    /* Previously the end split was absent from the tree on shadow failure. */
    prepare(1,0,true);fail_shadow=1;
    assert(vm_protect(&space,VM_USER_BASE,8*PAGE_SIZE,VM_PROT_WRITE|VM_PROT_USER)==K_ENOMEM);
    assert(rebuilds==0 && !locked);
    unsigned cases=0;
    for(unsigned layout=0;layout<2;++layout)for(unsigned mode=0;mode<3;++mode)
      for(unsigned start=0;start<32;++start)for(unsigned end=start+1;end<=32;++end)
        for(unsigned failure=0;failure<8;++failure) {
            prepare(layout?4:1,mode,true);
            if(failure==1)fail_alloc=1;
            if(failure==2)fail_alloc=2;
            if(failure==3)fail_shadow=1;
            if(failure==4)fail_shadow=2;
            if(failure==5)fail_protect=1;
            if(failure==6)fail_protect=2;
            if(failure==7)fail_flush=1;
            uint32_t prot=VM_PROT_READ|VM_PROT_WRITE|VM_PROT_USER;
            /* Allocation failures exercise writable private-file shadows;
             * PTE failures exercise a real user write restriction. */
            if(failure>=5 || failure==0)prot=VM_PROT_READ|VM_PROT_USER;
            kstatus_t status=vm_protect(&space,VM_USER_BASE+start*PAGE_SIZE,(end-start)*PAGE_SIZE,prot);
            assert(!locked && rebuilds==0);
            for(unsigned i=0;i<32;++i) {
                vm_area_t *a=find_area(&space,VM_USER_BASE+i*PAGE_SIZE);assert(a);
                uint32_t expected=(status==K_OK && i>=start && i<end)?prot:
                                  VM_PROT_READ|VM_PROT_WRITE|VM_PROT_USER;
                assert(a->prot==expected && !(a->flags&VM_AREA_CLONE_OLD_COW));
                uint32_t hardware=(expected&VM_PROT_WRITE)?initial_flags:X86_PAGE_USER;
                assert(ptes[i]==(resident[i]?hardware:initial_flags));
            }
            assert(bumps==(status==K_OK));++cases;
        }
    for(unsigned mode=0;mode<3;++mode) {
        prepare(4,mode,false);
        assert(vm_protect(&space,VM_USER_BASE,32*PAGE_SIZE,VM_PROT_READ|VM_PROT_USER)==K_OK);
        assert(rebuilds==0);
        assert(vm_protect(&space,VM_USER_BASE,32*PAGE_SIZE,VM_PROT_READ|VM_PROT_USER)==K_OK);
        assert(rebuilds==0);
    }
    prepare(1,1,false);
    assert(vm_protect(&space,VM_USER_BASE,32*PAGE_SIZE,VM_PROT_WRITE|VM_PROT_EXEC)==K_EINVAL);
    assert(vm_protect_allow_rwx(&space,VM_USER_BASE,32*PAGE_SIZE,VM_PROT_WRITE|VM_PROT_EXEC|VM_PROT_USER)==K_OK);
    assert(!rebuilds);
    for(unsigned failure=0;failure<2;++failure) {
        prepare(4,0,true);
        areas[0].flags|=VM_AREA_CLONE_OLD_COW;
        areas[0].private_object=&shadows[100];refcount_init(&shadows[100].refs,1);
        fail_shadow=failure;
        assert(vm_protect(&space,areas[3].start,8*PAGE_SIZE,VM_PROT_WRITE|VM_PROT_USER)==
               (failure?K_ENOMEM:K_OK));
        assert(areas[0].private_object==&shadows[100] &&
               (areas[0].flags&VM_AREA_CLONE_OLD_COW) && atomic_load(&shadows[100].refs.value)==1);
        assert(transaction_visits==(failure?2:6));
    }
    for(unsigned target=0;target<4;++target) {
        /* 合并只额外访问左邻居、首段及右邻居，不扫描无关前缀或后缀。 */
        unsigned merge_visits=(target>0?2:0)+(target<3?1:0);
        prepare(4,0,false);
        assert(vm_protect(&space,areas[target].start,8*PAGE_SIZE,VM_PROT_READ|VM_PROT_USER)==K_OK);
        assert(transaction_visits==2+merge_visits);
        prepare(4,1,false);
        assert(vm_protect(&space,areas[target].start,8*PAGE_SIZE,VM_PROT_READ|VM_PROT_USER)==K_OK);
        assert(transaction_visits==3+merge_visits);
        prepare(4,1,false);fail_protect=1;
        assert(vm_protect(&space,areas[target].start,8*PAGE_SIZE,VM_PROT_READ|VM_PROT_USER)==K_EINVAL);
        assert(transaction_visits==3);
    }
    for(unsigned failure=1;failure<=3;++failure) {
        prepare(4,0,true);fail_shadow=failure;
        /* The untouched suffix belongs to another pending operation marker;
         * failed allocation must neither release nor clear it. */
        areas[3].private_object=&shadows[100];
        areas[3].flags|=VM_AREA_CLONE_OLD_COW;
        refcount_init(&shadows[100].refs,1);
        assert(vm_protect(&space,VM_USER_BASE,32*PAGE_SIZE,
                          VM_PROT_WRITE|VM_PROT_USER)==K_ENOMEM);
        assert(transaction_visits==4+failure+(failure-1));
        assert(!rebuilds && !bumps && !protect_calls);
        for(unsigned i=0;i<3;++i)
            assert(!areas[i].private_object && !(areas[i].flags&VM_AREA_CLONE_OLD_COW));
        for(unsigned i=0;i<shadow_count;++i)assert(!atomic_load(&shadows[i].refs.value));
        assert(areas[3].private_object==&shadows[100] &&
               (areas[3].flags&VM_AREA_CLONE_OLD_COW) &&
               atomic_load(&shadows[100].refs.value)==1);
    }
    for(unsigned failure=1;failure<=32;++failure) {
        prepare(4,1,false);fail_protect=failure;
        assert(vm_protect(&space,VM_USER_BASE,32*PAGE_SIZE,
                          VM_PROT_READ|VM_PROT_USER)==K_EINVAL);
        assert(protect_calls==failure+((failure+7)/8)*8 && !rebuilds && !bumps);
        for(unsigned i=0;i<32;++i)assert(ptes[i]==initial_flags);
        for(unsigned i=0;i<4;++i)
            assert(areas[i].prot==(VM_PROT_READ|VM_PROT_WRITE|VM_PROT_USER));
    }
    /* End==VM_USER_END uses the returned last VMA too, including a start split
     * in the last user mapping; no out-of-range second lookup is needed. */
    for(unsigned mode=0;mode<3;++mode) {
        prepare(1,mode,false);mock_base=VM_USER_END-32*PAGE_SIZE;
        areas[0].start=mock_base;areas[0].end=VM_USER_END;rebuild_area_tree(&space);rebuilds=visits=0;
        assert(vm_protect(&space,mock_base+PAGE_SIZE,31*PAGE_SIZE,VM_PROT_READ|VM_PROT_USER)==K_OK);
        assert(rebuilds==0 && find_area(&space,VM_USER_END-1)->prot==(VM_PROT_READ|VM_PROT_USER));
        assert(find_area(&space,mock_base)->prot==(VM_PROT_READ|VM_PROT_WRITE|VM_PROT_USER));
    }
    printf("mprotect indexes: %u complete transactions, real incremental splits/RB trees and failure exits; list-based gap oracle at every unlock, bounded target visits, unrelated shadows/markers and top user boundary preserved; zero rebuilds including split/allocation/MMU failure cases passed\n",cases);
}
'''
with tempfile.TemporaryDirectory(prefix='liteos-mprotect-index-') as directory:
    binary=str(Path(directory)/'test')
    subprocess.run(['cc','-std=c11','-O2','-Wall','-Wextra','-Werror',
                    '-I'+str(root/'include'),'-fsanitize=address,undefined',
                    '-fno-pie','-no-pie','-x','c','-','-o',binary],
                   input=program,text=True,check=True)
    subprocess.run([binary],check=True,timeout=60)
