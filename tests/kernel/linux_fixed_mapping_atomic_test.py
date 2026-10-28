"""Exercise production MAP_FIXED adapter and VMA code at its former race gap."""
from pathlib import Path
import re
import resource
import subprocess
import tempfile

resource.setrlimit(resource.RLIMIT_CORE, (0, 0))

root = Path(__file__).resolve().parents[2]
mapping = (root / 'kernel/mm/map.c').read_text().replace('#include "internal.h"', '')
vma = (root / 'kernel/mm/vma.c').read_text()
copy_tail = vma[vma.index('vm_area_t *vm_area_copy_tail('):
                vma.index('vm_area_t *vm_area_split(')]
# Tail preparation still requires the protected source VMA. Give that
# allocator call its own fixture entry so ordinary mapping allocations can
# assert they never run under map_lock, without weakening split coverage.
copy_tail = copy_tail.replace('area_allocate()', 'tail_area_allocate()')
linux = (root / 'kernel/syscall/linux.c').read_text()
start = linux.index('static int64_t linux_mmap(')
start = linux.index('    if ((flags & LINUX_MAP_FIXED_NOREPLACE) != 0U) {', start)
end = linux.index('    if ((flags & LINUX_MAP_ANONYMOUS) == 0U)', start)
fixed_branch = linux[start:end]
end = linux.index('    if (object != 0) vm_object_put(object);', end)
start = linux.rfind('\n    vm_space_t *space = process_vm_reference(process);', start, end)
map_call = linux[start:end]

fixture = r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdatomic.h>
#include <stdlib.h>
#define PAGE_SIZE 4096ULL
#define PAGE_SHIFT 12
#define X86_64_USER_TOP 0x7fffffffffffULL
#define K_OK 0
#define K_EINVAL (-22)
#define K_ENOMEM (-12)
#define K_EBUSY (-16)
#define K_EIO (-5)
#define LINUX_EEXIST (-17)
#define LINUX_MAP_FIXED 0x10U
#define LINUX_MAP_FIXED_NOREPLACE 0x100000U
enum { VM_PROT_READ=1, VM_PROT_WRITE=2, VM_PROT_EXEC=4, VM_PROT_USER=8 };
enum { VM_MAP_PRIVATE=1, VM_MAP_SHARED=2, VM_MAP_FIXED=4,
       VM_MAP_STACK=8, VM_MAP_GUARD=16, VM_MAP_ALLOW_RWX=32, VM_MAP_LOW32=64 };
enum { VM_OBJECT_ANON, VM_OBJECT_FILE, VM_OBJECT_SHARED };
typedef int kstatus_t;
typedef uintptr_t vaddr_t;
typedef struct node { struct node *next, *prev; } list_head_t;
#define VM_OBJECT_FLAG_MAP_OWNED 2U
typedef struct { unsigned type, refs, flags; uint64_t size, identity; } vm_object_t;
typedef struct {
    uint64_t start, end, object_offset;
    uint64_t subtree_min_start, subtree_max_end;
    uint32_t prot, flags;
    vm_object_t *object, *private_object;
    list_head_t ordered_node;
} vm_area_t;
typedef struct {
    list_head_t area_list;
    struct { vm_area_t *root; } areas;
    atomic_uint_fast64_t commit_pages, advice_generation;
    unsigned generation, locked;
} vm_space_t;
typedef struct { vm_space_t* vm; } process_t;
static vm_space_t *process_vm_reference(process_t *p) {return p->vm;}
static void vm_space_put(vm_space_t *p) {assert(p && !p->locked);}
static unsigned objects_alive, areas_alive, tlb_unmaps;
static vm_space_t *locked_space;
static vm_space_t *allocation_race_space;
static unsigned allocation_races;
struct anon_page_node;
static kstatus_t anon_page_retire_range(vm_object_t *o,uint64_t first,uint64_t end,
                                        struct anon_page_node **out) {
    (void)o;(void)out;assert(locked_space && first<end);return K_OK;
}
static void anon_page_release_retired(struct anon_page_node *pages) {assert(!locked_space && !pages);}
static int allocations_before_failure = -1;
static bool fail_tlb, inject_on_gap, injected;
static uint64_t identity;
static vaddr_t target = 0x11000;
static kstatus_t vm_map_object(vm_space_t*, vm_object_t*, vaddr_t*, uint64_t,
                              size_t, uint32_t, uint32_t);
