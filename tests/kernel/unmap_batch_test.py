"""Exercise production batch unmap, including ACK failure and shared tables."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'kernel/arch/x86_64/mm/page_table.c').read_text()
begin = source.index('kstatus_t x86_unmap_user_page_batch(')
end = source.index('\n}', begin) + 2
defines = '\n'.join(line for line in
    (root / 'kernel/arch/x86_64/mm/mmu_internal.h').read_text().splitlines()
    if line.startswith('#define PTE_'))
program = r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
typedef int kstatus_t;
typedef uint64_t vaddr_t;
typedef struct { uint64_t value; } paddr_t;
typedef struct { atomic_int mapcount; } page_t;
#define PAGE_SIZE 4096ULL
#define X86_64_USER_TOP ((1ULL<<47)-1)
#define X86_USER_UNMAP_BATCH_MAX 64U
#define K_OK 0
#define K_EINVAL -22
#define K_EIO -5
static uint64_t tables[6][512];
static bool freed[6], ack_success=true, acknowledged;
static page_t pages[64];
static unsigned locks, flushes, releases, empty_checks;
static uint64_t expected[64];
static size_t expected_count;
static paddr_t paddr_make(uint64_t p) { return (paddr_t){p}; }
''' + defines + r'''
static uint64_t *table_from_physical(uint64_t entry) {
    unsigned index=(entry&PTE_ADDRESS_MASK)>>12;
    if (!index || index>=6) return 0;
    assert(!freed[index]); return tables[index];
}
static void table_lock(paddr_t root,uint64_t va) { (void)root;(void)va;assert(!locks); ++locks; }
static void table_unlock(void) { assert(locks==1); --locks; }
static page_t *phys_to_page(paddr_t physical) {
    unsigned index=(physical.value>>12)-256;
    return index<64 ? &pages[index] : 0;
}
static bool x86_tlb_shootdown_root(paddr_t root) {
    assert(root.value==4096 && locks==1); ++flushes;
    for (size_t i=0;i<expected_count;++i) {
        uint64_t va=expected[i];
        unsigned pt=(va>>21)==0 ? 4 : 5;
        assert(tables[pt][(va>>12)&511]==0);
        assert(atomic_load(&pages[i].mapcount)==2);
    }
    assert(releases==0); acknowledged=ack_success;
    return ack_success;
}
static void release_dynamic_table(uint64_t *parent,uint64_t *table) {
    assert(acknowledged && locks==1);
    for (unsigned i=0;i<512;++i) {
        ++empty_checks;
        if (table[i]) return;
    }
    unsigned index=(unsigned)(table-tables[0])/512;
    assert(index<6 && !freed[index]);
    *parent=0;freed[index]=true;++releases;
    memset(table,0xa5,4096); /* catch reuse of saved, freed parent pointers */
}
''' + source[begin:end] + r'''
static void reset(size_t count,bool split) {
    memset(tables,0,sizeof(tables));memset(freed,0,sizeof(freed));
    locks=flushes=releases=empty_checks=0;ack_success=true;acknowledged=false;
    tables[1][0]=8193;tables[2][0]=12289;tables[3][0]=16385;
    if (split) tables[3][1]=20481;
    expected_count=count;
    for (size_t i=0;i<count;++i) {
        expected[i]=(split && i>=32 ? (1ULL<<21)+(i-32)*PAGE_SIZE : i*PAGE_SIZE);
        unsigned pt=(expected[i]>>21)==0 ? 4 : 5;
        tables[pt][(expected[i]>>12)&511]=(256+i)*PAGE_SIZE|PTE_PRESENT|PTE_USER|PTE_WRITE|PTE_ACCESSED|PTE_DIRTY;
        atomic_store(&pages[i].mapcount,2);
    }
}
int main(void) {
    size_t removed=123;
    reset(64,false);
    assert(x86_unmap_user_page_batch((paddr_t){4096},expected,64,&removed)==K_OK);
    assert(removed==64 && flushes==1 && releases==3 && locks==0);
    for (unsigned i=0;i<64;++i) assert(atomic_load(&pages[i].mapcount)==1);
    reset(64,true);
    assert(x86_unmap_user_page_batch((paddr_t){4096},expected,64,&removed)==K_OK);
    assert(removed==64 && flushes==1 && releases==4 && !locks);
    /* 末尾仍有一个叶节点时，即使只修改了一个 PT，对每个移除页面重复扫描同一张表也
     * 会造成较大开销。 */
    reset(64,false);tables[4][511]=0x200000|PTE_PRESENT|PTE_USER;
    assert(x86_unmap_user_page_batch((paddr_t){4096},expected,64,&removed)==K_OK);
    assert(removed==64 && flushes==1 && !releases && !locks);
    assert(tables[4][511]==(0x200000|PTE_PRESENT|PTE_USER));
    printf("64 removals with surviving leaf: %u empty-table checks\n",empty_checks);
    assert(empty_checks<=514);
    reset(64,true);
    tables[4][511]=tables[5][511]=0x200000|PTE_PRESENT|PTE_USER;
    assert(x86_unmap_user_page_batch((paddr_t){4096},expected,64,&removed)==K_OK);
    assert(removed==64 && !releases && empty_checks<=1026);
    /* PT/PD 组的最后一个地址可能是未映射空洞。它不能阻止回收先前组中已清空的上级表。 */
    for (unsigned shift=21;shift<=39;shift+=9) {
        reset(1,false);
        uint64_t trailing_hole[]={0,1ULL<<shift};
        assert(x86_unmap_user_page_batch((paddr_t){4096},trailing_hole,2,&removed)==K_OK);
        assert(removed==1 && releases==3 && !locks);
    }
    reset(64,true);ack_success=false;
    uint64_t originals[64];
    for (unsigned i=0;i<64;++i) originals[i]=tables[i<32?4:5][i%32];
    assert(x86_unmap_user_page_batch((paddr_t){4096},expected,64,&removed)==K_EIO);
    assert(!removed && flushes==1 && !releases && !locks);
    for (unsigned i=0;i<64;++i) {
        assert(tables[i<32?4:5][i%32]==originals[i]);
        assert(atomic_load(&pages[i].mapcount)==2);
    }
    reset(2,false);tables[4][1]|=PTE_GLOBAL;
    uint64_t first=tables[4][0];
    assert(x86_unmap_user_page_batch((paddr_t){4096},expected,2,&removed)==K_EINVAL);
    assert(tables[4][0]==first && !flushes && !locks);
    reset(1,false);tables[3][0]|=PTE_LARGE;
    assert(x86_unmap_user_page_batch((paddr_t){4096},expected,1,&removed)==K_EINVAL);
    assert(!flushes && !locks);
    reset(1,false);tables[4][0]&=~PTE_USER;
    assert(x86_unmap_user_page_batch((paddr_t){4096},expected,1,&removed)==K_EINVAL);
    reset(0,false);uint64_t holes[]={0,PAGE_SIZE};
    assert(x86_unmap_user_page_batch((paddr_t){4096},holes,2,&removed)==K_OK);
    assert(!removed && !flushes && !releases && !locks);
    uint64_t bad[]={PAGE_SIZE,PAGE_SIZE};
    assert(x86_unmap_user_page_batch((paddr_t){4096},bad,2,&removed)==K_EINVAL);
    bad[1]=0;
    assert(x86_unmap_user_page_batch((paddr_t){4096},bad,2,&removed)==K_EINVAL);
    bad[0]=1;
    assert(x86_unmap_user_page_batch((paddr_t){4096},bad,1,&removed)==K_EINVAL);
    bad[0]=0xffff800000000000ULL;
    assert(x86_unmap_user_page_batch((paddr_t){4096},bad,1,&removed)==K_EINVAL);
    assert(x86_unmap_user_page_batch((paddr_t){0},holes,2,&removed)==K_EINVAL);
    assert(x86_unmap_user_page_batch((paddr_t){4096},holes,65,&removed)==K_EINVAL);
    assert(x86_unmap_user_page_batch((paddr_t){4096},holes,0,&removed)==K_EINVAL);
    assert(x86_unmap_user_page_batch((paddr_t){4096},0,2,&removed)==K_EINVAL);
    assert(x86_unmap_user_page_batch((paddr_t){4096},holes,2,0)==K_EINVAL);
    puts("batch unmap: 64 PTEs/one ACK round; rollback, ownership and shared-table reclamation passed");
}
'''
with tempfile.TemporaryDirectory(prefix='liteos-unmap-batch-') as directory:
    path = Path(directory)
    (path / 'test.c').write_text(program)
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
                    str(path / 'test.c'), '-o', str(path / 'test')], check=True)
    subprocess.run([str(path / 'test')], check=True)
