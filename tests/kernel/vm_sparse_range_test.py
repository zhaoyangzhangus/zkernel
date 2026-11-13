"""Run production unmap/protection loops against sparse and failing PTEs."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
def function(path, name):
    source = (root / path).read_text()
    begin = source.index(name)
    begin = source.rfind('\n', 0, begin) + 1
    brace = source.index('{', begin)
    depth = 0
    for end in range(brace, len(source)):
        depth += (source[end] == '{') - (source[end] == '}')
        if depth == 0:
            return source[begin:end+1]

program = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdatomic.h>
#include <stdio.h>
typedef int kstatus_t;
typedef uint64_t vaddr_t;
typedef struct { uint64_t value; } paddr_t;
typedef struct { paddr_t root_table; atomic_uint_fast64_t rss_pages; } vm_space_t;
typedef struct { unsigned type; } vm_object_t;
#define VM_OBJECT_FILE 2U
typedef struct { uint64_t start,end; vm_object_t *object,*private_object; } vm_area_t;
#define PAGE_SIZE 4096ULL
#define K_OK 0
#define K_ENOENT -2
#define K_EINVAL -22
#define K_EIO -5
#define X86_CACHE_WB 0
#define X86_PAGE_USER 2U
#define X86_PAGE_GLOBAL 4U
#define X86_USER_UNMAP_BATCH_MAX 64U
static const uint64_t pages[] = {0x5000, 1ULL<<30, (1ULL<<46)-PAGE_SIZE};
static unsigned calls, changes, flags_seen;
static unsigned dense_pages, batches, accounting_calls;
static size_t batch_sizes[4];
static uint64_t fail_scan_at, fail_change_at;
static int disappeared;
static int x86_next_present_page(paddr_t root,uint64_t start,uint64_t end,vaddr_t *out) {
    (void)root; ++calls; assert(calls < 200);
    if (fail_scan_at && start >= fail_scan_at) return K_EIO;
    if (dense_pages) {
        if (start>=dense_pages*PAGE_SIZE) return K_ENOENT;
        *out=start;return K_OK;
    }
    for (unsigned i=0;i<3;++i) if (pages[i]>=start && pages[i]<end) {
        *out=pages[i];return K_OK;
    }
    return K_ENOENT;
}
static int x86_unmap_page(paddr_t root, uint64_t va, void *out) {
    (void)root;(void)out;++changes;
    if (va==fail_change_at) return K_EIO;
    return disappeared && va==pages[0] ? K_ENOENT : K_OK;
}
static int x86_unmap_user_page_batch(paddr_t root,const vaddr_t *addresses,size_t count,size_t *unmapped) {
    (void)root; *unmapped=0;
    assert(batches<4 && count && count<=64);
    batch_sizes[batches++]=count;
    for (size_t i=1;i<count;++i) assert(addresses[i]>addresses[i-1]);
    for (size_t i=0;i<count;++i) if (fail_change_at && addresses[i]==fail_change_at) return K_EIO;
    for (size_t i=0;i<count;++i) {
        ++changes;
        if (!(disappeared && addresses[i]==pages[0])) ++*unmapped;
    }
    return K_OK;
}
static unsigned vm_area_page_flags(const vm_area_t *area,uint64_t va,unsigned prot) {
    assert(va>=area->start && va<area->end); return prot;
}
static int x86_protect_page(paddr_t root,uint64_t va,unsigned flags,int cache) {
    (void)cache;flags_seen=flags;return x86_unmap_page(root,va,0);
}
static int x86_protect_user_page_deferred(paddr_t root,uint64_t va,unsigned flags,int cache,bool *changed) {
    int status=x86_protect_page(root,va,flags,cache);
    *changed=status==K_OK;
    return status;
}
/* 事务夹具模拟融合接口；真实页表的锁次数由融合遍历回归验证。 */
static kstatus_t x86_protect_next_user_page_deferred(paddr_t root,vaddr_t start,
    uint64_t end,uint32_t flags,int mode,vaddr_t *out,bool *changed) {
    *changed=false;
    kstatus_t status=x86_next_present_page(root,start,end,out);
    return status==K_OK ? x86_protect_user_page_deferred(root,*out,flags,mode,changed) : status;
}
''' + function('kernel/mm/tlb.c', 'rss_decrement(').replace(
    'if (count == 0U) return;', 'if (count == 0U) return;\n    ++accounting_calls;') + '\n' + \
    function('kernel/mm/tlb.c', 'vm_tlb_unmap_range(') + '\n' + \
    function('kernel/mm/protection.c', 'protect_resident_pages(') + r'''
static void reset(void) { calls=changes=flags_seen=dense_pages=batches=accounting_calls=0;fail_scan_at=fail_change_at=0;disappeared=0; }
int main(void) {
    vm_space_t space = {.root_table={4096}};
    vm_area_t area = {.start=0,.end=1ULL<<46};
    atomic_init(&space.rss_pages,3);
    reset(); assert(vm_tlb_unmap_range(&space,0,area.end)==K_OK);
    assert(calls==3 && changes==3 && atomic_load(&space.rss_pages)==0);
    reset(); atomic_store(&space.rss_pages,3);fail_change_at=pages[1];
    assert(vm_tlb_unmap_range(&space,0,area.end)==K_EIO);
    assert(changes==0 && atomic_load(&space.rss_pages)==3);
    reset(); atomic_store(&space.rss_pages,3);fail_scan_at=pages[0]+PAGE_SIZE;
    assert(vm_tlb_unmap_range(&space,0,area.end)==K_EIO);
    assert(changes==1 && atomic_load(&space.rss_pages)==2);
    reset();atomic_store(&space.rss_pages,0);disappeared=1;
    assert(vm_tlb_unmap_range(&space,0,area.end)==K_OK);
    assert(atomic_load(&space.rss_pages)==0);
    reset();assert(vm_tlb_unmap_range(&space,0,PAGE_SIZE)==K_OK && changes==0);
    assert(vm_tlb_unmap_range(0,0,area.end)==K_EINVAL);
    assert(vm_tlb_unmap_range(&space,area.end,area.end)==K_EINVAL);
    bool deferred_changed=false;
    reset(); assert(protect_resident_pages(&space,&area,7,&deferred_changed)==K_OK);
    assert(changes==3 && calls==3 && flags_seen==7);
    reset();fail_change_at=pages[1];
    assert(protect_resident_pages(&space,&area,1,&deferred_changed)==K_EIO && changes==2);
    reset();assert(protect_resident_pages(&space,&area,1,&deferred_changed)==K_OK);
    assert(changes==3 && flags_seen==1); /* rollback uses old permissions */
    reset();fail_scan_at=pages[0]+PAGE_SIZE;
    assert(protect_resident_pages(&space,&area,1,&deferred_changed)==K_EIO && changes==1);
    reset();disappeared=1;assert(protect_resident_pages(&space,&area,7,&deferred_changed)==K_OK);
    reset();deferred_changed=false;
    assert(protect_resident_pages(&space,&area,2,&deferred_changed)==K_OK);
    assert(changes==3 && deferred_changed);
    reset();dense_pages=130;atomic_store(&space.rss_pages,130);
    assert(vm_tlb_unmap_range(&space,0,130*PAGE_SIZE)==K_OK);
    assert(batches==3 && batch_sizes[0]==64 && batch_sizes[1]==64 && batch_sizes[2]==2);
    assert(changes==130 && atomic_load(&space.rss_pages)==0 && accounting_calls==1);
    reset();dense_pages=130;fail_change_at=100*PAGE_SIZE;atomic_store(&space.rss_pages,130);
    assert(vm_tlb_unmap_range(&space,0,130*PAGE_SIZE)==K_EIO);
    assert(batches==2 && changes==64 && atomic_load(&space.rss_pages)==66);
    unsigned cases=0;
    for(unsigned count=1;count<=130;++count)
      for(unsigned failure=0;failure<=count;++failure)
        for(unsigned scan_failure=0;scan_failure<2;++scan_failure) {
            reset();dense_pages=count;atomic_store(&space.rss_pages,count);
            if(failure) {
                if(scan_failure) fail_scan_at=(failure-1)*PAGE_SIZE+1;
                else fail_change_at=(failure-1)*PAGE_SIZE;
            }
            /* MMU 测试夹具中，零表示未注入故障。 */
            bool injected=failure && (scan_failure ? failure<count : failure>1);
            kstatus_t status=vm_tlb_unmap_range(&space,0,count*PAGE_SIZE);
            assert(status==(injected ? K_EIO : K_OK));
            unsigned completed=!injected ? count : scan_failure ? failure :
                               ((failure-1)/64)*64;
            assert(changes==completed && atomic_load(&space.rss_pages)==count-completed);
            assert(accounting_calls==(completed!=0));
            ++cases;
        }
    printf("RSS unmap: %u dense batch/scan failure cases; one update per acknowledged prefix, zero empty updates passed\n",cases);
    puts("64 TiB sparse unmap/protect: 3 PTEs; holes, RSS, failure propagation and old-permission restoration passed");
}
'''
with tempfile.TemporaryDirectory(prefix='liteos-vm-range-') as directory:
    path = Path(directory)
    (path / 'test.c').write_text(program)
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
                    str(path / 'test.c'), '-o', str(path / 'test')], check=True)
    subprocess.run([str(path / 'test')], check=True)
