"""Exercise the production resident-page mprotect transaction with mocked MMU."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'kernel/mm/protection.c').read_text()
helper = source[source.index('static kstatus_t protect_resident_pages('):
                source.index('static bool range_fully_mapped(')]
transaction = source[source.index('    if (x86_page_range_has_present_pages('):
                     source.index('\nkstatus_t vm_protect(')]
program = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdatomic.h>
#include <stddef.h>
#include <string.h>
#include <kernel/list.h>
#define K_OK 0
#define K_ENOENT -2
#define K_EIO -5
#define K_EINVAL -22
#define X86_PAGE_USER 2U
#define X86_PAGE_GLOBAL 4U
#define X86_CACHE_WB 0
typedef struct { list_head_t ordered_node; uint64_t start,end; uint32_t prot; bool cow; } vm_area_t;
typedef struct { list_head_t area_list; uint64_t root_table; } vm_space_t;
static vm_area_t areas[2];
static vm_space_t space;
static uint32_t pages[32];
static bool present[32], locked;
static unsigned flushes, synchronous, calls, fail_call, fail_flush, bumps;
static vm_area_t *area_from_list(list_head_t *p) {
    return (vm_area_t *)((char *)p - offsetof(vm_area_t,ordered_node));
}
static uint32_t vm_area_page_flags(const vm_area_t *a, uint64_t va, uint32_t prot) {
    (void)va; return a->cow ? prot & ~1U : prot;
}
static kstatus_t x86_next_present_page(uint64_t root, vaddr_t va, uint64_t end, vaddr_t *out) {
    (void)root;
    for (; va < end; va += PAGE_SIZE)
        if (present[va / PAGE_SIZE]) { *out=va; return K_OK; }
    return K_ENOENT;
}
static kstatus_t x86_protect_user_page_deferred(uint64_t root, vaddr_t va,
        uint32_t flags, unsigned cache, bool *changed) {
    (void)root; (void)cache; assert(locked);
    *changed=false;
    if (++calls == fail_call) return K_EINVAL;
    unsigned i=va/PAGE_SIZE;
    *changed=pages[i]!=flags; pages[i]=flags;
    return K_OK;
}
static kstatus_t x86_protect_page(uint64_t root, vaddr_t va, uint32_t flags, unsigned cache) {
    (void)root; (void)cache; assert(locked);
    ++synchronous; pages[va/PAGE_SIZE]=flags; return K_OK;
}
static bool x86_tlb_shootdown_root(uint64_t root) {
    (void)root; assert(locked);
    /* No VMA publication may precede acknowledgement, even on rollback. */
    assert(areas[0].prot==3 && areas[1].prot==3);
    return ++flushes != fail_flush;
}
static void vm_tlb_bump_generation(vm_space_t *s) { (void)s; assert(locked); ++bumps; }
static void map_unlock(vm_space_t *s) { (void)s; assert(locked); locked=false; }
/* 此夹具隔离页表事务，真实合并和引用释放由索引及合并回归覆盖。 */
static void vm_area_merge_range(vm_space_t *s,vm_area_t *first,
                                list_head_t *end,list_head_t *retired) {
    assert(locked && s==&space && first==&areas[0]);
    assert(end==&space.area_list || end==&areas[1].ordered_node);
    assert(list_empty(retired));
}
static void area_release(vm_area_t *a) { (void)a; assert(!"unexpected retired VMA"); }
static bool x86_page_range_has_present_pages(uint64_t root,vaddr_t start,uint64_t end) {
    (void)root;assert(locked);
    for(uint64_t va=start;va<end;va+=PAGE_SIZE)if(present[va/PAGE_SIZE])return true;
    return false;
}
''' + helper + r'''
static kstatus_t transaction(vm_space_t *space, vaddr_t address, uint64_t end, uint32_t prot) {
    (void)address;
    vm_area_t *start_area=area_from_list(space->area_list.next);
    list_head_t *range_end=&space->area_list;
    for(list_head_t *item=&start_area->ordered_node;item!=&space->area_list;item=item->next) {
        if(area_from_list(item)->start>=end) {range_end=item;break;}
    }
''' + transaction + r'''
static void reset(void) {
    memset(pages,0,sizeof pages); memset(present,0,sizeof present);
    areas[0]=(vm_area_t){.start=0,.end=16*PAGE_SIZE,.prot=3};
    areas[1]=(vm_area_t){.start=16*PAGE_SIZE,.end=32*PAGE_SIZE,.prot=3};
    space.area_list.next=&areas[0].ordered_node; areas[0].ordered_node.next=&areas[1].ordered_node;
    areas[1].ordered_node.next=&space.area_list;
    for(unsigned i=0;i<32;i++) { present[i]=true; pages[i]=3; }
    flushes=synchronous=calls=fail_call=fail_flush=bumps=0; locked=true;
}
int main(void) {
    reset(); assert(transaction(&space,0,32*PAGE_SIZE,2)==K_OK);
    assert(flushes==1 && calls==32 && bumps==1 && !locked && areas[0].prot==2 && areas[1].prot==2);
    for(unsigned i=0;i<32;i++) assert(pages[i]==2);
    reset(); assert(transaction(&space,0,32*PAGE_SIZE,3)==K_OK);
    assert(flushes==0 && calls==32 && !locked);
    reset(); for(unsigned i=0;i<32;i++) present[i]=i==5 || i==30;
    assert(transaction(&space,0,32*PAGE_SIZE,2)==K_OK && calls==2 && flushes==1);
    reset(); memset(present,0,sizeof present);
    assert(transaction(&space,0,32*PAGE_SIZE,2)==K_OK && calls==0 && flushes==0 && bumps==1);
    assert(!locked && areas[0].prot==2 && areas[1].prot==2);
    reset(); assert(transaction(&space,0,16*PAGE_SIZE,2)==K_OK);
    assert(calls==16 && flushes==1 && areas[0].prot==2 && areas[1].prot==3);
    for(unsigned i=16;i<32;i++) assert(pages[i]==3);
    reset(); assert(transaction(&space,0,32*PAGE_SIZE,4)==K_OK);
    assert(synchronous==32 && calls==0 && flushes==0);
    reset(); areas[0].cow=true;
    assert(transaction(&space,0,32*PAGE_SIZE,3)==K_OK && flushes==1);
    for(unsigned i=0;i<32;i++) assert(pages[i]==(i<16 ? 2U : 3U));
    reset(); fail_call=20;
    assert(transaction(&space,0,32*PAGE_SIZE,2)==K_EINVAL);
    assert(flushes==1 && !locked && areas[0].prot==3 && areas[1].prot==3);
    for(unsigned i=0;i<32;i++) assert(pages[i]==3);
    reset(); fail_flush=1;
    assert(transaction(&space,0,32*PAGE_SIZE,2)==K_EIO);
    assert(flushes==2 && !locked && areas[0].prot==3 && areas[1].prot==3);
    for(unsigned i=0;i<32;i++) assert(pages[i]==3);
    reset(); fail_call=20; fail_flush=1;
    assert(transaction(&space,0,32*PAGE_SIZE,2)==K_EIO);
    assert(flushes==1 && !locked && areas[0].prot==3 && areas[1].prot==3);
    for(unsigned i=0;i<32;i++) assert(pages[i]==3);
    reset(); fail_call=1;
    assert(transaction(&space,0,32*PAGE_SIZE,2)==K_EINVAL && flushes==0 && !locked);
    for(unsigned failure=1;failure<=32;++failure) {
        reset();fail_call=failure;
        assert(transaction(&space,0,32*PAGE_SIZE,2)==K_EINVAL);
        assert(calls==failure+((failure+15)/16)*16);
        assert(!locked && !bumps && areas[0].prot==3 && areas[1].prot==3);
        for(unsigned i=0;i<32;++i)assert(pages[i]==3);
    }
    puts("mprotect batch: one acknowledged flush, no-op, sparse, native, COW and failure rollback PASS");
}
'''
with tempfile.TemporaryDirectory(prefix='liteos-mprotect-batch-') as directory:
    path = Path(directory)
    (path / 'test.c').write_text(program)
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                    '-I'+str(root/'include'),
                    '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
                    str(path / 'test.c'), '-o', str(path / 'test')], check=True)
    subprocess.run([str(path / 'test')], check=True)