static bool x86_is_canonical(uint64_t address) { return address <= X86_64_USER_TOP; }
static bool range_size_valid(uint64_t size) { return size && !(size & (PAGE_SIZE-1)); }
static bool range_valid(uint64_t address, uint64_t size, uint64_t* end) {
    if (!range_size_valid(size) || address < 0x10000 || (address & (PAGE_SIZE-1)) ||
        address > X86_64_USER_TOP || size > X86_64_USER_TOP+1-address) return false;
    *end = address+size; return true;
}
static vm_area_t* area_from_list(list_head_t* node) {
    return (vm_area_t*)((char*)node - offsetof(vm_area_t, ordered_node));
}
static void list_init(list_head_t* node) { node->next=node->prev=node; }
static void list_add_before(list_head_t* at, list_head_t* node) {
    node->prev=at->prev; node->next=at; at->prev->next=node; at->prev=node;
}
static void list_del(list_head_t* node) { node->prev->next=node->next; node->next->prev=node->prev; }
static vm_area_t* find(vm_space_t* space, vaddr_t address) {
    for (list_head_t* n=space->area_list.next; n!=&space->area_list; n=n->next) {
        vm_area_t* area=area_from_list(n);
        if (area->start<=address && address<area->end) return area;
    }
    return 0;
}
static vm_area_t* find_area_at_or_after(const vm_space_t* space,uint64_t address) {
    for(list_head_t* n=space->area_list.next;n!=&space->area_list;n=n->next) {
        vm_area_t* area=area_from_list(n);if(area->end>address)return area;
    }
    return 0;
}
static uint64_t find_gap(vm_space_t* space, uint64_t hint, uint64_t size) {
    uint64_t address=hint < 0x10000 ? 0x10000 : hint;
    for (list_head_t* n=space->area_list.next; n!=&space->area_list; n=n->next) {
        vm_area_t* area=area_from_list(n);
        if (area->end<=address) continue;
        if (area->start-address>=size && area->start>=address) break;
        address=area->end;
    }
    return address;
}
static void vm_object_get(vm_object_t* object) { if (object) ++object->refs; }
static void vm_object_put(vm_object_t* object) {
    if (object && object->refs==1) assert(!locked_space);
    if (object && !--object->refs) { --objects_alive; free(object); }
}
static kstatus_t vm_object_create_anon(size_t size, vm_object_t** out) {
    *out=calloc(1,sizeof(**out)); assert(*out);
    (*out)->size=size; (*out)->refs=1; (*out)->identity=++identity;
    ++objects_alive; return K_OK;
}
static vm_area_t* fixture_area_allocate(void) {
    if (allocation_race_space && !locked_space) {
        vm_space_t *space=allocation_race_space;allocation_race_space=0;
        vaddr_t address=target;++allocation_races;
        assert(vm_map_object(space,0,&address,0,PAGE_SIZE,VM_PROT_USER,
                             VM_MAP_PRIVATE|VM_MAP_FIXED)==K_OK);
    }
    if (!allocations_before_failure) return 0;
    if (allocations_before_failure>0) --allocations_before_failure;
    vm_area_t* area=calloc(1,sizeof(*area)); assert(area); ++areas_alive; return area;
}
static vm_area_t* area_allocate(void) {
    assert(!locked_space);return fixture_area_allocate();
}
static vm_area_t* tail_area_allocate(void) {
    assert(locked_space);return fixture_area_allocate();
}
static void area_release(vm_area_t* area) {
    vm_object_put(area->object); vm_object_put(area->private_object);
    --areas_alive; free(area);
}
static void rebuild_area_tree(vm_space_t* space) {
    uint64_t last=0;
    space->areas.root=space->area_list.next==&space->area_list?0:area_from_list(space->area_list.next);
    for (list_head_t* n=space->area_list.next; n!=&space->area_list; n=n->next) {
        vm_area_t* area=area_from_list(n);
        assert(area->start>=last && area->end>area->start); last=area->end;
    }
    if(space->areas.root) {
        space->areas.root->subtree_min_start=space->areas.root->start;
        space->areas.root->subtree_max_end=last;
    }
}
static vm_area_t *area_from_rb(vm_area_t *node) { return node; }
static void vm_tlb_bump_generation(vm_space_t* space) { ++space->generation; }
static void insert_area_tree(vm_space_t* space, vm_area_t* area) {
    (void)area; rebuild_area_tree(space);
}
static void update_area_tree(vm_area_t* area) { (void)area; }
static void erase_area_tree(vm_space_t* space, vm_area_t* area) {
    (void)area; space->areas.root=0;
}
static kstatus_t vm_tlb_unmap_range(vm_space_t* space, uint64_t start, uint64_t end) {
    assert(space->locked && end>start); ++tlb_unmaps;
    return fail_tlb ? K_EIO : K_OK;
}
static void map_lock(vm_space_t* space) {
    assert(!space->locked && !locked_space);space->locked=1;locked_space=space;
}
static void map_unlock(vm_space_t* space) {
    assert(space->locked && locked_space==space);space->locked=0;locked_space=0;
    uint64_t pages=0;
    for (list_head_t* n=space->area_list.next; n!=&space->area_list; n=n->next) {
        vm_area_t* area=area_from_list(n);
        pages+=(area->end-area->start)>>PAGE_SHIFT;
    }
    assert(pages==atomic_load(&space->commit_pages));
    if (inject_on_gap && !find(space,target)) {
        inject_on_gap=false; injected=true;
        vaddr_t address=target;
        assert(vm_map_object(space,0,&address,0,PAGE_SIZE,VM_PROT_USER,
                             VM_MAP_FIXED|VM_MAP_PRIVATE)==K_OK);
    }
}
''' + copy_tail + mapping + r'''
static int64_t mapping_adapter(process_t* process, vaddr_t address, size_t map_size,
                               vm_object_t* object, uint32_t vm_prot,
                               uint64_t flags) {
    uint32_t vm_flags=VM_MAP_PRIVATE;
    kstatus_t status;
''' + fixed_branch + map_call + r'''
    return status==K_OK ? (int64_t)address : status;
}
static int64_t fixed_adapter(process_t* process, vaddr_t address, size_t map_size,
                             vm_object_t* object, uint32_t vm_prot) {
    return mapping_adapter(process,address,map_size,object,vm_prot,LINUX_MAP_FIXED);
}
static void setup(vm_space_t* space) {
    *space=(vm_space_t){0}; space->area_list.next=space->area_list.prev=&space->area_list;
    allocations_before_failure=-1; fail_tlb=inject_on_gap=injected=false;
    vaddr_t address=0x10000;
    assert(vm_map_object(space,0,&address,0,3*PAGE_SIZE,VM_PROT_READ|VM_PROT_WRITE,
                         VM_MAP_PRIVATE|VM_MAP_FIXED)==K_OK);
    assert(find(space,address)->object->flags & VM_OBJECT_FLAG_MAP_OWNED);
}
static void cleanup(vm_space_t* space) {
    inject_on_gap=false; allocations_before_failure=-1; fail_tlb=false;
    assert(vm_unmap(space,0x10000,16*PAGE_SIZE)==K_OK);
    assert(!areas_alive && !objects_alive && !space->locked);
}
int main(void) {
    vm_space_t space; setup(&space); process_t process={&space};
    cleanup(&space);
    /* Allocation is preemptible: a competing mapping may appear before the
     * caller acquires map_lock. Both entry points must revalidate/publish
     * against that new state, not an earlier unlocked snapshot. */
    for(unsigned replace=0;replace<2;++replace) {
        allocation_race_space=&space;
        vaddr_t address=target;
        kstatus_t result=vm_map_object_impl(&space,0,&address,0,PAGE_SIZE,
            VM_PROT_USER,VM_MAP_PRIVATE|VM_MAP_FIXED,replace);
        assert(result==(replace?K_OK:K_EBUSY));
        assert(allocation_races==replace+1&&!allocation_race_space);
        assert(!locked_space&&areas_alive==1&&objects_alive==1&&space.commit_pages==1);
        assert(find(&space,target));cleanup(&space);
    }
    /* A high hint must not escape MAP_32BIT, and a full low range must fail
     * before publishing a VMA or retaining backing objects. */
    vaddr_t low=0x90000000;
    assert(vm_map_object(&space,0,&low,0,PAGE_SIZE,VM_PROT_USER,
                         VM_MAP_PRIVATE|VM_MAP_LOW32)==K_OK && low==0x10000);
    assert(vm_unmap(&space,low,PAGE_SIZE)==K_OK);
    low=0x10000;
    assert(vm_map_object(&space,0,&low,0,0x80000000ULL-low,VM_PROT_USER,
                         VM_MAP_PRIVATE|VM_MAP_FIXED)==K_OK);
    unsigned before_objects=objects_alive, before_areas=areas_alive;
    low=0x90000000;
    assert(vm_map_object(&space,0,&low,0,PAGE_SIZE,VM_PROT_USER,
                         VM_MAP_PRIVATE|VM_MAP_LOW32)==K_ENOMEM);
    assert(low==0x90000000 && objects_alive==before_objects && areas_alive==before_areas);
    assert(vm_unmap(&space,0x10000,0x80000000ULL-0x10000)==K_OK);
    assert(!areas_alive && !objects_alive && !space.locked);
    /* Every locked failure must drop transaction-owned backing/shadows,
     * but never consume a borrowed file reference or publish the address. */
    for (unsigned file_backed=0;file_backed<2;++file_backed)
      for (unsigned failure=0;failure<6;++failure) {
        setup(&space);
        vm_object_t *borrowed=0;
        if (file_backed) {
            assert(vm_object_create_anon(3*PAGE_SIZE,&borrowed)==K_OK);
            borrowed->type=VM_OBJECT_FILE;
        }
        unsigned before_objects=objects_alive, before_areas=areas_alive;
        vaddr_t requested=failure==0 ? target+1 : failure==2 ? 0x20000 : target;
        vaddr_t unchanged=requested;
        bool replace=failure>=3;
        if (failure==2 || failure==3) allocations_before_failure=0;
        if (failure==4) allocations_before_failure=1;
        if (failure==5) fail_tlb=true;
        kstatus_t expected=failure==0 ? K_EINVAL : failure==1 ? K_EBUSY :
                           failure==5 ? K_EIO : K_ENOMEM;
        kstatus_t result=vm_map_object_impl(&space,borrowed,&requested,0,PAGE_SIZE,
            VM_PROT_USER|VM_PROT_READ|VM_PROT_WRITE,
            VM_MAP_PRIVATE|VM_MAP_FIXED,replace);
        assert(result==expected && requested==unchanged && !locked_space);
        assert(objects_alive==before_objects && areas_alive==before_areas);
        assert(space.commit_pages==3 && (!borrowed || borrowed->refs==1));
        vm_object_put(borrowed);
        cleanup(&space);
      }
    setup(&space);
    vm_object_t* original=find(&space,target)->object;
    uint64_t original_id=original->identity;
    inject_on_gap=true;
    assert(fixed_adapter(&process,target,PAGE_SIZE,0,VM_PROT_USER)==(int64_t)target);
    assert(!injected && find(&space,target)->object->identity!=original_id);
    assert(find(&space,0x10000)->object==original);
    assert(find(&space,0x12000)->object==original);
    assert(find(&space,0x12000)->object_offset==2*PAGE_SIZE);
    assert(space.commit_pages==3 && tlb_unmaps);
    cleanup(&space);
    setup(&space); original=find(&space,target)->object;
    vaddr_t address=target;
    assert(vm_map_object(&space,0,&address,0,PAGE_SIZE,VM_PROT_USER,
                         VM_MAP_PRIVATE|VM_MAP_FIXED)==K_EBUSY);
    assert(find(&space,target)->object==original && space.commit_pages==3);
    assert(mapping_adapter(&process,target,PAGE_SIZE,0,VM_PROT_USER,
                            LINUX_MAP_FIXED_NOREPLACE)==LINUX_EEXIST);
    assert(mapping_adapter(&process,target,PAGE_SIZE,0,VM_PROT_USER,
                            LINUX_MAP_FIXED_NOREPLACE|LINUX_MAP_FIXED)==K_EINVAL);
    assert(mapping_adapter(&process,0,PAGE_SIZE,0,VM_PROT_USER,
                            LINUX_MAP_FIXED_NOREPLACE)==K_EINVAL);
    assert(find(&space,target)->object==original && space.commit_pages==3);
    allocations_before_failure=0;
    assert(fixed_adapter(&process,target,PAGE_SIZE,0,VM_PROT_USER)==K_ENOMEM);
    assert(find(&space,target)->object==original && space.commit_pages==3);
    allocations_before_failure=1; /* Replacement allocated, split allocation fails. */
    assert(fixed_adapter(&process,target,PAGE_SIZE,0,VM_PROT_USER)==K_ENOMEM);
    assert(find(&space,target)->object==original && space.commit_pages==3);
    allocations_before_failure=-1; fail_tlb=true;
    assert(fixed_adapter(&process,target,PAGE_SIZE,0,VM_PROT_USER)==K_EIO);
    assert(find(&space,target)->object==original && space.commit_pages==3);
    cleanup(&space);
    setup(&space);
    assert(fixed_adapter(&process,0x10000,3*PAGE_SIZE,0,VM_PROT_USER)==0x10000);
    assert(areas_alive==1 && objects_alive==1 && space.commit_pages==3);
    cleanup(&space);
    setup(&space);
    assert(fixed_adapter(&process,target,PAGE_SIZE,0,VM_PROT_USER)==(int64_t)target);
    assert(fixed_adapter(&process,0x10000,4*PAGE_SIZE,0,VM_PROT_USER)==0x10000);
    assert(areas_alive==1 && objects_alive==1 && space.commit_pages==4);
    cleanup(&space);
    setup(&space); original=find(&space,target)->object;
    address=target;
    assert(vm_map_object_replace(&space,0,&address,0,PAGE_SIZE,VM_PROT_USER,
                                 VM_MAP_PRIVATE)==K_EINVAL);
    assert(find(&space,target)->object==original);
    assert(fixed_adapter(&process,target+1,PAGE_SIZE,0,VM_PROT_USER)==K_EINVAL);
    assert(find(&space,target)->object==original && space.commit_pages==3);
    vm_object_t* file=0;
    assert(vm_object_create_anon(3*PAGE_SIZE,&file)==K_OK);
    file->type=VM_OBJECT_FILE;
    assert(fixed_adapter(&process,target,PAGE_SIZE,file,
                          VM_PROT_USER|VM_PROT_READ|VM_PROT_WRITE)==(int64_t)target);
    vm_area_t* area=find(&space,target);
    assert(area->object==file && file->refs==2 && area->private_object);
    assert(!(file->flags & VM_OBJECT_FLAG_MAP_OWNED));
    assert(area->private_object->flags & VM_OBJECT_FLAG_MAP_OWNED);
    vm_object_put(file);
    cleanup(&space);
    /* Mixed replacement/unmap shapes exercise full, head, tail and middle
     * removal across fragmented VMAs, checking every address against a model. */
    setup(&space);
    bool occupied[16]={true,true,true};
    uint32_t random=0x5070;
    for (unsigned iteration=0; iteration<2000; ++iteration) {
        random=random*1664525U+1013904223U;
        unsigned first=(random>>8)%16;
        unsigned count=1+(random>>16)%4;
        if (count>16-first) count=16-first;
        address=0x10000+first*PAGE_SIZE;
        if (random&1) {
            assert(vm_map_object_replace(&space,0,&address,0,count*PAGE_SIZE,
                                          VM_PROT_USER,VM_MAP_PRIVATE|VM_MAP_FIXED)==K_OK);
            for (unsigned i=first;i<first+count;++i) occupied[i]=true;
        } else {
            assert(vm_unmap(&space,address,count*PAGE_SIZE)==K_OK);
            for (unsigned i=first;i<first+count;++i) occupied[i]=false;
        }
        unsigned expected_pages=0;
        for (unsigned i=0;i<16;++i) {
            assert((find(&space,0x10000+i*PAGE_SIZE)!=0)==occupied[i]);
            expected_pages+=occupied[i];
        }
        assert(space.commit_pages==expected_pages);
    }
    cleanup(&space);
}
'''
with tempfile.TemporaryDirectory(prefix='liteos-fixed-mapping-') as directory:
    path=Path(directory)
    before_lock='    map_lock(space);\n    uint64_t address = (uint64_t)*inout_address;'
    allocation='    vm_area_t *area = area_allocate();'
    assert fixture.count(before_lock)==1 and fixture.count(allocation)==1
    locked_allocation=fixture.replace(before_lock,
        '    uint64_t address = (uint64_t)*inout_address;').replace(
            allocation,'    map_lock(space);\n'+allocation)
    for name,unit,broken in [('current',fixture,False),('locked-allocation',locked_allocation,True)]:
        source_file=path/(name+'.c');binary=path/name
        source_file.write_text(unit)
        subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-Wno-unused-function',
                        '-fsanitize=address,undefined','-fno-pie','-no-pie',
                        str(source_file),'-o',str(binary)],check=True)
        result=subprocess.run([str(binary)],capture_output=True)
        assert (result.returncode!=0)==broken,result.stderr.decode()
        if broken:
            assert b'!locked_space' in result.stderr,result.stderr.decode()
            print('Negative control rejected: mapping allocation under VM lock')
print('Production MAP_FIXED race gap, unlocked allocation/reentry, native non-replacement, splitting, references, accounting and failure paths passed')
