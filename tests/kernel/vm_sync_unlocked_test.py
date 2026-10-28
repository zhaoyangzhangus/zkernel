"""Production msync snapshots survive unmap; I/O and cleanup are unlocked."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'kernel/mm/protection.c').read_text()
begin = source.index('kstatus_t vm_sync(')
body = source[begin:source.index('kstatus_t vm_advise(', begin)]
program = r'''
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <kernel/vm.h>
#include <uapi/mm.h>
static vm_space_t space;
static vm_area_t areas[3];
static vm_object_t objects[3];
static unsigned locked,allocations,frees,calls;
static int fail_alloc,fail_callback;
static bool unmap_during_sync;
static void map_lock(vm_space_t *s) {assert(s==&space&&!locked);locked=1;}
static void map_unlock(vm_space_t *s) {assert(s==&space&&locked);locked=0;}
static bool range_valid(uint64_t a,size_t n,uint64_t *end) {
    if(!n||n>UINT64_MAX-a)return false;*end=a+n;return true;
}
static bool range_fully_mapped(vm_space_t *s,uint64_t a,uint64_t end,
        vm_area_t **first,vm_area_t **last) {
    assert(s==&space&&locked&&a==0x10000&&end==0x13000);
    *first=&areas[0];*last=&areas[2];return true;
}
static vm_area_t *area_from_list(list_head_t *item) {
    return (vm_area_t *)((char *)item-__builtin_offsetof(vm_area_t,ordered_node));
}
void *kmalloc(size_t size,uint32_t flags) {
    (void)flags;assert(locked);
    if((int)allocations++==fail_alloc)return 0;
    return malloc(size);
}
void kfree(void *p) {assert(!locked);++frees;free(p);}
void vm_object_get(vm_object_t *o) {
    assert(locked);assert(atomic_fetch_add(&o->refs.value,1U)==1);
}
void vm_object_put(vm_object_t *o) {
    assert(!locked);assert(atomic_fetch_sub(&o->refs.value,1U)>0);
}
static kstatus_t sync_file(void *mapping,uint64_t first,uint64_t count) {
    assert(!locked&&count==1);
    unsigned i=(unsigned)(uintptr_t)mapping;
    assert(i==calls&&first==10+i*2);
    assert(atomic_load(&objects[i].refs.value)>0);
    ++calls;
    if(unmap_during_sync&&calls==1) {
        /* All three mappings disappear during the first blocking callback.
         * Poison the old list/VMA storage so a resumed traversal is invalid. */
        map_lock(&space);list_init(&space.area_list);
        memset(areas,0xa5,sizeof(areas));map_unlock(&space);
        for(unsigned j=0;j<3;++j)vm_object_put(&objects[j]);
    }
    return (int)calls==fail_callback?K_EIO:K_OK;
}
''' + body + r'''
static const vm_file_ops_t ops={.sync=sync_file};
static void reset(void) {
    memset(&space,0,sizeof(space));list_init(&space.area_list);
    locked=allocations=frees=calls=0;fail_alloc=fail_callback=-1;
    unmap_during_sync=false;
    for(unsigned i=0;i<3;++i) {
        memset(&areas[i],0,sizeof(areas[i]));memset(&objects[i],0,sizeof(objects[i]));
        atomic_init(&objects[i].refs.value,1);
        objects[i].type=VM_OBJECT_FILE;objects[i].u.file.ops=&ops;
        objects[i].u.file.mapping=(void *)(uintptr_t)i;
        objects[i].u.file.file_offset=(10+i*2)*PAGE_SIZE;
        areas[i].start=0x10000+i*PAGE_SIZE;areas[i].end=areas[i].start+PAGE_SIZE;
        areas[i].flags=VM_MAP_SHARED;areas[i].object=&objects[i];
        list_add_tail(&space.area_list,&areas[i].ordered_node);
    }
}
int main(void) {
    for(int fail=-1;fail<3;++fail) {
        reset();fail_alloc=fail;
        assert(vm_sync(&space,0x10000,3*PAGE_SIZE,OS_VM_SYNC_SYNC)==(fail<0?K_OK:K_ENOMEM));
        assert(!locked&&calls==(fail<0?3:0)&&frees==(unsigned)(fail<0?3:fail));
        for(unsigned j=0;j<3;++j)assert(atomic_load(&objects[j].refs.value)==1);
    }
    for(int fail=-1;fail<=3;++fail) {
        reset();unmap_during_sync=true;fail_callback=fail;
        assert(vm_sync(&space,0x10000,3*PAGE_SIZE,OS_VM_SYNC_SYNC)==(fail>0?K_EIO:K_OK));
        assert(calls==(unsigned)(fail>0?fail:3)&&frees==3&&!locked);
        for(unsigned j=0;j<3;++j)assert(!atomic_load(&objects[j].refs.value));
    }
    reset();areas[2].flags=VM_MAP_GUARD;
    assert(vm_sync(&space,0x10000,3*PAGE_SIZE,OS_VM_SYNC_SYNC)==K_EINVAL);
    assert(!calls&&frees==2&&!locked);
    puts("Unlocked msync: concurrent unmap poisons VMA/list; pinned mappings survive; every allocation/callback failure releases all requests and refs, no I/O/free under map guard passed");
}
'''
with tempfile.TemporaryDirectory(prefix='vm-sync-unlocked-') as directory:
    binary = str(Path(directory) / 'test')
    subprocess.run(['cc', '-std=gnu11', '-O2', '-Wall', '-Wextra', '-Werror',
                    '-Wno-misleading-indentation', '-fsanitize=address,undefined',
                    '-fno-pie', '-no-pie', '-I' + str(root / 'include'),
                    '-x', 'c', '-', '-o', binary], input=program, text=True, check=True)
    subprocess.run([binary], check=True)
