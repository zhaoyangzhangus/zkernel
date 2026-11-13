"""Actual wait implementation: singleton routing skips multi-queue preparation."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'kernel/sync/wait.c').read_text()
signature = 'static void queue_pointer_sort(wait_queue_t **queues, size_t count) {'
assert signature in source
source = '#include <stdatomic.h>\natomic_uint host_sort_calls;\n' + source.replace(
    signature, signature + '\n    atomic_fetch_add(&host_sort_calls, 1);')
program = r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <kernel/wait.h>
extern atomic_uint host_sort_calls;
void host_fail_next_allocation(void);
void host_clear_allocation_failure(void);
static unsigned checks;
static bool ready(void *context) { ++checks;return *(bool*)context; }
int main(void) {
 wait_queue_t a,b;wait_queue_init(&a);wait_queue_init(&b);
 bool value=true;wait_queue_t *one[]={&a};
 for(unsigned i=0;i<10000;++i) {
  unsigned before=checks;
  assert(wait_on_queue(&a,ready,&value,UINT64_MAX)==K_OK && checks==before+1);
  assert(wait_on_queues(one,1,ready,&value,0)==K_OK && checks==before+2);
  value=false;
  assert(wait_on_queue(&a,ready,&value,0)==K_ETIMEDOUT && checks==before+3);
  assert(wait_on_queues(one,1,ready,&value,0)==K_ETIMEDOUT && checks==before+4);
  value=true;
 }
 assert(!atomic_load(&host_sort_calls));
 assert(list_empty(&a.waiters) && !atomic_load(&a.lock.state));
 assert(!atomic_load(&a.sequence));
 unsigned before=checks;
 assert(wait_on_queue(0,ready,&value,0)==K_EINVAL);
 assert(wait_on_queue(&a,0,&value,0)==K_EINVAL);
 assert(wait_on_queues(0,1,ready,&value,0)==K_EINVAL);
 assert(wait_on_queues(one,0,ready,&value,0)==K_EINVAL);
 wait_queue_t *bad[]={0};
 assert(wait_on_queues(bad,1,ready,&value,0)==K_EINVAL && checks==before);
 assert(wait_on_queues(one,SIZE_MAX,ready,&value,0)==K_EOVERFLOW);
 // 单队列就绪状态没有分配失败的可能。待处理的失败状态必须留给下一次真实的多队列分配。
 host_fail_next_allocation();
 assert(wait_on_queue(&a,ready,&value,0)==K_OK);
 assert(wait_on_queues(one,1,ready,&value,0)==K_OK);
 wait_queue_t *many[]={&b,&a,&b,&a};
 assert(wait_on_queues(many,4,ready,&value,0)==K_OK);
 wait_queue_t *large[]={&b,&a,&b,&a,&b};
 assert(wait_on_queues(large,5,ready,&value,0)==K_ENOMEM);
 host_clear_allocation_failure();
 assert(wait_on_queues(large,5,ready,&value,0)==K_OK);
 assert(large[0]==&b && large[1]==&a && large[2]==&b && large[3]==&a && large[4]==&b);
 assert(atomic_load(&host_sort_calls)==2);
 wait_queue_t *scratch[]={&b,&a,&b,&a,&b};
 host_fail_next_allocation();
 before=checks;
 assert(wait_on_queues_in_place(scratch,5,ready,&value,0)==K_OK && checks==before+1);
 assert((uintptr_t)scratch[0]<(uintptr_t)scratch[1]);
 assert((scratch[0]==&a && scratch[1]==&b) || (scratch[0]==&b && scratch[1]==&a));
 assert(wait_on_queues(large,5,ready,&value,0)==K_ENOMEM);
 host_clear_allocation_failure();
 value=false;
 assert(wait_on_queues_in_place(scratch,2,ready,&value,0)==K_ETIMEDOUT);
 value=true;
 assert(wait_on_queues_in_place(one,1,ready,&value,0)==K_OK);
 wait_queue_t *invalid_scratch[]={&b,0,&a},*saved[]={&b,0,&a};
 before=checks;
 assert(wait_on_queues_in_place(invalid_scratch,3,ready,&value,0)==K_EINVAL);
 assert(!memcmp(invalid_scratch,saved,sizeof(saved)) && checks==before);
 assert(wait_on_queues_in_place(0,1,ready,&value,0)==K_EINVAL);
 assert(wait_on_queues_in_place(scratch,0,ready,&value,0)==K_EINVAL);
 assert(wait_on_queues_in_place(scratch,2,0,&value,0)==K_EINVAL);
 assert(wait_on_queues_in_place(scratch,SIZE_MAX,ready,&value,0)==K_EOVERFLOW);
 wait_queue_t *invalid_many[]={&a,0};
 assert(wait_on_queues(invalid_many,2,ready,&value,0)==K_EINVAL);
 assert(atomic_load(&host_sort_calls)==4);
 assert(list_empty(&a.waiters) && list_empty(&b.waiters));
 puts("PASS: 40000 singleton ready/timeout calls skip sorting; invalid inputs, allocation isolation and multi-queue routing retained");
}
'''
with tempfile.TemporaryDirectory(prefix='liteos-single-wait-') as directory:
    path = Path(directory)
    (path / 'wait.c').write_text(source)
    (path / 'test.c').write_text(program)
    binary = str(path / 'test')
    subprocess.run(['cc', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                    '-pthread', '-DOS_WAIT_HOST_TEST', '-I' + str(root / 'include'),
                    '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
                    str(path / 'wait.c'), str(root / 'kernel/object/object.c'),
                    str(root / 'tests/kernel/wait_subscription_host.c'),
                    str(path / 'test.c'), '-o', binary], check=True)
    subprocess.run([binary], check=True)
