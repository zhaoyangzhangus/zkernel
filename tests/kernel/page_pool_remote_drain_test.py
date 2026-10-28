"""Production remote-list drainage: exact old state and one lazy base query."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'kernel/mm/phys/percpu_page.c').read_text()
by_id = source[source.index('static page_pool_group_t *pool_group_by_id('):source.index('static uint32_t pool_group_id(')]
push = source[source.index('static void pool_partial_push('):source.index('static uint32_t pool_partial_pop(')]
notify = source[source.index('static void pool_remote_notify('):source.index('static void pool_drain_remote(')]
drain = source[source.index('static void pool_drain_remote('):source.index('static PAGE_NOINLINE page_t *pool_alloc_slow(')]
cpu_drain = source[source.index('void pool_drain_cpu('):source.index('void pool_reclaim_all(')]
previous = drain.replace('pool_drain_remote(', 'previous_drain(')
previous = previous.replace('    uint64_t local_base_pfn = 0;\n    bool local_base_known = false;\n', '')
start = previous.index('            if (pool->base != 0 && !local_base_known)')
end = previous.index('                pool->free_mask |= bits;', start)
previous = previous[:start] + '''            if (pool->base != 0 &&
                (page_to_phys(pool->base).value >> PAGE_SHIFT) == group->base_pfn) {
''' + previous[end:]
program = '#include "' + str(root / 'kernel/mm/phys/internal.h') + '"\n' + r'''
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
static page_pool_group_t groups[32];
page_pool_group_t *g_pool_groups=groups;
uint32_t g_pool_group_count=32;
page_remote_heads_t g_remote_heads[MAX_CPUS];
static page_t base;
static unsigned queries,expected_queries;
static uint64_t physical;
paddr_t page_to_phys(const page_t *page) {
    assert(page==&base);++queries;return (paddr_t){physical};
}
''' + by_id + push + notify + drain + previous + r'''
static bool irq_enabled;
static unsigned irq_saves,irq_restores,drain_routes;
static bool pool_held;
void pool_cpu_lock(uint32_t cpu){assert(!cpu&&!irq_enabled&&!pool_held);pool_held=true;}
void pool_cpu_unlock(uint32_t cpu){assert(!cpu&&!irq_enabled&&pool_held);pool_held=false;}
static uint64_t test_irq_save(void) {
    uint64_t flags=irq_enabled?1ULL<<9:0;
    ++irq_saves;irq_enabled=false;return flags;
}
static void test_irq_restore(uint64_t flags) {
    assert(!irq_enabled);++irq_restores;irq_enabled=(flags&(1ULL<<9))!=0;
}
static void pool_drain_one(uint32_t cpu,uint8_t zone,uint8_t order) {
    assert(cpu==0&&!irq_enabled&&pool_held&&zone<PAGE_ZONE_COUNT&&order<=PAGE_POOL_SMALL_MAX_ORDER);
    unsigned bit=zone*(PAGE_POOL_SMALL_MAX_ORDER+1)+order;
    assert(!(drain_routes&(1U<<bit)));drain_routes|=1U<<bit;
}
#define page_pool_irq_save test_irq_save
#define page_pool_irq_restore test_irq_restore
''' + cpu_drain + r'''
static uint64_t random64(uint64_t *seed) {
    *seed=*seed*6364136223846793005ULL+1;return *seed;
}
static page_local_pool_t setup(unsigned trial) {
    uint64_t seed=trial+1;
    memset(groups,0,sizeof(groups));queries=expected_queries=0;
    atomic_store(&g_remote_heads[0].head[0][0],PAGE_POOL_GROUP_NONE);
    page_local_pool_t pool={.base=trial%3?&base:0,
        .partial_head=PAGE_POOL_GROUP_NONE,.free_mask=random64(&seed)};
    physical=trial%11==0?UINT64_MAX:(uint64_t)(trial%34)*64*PAGE_SIZE;
    for(unsigned i=0;i<32;++i) {
        groups[i].base_pfn=(uint64_t)i*64;
        groups[i].partial_next=groups[i].remote_next=PAGE_POOL_GROUP_NONE;
        uint64_t bits=trial%7==0?0:random64(&seed);
        if(trial%5==0&&i%2==0)bits=0;
        groups[i].free_mask=i%3?random64(&seed):0;
        if(groups[i].free_mask)pool_partial_push(&pool,i,&groups[i]);
        atomic_store(&groups[i].remote_mask,bits);
        if(trial%13!=0) {
            pool_remote_notify(0,0,0,i,&groups[i]);
            if(bits&&pool.base)++expected_queries;
        }
    }
    return pool;
}
static void *producer(void *argument) {
    unsigned worker=(unsigned)(uintptr_t)argument;
    for(unsigned i=worker;i<32;i+=4) {
        atomic_store(&groups[i].remote_mask,1ULL<<i);
        pool_remote_notify(0,0,0,i,&groups[i]);
    }
    return 0;
}
int main(void) {
    for(unsigned state=0;state<2;++state) {
        irq_enabled=state;irq_saves=irq_restores=drain_routes=0;
        pool_drain_cpu(0);
        assert(irq_enabled==(bool)state&&irq_saves==1&&irq_restores==1);
        assert(drain_routes==((1U<<(PAGE_ZONE_COUNT*(PAGE_POOL_SMALL_MAX_ORDER+1)))-1));
        pool_drain_cpu(MAX_CPUS);
        assert(irq_enabled==(bool)state&&irq_saves==1&&irq_restores==1);
    }
    unsigned old_queries=0,new_queries=0;
    for(unsigned trial=0;trial<20000;++trial) {
        page_local_pool_t old=setup(trial);
        previous_drain(0,0,0,&old);
        assert(queries==expected_queries);old_queries+=queries;
        page_pool_group_t saved[32];memcpy(saved,groups,sizeof(saved));
        unsigned expected=expected_queries?1:0;
        page_local_pool_t actual=setup(trial);
        pool_drain_remote(0,0,0,&actual);
        assert(queries==expected);new_queries+=queries;
        assert(actual.base==old.base&&actual.free_mask==old.free_mask);
        assert(actual.partial_head==old.partial_head);
        assert(!memcmp(saved,groups,sizeof(saved)));
        assert(atomic_load(&g_remote_heads[0].head[0][0])==PAGE_POOL_GROUP_NONE);
        queries=0;pool_drain_remote(0,0,0,&actual);assert(!queries);
    }
    for(unsigned round=0;round<1000;++round) {
        memset(groups,0,sizeof(groups));physical=0;queries=0;
        for(unsigned i=0;i<32;++i)groups[i].base_pfn=(uint64_t)i*64;
        atomic_store(&g_remote_heads[0].head[0][0],PAGE_POOL_GROUP_NONE);
        page_local_pool_t pool={.base=&base,.partial_head=PAGE_POOL_GROUP_NONE};
        pthread_t workers[4];
        for(unsigned i=0;i<4;++i)assert(!pthread_create(&workers[i],0,producer,(void *)(uintptr_t)i));
        for(unsigned i=0;i<4;++i)assert(!pthread_join(workers[i],0));
        pool_drain_remote(0,0,0,&pool);assert(queries==1&&pool.free_mask==1);
        uint32_t id=pool.partial_head;unsigned seen=0,count=0;
        while(id!=PAGE_POOL_GROUP_NONE) {
            assert(id>0&&id<32&&!(seen&(1U<<id)));
            seen|=1U<<id;++count;id=groups[id].partial_next;
        }
        assert(count==31&&seen==0xfffffffeU);
        for(unsigned i=0;i<32;++i) {
            assert(!atomic_load(&groups[i].remote_mask));
            assert(groups[i].free_mask==(i?1ULL<<i:0));
        }
    }
    assert(old_queries>new_queries);
    puts("CPU drain: every zone/order executes with IRQs masked; enabled/disabled entry state restored, invalid CPU has no side effects");
    printf("Remote drain: 20000 exact queue/mask/partial-list oracle states; base queries %u -> %u; empty/zero-mask drains query nothing; 1000 four-producer list publications preserve all 32 groups\n",old_queries,new_queries);
}
'''
with tempfile.TemporaryDirectory(prefix='liteos-pool-remote-') as directory:
    binary = str(Path(directory) / 'test')
    subprocess.run(['cc', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-fno-pie', '-no-pie', '-pthread',
                    '-I' + str(root / 'include'), '-x', 'c', '-', '-o', binary],
                   input=program, text=True, check=True)
    subprocess.run([binary], check=True, timeout=60)
