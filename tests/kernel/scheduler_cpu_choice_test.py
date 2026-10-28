"""Actual CPU choice and common affinity predicate against the old routing."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
header = (root / 'kernel/sched/internal.h').read_text()
start = header.index('static inline bool scheduler_affinity_mask_allows(')
helper = header[start:header.index('static inline uint64_t scheduler_make_queue_snapshot(', start)]
source = (root / 'kernel/sched/balance.c').read_text()
functions = source[source.index('bool scheduler_cpu_available('):]
assert 'while (candidates != 0U)' in functions
functions = functions.replace('while (candidates != 0U) {',
                              'while (candidates != 0U) { ++candidate_visits;')
functions = functions.replace('uint64_t candidates = mask->bits[base >> 6];',
                              '++word_visits; uint64_t candidates = mask->bits[base >> 6];')
assert 'static bool scheduler_affinity_allows' not in source
assert 'static bool scheduler_affinity_allows' not in (root / 'kernel/sched/core.c').read_text()
reference = r'''
static uint32_t previous_choose_cpu(thread_t *thread,uint32_t current_cpu) {
    uint32_t best_cpu=current_cpu,best_load=UINT32_MAX;
    for(uint32_t cpu=0;cpu<g_cpu_count;++cpu) {
        if(!scheduler_cpu_available(cpu)||!scheduler_affinity_allows(thread,cpu))continue;
        uint32_t load=scheduler_snapshot_load(&g_cpus[cpu]);
        if(load<best_load||(load==best_load&&cpu==current_cpu)) {
            best_cpu=cpu;best_load=load;
        }
    }
    return best_cpu;
}
'''
code = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define MAX_CPUS 256
#define SCHED_ENTITY_INITIAL_PLACEMENT 2U
typedef struct {uint64_t bits[MAX_CPUS/64];} cpumask_t;
typedef struct {
    cpumask_t affinity;
    struct {unsigned flags;} sched;
    unsigned owner_cpu;
} thread_t;
typedef struct {struct {void *idle;} queue;unsigned load;} scheduler_cpu_t;
static scheduler_cpu_t g_cpus[MAX_CPUS];
static uint32_t g_cpu_count;
static bool online[MAX_CPUS];
static unsigned availability_calls,load_calls;
static unsigned candidate_visits,word_visits;
static unsigned saturated_corrections;
static bool x86_smp_cpu_online(unsigned id) {
    assert(id<MAX_CPUS);++availability_calls;return online[id];
}
static uint32_t scheduler_snapshot_load(scheduler_cpu_t *cpu) {
    ++load_calls;return cpu->load;
}
'''+helper+functions+reference+r'''
static unsigned check(thread_t *thread,unsigned current) {
    thread_t before;if(thread)before=*thread;
    availability_calls=load_calls=candidate_visits=word_visits=0;
    unsigned actual=scheduler_choose_cpu(thread,current);
    unsigned visits=availability_calls,loads=load_calls;
    assert(candidate_visits==visits);
    assert(word_visits<=(thread?(g_cpu_count+63)/64:0));
    availability_calls=load_calls=0;
    unsigned expected=previous_choose_cpu(thread,current);
    assert(loads<=load_calls&&visits<=availability_calls);
    uint64_t best=UINT64_MAX;unsigned oracle=current;bool found=false;
    for(unsigned cpu=0;cpu<g_cpu_count;++cpu) {
        if(!thread||!(thread->affinity.bits[cpu/64]&(1ULL<<(cpu%64)))||
           !online[cpu]||!g_cpus[cpu].queue.idle)continue;
        uint64_t load=g_cpus[cpu].load;
        if(load<best||(load==best&&cpu==current)) {best=load;oracle=cpu;found=true;}
    }
    assert(actual==oracle);
    if(actual!=expected) {
        assert(found&&best==UINT32_MAX);++saturated_corrections;
    }
    unsigned selected=0xdeadbeef;
    bool selected_ok=scheduler_select_cpu_mask(thread?&thread->affinity:0,current,&selected);
    assert(selected_ok==found&&selected==(found?oracle:0xdeadbeef));
    unsigned allowed=0;
    for(unsigned cpu=0;cpu<g_cpu_count;++cpu)
        if(thread&&(thread->affinity.bits[cpu/64]&(1ULL<<(cpu%64))))++allowed;
    assert(visits<=allowed);
    if(thread)assert(!memcmp(thread,&before,sizeof(before)));
    return visits;
}
int main(void) {
    thread_t thread={0};unsigned cases=0;uint32_t seed=73;
    /* Exhaust all 8-CPU online/allowed combinations and nine current IDs,
     * including equal loads, idle-less queues and no legal destination. */
    g_cpu_count=8;
    for(unsigned mask=0;mask<256;++mask)
    for(unsigned on=0;on<256;++on)
    for(unsigned current=0;current<=8;++current) {
        thread.affinity.bits[0]=mask;
        for(unsigned i=0;i<8;++i) {
            online[i]=(on&(1U<<i))!=0;
            g_cpus[i].queue.idle=(i+mask)%5 ? &thread : 0;
            g_cpus[i].load=(on%3==0)?UINT32_MAX:(on%3==1)?7:(i+on)%4;
        }
        check(&thread,current);++cases;
    }
    /* Cross-word masks and every discovered count, poisoned out-of-range
     * affinity bits, invalid current IDs and NULL thread fallback. */
    for(unsigned trial=0;trial<50000;++trial) {
        seed=seed*1664525U+1013904223U;g_cpu_count=seed%(MAX_CPUS+1);
        for(unsigned i=0;i<MAX_CPUS/64;++i) {
            seed=seed*1664525U+1013904223U;
            thread.affinity.bits[i]=(uint64_t)seed<<32;
            seed=seed*1664525U+1013904223U;thread.affinity.bits[i]|=seed;
        }
        for(unsigned i=0;i<MAX_CPUS;++i) {
            seed=seed*1664525U+1013904223U;
            online[i]=(seed&7)!=0;g_cpus[i].queue.idle=(seed&15)?&thread:0;
            g_cpus[i].load=trial%7?seed%13:UINT32_MAX;
        }
        check(&thread,trial%(MAX_CPUS+3));check(0,trial);cases+=2;
    }
    g_cpu_count=MAX_CPUS;
    for(unsigned cpu=0;cpu<MAX_CPUS;++cpu) {
        memset(thread.affinity.bits,0,sizeof(thread.affinity.bits));
        thread.affinity.bits[cpu/64]=1ULL<<(cpu%64);
        online[cpu]=1;g_cpus[cpu].queue.idle=&thread;g_cpus[cpu].load=0;
        assert(check(&thread,0)==1);++cases;
        thread.sched.flags=SCHED_ENTITY_INITIAL_PLACEMENT;
        assert(scheduler_choose_start_cpu(&thread,0)==cpu);
        thread.sched.flags=0;thread.owner_cpu=cpu;
        assert(scheduler_choose_start_cpu(&thread,0)==cpu);
        online[cpu]=0;assert(scheduler_choose_start_cpu(&thread,MAX_CPUS+1)==MAX_CPUS+1);
    }
    assert(!scheduler_affinity_allows(0,0));
    assert(!scheduler_affinity_allows(&thread,MAX_CPUS));
    assert(!scheduler_affinity_allows(&thread,UINT32_MAX));
    cpumask_t empty={0};unsigned out=17;availability_calls=0;
    assert(!scheduler_select_cpu_mask(0,0,&out)&&out==17&&!availability_calls);
    assert(!scheduler_select_cpu_mask(&empty,0,0)&&!availability_calls);
    assert(!scheduler_select_cpu_mask(&empty,0,&out)&&out==17&&!availability_calls);
    /* Corrupt/discovery-overflow counts must not read past the fixed mask or
     * CPU array. Test the selector directly: the old oracle is unbounded. */
    const unsigned oversized_counts[]={MAX_CPUS+1U,MAX_CPUS+64U,UINT32_MAX};
    for(unsigned i=0;i<sizeof(oversized_counts)/sizeof(oversized_counts[0]);++i) {
        g_cpu_count=oversized_counts[i];
        memset(thread.affinity.bits,0xff,sizeof(thread.affinity.bits));
        for(unsigned cpu=0;cpu<MAX_CPUS;++cpu) {
            online[cpu]=1;g_cpus[cpu].queue.idle=&thread;g_cpus[cpu].load=7;
        }
        availability_calls=load_calls=candidate_visits=word_visits=0;
        assert(scheduler_select_cpu_mask(&thread.affinity,MAX_CPUS-1U,&out));
        assert(out==MAX_CPUS-1U&&candidate_visits==MAX_CPUS);
        assert(availability_calls==MAX_CPUS&&load_calls==MAX_CPUS&&word_visits==4);
        out=17;
        assert(!scheduler_select_cpu_mask(&empty,0,&out)&&out==17);
    }
    g_cpu_count=MAX_CPUS;
    memset(thread.affinity.bits,0xff,sizeof(thread.affinity.bits));
    for(unsigned cpu=0;cpu<MAX_CPUS;++cpu) {
        online[cpu]=1;g_cpus[cpu].queue.idle=&thread;g_cpus[cpu].load=0;
    }
    assert(check(&thread,0)==1);
    assert(check(&thread,MAX_CPUS-1)==MAX_CPUS);
    puts("Idle local CPU selection: 256 eligible CPUs -> 1 load/candidate read on CPU0; equal-load local preference preserved");
    puts("Oversized CPU counts: 257, 320 and UINT32_MAX bounded to four mask words/256 CPUs; empty output preserved");
    assert(saturated_corrections>0);
    printf("CPU choice: %u independent minimum-load/eligibility cases; %u saturated-load corrections retained; singleton masks visit 4 words/1 candidate instead of 256 CPUs; exact sparse/dense/partial-word traversal, ties/start/immutability passed\n",cases,saturated_corrections);
}
'''
with tempfile.TemporaryDirectory(prefix='liteos-cpu-choice-') as directory:
    binary = str(Path(directory) / 'test')
    subprocess.run(['cc', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
                    '-x', 'c', '-', '-o', binary], input=code, text=True, check=True)
    subprocess.run([binary], check=True, timeout=60)
