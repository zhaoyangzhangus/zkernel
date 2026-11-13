"""Actual dense teardown registry: idempotence, snapshot bounds and races."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'kernel/process/process.c').read_text()
snapshot = source[source.index('static uint32_t process_snapshot_teardown_callbacks('):
                  source.index('static const object_ops_t g_process_ops')]
register = source[source.index('kstatus_t process_register_teardown_callback('):
                  source.index('void process_initialize_object(')]
snapshot = snapshot.replace('__builtin_memcpy(', 'tracked_copy(')
fixture = r'''
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#define K_OK 0
#define K_EINVAL -22
#define K_ENOSPC -28
#define PROCESS_TEARDOWN_CALLBACK_MAX 8U
typedef int kstatus_t;
typedef struct { unsigned dummy; } process_t;
typedef void (*process_teardown_callback_t)(process_t *);
static process_teardown_callback_t g_process_teardown_callbacks[8];
static uint32_t g_process_teardown_count;
static pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER;
static _Thread_local unsigned held;
static atomic_uint locks,copies,callback_calls;
static bool reenter;
static void process_global_initialize(void) { assert(!held); }
static void process_teardown_lock(void) { assert(!held);pthread_mutex_lock(&lock);++held;++locks; }
static void process_teardown_unlock(void) { assert(held==1);--held;pthread_mutex_unlock(&lock); }
static void tracked_copy(void *out,const void *in,size_t bytes) {
 assert(held && in==g_process_teardown_callbacks && bytes==g_process_teardown_count*sizeof(process_teardown_callback_t));
 ++copies;memcpy(out,in,bytes);
}
kstatus_t process_register_teardown_callback(process_teardown_callback_t callback);
static void c2(process_t *p);
static void c0(process_t *p) { assert(!held && p);++callback_calls;if(reenter)assert(process_register_teardown_callback(c2)==K_OK); }
#define CALLBACK(n) static void c##n(process_t *p) { assert(!held && p);++callback_calls; }
CALLBACK(1) CALLBACK(2) CALLBACK(3) CALLBACK(4) CALLBACK(5) CALLBACK(6) CALLBACK(7) CALLBACK(8)
static process_teardown_callback_t callbacks[]={c0,c1,c2,c3,c4,c5,c6,c7,c8};
'''
checks = r'''
static void clear_registry(void) {
 assert(!held);memset(g_process_teardown_callbacks,0,sizeof(g_process_teardown_callbacks));
 g_process_teardown_count=0;locks=copies=callback_calls=0;
}
static void verify_snapshot(void) {
 process_teardown_callback_t out[9];for(unsigned i=0;i<9;++i)out[i]=c8;
 unsigned n=process_snapshot_teardown_callbacks(out);assert(n<=8 && !held);
 for(unsigned i=0;i<n;++i) {
  bool known=false;for(unsigned j=0;j<8;++j)if(out[i]==callbacks[j])known=true;
  assert(known);for(unsigned j=0;j<i;++j)assert(out[i]!=out[j]);
 }
 for(unsigned i=n;i<9;++i)assert(out[i]==c8);
}
static void *worker(void *opaque) {
 size_t id=(size_t)opaque;
 for(unsigned i=0;i<100;++i) {
  assert(process_register_teardown_callback(callbacks[(id+i)%8])==K_OK);
  verify_snapshot();
 }
 return NULL;
}
int main(void) {
 for(unsigned rotation=0;rotation<8;++rotation)for(unsigned count=0;count<=8;++count) {
  clear_registry();assert(process_register_teardown_callback(NULL)==K_EINVAL && !atomic_load(&locks));
  assert(!process_snapshot_teardown_callbacks(NULL) && !atomic_load(&locks));
  for(unsigned i=0;i<count;++i)assert(process_register_teardown_callback(callbacks[(rotation+i)%8])==K_OK);
  process_teardown_callback_t out[9];for(unsigned i=0;i<9;++i)out[i]=c8;
  assert(process_snapshot_teardown_callbacks(out)==count);
  for(unsigned i=0;i<count;++i) {
   assert(out[i]==callbacks[(rotation+i)%8]);
   assert(process_register_teardown_callback(out[i])==K_OK);
  }
  for(unsigned i=count;i<9;++i)assert(out[i]==c8);
  assert(g_process_teardown_count==count);
  if(count==8)assert(process_register_teardown_callback(c8)==K_ENOSPC);
  verify_snapshot();
 }
 // 回调可以注册另一个回调；已捕获的回调集合不可更改。
 clear_registry();assert(process_register_teardown_callback(c0)==K_OK);
 assert(process_register_teardown_callback(c1)==K_OK);
 process_teardown_callback_t out[8];unsigned n=process_snapshot_teardown_callbacks(out);assert(n==2);
 reenter=true;process_t p={0};for(unsigned i=0;i<n;++i)out[i](&p);reenter=false;
 assert(atomic_load(&callback_calls)==2 && g_process_teardown_count==3);
 assert(process_snapshot_teardown_callbacks(out)==3 && out[2]==c2);
 for(unsigned round=0;round<30;++round) {
  clear_registry();pthread_t jobs[16];
  for(size_t i=0;i<16;++i)assert(!pthread_create(&jobs[i],NULL,worker,(void*)i));
  for(unsigned i=0;i<16;++i)assert(!pthread_join(jobs[i],NULL));
  assert(g_process_teardown_count==8 && atomic_load(&locks)==3200 && atomic_load(&copies)==1600);
  verify_snapshot();assert(process_register_teardown_callback(c8)==K_ENOSPC);
 }
 puts("72 registry layouts, reentrant callback snapshot and 48000 concurrent registrations/snapshots: dense order, unique entries and copy bounds passed");
}
'''
with tempfile.TemporaryDirectory(prefix='liteos-teardown-registry-') as directory:
    path = Path(directory)
    def run(actual):
        (path / 'test.c').write_text(fixture + snapshot + actual + checks)
        subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-pthread',
                        '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
                        str(path / 'test.c'), '-o', str(path / 'test')], check=True)
        return subprocess.run([str(path / 'test')], capture_output=True, text=True)
    result = run(register)
    assert result.returncode == 0, result.stdout + result.stderr
    print(result.stdout.strip())
    negative = register.replace('if (g_process_teardown_callbacks[index] == callback)', 'if (false)')
    result = run(negative)
    assert result.returncode != 0 and 'Assertion' in result.stderr
    print('Negative control rejected: duplicate registration consumes slots')
