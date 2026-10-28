"""Actual cache-only allocator: no waiting, initialization, pages or magazines."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'kernel/mm/kmalloc.c').read_text()
layout = source[source.index('#define KMEM_MAGIC'):source.index('static void bytes_fill(')]
class_helpers = source[source.index('static uint16_t class_index_for_size('):
                       source.index('static kmem_magazine_t *current_magazine(')]
partial_head = source[source.index('static slab_page_t *cache_partial_head('):
                      source.index('static void cache_partial_insert(')]
remove_head = source[source.index('static void cache_partial_remove_head('):
                     source.index('static void cache_release_empty(')]
allocate = source[source.index('void *kmalloc_try_cached('):source.index('void *kzalloc(')]
code = r'''
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#define MAX_CPUS 4U
#define CACHELINE_SIZE 64U
typedef struct {unsigned owner;} page_t;
typedef struct {atomic_uint state;} spinlock_t;
''' + layout + r'''
static unsigned tries,unlocks,activations,errors;
static bool spinlock_try_lock(spinlock_t *lock) {
    ++tries;unsigned expected=0;
    return atomic_compare_exchange_strong(&lock->state,&expected,1);
}
static void spinlock_unlock(spinlock_t *lock) {assert(atomic_load(&lock->state)==1);atomic_store(&lock->state,0);++unlocks;}
static void record_error(void){++errors;}
static void *activate_payload(uint16_t index,void *payload,size_t n,uint32_t flags,bool zero) {
    assert(index<KMEM_CLASS_COUNT&&!atomic_load(&g_caches[index].lock.state)&&!zero);
    if(!payload)return NULL;
    allocation_header_t *h=(void *)((uint8_t *)payload-KMEM_PAYLOAD_OFFSET);
    assert(h->state==KMEM_STATE_FREE&&h->class_index==index&&n<=g_slot_sizes[index]-KMEM_PAYLOAD_OFFSET-KMEM_TRAILER_SIZE);
    h->state=KMEM_STATE_ALLOCATED;h->requested=n;h->flags=flags;++activations;return payload;
}
''' + class_helpers + partial_head + remove_head + allocate + r'''
int main(void) {
    assert(!kmalloc_try_cached(0,0)&&!tries);
    atomic_store(&g_init_state,1);
    assert(!kmalloc_try_cached(32,0)&&!tries); /* interrupted initializer cannot spin */
    atomic_store(&g_init_state,2);
    assert(!kmalloc_try_cached(SIZE_MAX,0)&&!tries);
    for(unsigned cls=0;cls<KMEM_CLASS_COUNT;++cls) {
        size_t n=class_capacity(cls);
        assert(class_index_for_size(n)==cls);
        kmem_cache_t *cache=&g_caches[cls];
        atomic_store(&cache->lock.state,1);
        unsigned before=tries;
        assert(!kmalloc_try_cached(n,7)&&tries==before+1&&atomic_load(&cache->lock.state)==1);
        atomic_store(&cache->lock.state,0);
        assert(!kmalloc_try_cached(n,7)&&!atomic_load(&cache->lock.state));
        slab_page_t slab={.magic=KMEM_SLAB_MAGIC,.free_count=2,.slot_count=2,.class_index=cls};
        uint8_t *slots=calloc(2,g_slot_sizes[cls]);assert(slots);
        void *first=NULL,*second=NULL;
        for(unsigned slot=0;slot<2;++slot) {
            allocation_header_t *h=(void *)(slots+slot*g_slot_sizes[cls]);
            h->class_index=cls;h->state=KMEM_STATE_FREE;
            void *p=header_payload(h);assert(payload_header(p)==h);
            if(!slot)first=p;else second=p;
        }
        *(void **)first=second;*(void **)second=NULL;
        cache->slabs=&slab;slab.free_list=first;slab.prev_link=&cache->slabs;
        assert(kmalloc_try_cached(n,7)==first&&slab.free_count==1&&cache->slabs==&slab);
        assert(kmalloc_try_cached(n,9)==second&&slab.free_count==0&&!cache->slabs&&!slab.prev_link);
        assert(!kmalloc_try_cached(n,0));
        free(slots);
    }
    assert(activations==2*KMEM_CLASS_COUNT&&!errors);
    assert(unlocks+KMEM_CLASS_COUNT==tries); /* only contention misses skip unlock */
    for(unsigned cpu=0;cpu<MAX_CPUS;++cpu)
        for(unsigned cls=0;cls<KMEM_CLASS_COUNT;++cls)
            assert(!g_magazines[cpu].classes[cls].head&&!g_magazines[cpu].classes[cls].count);
}
'''
with tempfile.TemporaryDirectory(prefix='kmalloc-try-cached-') as directory:
    base = Path(directory)
    (base / 'test.c').write_text(code)
    subprocess.run(['cc', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                    '-Wno-unused-variable', '-fsanitize=address,undefined',
                    '-fno-pie', '-no-pie', str(base / 'test.c'), '-o', str(base / 'test')], check=True)
    subprocess.run([str(base / 'test')], check=True, timeout=15)
print('Cache-only allocation: interrupted initializer/owner fail without spinning, all size classes, empty cache, slot/list ownership and no magazine mutation passed')
