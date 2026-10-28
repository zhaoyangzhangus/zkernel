"""Check RAM-only boot timing and coverage of the measured production phases."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
header = (root/'include/kernel/boot_profile.h').read_text()
phases = header.split('enum kernel_boot_phase {',1)[1].split('BOOT_PROFILE_COUNT',1)[0]
phases = [line.strip().rstrip(',') for line in phases.splitlines() if line.strip()]
sources = '\n'.join((root/('kernel/init/'+name+'.c')).read_text()
                    for name in ('main','self_tests','userspace','runtime'))
for phase in phases:
    expected = 1
    assert sources.count('kernel_boot_begin('+phase+')') == expected, phase
    assert sources.count('kernel_boot_end('+phase+')') == expected, phase
assert 'kernel_serial' not in header and 'kmalloc' not in header and 'vfs_' not in header
program = r'''
#include <assert.h>
#include <stdio.h>
#include <kernel/boot_profile.h>
kernel_boot_span_t g_kernel_boot_profile[BOOT_PROFILE_COUNT];
static uint64_t clock_ticks;
uint64_t telemetry_timestamp(void) { return ++clock_ticks; }
int main(void) {
 for(unsigned i=0; i<BOOT_PROFILE_COUNT; ++i) {
  assert(!g_kernel_boot_profile[i].begin_tsc && !g_kernel_boot_profile[i].end_tsc);
  kernel_boot_begin(i);
  assert(g_kernel_boot_profile[i].begin_tsc==2*i+1 && !g_kernel_boot_profile[i].end_tsc);
  kernel_boot_end(i);
  assert(g_kernel_boot_profile[i].end_tsc==2*i+2);
 }
 assert(clock_ticks==2*BOOT_PROFILE_COUNT);
 printf("Boot profile: %u phase pairs, unfinished-stage visibility and RAM-only timestamps PASS\n",BOOT_PROFILE_COUNT);
}
'''
with tempfile.TemporaryDirectory(prefix='boot-profile-') as directory:
    path=Path(directory)
    (path/'test.c').write_text(program)
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror',
                    '-I',str(root/'include'),str(path/'test.c'),'-o',str(path/'test')],check=True)
    subprocess.run([str(path/'test')],check=True,timeout=15)
