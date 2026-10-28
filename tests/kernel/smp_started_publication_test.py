"""Schedule the BSP exactly at the real AP's STARTED publication."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'kernel/sched/smp.c').read_text()
step = source.index('SMP_AP_STEP_STARTED, memory_order_release);')
start = source.rfind('    if (valid) {', 0, step)
end = source.index('    if (valid) {', step)
publication = source[start:end]
wait = source[source.index('static bool wait_for_ap('):
              source.index('static uint32_t current_legacy_apic_id(')]
prefix = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdatomic.h>
enum {AP_STATE_OFFLINE, AP_STATE_BOOTING, AP_STATE_STARTED, AP_STATE_ONLINE, AP_STATE_FAILED};
enum {SMP_AP_STEP_STARTED=108};
static atomic_uint g_ap_states[16],g_ap_boot_steps[16],g_started_count;
static uint64_t clock_ticks;
static unsigned expected_count;
static bool observing;
static uint64_t x86_read_tsc(void) {return clock_ticks++;}
static uint64_t x86_timeout_ns_to_tsc(uint64_t n) {return n;}
''' + wait + r'''
/* Interrupt the AP immediately after its release-store, before its next
 * instruction. The acquire-reader must already see all startup metadata. */
static void publish_store(atomic_uint *p,unsigned value,memory_order order) {
 atomic_store_explicit(p,value,order);
 if(!observing)return;
 for(unsigned cpu=1;cpu<16;++cpu) {
  if(p!=&g_ap_states[cpu])continue;
  if(value==AP_STATE_STARTED) {
   assert(wait_for_ap(cpu,10));
   assert(atomic_load_explicit(&g_started_count,memory_order_acquire)==expected_count);
   assert(atomic_load_explicit(&g_ap_boot_steps[cpu],memory_order_relaxed)==SMP_AP_STEP_STARTED);
  } else {
   assert(value==AP_STATE_FAILED && !wait_for_ap(cpu,10));
   assert(atomic_load_explicit(&g_started_count,memory_order_acquire)==expected_count);
  }
 }
}
#undef atomic_store_explicit
#define atomic_store_explicit publish_store
static void ap_publish(unsigned cpu_index,bool valid) {
 observing=true;
'''
suffix = r'''
 observing=false;
}
int main(void) {
 for(unsigned total=2;total<=16;++total) {
  atomic_init(&g_started_count,1);
  for(unsigned cpu=1;cpu<total;++cpu) {
   atomic_init(&g_ap_states[cpu],AP_STATE_BOOTING);
   atomic_init(&g_ap_boot_steps[cpu],107);
   expected_count=cpu+1;ap_publish(cpu,true);
  }
  assert(atomic_load(&g_started_count)==total);
 }
 atomic_init(&g_started_count,1);expected_count=1;
 atomic_init(&g_ap_states[1],AP_STATE_BOOTING);
 atomic_init(&g_ap_boot_steps[1],107);ap_publish(1,false);
 assert(atomic_load(&g_ap_boot_steps[1])==107);
}
'''
store = '''    atomic_store_explicit(&g_ap_states[cpu_index],
                          valid ? AP_STATE_STARTED : AP_STATE_FAILED,
                          memory_order_release);'''
assert publication.count(store) == 1
variants = [('production', publication),
            ('state before count', store + '\n' + publication.replace(store, ''))]
with tempfile.TemporaryDirectory(prefix='smp-started-') as directory:
    binary = str(Path(directory) / 'test')
    for label, block in variants:
        subprocess.run(['cc', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                        '-x', 'c', '-', '-o', binary], input=prefix+block+suffix,
                       text=True, check=True)
        result = subprocess.run([binary], capture_output=True, timeout=5)
        assert (result.returncode == 0) == (label == 'production'), (label,result.stderr)
print('SMP AP publication: 2..16 CPUs, BSP observes count/step at STARTED, failed AP excluded; old ordering rejected')
