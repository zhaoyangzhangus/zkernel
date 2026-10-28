"""Actual sender: completion timing does not change coalescing/retry policy."""
from pathlib import Path
import subprocess
import tempfile

root=Path(__file__).resolve().parents[2]
source=(root/'kernel/sched/smp.c').read_text()
start=source.index('bool x86_smp_request_reschedule(')
end=source.index('bool x86_smp_take_reschedule_request(',start)
core=(root/'kernel/sched/core.c').read_text()
begin=core.index('uint64_t scheduler_trace_reschedule_begin(void) {')
begin_probe=core[begin:core.index('void scheduler_trace_reschedule_send(',begin)]
program=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdatomic.h>
#include <stddef.h>
#define X86_SMP_IPI_VECTOR 0xf0
typedef struct { uint32_t apic_id; } cpu_t;
typedef struct { uint32_t cpu_count; cpu_t cpus[4]; } x86_acpi_platform_t;
static x86_acpi_platform_t platform={4,{{7},{8},{9},{10}}};
static bool available=true, online=true;
static atomic_bool g_reschedule_pending[4];
static uint64_t now=100, start_seen, elapsed_seen;
static unsigned calls, complete_calls, succeed_at, attempts_seen, requests;
static bool accepted_seen, requested_send;
static volatile uint64_t g_sched_wake_trace_tid=1;
static unsigned clock_reads;
static const x86_acpi_platform_t *x86_acpi_platform(void) { return available ? &platform : NULL; }
static bool x86_smp_cpu_online(uint32_t cpu) { assert(cpu<4);return online; }
static uint64_t x86_read_tsc(void) { ++clock_reads;return now; }
''' + begin_probe + r'''
static void scheduler_trace_reschedule_request(uint32_t cpu,bool send) {
    assert(cpu==2);++requests;requested_send=send;
}
static bool lapic_send_fixed(uint32_t apic,uint8_t vector) {
    assert(apic==9 && vector==0xf0 && atomic_load(&g_reschedule_pending[2]));
    ++calls;now+=10000;return calls==succeed_at;
}
static void scheduler_trace_reschedule_send(uint32_t cpu,uint64_t started,
                                           bool accepted,uint32_t attempts) {
    assert(cpu==2);++complete_calls;start_seen=started;elapsed_seen=now-started;
    accepted_seen=accepted;attempts_seen=attempts;
}
''' + source[start:end] + r'''
int main(void) {
    available=false;assert(!x86_smp_request_reschedule(2) && !calls && !complete_calls);
    available=true;online=false;assert(!x86_smp_request_reschedule(2));online=true;
    assert(!x86_smp_request_reschedule(4));
    succeed_at=2;assert(x86_smp_request_reschedule(2));
    assert(calls==2 && complete_calls==1 && requests==1 && requested_send);
    assert(start_seen==100 && elapsed_seen==20000 && accepted_seen && attempts_seen==2);
    assert(x86_smp_request_reschedule(2));
    assert(calls==2 && complete_calls==1 && requests==2 && !requested_send);
    atomic_store(&g_reschedule_pending[2],false);calls=0;succeed_at=0;
    assert(!x86_smp_request_reschedule(2));
    assert(calls==4 && complete_calls==2 && !accepted_seen && attempts_seen==4);
    assert(elapsed_seen==40000 && atomic_load(&g_reschedule_pending[2]));
    assert(x86_smp_request_reschedule(2) && calls==4 && complete_calls==2);
    /* Target fallback consumes the retained request; a new wake can send again. */
    atomic_store(&g_reschedule_pending[2],false);calls=0;succeed_at=1;
    assert(x86_smp_request_reschedule(2));
    assert(calls==1 && complete_calls==3 && accepted_seen && attempts_seen==1);
    /* The default disabled probe must not read the timestamp counter, even
     * for actual sends and all four failed hardware submission attempts. */
    g_sched_wake_trace_tid=0;
    for(unsigned trial=0;trial<10000;++trial) {
        atomic_store(&g_reschedule_pending[2],false);calls=0;succeed_at=trial%5;
        unsigned before=clock_reads;
        bool result=x86_smp_request_reschedule(2);
        assert(result==(succeed_at!=0));
        assert(clock_reads==before&&start_seen==0);
        before=clock_reads;unsigned sent=calls;
        assert(x86_smp_request_reschedule(2)&&clock_reads==before&&calls==sent);
    }
    g_sched_wake_trace_tid=1;atomic_store(&g_reschedule_pending[2],false);
    calls=0;succeed_at=1;unsigned before=clock_reads;
    assert(x86_smp_request_reschedule(2)&&clock_reads==before+1);
}
'''
with tempfile.TemporaryDirectory(prefix='liteos-reschedule-send-') as directory:
    binary=str(Path(directory)/'test')
    subprocess.run(['cc','-std=gnu11','-Wall','-Wextra','-Werror',
                    '-fsanitize=address,undefined','-fno-pie','-no-pie',
                    '-x','c','-','-o',binary],input=program,text=True,check=True)
    subprocess.run([binary],check=True)
print('IPI send probe: actual completion, retries, coalescing, retained fallback and 10000 disabled-probe sends without timestamp reads passed')
