"""Exercise the production device lifecycle lock against scheduler preemption."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root/'kernel/drivers/net/core.c').read_text()
start = source.index('static bool e1000_lifecycle_try_lock(void)')
end = source.index('void e1000_configure_link_local(', start)
program = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <pthread.h>
static atomic_uint g_e1000_lifecycle_lock;
static _Thread_local unsigned preempt_depth;
static atomic_uint unsafe_acquisitions, protected_visits;
static void sched_preempt_disable(void) { ++preempt_depth; }
static void sched_preempt_enable(void) {
    assert(preempt_depth);
    --preempt_depth;
}
static unsigned exchange_with_scheduler_probe(atomic_uint* lock, unsigned value,
                                              memory_order order) {
    unsigned previous=atomic_exchange_explicit(lock,value,order);
    if (!previous && !preempt_depth) ++unsafe_acquisitions;
    return previous;
}
#undef atomic_exchange_explicit
#define atomic_exchange_explicit exchange_with_scheduler_probe
''' + source[start:end] + r'''
#undef atomic_exchange_explicit
static void* contender(void* arg) {
    (void)arg;
    for (unsigned i=0;i<10000;++i) {
        bool acquired=e1000_lifecycle_try_lock();
        if (acquired) {
            assert(preempt_depth==1);
            ++protected_visits;
            e1000_lifecycle_unlock();
        }
        assert(preempt_depth==0);
    }
    return 0;
}
int main(void) {
    assert(e1000_lifecycle_try_lock());
    assert(!atomic_load(&unsafe_acquisitions));
    assert(preempt_depth==1);
    assert(!e1000_lifecycle_try_lock());
    assert(preempt_depth==1); /* Failed nested try must preserve outer guard. */
    e1000_lifecycle_unlock();
    assert(!preempt_depth && !atomic_load(&g_e1000_lifecycle_lock));
    sched_preempt_disable();
    assert(e1000_lifecycle_try_lock());
    assert(preempt_depth==2);
    e1000_lifecycle_unlock();
    assert(preempt_depth==1);
    sched_preempt_enable();
    pthread_t workers[4];
    for (unsigned i=0;i<4;++i) assert(!pthread_create(&workers[i],0,contender,0));
    for (unsigned i=0;i<4;++i) assert(!pthread_join(workers[i],0));
    assert(protected_visits>0 && !unsafe_acquisitions);
    assert(!preempt_depth && !atomic_load(&g_e1000_lifecycle_lock));
}
'''
with tempfile.TemporaryDirectory(prefix='liteos-net-lifecycle-') as directory:
    path=Path(directory)
    (path/'test.c').write_text(program)
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror',
                    '-Wno-unused-function','-pthread','-fsanitize=address,undefined',
                    '-fno-pie','-no-pie',str(path/'test.c'),'-o',str(path/'test')],check=True)
    subprocess.run([str(path/'test')],check=True)
print('Network lifecycle: preempt-safe ownership, nested/failed tries and four-thread contention passed')
