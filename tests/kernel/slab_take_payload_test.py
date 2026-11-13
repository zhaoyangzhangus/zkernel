"""Actual shared slab-slot removal and both callers against old routes."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'kernel/mm/kmalloc.c').read_text()
layout = source[source.index('#define KMEM_MAGIC'):source.index('static void bytes_fill(')]
partial = source[source.index('static slab_page_t *cache_partial_head('):source.index('/* All slots being globally free')]
take = source[source.index('static void *cache_take_payload_locked('):source.index('static bool magazine_refill(')]
refill = source[source.index('static bool magazine_refill('):source.index('static void magazine_flush(')]
single = source[source.index('static void *allocate_from_cache('):source.index('static void *allocate_large(')]
program = r'''
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#define MAX_CPUS 1U
#define CACHELINE_SIZE 64U
#define PAGE_SIZE 4096U
typedef struct {unsigned owner;} page_t;
typedef struct {atomic_uint state;} spinlock_t;
''' + layout + r'''
static slab_page_t slabs[32];
static void *slots[32][42];
static unsigned used,created,requests,limit,errors,refills,activations,locks;
static bool held;
static uint16_t cls;
static size_t requested;
static uint32_t flags_now;
static bool zero_now;
static void record_error(void) {assert(held);++errors;}
static void cache_lock(kmem_cache_t *cache) {assert(cache==&g_caches[cls]&&!held);held=true;++locks;}
static void cache_unlock(kmem_cache_t *cache) {assert(cache==&g_caches[cls]&&held);held=false;}
static void record_fastpath_refill(void) {assert(!held);++refills;}
static slab_page_t *new_slab(unsigned count) {
    assert(used<32&&count<=42);unsigned index=used++;
    slab_page_t *slab=&slabs[index];slab->magic=KMEM_SLAB_MAGIC;
    slab->slot_count=slab->free_count=count;slab->class_index=cls;
    for(unsigned i=0;i<count;++i) {
        slots[index][i]=slab->free_list;slab->free_list=&slots[index][i];
    }
    return slab;
}
static slab_page_t *create_slab(uint16_t class_index) {
    assert(held&&class_index==cls);++requests;
    if(created==limit)return 0;
    ++created;return new_slab((PAGE_SIZE-KMEM_SLAB_HEADER_SIZE)/g_slot_sizes[cls]);
}
static void *activate_payload(uint16_t class_index,void *payload,size_t size,uint32_t flags,bool zero) {
    assert(!held&&payload&&class_index==cls&&size==requested&&flags==flags_now&&zero==zero_now);
    ++activations;return payload;
}
''' + partial + take + refill + single + r'''
/* 保留原有的重复算法，作为独立于辅助函数的对照。 */
static bool previous_refill(uint16_t class_index,kmem_magazine_t *magazine) {
    if(!magazine||class_index>=KMEM_CLASS_COUNT)return false;
    kmem_cache_t *cache=&g_caches[class_index];unsigned moved=0;cache_lock(cache);
    while(moved<KMEM_MAGAZINE_CAPACITY) {
        slab_page_t *slab=cache_partial_head(cache);
        if(!slab) {
            slab=create_slab(class_index);if(!slab)break;
            cache_partial_insert(cache,slab);
        }
        void *payload=slab->free_list;slab->free_list=*(void **)payload;--slab->free_count;
        if(!slab->free_list)cache_partial_remove_head(cache,slab);
        *(void **)payload=magazine->head;magazine->head=payload;++magazine->count;++moved;
    }
    cache_unlock(cache);if(moved)record_fastpath_refill();return moved!=0;
}
static void *previous_single(uint16_t class_index,size_t size,uint32_t flags,bool zero) {
    kmem_cache_t *cache=&g_caches[class_index];cache_lock(cache);
    slab_page_t *slab=cache_partial_head(cache);
    if(!slab) {
        slab=create_slab(class_index);
        if(!slab) {cache_unlock(cache);return 0;}
        cache_partial_insert(cache,slab);
    }
    void *payload=slab->free_list;slab->free_list=*(void **)payload;--slab->free_count;
    if(!slab->free_list)cache_partial_remove_head(cache,slab);
    cache_unlock(cache);return activate_payload(class_index,payload,size,flags,zero);
}
typedef struct {
    slab_page_t slabs[32];void *slots[32][42];kmem_cache_t caches[KMEM_CLASS_COUNT];
    void *head,*results[20];unsigned count,used,created,requests,errors,refills,activations,locks;
} snapshot_t;
static void setup(unsigned trial) {
    memset(slabs,0,sizeof(slabs));memset(slots,0,sizeof(slots));memset(g_caches,0,sizeof(g_caches));
    used=created=requests=errors=refills=activations=locks=0;held=false;
    for(unsigned i=0;i<trial%4;++i) {
        slab_page_t *slab=new_slab(1+(trial+i)%7);
        cache_partial_insert(&g_caches[cls],slab);
    }
    if(trial%3==0) {
        /* 历史上格式错误的满载头部：保留诊断性剪枝行为。 */
        slab_page_t *slab=new_slab(0);slab->next=g_caches[cls].slabs;
        slab->prev_link=&g_caches[cls].slabs;
        if(slab->next)slab->next->prev_link=&slab->next;
        g_caches[cls].slabs=slab;
    }
}
static void run(unsigned trial,bool previous,snapshot_t *out) {
    setup(trial);memset(out,0,sizeof(*out));kmem_magazine_t magazine={0};
    bool success=previous?previous_refill(cls,&magazine):magazine_refill(cls,&magazine);
    assert(success==(magazine.count!=0)&&magazine.count<=KMEM_MAGAZINE_CAPACITY);
    unsigned seen=0;
    for(void *payload=magazine.head;payload;payload=*(void **)payload)assert(++seen<=magazine.count);
    assert(seen==magazine.count);
    for(unsigned i=0;i<20;++i)out->results[i]=previous?
        previous_single(cls,requested,flags_now,zero_now):allocate_from_cache(cls,requested,flags_now,zero_now);
    slab_page_t **link=&g_caches[cls].slabs;
    while(*link) {
        assert((*link)->prev_link==link&&(*link)->free_list&&(*link)->free_count);
        link=&(*link)->next;
    }
    assert(!held);out->head=magazine.head;out->count=magazine.count;
    memcpy(out->slabs,slabs,sizeof(slabs));memcpy(out->slots,slots,sizeof(slots));
    memcpy(out->caches,g_caches,sizeof(g_caches));
    out->used=used;out->created=created;out->requests=requests;out->errors=errors;
    out->refills=refills;out->activations=activations;out->locks=locks;
}
int main(void) {
    unsigned cases=0;
    for(cls=0;cls<KMEM_CLASS_COUNT;++cls)
    for(limit=0;limit<5;++limit)
    for(unsigned trial=0;trial<100;++trial) {
        requested=1+trial%((unsigned)g_slot_sizes[cls]-KMEM_PAYLOAD_OFFSET-KMEM_TRAILER_SIZE);
        flags_now=trial;zero_now=trial%2;
        snapshot_t old,actual;run(trial,true,&old);run(trial,false,&actual);
        assert(!memcmp(&old,&actual,sizeof(old)));++cases;
    }
    cls=0;setup(1);kmem_magazine_t magazine={0};
    assert(!magazine_refill(cls,0));assert(!magazine_refill(KMEM_CLASS_COUNT,&magazine));
    assert(!locks&&!requests);
    printf("Slab shared take: %u exact old/new magazine+direct routes across all classes, partial/full heads, OOM and partial refills; identical objects/order/free counts/backlinks/diagnostics/lock scope and activation arguments\n",cases);
}
'''
with tempfile.TemporaryDirectory(prefix='liteos-slab-take-') as directory:
    binary = str(Path(directory) / 'test')
    subprocess.run(['cc', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                    '-Wno-unused-variable', '-fsanitize=address,undefined',
                    '-fno-pie', '-no-pie', '-x', 'c', '-', '-o', binary],
                   input=program, text=True, check=True)
    subprocess.run([binary], check=True, timeout=60)
