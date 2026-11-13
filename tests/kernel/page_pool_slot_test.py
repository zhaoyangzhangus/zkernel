"""Production local-page slot calculation against division-based oracle."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'kernel/mm/phys/percpu_page.c').read_text()
begin = source.index('static bool pool_page_in_local_group(')
end = source.index('bool pool_free_small(', begin)
function = source[begin:end]
previous = function.replace('pool_page_in_local_group(', 'previous_slot(')
previous = previous.replace('(delta & (slot_size - 1U))', '(delta % slot_size)')
previous = previous.replace('(delta >> (PAGE_SHIFT + order))', '(delta / slot_size)')
previous = previous.replace('    return true;', '    return *bit_out < PAGE_POOL_GROUP_SLOTS;')
prefix = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#define PAGE_SIZE 4096ULL
#define PAGE_SHIFT 12U
#define PAGE_POOL_GROUP_SLOTS 64U
#define PAGE_LIKELY(x) (x)
typedef struct {uint64_t physical;} page_t;
typedef struct {page_t *base;uint64_t free_mask;uint32_t contiguous_pages;} page_local_pool_t;
typedef struct {uint64_t value;} paddr_t;
static paddr_t page_to_phys(const page_t *page) { return (paddr_t){page->physical}; }
'''
body = r'''
static unsigned cases;
static void check(uint64_t base,uint64_t physical,unsigned order) {
    page_t page={physical},head={base};page_local_pool_t pool={&head,0xabcdef,0};
    unsigned actual=0xdeadbeef,expected=actual;
    bool a=pool_page_in_local_group(&page,&pool,order,&actual);
    bool b=previous_slot(&page,&pool,order,&expected);
    assert(a==b&&actual==expected);
    assert(page.physical==physical&&head.physical==base&&pool.base==&head&&pool.free_mask==0xabcdef);
    if(a)assert(actual<64&&physical-base==(uint64_t)actual*(PAGE_SIZE<<order));
    else assert(actual==0xdeadbeef);
    ++cases;
}
int main(void) {
    for(unsigned order=0;order<=5;++order) {
        uint64_t size=PAGE_SIZE<<order;
        for(unsigned slot=0;slot<=65;++slot)
        for(unsigned offset=0;offset<size;++offset) {
            check(0x1000000,0x1000000+slot*size+offset,order);
            check(0,slot*size+offset,order);
        }
        check(UINT64_MAX-size*64,UINT64_MAX-size,order);
        check(UINT64_MAX-size*64,UINT64_MAX,order);
        check(UINT64_MAX,0,order);check(4096,4095,order);
        page_t page={0};page_local_pool_t pool={&page,0,0};unsigned out=7;
        assert(!pool_page_in_local_group(0,&pool,order,&out)&&out==7);
        assert(!pool_page_in_local_group(&page,0,order,&out)&&out==7);
        assert(!pool_page_in_local_group(&page,&pool,order,0));
        pool.base=0;assert(!pool_page_in_local_group(&page,&pool,order,&out)&&out==7);
    }
    uint64_t seed=73;
    for(unsigned n=0;n<100000;++n) {
        seed=seed*6364136223846793005ULL+1U;uint64_t base=seed&~4095ULL;
        seed=seed*6364136223846793005ULL+1U;
        check(base,seed,n%6);
    }
    printf("Pool slots: %u exact division-oracle cases, every byte offset/order/slot, underflow/overflow and failed-output preservation passed\n",cases);
}
'''
with tempfile.TemporaryDirectory(prefix='liteos-pool-slot-') as directory:
    binary = str(Path(directory) / 'test')
    subprocess.run(['cc', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
                    '-x', 'c', '-', '-o', binary], input=prefix+function+previous+body,
                   text=True, check=True)
    subprocess.run([binary], check=True, timeout=60)
    for label, implementation, name in [('previous', previous, 'previous_slot'),
                                         ('production', function, 'pool_page_in_local_group')]:
        # 生成使用运行时阶数和调用方元数据的真实辅助函数，而不是可能被编译器简化掉的
        # 常量阶测试夹具。
        implementation = implementation.replace('static bool '+name+'(', 'bool '+name+'(')
        assembly = subprocess.run(['cc', '-std=c11', '-O2', '-S', '-x', 'c', '-', '-o', '-'],
                                  input=prefix+implementation, text=True,
                                  capture_output=True, check=True).stdout
        divides = any(line.strip().startswith(('divq ', 'divl ', 'idivq ', 'idivl '))
                      or line.strip().startswith(('divq\t', 'divl\t')) for line in assembly.splitlines())
        assert divides == (label == 'previous'), (label, assembly)
    print('Runtime-order x86-64 assembly: previous integer division present; production contains no divide instruction')
