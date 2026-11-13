"""Production batched RSS accounting: saturation, retry and concurrent updates."""
from pathlib import Path
import subprocess
import tempfile

root=Path(__file__).resolve().parents[2]
source=(root/'kernel/mm/tlb.c').read_text()
start=source.index('static void rss_decrement(')
helper=source[start:source.index('\n/*',start)]
assert source.count('rss_decrement(space, total_unmapped);') == 1
assert 'total_unmapped += unmapped;' in source
assert 'index < unmapped' not in source
helper=helper.replace('atomic_load_explicit(&space->rss_pages, memory_order_relaxed)',
                      'checked_load(&space->rss_pages, memory_order_relaxed)')
helper=helper.replace('atomic_compare_exchange_weak_explicit(', 'checked_cas(')
program=r'''
#include <assert.h>
#include <stdio.h>
#include <pthread.h>
#include <kernel/vm.h>
static atomic_uint attempts,loads;
static _Thread_local unsigned inject;
static uint64_t checked_load(atomic_uint_fast64_t *p,memory_order order) {
    assert(order==memory_order_relaxed);++loads;return atomic_load_explicit(p,order);
}
static bool checked_cas(atomic_uint_fast64_t *p,uint64_t *expected,uint64_t desired,
                        memory_order success,memory_order failure) {
    assert(success==memory_order_relaxed && failure==memory_order_relaxed);++attempts;
    if(inject) {--inject;atomic_fetch_add(p,7);}
    return atomic_compare_exchange_weak_explicit(p,expected,desired,success,failure);
}
''' + helper + r'''
static vm_space_t space;
static void check(uint64_t value,size_t count) {
    atomic_store(&space.rss_pages,value);atomic_store(&attempts,0);atomic_store(&loads,0);
    rss_decrement(&space,count);
    uint64_t expected=value>count?value-count:0;
    assert(atomic_load(&space.rss_pages)==expected);
    assert(atomic_load(&loads)==(count!=0));
    assert(atomic_load(&attempts)==(count!=0 && value!=0));
}
static void *update(void *opaque) {
    unsigned id=(unsigned)(uintptr_t)opaque;
    for(unsigned i=0;i<10000;++i) {
        if(id<4)rss_decrement(&space,1+i%64);
        else atomic_fetch_add_explicit(&space.rss_pages,1+i%17,memory_order_relaxed);
    }
    return 0;
}
int main(void) {
    unsigned cases=0;
    for(uint64_t value=0;value<512;++value)for(size_t count=0;count<=257;++count) {
        /* 使用此前逐页饱和处理的基准，而不是会回绕的减法。 */
        uint64_t old=value;
        for(size_t i=0;i<count;++i)if(old)--old;
        check(value,count);assert(atomic_load(&space.rss_pages)==old);++cases;
    }
    const uint64_t values[]={0,1,63,64,65,UINT32_MAX,UINT64_MAX-64,UINT64_MAX};
    const size_t counts[]={0,1,63,64,65,SIZE_MAX-1,SIZE_MAX};
    for(unsigned i=0;i<sizeof(values)/sizeof(*values);++i)
      for(unsigned j=0;j<sizeof(counts)/sizeof(*counts);++j) {check(values[i],counts[j]);++cases;}
    atomic_store(&space.rss_pages,100);atomic_store(&attempts,0);inject=3;
    rss_decrement(&space,64);
    assert(atomic_load(&space.rss_pages)==57 && atomic_load(&attempts)==4 && !inject);
    uint64_t total_sub=0,total_add=0;
    for(unsigned i=0;i<10000;++i) {total_sub+=1+i%64;total_add+=1+i%17;}
    atomic_store(&space.rss_pages,10000000);atomic_store(&attempts,0);
    pthread_t workers[8];
    for(unsigned i=0;i<8;++i)assert(!pthread_create(&workers[i],0,update,(void *)(uintptr_t)i));
    for(unsigned i=0;i<8;++i)assert(!pthread_join(workers[i],0));
    assert(atomic_load(&space.rss_pages)==10000000-4*total_sub+4*total_add);
    assert(atomic_load(&attempts)>=40000);
    printf("RSS batch: %u saturation/zero/overflow oracle cases, forced CAS retries, 80000 concurrent additions/subtractions; one uncontended CAS per nonempty batch passed\n",cases);
}
'''
with tempfile.TemporaryDirectory(prefix='liteos-rss-batch-') as directory:
    binary=str(Path(directory)/'test')
    subprocess.run(['cc','-std=c11','-O2','-Wall','-Wextra','-Werror','-pthread',
                    '-I'+str(root/'include'),'-fsanitize=address,undefined',
                    '-fno-pie','-no-pie','-x','c','-','-o',binary],
                   input=program,text=True,check=True)
    subprocess.run([binary],check=True,timeout=30)
