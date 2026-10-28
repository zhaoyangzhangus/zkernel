"""Run production userspace bring-up with traceable boot dependencies/failures."""
from pathlib import Path
import resource
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root/'kernel/init/userspace.c').read_text()
start = source.index('BOOLEAN kernel_init_userspace(')
end = source.index('\n}', start)+2
body = source[start:end]

# Firmware description must exist even for the older early-probe builds.
devices = (root/'kernel/init/devices.c').read_text()
assert devices.index('nv_rm_driver_set_screen_info(') < devices.index('nv_rm_driver_probe(')
post = (root/'kernel/init/self_tests.c').read_text()
assert post.index('if (!kernel_init_filesystem(') < post.index('if (!kernel_init_userspace(')
assert post.index('if (!kernel_init_network(') < post.index('if (!kernel_init_graphics(') < post.index('if (!kernel_init_userspace(')
assert 'userspace_finish_device_init(' not in post
assert 'userspace_finish_device_init(' not in body
runtime = (root/'kernel/init/runtime.c').read_text()
phase5 = runtime.split('#if OS_NVIDIA_PHASE5\n    if (userspace_device_init_done()) {', 1)[1].split('#elif', 1)[0]
assert 'nv_rm_driver_probe(' not in phase5
assert 'NV_GPU_GSP_READY' in phase5
assert 'if (realtest_enabled())\n                realtest_finish_failure();' in phase5

program = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <kernel/init_userspace.h>
enum { EARLY_WORKER=1, BOOTSTRAP, IPC, DEFERRED, GPU, WINDOWS, SERVICES, WORKER };
static unsigned trace[16], count, fail_at, halts, cpus;
static int gpu_result;
static bool step(unsigned code) {
 if (code==BOOTSTRAP) assert(count && trace[0]==EARLY_WORKER && fail_at!=EARLY_WORKER);
 assert(count < 16); trace[count++]=code; return code != fail_at;
}
static void write_text(const CHAR8 *text) { assert(text); }
static void write_number(UINT32 value) { (void)value; }
static void halt(void) { ++halts; }
static BOOLEAN fail(const kernel_init_userspace_hooks_t *hooks, const CHAR8 *text) {
 hooks->write(text); hooks->halt(); return 0;
}
#if OS_NVIDIA_PHASE5
static int probe(void) {
 assert(count && trace[count-1]==DEFERRED && fail_at != DEFERRED);
 assert(step(GPU)); return gpu_result;
}
#endif
static bool finish_device(void) {
 if (!step(DEFERRED)) return false;
#if OS_NVIDIA_PHASE5
 (void)probe();
#endif
 return true;
}
#define user_init_bootstrap_self_test() step(BOOTSTRAP)
#define user_init_failure_result() 0
#define user_init_failure_stage() 0
#define ipc_self_test() step(IPC)
#define deferred_start_worker() step(EARLY_WORKER)
#define userspace_start_device_init() finish_device()
#define nv_rm_driver_probe() probe()
#define window_server_init() step(WINDOWS)
#define user_init_start() step(SERVICES)
#define x86_smp_discovered_count() cpus
#define userspace_start_window_worker(...) step(WORKER)
#define userspace_fail(hooks, text) fail(hooks, text)
#define kernel_debug_stage(...) ((void)0)
#define kernel_debug_stage_fail(...) ((void)0)
#define kernel_debug_stage_pending(...) ((void)0)
''' + body + r'''
int main(void) {
 kernel_init_userspace_hooks_t hooks={write_text, write_number, halt};
 unsigned cases=0;
 for (unsigned smp=0; smp<2; ++smp) for (unsigned gpu=0; gpu<3; ++gpu)
 for (unsigned failure=0; failure<=WORKER; ++failure) {
  if (failure==GPU) continue; /* Optional GPU failure is not fatal. */
  fail_at=failure; gpu_result=gpu==0?0:gpu==1?-5:-19;
  count=halts=0; cpus=smp?16:1;
  /* Only failures beyond GPU allow its call. Earlier failures exit before it;
   * the assertion in probe tests precisely that worker startup succeeded. */
  bool result=kernel_init_userspace(&hooks);
  unsigned expected[16], n=0;
  bool failed=false;
  for (unsigned code=EARLY_WORKER; code<=WORKER; ++code) {
   if (code==GPU && !OS_NVIDIA_PHASE5) continue;
   if (code==WORKER && !smp) continue;
   expected[n++]=code;
   if (code==failure) { failed=true; break; }
  }
  assert(result==!failed && halts==(unsigned)failed);
  assert(count==n && !memcmp(trace,expected,n*sizeof(*trace)));
  ++cases;
 }
 count=halts=0; assert(!kernel_init_userspace(NULL) && !count && !halts);
 hooks.write=NULL; assert(!kernel_init_userspace(&hooks) && !count && !halts);
 printf("NVIDIA early boot: phase5=%u, %u UP/SMP, optional GPU success/failure/absence and fatal dependency cases PASS\n",
        OS_NVIDIA_PHASE5,cases);
}
'''
with tempfile.TemporaryDirectory(prefix='nvidia-boot-order-') as directory:
    path=Path(directory)
    def run(text, enabled):
        (path/'test.c').write_text(text)
        subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror',
                        '-fsanitize=address,undefined','-no-pie',
                        '-DOS_NVIDIA_PHASE5='+str(enabled),'-I',str(root/'include'),
                        str(path/'test.c'),'-o',str(path/'test')],check=True)
        def no_core():
            resource.setrlimit(resource.RLIMIT_CORE, (0,0))
        return subprocess.run([str(path/'test')],capture_output=True,text=True,
                              timeout=15,preexec_fn=no_core)
    for enabled in (0,1):
        result=run(program,enabled)
        assert result.returncode==0,result.stderr
        print(result.stdout,end='')
    for label, bad_body in (
        ('用户态等待早于定时器工作线程启动',
         body.replace('if (!deferred_start_worker())', 'if (0)')),
        ('GPU task queued after service publication',
         body.replace('if (!userspace_start_device_init())', 'if (0)').replace(
             '    if (x86_smp_discovered_count()',
             '    (void)userspace_start_device_init();\n    if (x86_smp_discovered_count()')),
    ):
        result=run(program.replace(body,bad_body),1)
        assert result.returncode!=0,label
        print('Negative control rejected:',label)
