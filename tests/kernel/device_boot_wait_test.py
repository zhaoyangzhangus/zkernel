"""Run the actual boot wait decision with CPU instructions modeled as events."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root/'kernel/init/userspace.c').read_text()
start = source.index('static void userspace_device_init_poll(')
body = source[start:source.index('\n}', start)+2]
instructions = {
    '__asm__ volatile ("pushfq; popq %0; cli" : "=r"(flags) : : "memory");':
        'flags = irq_save();',
    '__asm__ volatile ("sti" : : : "memory");': 'irq_enable();',
    '__asm__ volatile ("sti; hlt" : : : "memory");': 'irq_enable_and_halt();',
    '__asm__ volatile ("cli" : : : "memory");': 'irq_disable();',
}
for instruction, replacement in instructions.items():
    assert instruction in body
    body = body.replace(instruction, replacement)
assert '__asm__' not in body
program = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
static bool interrupts, pending, ready, arrival;
static unsigned scheduled, halted, checks;
static uint64_t irq_save(void) {
 uint64_t saved=interrupts?512:0; interrupts=false; return saved;
}
static void irq_enable(void) { interrupts=true; }
static void irq_disable(void) { interrupts=false; }
static bool x86_smp_take_reschedule_request(void) {
 assert(!interrupts); ++checks;
 bool result=pending; pending=false; return result;
}
static unsigned sched_runnable_count(void) { assert(!interrupts); return ready; }
static int sched_try_run_ready(void) {
 assert(interrupts && (ready || checks)); ++scheduled; return 1;
}
static void irq_enable_and_halt(void) {
 assert(!interrupts && !ready);
 /* A wake arriving after the CLI-protected check is retained until STI;HLT. */
 if(arrival) pending=true;
 interrupts=true; ++halted;
}
''' + body + r'''
int main(void) {
 unsigned cases=0;
 for(unsigned flags=0; flags<16; ++flags) {
  bool initial=!!(flags&1);
  interrupts=initial; pending=!!(flags&2); ready=!!(flags&4); arrival=!!(flags&8);
  scheduled=halted=checks=0;
  userspace_device_init_poll();
  assert(interrupts==initial);
  assert(scheduled==!!(flags&6) && halted==!(flags&6));
  if(!(flags&6) && arrival) assert(pending);
  ++cases;
 }
 printf("Boot wait: %u IRQ-state, runnable work, remote wake and check/sleep race cases PASS\n",cases);
}
'''
with tempfile.TemporaryDirectory(prefix='device-boot-wait-') as directory:
    path=Path(directory)
    (path/'test.c').write_text(program)
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror',
                    '-fsanitize=address,undefined','-no-pie',str(path/'test.c'),
                    '-o',str(path/'test')],check=True)
    subprocess.run([str(path/'test')],check=True,timeout=15)
