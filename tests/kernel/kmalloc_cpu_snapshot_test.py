"""Actual allocator CPU snapshot: local cache/stat/progress routing."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'kernel/mm/kmalloc.c').read_text()
layout = source[source.index('#define KMEM_MAGIC'):source.index('static void bytes_fill(')]
helpers = source[source.index('static kmem_magazine_t *current_magazine('):source.index('static void record_fastpath_refill(')]
allocate = source[source.index('static void *kmalloc_internal('):source.index('void *kmalloc(')]
previous = allocate.replace('kmalloc_internal(', 'previous_alloc(')
previous = previous.replace('    sched_preempt_disable();\n', '', 1)
previous = previous.replace('    initialize_allocator();', '    sched_preempt_disable();\n    initialize_allocator();')
previous = previous.replace('current_magazine(i, cpu_index)', 'previous_magazine(i)')
previous = previous.replace('record_fastpath_hit(cpu_index)', 'previous_hit()')
program = r'''
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#define MAX_CPUS 256U
#define CACHELINE_SIZE 64U
typedef struct {unsigned owner;} page_t;
typedef struct {atomic_uint state;} spinlock_t;
''' + layout + r'''
static void *payloads[MAX_CPUS];static unsigned fallback;
static unsigned current_cpu,initial_cpu,reads,depth,initial_depth,mode,selected_class;
static unsigned refill_calls,cache_calls,large_calls,init_calls,activations;
static bool migrate;
static uint32_t x86_current_cpu_index(void) {++reads;return current_cpu;}
static void sched_preempt_disable(void) {
    /* Inject the migration possible immediately before pinning the CPU. */
    if(!depth&&migrate)current_cpu=3;
    ++depth;
}
static void sched_preempt_enable(void) {assert(depth==initial_depth+1);--depth;}
static void initialize_allocator(void) {assert(depth==initial_depth+1);++init_calls;}
static uint16_t class_index_for_size(size_t size) {assert(size);return selected_class;}
static bool magazine_refill(uint16_t cls,kmem_magazine_t *magazine) {
    assert(depth==initial_depth+1&&current_cpu<MAX_CPUS&&cls==selected_class);
    assert(magazine==&g_magazines[current_cpu].classes[cls]);++refill_calls;
    if(mode>=2)return false;
    magazine->head=&payloads[current_cpu];magazine->count=1;return true;
}
static void *activate_payload(uint16_t cls,void *payload,size_t size,uint32_t flags,bool zero) {
    (void)zero;assert(cls==selected_class&&size==7&&flags==19);
    assert(payload==&payloads[current_cpu]&&depth==initial_depth+1);++activations;return payload;
}
static void *allocate_from_cache(uint16_t cls,size_t size,uint32_t flags,bool zero) {
    (void)zero;assert(cls==selected_class&&size==7&&flags==19&&depth==initial_depth+1);
    ++cache_calls;return mode==3?0:&fallback;
}
static void *allocate_large(size_t size,uint32_t flags,bool zero) {
    (void)zero;assert(size==7&&flags==19&&depth==initial_depth);
    ++large_calls;return mode==3?0:&fallback;
}
''' + helpers + r'''
static kmem_magazine_t *previous_magazine(uint16_t cls) {
    if(cls>=KMEM_CLASS_COUNT)return 0;
    unsigned cpu=x86_current_cpu_index();
    return cpu<MAX_CPUS?&g_magazines[cpu].classes[cls]:0;
}
static void previous_hit(void) {record_fastpath_hit(x86_current_cpu_index());}
''' + allocate + previous + r'''
typedef struct {
    kmem_cpu_magazines_t magazines[MAX_CPUS];kmem_cpu_stats_t stats[MAX_CPUS];
    unsigned refill_calls,cache_calls,large_calls,init_calls,activations;
} snapshot_t;
static void reset(void) {
    memset(g_magazines,0,sizeof(g_magazines));memset(g_cpu_stats,0,sizeof(g_cpu_stats));
    memset((void *)g_kmem_progress,0,sizeof(g_kmem_progress));memset(payloads,0,sizeof(payloads));
    current_cpu=initial_cpu;depth=initial_depth;
    reads=refill_calls=cache_calls=large_calls=init_calls=activations=0;
    if(mode==0&&selected_class<KMEM_CLASS_COUNT)
        for(unsigned cpu=0;cpu<MAX_CPUS;++cpu) {
            g_magazines[cpu].classes[selected_class].head=&payloads[cpu];
            g_magazines[cpu].classes[selected_class].count=1;
        }
}
static void snapshot(snapshot_t *out) {
    memset(out,0,sizeof(*out));memcpy(out->magazines,g_magazines,sizeof(g_magazines));
    memcpy(out->stats,g_cpu_stats,sizeof(g_cpu_stats));
    out->refill_calls=refill_calls;out->cache_calls=cache_calls;out->large_calls=large_calls;
    out->init_calls=init_calls;out->activations=activations;
}
int main(void) {
    unsigned cases=0,old_reads=0,new_reads=0,corrected=0;
    static const unsigned cpus[]={0,MAX_CPUS-1,MAX_CPUS,UINT32_MAX};
    for(selected_class=0;selected_class<=KMEM_CLASS_COUNT;++selected_class)
    for(mode=0;mode<4;++mode)
    for(unsigned c=0;c<4;++c)
    for(unsigned migration=0;migration<2;++migration)
    for(initial_depth=0;initial_depth<4;++initial_depth)
    for(unsigned zero=0;zero<2;++zero) {
        initial_cpu=cpus[c];migrate=migration;reset();
        void *expected=previous_alloc(7,19,zero);old_reads+=reads;
        unsigned prior_reads=reads;uint32_t prior_progress[MAX_CPUS];
        memcpy(prior_progress,(const void *)g_kmem_progress,sizeof(prior_progress));
        snapshot_t old,actual;snapshot(&old);
        reset();void *result=kmalloc_internal(7,19,zero);new_reads+=reads;snapshot(&actual);
        assert(result==expected&&!memcmp(&old,&actual,sizeof(old)));
        assert(reads==1&&depth==initial_depth&&init_calls==1);
        unsigned owner=initial_depth==0&&migrate?3:initial_cpu;
        for(unsigned cpu=0;cpu<MAX_CPUS;++cpu)
            assert(g_kmem_progress[cpu]==(cpu==owner?90U:0U));
        if(owner!=initial_cpu) {
            assert(prior_progress[owner]==0);
            if(initial_cpu<MAX_CPUS)assert(prior_progress[initial_cpu]==90);
            ++corrected;
        } else assert(!memcmp(prior_progress,(const void *)g_kmem_progress,sizeof(prior_progress)));
        if(activations)assert(prior_reads==3);
        assert(current_magazine(KMEM_CLASS_COUNT,owner)==0);
        assert(current_magazine(0,MAX_CPUS)==0);
        reset();assert(!kmalloc_internal(0,19,zero)&&reads==0&&depth==initial_depth&&!init_calls);
        ++cases;
    }
    assert(corrected&&old_reads>new_reads);
    printf("Allocator CPU snapshot: %u exact cache/stat/route cases; CPU reads %u -> %u (cached hit 3 -> 1); %u migration-before-pin diagnostics corrected; nested preemption, CPU/class bounds, fallback/zero-size paths preserved\n",cases,old_reads,new_reads,corrected);
}
'''
with tempfile.TemporaryDirectory(prefix='liteos-kmalloc-cpu-') as directory:
    binary = str(Path(directory) / 'test')
    subprocess.run(['cc', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                    '-Wno-unused-variable', '-fsanitize=address,undefined',
                    '-fno-pie', '-no-pie', '-x', 'c', '-', '-o', binary],
                   input=program, text=True, check=True)
    subprocess.run([binary], check=True, timeout=60)
