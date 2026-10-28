"""Real ARP/NDP cache operations must pin each lock owner, including nesting."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
code = r'''
#include <assert.h>
#include <pthread.h>
#include <string.h>
#include "kernel/net_core.h"
static _Thread_local unsigned depth,outer,held,acquisitions;
void sched_preempt_disable(void){++depth;}
void sched_preempt_enable(void){assert(depth==outer+1&&!held);--depth;}
static void checked_lock(spinlock_t *lock){
 assert(depth==outer+1&&!held);spinlock_lock(lock);++held;++acquisitions;
}
static void checked_unlock(spinlock_t *lock){
 assert(depth==outer+1&&held==1);--held;spinlock_unlock(lock);
}
#define spinlock_lock checked_lock
#define spinlock_unlock checked_unlock
#include "kernel/net/core/core.c"
static net_arp_cache_t arp;
static net_ipv6_neighbor_cache_t ndp;
static const uint8_t address[16]={0xfe,0x80,[15]=1};
static void *worker(void *arg){
 outer=depth=(unsigned)(uintptr_t)arg;
 for(unsigned i=0;i<2000;++i){
  uint8_t mac[6],out[6];memset(mac,i&255,sizeof(mac));
  assert(net_arp_cache_update(&arp,1,mac,i)==K_OK);
  assert(net_arp_cache_lookup(&arp,1,out));
  for(unsigned j=1;j<6;++j)assert(out[j]==out[0]);
  assert(net_ipv6_neighbor_cache_update(&ndp,address,mac,i)==K_OK);
  assert(net_ipv6_neighbor_cache_lookup(&ndp,address,out));
  for(unsigned j=1;j<6;++j)assert(out[j]==out[0]);
  assert(depth==outer&&!held);
 }
 uint8_t out[6];
 assert(!net_arp_cache_lookup(&arp,2,out));
 uint8_t absent[16]={0xfe,0x80,[15]=2};
 assert(!net_ipv6_neighbor_cache_lookup(&ndp,absent,out));
 assert(depth==outer&&!held&&acquisitions==8002);
 unsigned before=acquisitions;
 assert(!net_arp_cache_lookup(0,1,out));
 assert(net_arp_cache_update(0,1,out,0)==K_EINVAL);
 assert(!net_ipv6_neighbor_cache_lookup(0,address,out));
 assert(net_ipv6_neighbor_cache_update(0,address,out,0)==K_EINVAL);
 assert(depth==outer&&!held&&acquisitions==before);
 return 0;
}
int main(void){
 net_arp_cache_init(&arp);net_ipv6_neighbor_cache_init(&ndp);
 pthread_t threads[4];
 for(unsigned i=0;i<4;++i)assert(!pthread_create(&threads[i],0,worker,(void*)(uintptr_t)i));
 for(unsigned i=0;i<4;++i)assert(!pthread_join(threads[i],0));
 assert(!atomic_load(&arp.lock.state)&&!atomic_load(&ndp.lock.state));
}
'''
with tempfile.TemporaryDirectory(prefix='net-cache-preempt-') as directory:
    path = Path(directory)
    for negative in (False, True):
        candidate = code
        if negative:
            source = (root / 'kernel/net/core/core.c').read_text()
            source = source.replace('    sched_preempt_disable();\n', '')
            source = source.replace('    sched_preempt_enable();\n', '')
            # Resolve the only relative core include after inlining it here.
            source = source.replace('#include "internal.h"', '#include "kernel/net/core/internal.h"')
            candidate = candidate.replace('#include "kernel/net/core/core.c"', source)
        (path / 'test.c').write_text(candidate)
        subprocess.run(['cc', '-std=gnu11', '-O2', '-Wall', '-Wextra', '-Werror',
                        '-pthread', '-ffunction-sections', '-fdata-sections',
                        '-Wl,--gc-sections', '-fsanitize=address,undefined',
                        '-fno-pie', '-no-pie', '-I', str(root), '-I', str(root / 'include'),
                        str(path / 'test.c'), '-o', str(path / 'test')], check=True)
        result = subprocess.run([str(path / 'test')], capture_output=True)
        assert (result.returncode != 0) if negative else (result.returncode == 0), result.stderr
print('ARP/NDP cache: 32008 real concurrent guarded operations, nested preemption, hits/misses, invalid inputs and old-policy negative control PASS')
