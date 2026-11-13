"""Actual syscall timer: retain separate maxima for each leader and syscall."""
from pathlib import Path
import subprocess
import tempfile
root = Path(__file__).resolve().parents[2]
source = (root / 'kernel/syscall/handlers.c').read_text()
begin = source.index('typedef struct syscall_delay_diagnostic')
end = source.index('\n}', source.index('static void syscall_record_delay(', begin)) + 2
production = source[begin:source.index('typedef struct syscall_cost_diagnostic', begin)]
production += source[source.index('static bool syscall_measure_linux_delay', begin):end]
program = r'''
#include <assert.h>
#include <stdint.h>
#include <stddef.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdbool.h>
typedef struct { uint64_t pid, linux_thread_group_leader_tid; } process_t;
typedef struct { uint64_t tid; process_t *process; } thread_t;
static struct { uint64_t tsc_hz; } x86_boot_cpu_features={1000000000ULL};
static uint64_t x86_tsc_to_ns(uint64_t ticks) { return ticks; }
''' + production + r'''
int main(void) {
    process_t browser={125,126}, renderer={174,175};
    thread_t main_thread={126,&browser}, worker={148,&browser};
    thread_t other_leader={175,&renderer}, other_worker={176,&renderer};
    assert(!syscall_measure_linux_delay(0));
    assert(syscall_measure_linux_delay(&main_thread));
    assert(!syscall_measure_linux_delay(&worker));
    assert(syscall_measure_linux_delay(&other_leader));
    g_syscall_delay_process=125;
    assert(syscall_measure_linux_delay(&main_thread));
    assert(syscall_measure_linux_delay(&worker));
    assert(!syscall_measure_linux_delay(&other_leader));
    assert(!syscall_measure_linux_delay(&other_worker));
    g_syscall_delay_process=0;
    syscall_record_delay(125,126,202,0x4000,128,-11,9999999);
    assert(!g_syscall_delays[0].tid);
    syscall_record_delay(125,126,202,0x4000,128,-11,10000000);
    assert(g_syscall_delays[0].tid==126 && g_syscall_delays[0].duration_ns==10000000);
    syscall_record_delay(125,126,202,0x8000,128,0,300000000);
    assert(g_syscall_delays[0].argument0==0x8000 && g_syscall_delays[0].result==0);
    syscall_record_delay(125,126,202,0xc000,128,-110,200000000);
    assert(g_syscall_delays[0].duration_ns==300000000);
    syscall_record_delay(125,126,232,0x1234,0,0,60000000000ULL);
    assert(g_syscall_delays[1].number==232 && g_syscall_delays[0].number==202);
    syscall_record_delay(174,175,202,0x4000,128,0,200000000);
    assert(g_syscall_delays[2].tid==175 && g_syscall_delays[0].tid==126);
    /* 原生 fork 绝不能与 Linux lstat 的系统调用编号 6 重叠。 */
    syscall_record_delay(125,148,(1ULL<<63)|6,0,0,380,285000000);
    syscall_record_delay(125,148,6,0,0,-2,11000000);
    assert(g_syscall_delays[3].number==((1ULL<<63)|6));
    assert(g_syscall_delays[4].number==6);
    syscall_record_delay(249,250,(1ULL<<63)|4,0x1234000,0,0,230000000);
    assert(g_syscall_delays[5].number==((1ULL<<63)|4));
    assert(g_syscall_delays[5].duration_ns==230000000);
    assert(!atomic_flag_test_and_set(&g_syscall_delay_guard));
    syscall_record_delay(125,126,202,0x4000,128,0,400000000);
    assert(g_syscall_delays[0].duration_ns==300000000);
    assert(atomic_flag_test_and_set(&g_syscall_delay_guard));
    atomic_flag_clear(&g_syscall_delay_guard);
    for (unsigned i=6;i<64;++i) syscall_record_delay(i,i,i,0,0,0,10000000);
    syscall_record_delay(999,999,1,0,0,0,60000000000ULL);
    assert(g_syscall_delays[63].tid==63);
    x86_boot_cpu_features.tsc_hz=0;
    syscall_record_delay(125,126,202,0x4000,128,0,400000000);
    assert(g_syscall_delays[0].duration_ns==300000000);
    puts("syscall delays: selected-process isolation, leader fallback, threshold, per-thread/per-call maxima, idle separation, bounded capacity and busy guard passed");
}
'''
with tempfile.TemporaryDirectory(prefix='liteos-syscall-delay-') as directory:
    path = Path(directory)
    (path / 'test.c').write_text(program)
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror',
                    '-fsanitize=address,undefined','-fno-pie','-no-pie',
                    str(path/'test.c'),'-o',str(path/'test')],check=True)
    subprocess.run([str(path/'test')],check=True)
