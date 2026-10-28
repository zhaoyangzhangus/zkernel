"""Real VMA lower-bound and munmap: holes, cuts, retirement and failure exits."""
from pathlib import Path
import subprocess
import tempfile

root=Path(__file__).resolve().parents[2]
source=(root/'kernel/mm/map.c').read_text()
unmap=source.replace('#include "internal.h"', '')
unmap=unmap.replace('vm_area_t *area = area_from_list(item);',
                    'vm_area_t *area = area_from_list(item); ++scans;')
vma=(root/'kernel/mm/vma.c').read_text()
tree=vma[vma.index('vm_area_t *area_from_list('):vma.index('vm_area_t *area_allocate(')]
tree=tree.replace('void rebuild_area_tree(vm_space_t *space) {',
                  'void rebuild_area_tree(vm_space_t *space) { ++rebuilds; tree_dirty=false;')
tree=tree.replace('void update_area_tree(vm_area_t *area) {',
                  'void update_area_tree(vm_area_t *area) { tree_dirty=false;')
tree=tree.replace('void erase_area_tree(vm_space_t *space, vm_area_t *area) {',
                  'void erase_area_tree(vm_space_t *space, vm_area_t *area) { ++erasures; tree_dirty=false;')
tree=tree.replace('while (node != 0) {', 'while (node != 0) { assert(!tree_dirty); ++seeks;')
allocator=vma[vma.index('vm_area_t *area_allocate('):vma.index('vm_area_t *vm_area_split(')]
program=r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <kernel/vm.h>
#define VM_USER_BASE 0x10000ULL
#define VM_USER_END 0x800000000000ULL
static vm_space_t space;
static vm_area_t areas[128];static vm_object_t objects[4],shadows[4];
static bool live[128],initial[40],removed[40];
static bool tree_dirty;
static unsigned allocated,rebuilds,seeks,scans,locked,alloc_calls,tlb_calls,fail_alloc,fail_tlb,bumps;
static unsigned erasures;
struct anon_page_node { struct anon_page_node *next; };
static kstatus_t anon_page_retire_range(vm_object_t *o,uint64_t first,uint64_t end,
                                        struct anon_page_node **out) {
    (void)o;(void)first;(void)end;(void)out;assert(0);return K_OK;
}
static void anon_page_release_retired(struct anon_page_node *pages) {assert(!locked && !pages);}
''' + tree + r'''
void *kzalloc(size_t size,uint32_t flags) {
    (void)flags;assert(size==sizeof(vm_area_t));
    if(++alloc_calls==fail_alloc)return 0;
    assert(allocated<128);live[allocated]=true;return &areas[allocated++];
}
void vm_object_get(vm_object_t *o) {if(o)atomic_fetch_add(&o->refs.value,1);}
void vm_object_put(vm_object_t *o) {
    if(o) {assert(!locked);assert(atomic_fetch_sub(&o->refs.value,1)>0);}
}
static void area_release(vm_area_t *a) {
    unsigned i=(unsigned)(a-areas);assert(i<allocated && live[i]);
    assert(!locked || (!a->object && !a->private_object));
    live[i]=false;vm_object_put(a->object);vm_object_put(a->private_object);
}
static void map_lock(vm_space_t *s) {assert(s==&space && !locked);locked=1;}
static void validate(vm_space_t *s) {
    unsigned count=0;uint64_t total=0,last=0;
    for(list_head_t *item=s->area_list.next;item!=&s->area_list;item=item->next) {
        vm_area_t *a=area_from_list(item);++count;
        assert(a->start>=last && a->start<a->end);last=a->end;
        total+=(a->end-a->start)/PAGE_SIZE;
        for(uint64_t va=a->start;va<a->end;va+=PAGE_SIZE)assert(find_area(s,va)==a);
    }
    unsigned indexed=0;
    for(rb_node_t *node=rb_tree_first(&s->areas);node;node=rb_tree_next(node))++indexed;
    assert(indexed==count && total==atomic_load(&s->commit_pages));
    /* Independent list oracle also verifies augmented gaps at every unlock,
     * including partial MMU/allocation failures and head/tail/middle cuts. */
    for(unsigned i=0;i<40;++i)for(unsigned pages=1;pages<=8;++pages) {
        uint64_t hint=VM_USER_BASE+i*PAGE_SIZE,cursor=hint,size=pages*PAGE_SIZE;
        for(list_head_t *item=s->area_list.next;item!=&s->area_list;item=item->next) {
            vm_area_t *a=area_from_list(item);
            if(a->start>=cursor && size<=a->start-cursor)break;
            if(cursor<a->end)cursor=a->end;
        }
        assert(find_gap(s,hint,size)==cursor);
    }
}
static void map_unlock(vm_space_t *s) {
    assert(s==&space && locked);
    if(list_empty(&s->area_list)){assert(!s->areas.root);tree_dirty=false;}
    validate(s);locked=0;
}
static kstatus_t vm_tlb_unmap_range(vm_space_t *s,uint64_t start,uint64_t end) {
    assert(s==&space && locked && start<end);if(++tlb_calls==fail_tlb)return K_EIO;
    tree_dirty=true;
    for(uint64_t va=start;va<end;va+=PAGE_SIZE) {
        unsigned i=(unsigned)((va-VM_USER_BASE)/PAGE_SIZE);
        assert(i<40 && initial[i] && !removed[i]);removed[i]=true;
    }
    return K_OK;
}
static void vm_tlb_bump_generation(vm_space_t *s) {assert(s==&space && locked);++bumps;}
kstatus_t vm_object_create_anon(size_t size,vm_object_t **out) {
    (void)size;(void)out;return K_ENOMEM;
}
''' + allocator + unmap + r'''
static void prepare(unsigned layout) {
    memset(&space,0,sizeof(space));memset(areas,0,sizeof(areas));memset(objects,0,sizeof(objects));
    memset(shadows,0,sizeof(shadows));memset(live,0,sizeof(live));memset(initial,0,sizeof(initial));
    memset(removed,0,sizeof(removed));list_init(&space.area_list);
    allocated=4;fail_alloc=fail_tlb=alloc_calls=tlb_calls=bumps=0;
    for(unsigned i=0;i<4;++i) {
        areas[i].start=VM_USER_BASE+i*8*PAGE_SIZE;
        areas[i].end=areas[i].start+(layout==0?8:layout==1?4:1+i)*PAGE_SIZE;
        areas[i].prot=VM_PROT_READ|VM_PROT_USER;areas[i].flags=VM_MAP_PRIVATE;
        areas[i].object=&objects[i];areas[i].private_object=&shadows[i];
        areas[i].object_offset=3*PAGE_SIZE;live[i]=true;
        refcount_init(&objects[i].refs,1);refcount_init(&shadows[i].refs,1);
        list_add_tail(&space.area_list,&areas[i].ordered_node);
        for(uint64_t va=areas[i].start;va<areas[i].end;va+=PAGE_SIZE) {
            initial[(va-VM_USER_BASE)/PAGE_SIZE]=true;atomic_fetch_add(&space.commit_pages,1);
        }
    }
    rebuild_area_tree(&space);rebuilds=seeks=scans=erasures=0;
}
int main(void) {
    /* Preparation must not edit the source/index, even before publication;
     * nullable backing/shadow references transfer only to the new owner. */
    for(unsigned layout=0;layout<3;++layout)
      for(unsigned mask=0;mask<4;++mask)for(unsigned failure=0;failure<2;++failure) {
        prepare(layout);vm_area_t *source=&areas[1];
        if(!(mask&1))source->object=0;
        if(!(mask&2))source->private_object=0;
        source->prot=VM_PROT_READ|VM_PROT_WRITE|VM_PROT_USER;
        source->flags=VM_MAP_PRIVATE|0x80000000U;
        vm_area_t snapshot=*source;rb_node_t *root=space.areas.root;
        fail_alloc=failure;map_lock(&space);
        vm_area_t *tail=vm_area_copy_tail(source,source->start+PAGE_SIZE);
        assert(!memcmp(source,&snapshot,sizeof(snapshot)) && space.areas.root==root);
        assert(!tlb_calls && !bumps && !rebuilds && alloc_calls==1);
        assert((tail!=0)==!failure);
        if(tail) {
            assert(tail->start==snapshot.start+PAGE_SIZE && tail->end==snapshot.end);
            assert(tail->object_offset==snapshot.object_offset+PAGE_SIZE);
            assert(tail->prot==snapshot.prot && tail->flags==snapshot.flags);
            assert(tail->object==snapshot.object && tail->private_object==snapshot.private_object);
            assert(tail->ordered_node.next==&tail->ordered_node && tail->ordered_node.prev==&tail->ordered_node);
            assert(!tail->rb.left && !tail->rb.right);
        }
        assert(atomic_load(&objects[1].refs.value)==1U+(!failure && (mask&1)!=0));
        assert(atomic_load(&shadows[1].refs.value)==1U+(!failure && (mask&2)!=0));
        map_unlock(&space);
        if(tail)area_release(tail);
        assert(atomic_load(&objects[1].refs.value)==1 && atomic_load(&shadows[1].refs.value)==1);
      }
    unsigned lookups=0,transactions=0;
    for(unsigned layout=0;layout<3;++layout) {
        prepare(layout);
        for(uint64_t va=VM_USER_BASE-1;va<VM_USER_BASE+40*PAGE_SIZE;va+=127) {
            vm_area_t *expected=0;
            for(unsigned i=0;i<4;++i)if(areas[i].end>va) {expected=&areas[i];break;}
            assert(find_area_at_or_after(&space,va)==expected);++lookups;
        }
        assert(!find_area_at_or_after(&space,UINT64_MAX));
        for(unsigned start=0;start<40;++start)for(unsigned end=start+1;end<=40;++end)
          for(unsigned failure=0;failure<6;++failure) {
            prepare(layout);
            if(failure<3)fail_alloc=failure;else fail_tlb=failure-2;
            kstatus_t status=vm_unmap(&space,VM_USER_BASE+start*PAGE_SIZE,(end-start)*PAGE_SIZE);
            assert(status==K_OK || status==K_ENOMEM || status==K_EIO);
            assert(!locked && bumps==(tlb_calls!=0) && rebuilds<=1);
            bool changed=false;unsigned refs[4]={0};
            for(unsigned i=0;i<40;++i) {
                vm_area_t *a=find_area(&space,VM_USER_BASE+i*PAGE_SIZE);
                assert((a!=0)==(initial[i] && !removed[i]));changed|=removed[i];
                if(a) {
                    unsigned owner=(unsigned)(a->object-objects);
                    assert(owner<4 && a->private_object==&shadows[owner]);
                    assert(a->object_offset+(VM_USER_BASE+i*PAGE_SIZE-a->start)==
                           (3+i-owner*8)*PAGE_SIZE);
                }
            }
            bool deleted=false;
            for(unsigned owner=0;owner<4;++owner) {
                bool all=true,any=false;
                for(unsigned i=owner*8;i<(owner+1)*8;++i)if(initial[i]) {
                    any=true;all&=removed[i];
                }
                deleted|=any && all;
            }
            bool clear_all=true;
            for(unsigned i=0;i<40;++i)if(initial[i] && (i<start || i>=end))clear_all=false;
            assert(rebuilds==(unsigned)(clear_all && changed && status!=K_OK));
            if(clear_all)assert(!erasures);
            assert(changed || !deleted);
            for(unsigned i=0;i<allocated;++i)if(live[i])++refs[areas[i].object-objects];
            for(unsigned i=0;i<4;++i) {
                assert(atomic_load(&objects[i].refs.value)==refs[i]);
                assert(atomic_load(&shadows[i].refs.value)==refs[i]);
            }
            ++transactions;
          }
    }
    prepare(0);
    assert(vm_unmap(&space,VM_USER_BASE+32*PAGE_SIZE,PAGE_SIZE)==K_OK);
    assert(!scans && !rebuilds && !bumps);
    prepare(1);
    assert(vm_unmap(&space,VM_USER_BASE+4*PAGE_SIZE,PAGE_SIZE)==K_OK);
    assert(scans==1 && !rebuilds && !tlb_calls && !bumps);
    prepare(0);fail_tlb=1;
    assert(vm_unmap(&space,areas[3].start,8*PAGE_SIZE)==K_EIO);
    assert(scans==1 && !rebuilds && bumps==1);
    assert(vm_unmap(0,VM_USER_BASE,PAGE_SIZE)==K_EINVAL);
    assert(vm_unmap(&space,VM_USER_BASE,0)==K_EINVAL);
    static vm_area_t large[4096];static vm_space_t large_space;
    list_init(&large_space.area_list);
    for(unsigned i=0;i<4096;++i) {
        large[i].start=VM_USER_BASE+(2*i+1)*PAGE_SIZE;
        large[i].end=large[i].start+PAGE_SIZE;
        list_add_tail(&large_space.area_list,&large[i].ordered_node);
    }
    rebuild_area_tree(&large_space);
    for(unsigned i=0;i<=8193;++i) {
        seeks=0;vm_area_t *expected=i/2<4096?&large[i/2]:0;
        assert(find_area_at_or_after(&large_space,VM_USER_BASE+i*PAGE_SIZE)==expected);
        assert(seeks<=24);++lookups;
    }
    /* Insertion overlap and list-position oracle, including an empty tree. */
    for(unsigned count=0;count<=4;++count)for(unsigned start=0;start<40;++start)
      for(unsigned length=1;length<=40;++length) {
        prepare(1);
        for(unsigned i=count;i<4;++i)list_del(&areas[i].ordered_node);
        rebuild_area_tree(&space);
        uint64_t address=VM_USER_BASE+start*PAGE_SIZE,end=address+length*PAGE_SIZE;
        list_head_t *position=&space.area_list;bool busy=false;
        for(list_head_t *item=space.area_list.next;item!=&space.area_list;item=item->next) {
            vm_area_t *a=area_from_list(item);
            if(a->start<end && address<a->end) {busy=true;break;}
            if(a->start>address) {position=item;break;}
        }
        vm_area_t *candidate=find_area_at_or_after(&space,address);
        assert(busy==(candidate!=0 && candidate->start<end));
        if(!busy)assert(position==(candidate?&candidate->ordered_node:&space.area_list));
    }
    unsigned insertions=0;
    for(unsigned layout=0;layout<3;++layout)
      for(unsigned start=0;start<40;++start)for(unsigned end=start+1;end<=40;++end)
        for(unsigned failure=0;failure<2;++failure) {
            prepare(layout);bool busy=false;
            for(unsigned i=start;i<end;++i)busy|=initial[i];
            vm_object_t object={0};object.type=VM_OBJECT_ANON;
            object.size=40*PAGE_SIZE;refcount_init(&object.refs,1);
            fail_alloc=failure;vaddr_t address=VM_USER_BASE+start*PAGE_SIZE;
            kstatus_t status=vm_map_object(&space,&object,&address,0,
                (end-start)*PAGE_SIZE,VM_PROT_READ|VM_PROT_USER,VM_MAP_PRIVATE|VM_MAP_FIXED);
            assert(status==(busy?K_EBUSY:failure?K_ENOMEM:K_OK));
            assert(!locked && !rebuilds && !tlb_calls && !tree_dirty);
            assert(bumps==(status==K_OK));
            assert(atomic_load(&object.refs.value)==1+(status==K_OK));
            for(unsigned i=0;i<40;++i) {
                vm_area_t *a=find_area(&space,VM_USER_BASE+i*PAGE_SIZE);
                bool inserted=status==K_OK && i>=start && i<end;
                assert((a!=0)==(inserted || initial[i]));
                if(inserted)assert(a->object==&object && !a->private_object &&
                    a->object_offset+VM_USER_BASE+i*PAGE_SIZE-a->start==(i-start)*PAGE_SIZE);
                else if(a)assert(a->object>=objects && a->object<objects+4);
            }
            ++insertions;
        }
    assert(insertions==4920);
    /* Full production replacement, including partial failure: every unlock
     * validates the real address tree against its authoritative list. */
    unsigned replacements=0;
    for(unsigned layout=0;layout<3;++layout)
      for(unsigned start=0;start<40;++start)for(unsigned end=start+1;end<=40;++end)
        for(unsigned failure=0;failure<7;++failure) {
            prepare(layout);
            vm_object_t replacement={0};replacement.type=VM_OBJECT_ANON;
            replacement.size=40*PAGE_SIZE;refcount_init(&replacement.refs,1);
            if(failure<4)fail_alloc=failure;else fail_tlb=failure-3;
            vaddr_t address=VM_USER_BASE+start*PAGE_SIZE;
            kstatus_t status=vm_map_object_replace(&space,&replacement,&address,0,
                (end-start)*PAGE_SIZE,VM_PROT_READ|VM_PROT_USER,VM_MAP_PRIVATE|VM_MAP_FIXED);
            assert(!locked && rebuilds<=1);
            bool changed=false;unsigned owners[4]={0},shadow_refs[4]={0},new_refs=0;
            for(unsigned i=0;i<40;++i) {
                vm_area_t *a=find_area(&space,VM_USER_BASE+i*PAGE_SIZE);
                bool replaced=status==K_OK && i>=start && i<end;
                assert((a!=0)==(replaced || (initial[i] && !removed[i])));
                if(replaced)assert(a->object==&replacement && !a->private_object &&
                    a->object_offset+VM_USER_BASE+i*PAGE_SIZE-a->start==(i-start)*PAGE_SIZE);
                else if(a)assert(a->object>=objects && a->object<objects+4);
                changed|=removed[i];
            }
            for(unsigned i=0;i<allocated;++i)if(live[i]) {
                vm_area_t *a=&areas[i];
                if(a->object==&replacement)++new_refs;
                else ++owners[a->object-objects];
                if(a->private_object)++shadow_refs[a->private_object-shadows];
            }
            assert(atomic_load(&replacement.refs.value)==1+new_refs);
            for(unsigned i=0;i<4;++i) {
                assert(atomic_load(&objects[i].refs.value)==owners[i]);
                assert(atomic_load(&shadows[i].refs.value)==shadow_refs[i]);
            }
            bool clear_all=true;
            for(unsigned i=0;i<40;++i)if(initial[i] && (i<start || i>=end))clear_all=false;
            if(status==K_OK)assert(rebuilds==0 && bumps==1+(tlb_calls!=0));
            else {
                assert(status==K_ENOMEM || status==K_EIO);
                assert(rebuilds==(unsigned)(clear_all && changed) && bumps==(tlb_calls!=0));
            }
            ++replacements;
        }
    printf("VMA seek: 24 nonmutating tail preparations, %u queries, %u unmaps, %u real-tree replacements; zero successful replacement rebuilds, partial-failure publication and linear full-clear fallback, offsets/references/accounting and hole/cut insertion positions passed\n",lookups,transactions,replacements);
}
'''
with tempfile.TemporaryDirectory(prefix='liteos-vm-unmap-seek-') as directory:
    binary=str(Path(directory)/'test')
    subprocess.run(['cc','-std=c11','-O2','-Wall','-Wextra','-Werror',
                    '-I'+str(root/'include'),'-fsanitize=address,undefined',
                    '-fno-pie','-no-pie','-x','c','-','-o',binary],
                   input=program,text=True,check=True)
    subprocess.run([binary],check=True,timeout=30)
