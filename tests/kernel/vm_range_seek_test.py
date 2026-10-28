"""Real VMA tree/list range checks, msync and madvise versus head-scan oracle."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root/'kernel/mm/protection.c').read_text()
code = source[source.index('static bool range_fully_mapped('):
              source.index('static kstatus_t vm_protect_impl(')]
vma = (root/'kernel/mm/vma.c').read_text()
vma = vma[vma.index('vm_area_t *area_from_list('):vma.index('static bool find_gap_subtree(')]
vma = vma.replace('vm_area_t *area_from_list(list_head_t *node) {',
                  'vm_area_t *area_from_list(list_head_t *node) { ++visits;')
vma = vma.replace('while (node != 0) {', 'while (node != 0) { ++seeks;')
oracle = r'''
static bool old_range(const vm_space_t *space,uint64_t start,uint64_t end,vm_area_t **first,vm_area_t **last) {
    uint64_t cursor=start;vm_area_t *initial=0;
    for(list_head_t *item=space->area_list.next;item!=&space->area_list;item=item->next) {
        vm_area_t *area=area_from_list(item);
        if(area->end<=cursor)continue;
        if(area->start>cursor)return false;
        if(!initial)initial=area;
        if(area->end>cursor)cursor=area->end;
        if(cursor>=end) {if(first)*first=initial;if(last)*last=area;return true;}
    }
    return false;
}
'''
old = code[code.index('static bool vm_advice_supported('):]
old = old.replace('range_fully_mapped(', 'old_range(')
old = old.replace('vm_advice_supported(', 'old_advice_supported(')
for name in ('vm_range_is_mapped', 'vm_sync', 'vm_advise'):
    old = old.replace(name+'(', 'old_'+name+'(')
old = old.replace('&first_area->ordered_node', 'space->area_list.next')
old = old.replace('item != range_end', 'item != &space->area_list')
old = old.replace('    list_head_t *range_end = last_area->ordered_node.next;\n', '')
old = old.replace('vm_area_t *area = area_from_list(item);',
                  'vm_area_t *area = area_from_list(item);\n'
                  '        if (area->end <= address || area->start >= end) continue;')
old = old.replace('if (area->start >= end) break;',
                  'if (area->end <= address || area->start >= end) continue;')
program = r'''
#include <assert.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <kernel/vm.h>
#include <uapi/mm.h>
#define VM_USER_BASE 0x10000ULL
#define VM_USER_END 0x800000000000ULL
static unsigned visits,seeks,locked,callbacks,fail_at,trace_count;
static uint64_t trace[128][4];
''' + vma + r'''
static void map_lock(vm_space_t *s) { assert(s && !locked);locked=1; }
static void map_unlock(vm_space_t *s) { assert(s && locked);locked=0; }
static kstatus_t record(unsigned kind,uint64_t id,uint64_t first,uint64_t end) {
    assert((kind==2 ? !locked : locked) && trace_count<128);
    trace[trace_count][0]=kind;trace[trace_count][1]=id;
    trace[trace_count][2]=first;trace[trace_count++][3]=end;
    return ++callbacks==fail_at?K_EIO:K_OK;
}
static kstatus_t sync_file(void *mapping,uint64_t first,uint64_t count) {
    return record(2,(uintptr_t)mapping,first,count);
}
void *kmalloc(size_t size,uint32_t flags) {(void)flags;return malloc(size);}
void kfree(void *pointer) {assert(!locked);free(pointer);}
void vm_object_get(vm_object_t *object) {
    assert(locked);atomic_fetch_add(&object->refs.value,1U);
}
void vm_object_put(vm_object_t *object) {
    assert(!locked&&atomic_fetch_sub(&object->refs.value,1U)>0U);
}
static kstatus_t vm_tlb_unmap_range(vm_space_t *s,uint64_t first,uint64_t end) {
    (void)s;return record(1,0,first,end);
}
typedef struct anon_page_node anon_page_node_t;
static kstatus_t anon_page_retire_range(vm_object_t *o,uint64_t first,uint64_t end,
                                        anon_page_node_t **out) {
    (void)out;
    return record(3,o->size,first,end);
}
static void anon_page_release_retired(anon_page_node_t *pages) {assert(!locked && !pages);}
static void vm_tlb_bump_generation(vm_space_t *s) { (void)s;(void)record(4,0,0,0); }
''' + code + oracle + old + r'''
static vm_area_t areas[4096];static vm_object_t objects[4096],shadows[4096];
static vm_space_t space;
static const vm_file_ops_t file_ops={.sync=sync_file};
static unsigned prepare(unsigned layout,unsigned count) {
    memset(&space,0,sizeof(space));list_init(&space.area_list);
    uint64_t cursor=VM_USER_BASE;
    for(unsigned i=0;i<count;++i) {
        memset(&areas[i],0,sizeof(areas[i]));memset(&objects[i],0,sizeof(objects[i]));
        areas[i].start=cursor;
        unsigned pages=layout==999?1:1+(i+layout)%4;
        cursor+=pages*PAGE_SIZE;areas[i].end=cursor;
        if(layout!=999 && (i+layout)%5==0)cursor+=PAGE_SIZE;
        areas[i].flags=(i+layout)%3==0?VM_MAP_PRIVATE:VM_MAP_SHARED;
        if(layout%3==0 && (i+layout)%11==0)areas[i].flags|=VM_MAP_GUARD;
        objects[i].type=(i+layout)%2?VM_OBJECT_FILE:VM_OBJECT_ANON;
        objects[i].size=i+1;shadows[i].size=i+4097;
        objects[i].u.file.mapping=(void *)(uintptr_t)(i+1);
        objects[i].u.file.ops=(i+layout)%7==0?0:&file_ops;
        objects[i].u.file.file_offset=(i+layout)%13==0?UINT64_MAX-4095:PAGE_SIZE*i;
        areas[i].object=&objects[i];areas[i].private_object=(i+layout)%4==0?&shadows[i]:0;
        areas[i].object_offset=(i+layout)%17==0?UINT64_MAX-4095:PAGE_SIZE;
        list_add_tail(&space.area_list,&areas[i].ordered_node);
    }
    rebuild_area_tree(&space);return (unsigned)((cursor-VM_USER_BASE)/PAGE_SIZE);
}
static void reset(unsigned failure) {
    visits=seeks=callbacks=trace_count=0;fail_at=failure;
    memset(trace,0,sizeof(trace));atomic_store(&space.advice_generation,0);assert(!locked);
}
int main(void) {
    unsigned ranges=0,operations=0;
    for(unsigned layout=0;layout<24;++layout) {
        unsigned limit=prepare(layout,32);
        for(unsigned start=0;start<=limit+1;++start)for(unsigned length=1;length<=limit+1;++length) {
            uint64_t begin=VM_USER_BASE+start*PAGE_SIZE,end=begin+length*PAGE_SIZE;
            vm_area_t *first=0,*expected=0,*last=0,*expected_last=0;
            bool old_result=old_range(&space,begin,end,&expected,&expected_last);
            bool result=range_fully_mapped(&space,begin,end,&first,&last);
            assert(result==old_result && first==expected && last==expected_last);
            assert(vm_range_is_mapped(&space,begin,length*PAGE_SIZE)==old_result && !locked);
            ++ranges;
        }
        for(unsigned start=0;start<=limit;++start)for(unsigned length=1;length<=8;++length)
          for(unsigned kind=0;kind<2;++kind)for(unsigned option=0;option<9;++option)
            for(unsigned failure=0;failure<3;++failure) {
                uint64_t reference[128][4],begin=VM_USER_BASE+start*PAGE_SIZE;
                unsigned expected_count;uint64_t expected_generation;kstatus_t expected_status;
                reset(failure);
                if(kind)expected_status=old_vm_advise(&space,begin,length*PAGE_SIZE,option==7?16:option==8?17:option);
                else expected_status=old_vm_sync(&space,begin,length*PAGE_SIZE,option);
                expected_count=trace_count;expected_generation=atomic_load(&space.advice_generation);
                memcpy(reference,trace,sizeof(trace));reset(failure);
                kstatus_t status=kind?vm_advise(&space,begin,length*PAGE_SIZE,option==7?16:option==8?17:option):
                                     vm_sync(&space,begin,length*PAGE_SIZE,option);
                assert(status==expected_status && trace_count==expected_count);
                assert(atomic_load(&space.advice_generation)==expected_generation && !locked);
                assert(!memcmp(trace,reference,sizeof(trace)));++operations;
            }
    }
    prepare(999,4096);reset(0);
    uint64_t begin=areas[4095].start;
    assert(vm_range_is_mapped(&space,begin,PAGE_SIZE));
    assert(visits==1 && seeks<=24);reset(0);
    assert(vm_sync(&space,begin,PAGE_SIZE,OS_VM_SYNC_SYNC)==K_OK);
    assert(visits==2 && seeks<=24);reset(0);
    areas[4095].object_offset=0;
    assert(vm_advise(&space,begin,PAGE_SIZE,OS_VM_ADVICE_DONTNEED)==K_OK);
    assert(visits==3 && seeks<=24);
    const unsigned targets[]={0,1,17,2048,4094};
    for(unsigned j=0;j<sizeof(targets)/sizeof(*targets);++j) {
        unsigned i=targets[j];begin=areas[i].start;
        areas[i].flags=VM_MAP_PRIVATE;areas[i].object_offset=0;
        objects[i].type=VM_OBJECT_ANON;
        reset(0);assert(vm_sync(&space,begin,PAGE_SIZE,OS_VM_SYNC_SYNC)==K_OK);
        assert(visits==2 && seeks<=24);
        reset(0);assert(vm_advise(&space,begin,PAGE_SIZE,OS_VM_ADVICE_NORMAL)==K_OK);
        assert(visits==2 && seeks<=24);
        reset(0);assert(vm_advise(&space,begin,PAGE_SIZE,OS_VM_ADVICE_DONTNEED)==K_OK);
        assert(visits==3 && seeks<=24);
    }
    assert(!vm_range_is_mapped(0,begin,PAGE_SIZE));
    assert(!vm_range_is_mapped(&space,begin,0));
    assert(!vm_range_is_mapped(&space,VM_USER_END-PAGE_SIZE,2*PAGE_SIZE));
    assert(vm_sync(0,begin,PAGE_SIZE,OS_VM_SYNC_SYNC)==K_EINVAL);
    assert(vm_advise(0,begin,PAGE_SIZE,OS_VM_ADVICE_NORMAL)==K_EINVAL);
    printf("VMA seek: %u range and %u msync/madvise status/callback/failure/generation oracle cases; 4096-area tail lookup visits 1, sync 2, DONTNEED 3, tree depth <=24 passed\n",ranges,operations);
}
'''
with tempfile.TemporaryDirectory(prefix='liteos-vm-range-seek-') as directory:
    binary=str(Path(directory)/'test')
    subprocess.run(['cc','-std=c11','-O2','-Wall','-Wextra','-Werror',
                    '-I'+str(root/'include'),'-fsanitize=address,undefined',
                    '-fno-pie','-no-pie','-x','c','-','-o',binary],
                   input=program,text=True,check=True)
    subprocess.run([binary],check=True,timeout=60)
