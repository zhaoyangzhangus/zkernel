"""Test actual page protection: unchanged PTEs must not send TLB IPIs."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'kernel/arch/x86_64/mm/page_table.c').read_text()
begin = source.index('kstatus_t x86_protect_page(')
end = source.index('\n}', begin) + 2
protect = source[begin:end]
begin = source.index('kstatus_t x86_protect_user_page_deferred(')
end = source.index('\n}', begin) + 2
deferred = source[begin:end]
begin = source.index('static kstatus_t page_table_protect_user_entry_locked(')
end = source.index('\n}', begin) + 2
deferred = source[begin:end] + '\n' + deferred
# 回滚使用的 INVLPG 是特权指令；宿主测试检查其 PTE 结果。
protect = protect.replace('__asm__ volatile ("invlpg (%0)" : : "r"(va) : "memory");',
                          '++local_invalidations;')
defines = '\n'.join(line for line in
    (root / 'kernel/arch/x86_64/mm/mmu_internal.h').read_text().splitlines()
    if line.startswith('#define PTE_'))
program = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
typedef int kstatus_t;
typedef uint64_t vaddr_t;
typedef struct { uint64_t value; } paddr_t;
enum x86_cache_mode { X86_CACHE_WB, X86_CACHE_UC };
#define K_OK 0
#define K_EINVAL -22
#define K_ENOENT -2
#define K_EIO -5
#define PAGE_SIZE 4096ULL
#define X86_64_USER_TOP ((1ULL << 47) - 1)
#define X86_PAGE_WRITE 1
#define X86_PAGE_USER 2
#define X86_PAGE_GLOBAL 4
#define X86_PAGE_EXEC 8
static uint64_t tables[5][512];
static unsigned locks, shootdowns, local_invalidations;
static bool shootdown_success = true;
static bool x86_is_canonical(uint64_t va) {
    return va < (1ULL << 47) || va >= 0xffff800000000000ULL;
}
static uint64_t *table_from_physical(uint64_t entry) {
    unsigned index = (entry & 0x000ffffffffff000ULL) >> 12;
    return index < 5 ? tables[index] : 0;
}
static void table_lock(paddr_t root,uint64_t va) { (void)root;(void)va;++locks; }
static void table_unlock(void) { --locks; }
static uint64_t x86_pte_cache_bits(enum x86_cache_mode mode, bool large) {
    (void)large;
    return mode == X86_CACHE_UC ? 16 : 0;
}
static bool x86_tlb_shootdown_page(paddr_t root, vaddr_t va) {
    (void)root; (void)va; ++shootdowns; return shootdown_success;
}
#define kernel_serial_printf_serial_only(...) ((void)0)
''' + defines + '\n' + protect + '\n' + deferred + r'''
int main(void) {
    const paddr_t root = {0};
    tables[0][0] = 4097; tables[1][0] = 8193; tables[2][0] = 12289;
    uint64_t *pte = &tables[3][5];
    const uint64_t original = 0x100000 | PTE_PRESENT | PTE_USER |
        PTE_NO_EXECUTE | PTE_ACCESSED | PTE_DIRTY;
    *pte = original;
    for (unsigned i = 0; i < 10000; ++i)
        assert(x86_protect_page(root, 5*PAGE_SIZE, X86_PAGE_USER, X86_CACHE_WB) == K_OK);
    assert(shootdowns == 0 && *pte == original && locks == 0);
    assert(x86_protect_page(root, 5*PAGE_SIZE, X86_PAGE_USER | X86_PAGE_WRITE, X86_CACHE_WB) == K_OK);
    assert(shootdowns == 0 && local_invalidations == 1 && *pte == (original | PTE_WRITE));
    assert(x86_protect_page(root, 5*PAGE_SIZE, X86_PAGE_USER, X86_CACHE_WB) == K_OK);
    assert(shootdowns == 1 && *pte == original);
    assert(x86_protect_page(root, 5*PAGE_SIZE, X86_PAGE_USER, X86_CACHE_UC) == K_OK);
    assert(shootdowns == 2 && *pte == (original | PTE_CACHE_DISABLE));
    shootdown_success = false;
    assert(x86_protect_page(root, 5*PAGE_SIZE, X86_PAGE_USER | X86_PAGE_EXEC, X86_CACHE_WB) == K_EIO);
    assert(shootdowns == 3 && local_invalidations == 2);
    assert(*pte == (original | PTE_CACHE_DISABLE) && locks == 0);
    assert(x86_protect_page(root, 6*PAGE_SIZE, X86_PAGE_USER, X86_CACHE_WB) == K_ENOENT);
    assert(x86_protect_page(root, 1, X86_PAGE_USER, X86_CACHE_WB) == K_EINVAL);
    assert(shootdowns == 3 && locks == 0);
    shootdown_success = true;
    *pte = original | PTE_GLOBAL;
    assert(x86_protect_page(root, 5*PAGE_SIZE,
        X86_PAGE_USER | X86_PAGE_GLOBAL | X86_PAGE_WRITE, X86_CACHE_WB) == K_OK);
    assert(shootdowns == 4 && local_invalidations == 2);
    *pte = original & ~PTE_USER;
    assert(x86_protect_page(root, 5*PAGE_SIZE, X86_PAGE_WRITE, X86_CACHE_WB) == K_OK);
    assert(shootdowns == 5 && locks == 0);
    tables[0][256] = 4097;
    *pte = original;
    assert(x86_protect_page(root, 0xffff800000005000ULL,
        X86_PAGE_USER | X86_PAGE_WRITE, X86_CACHE_WB) == K_OK);
    assert(shootdowns == 6 && local_invalidations == 2 && locks == 0);
    const paddr_t deferred_root = {16384};
    tables[4][0] = 4097;
    bool changed = false;
    *pte = original | PTE_WRITE;
    assert(x86_protect_user_page_deferred(deferred_root, 5*PAGE_SIZE,
        X86_PAGE_USER, X86_CACHE_WB, &changed) == K_OK && changed);
    assert(*pte == original && shootdowns == 6 && locks == 0);
    assert(x86_protect_user_page_deferred(deferred_root, 5*PAGE_SIZE,
        X86_PAGE_USER, X86_CACHE_WB, &changed) == K_OK && !changed);
    assert(x86_protect_user_page_deferred(deferred_root, 6*PAGE_SIZE,
        X86_PAGE_USER, X86_CACHE_WB, &changed) == K_ENOENT && !changed);
    *pte = original | PTE_GLOBAL;
    assert(x86_protect_user_page_deferred(deferred_root, 5*PAGE_SIZE,
        X86_PAGE_USER, X86_CACHE_WB, &changed) == K_EINVAL && !changed);
    assert(*pte == (original | PTE_GLOBAL));
    assert(x86_protect_user_page_deferred(deferred_root, 1,
        X86_PAGE_USER, X86_CACHE_WB, &changed) == K_EINVAL && !changed);
    assert(x86_protect_user_page_deferred(deferred_root, 5*PAGE_SIZE,
        X86_PAGE_GLOBAL | X86_PAGE_USER, X86_CACHE_WB, &changed) == K_EINVAL);
    assert(x86_protect_user_page_deferred(root, 5*PAGE_SIZE,
        X86_PAGE_USER, X86_CACHE_WB, &changed) == K_EINVAL);
    assert(x86_protect_user_page_deferred(deferred_root, 5*PAGE_SIZE,
        X86_PAGE_USER, X86_CACHE_WB, 0) == K_EINVAL);
    assert(shootdowns == 6 && locks == 0);
    puts("page protect: unchanged PTEs and same-page user write upgrades avoid IPIs; restrictions/cache/global/kernel changes retain shootdown and rollback");
}
'''
with tempfile.TemporaryDirectory(prefix='liteos-protect-noop-') as directory:
    path = Path(directory)
    (path / 'test.c').write_text(program)
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
                    str(path / 'test.c'), '-o', str(path / 'test')], check=True)
    subprocess.run([str(path / 'test')], check=True)
