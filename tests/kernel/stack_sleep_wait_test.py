"""使用真实等待核心验证栈上休眠队列的超时、取消和无分配清理。"""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
host = (root / 'tests/kernel/wait_subscription_host.c').read_text()
marker = 'static void *host_wait_thread_main(void *opaque) {'
helper = r'''
static bool host_sleep_never_ready(void *context) {
    (void)context;
    return false;
}
static kstatus_t host_stack_sleep(uint64_t timeout_ns) {
    wait_queue_t queue;
    wait_queue_init(&queue);
    kstatus_t status = wait_on_queue(&queue, host_sleep_never_ready, 0, timeout_ns);
    /* 返回后队列即将离开作用域，任何订阅或锁残留都会使后续唤醒访问失效栈。 */
    if (!list_empty(&queue.waiters) || atomic_load(&queue.lock.state) ||
        atomic_load(&sched_current_thread()->blocked_waiter)) abort();
    return status;
}
'''
assert marker in host
call = 'completion_wait(job->completion, job->timeout_ns)'
assert call in host
host = host.replace(marker, helper + marker).replace(call, 'host_stack_sleep(job->timeout_ns)')
program = r'''
#include <assert.h>
#include <stdio.h>
#include <kernel/wait.h>
typedef struct host_wait_job host_wait_job_t;
host_wait_job_t *host_completion_job_start(completion_t *, uint64_t);
bool host_wait_until_sleeping(host_wait_job_t *, uint64_t);
bool host_wait_until_done(host_wait_job_t *, uint64_t);
bool host_wait_until_schedule_entries(host_wait_job_t *, uint32_t, uint64_t);
bool host_wait_job_is_unblocked(const host_wait_job_t *);
bool host_cancel_waiter(host_wait_job_t *);
kstatus_t host_wait_job_status(const host_wait_job_t *);
void host_wait_job_join(host_wait_job_t *);
void host_wake_without_wait_notification(host_wait_job_t *);
void host_fail_next_allocation(void);
void host_clear_allocation_failure(void);
unsigned host_allocation_calls(void);
int main(void) {
    completion_t unused;
    completion_init(&unused, 0);
    unsigned allocations = host_allocation_calls();
    host_fail_next_allocation();
    for (unsigned i = 0; i < 100; ++i) {
        host_wait_job_t *job = host_completion_job_start(&unused, 1000000);
        assert(job && host_wait_until_done(job, 2000));
        assert(host_wait_job_status(job) == K_ETIMEDOUT);
        assert(host_wait_job_is_unblocked(job) && !host_cancel_waiter(job));
        host_wait_job_join(job);
    }
    host_wait_job_t *job = host_completion_job_start(&unused, 3600000000000ULL);
    assert(job && host_wait_until_sleeping(job, 2000));
    /* 无关调度唤醒不能使休眠提前成功，也不能遗留上一轮栈订阅。 */
    host_wake_without_wait_notification(job);
    assert(host_wait_until_schedule_entries(job, 2, 2000));
    assert(!host_wait_until_done(job, 5));
    assert(host_cancel_waiter(job) && host_wait_until_done(job, 2000));
    assert(host_wait_job_status(job) == K_ECANCELED);
    assert(host_wait_job_is_unblocked(job) && !host_cancel_waiter(job));
    host_wait_job_join(job);
    assert(host_allocation_calls() == allocations);
    host_clear_allocation_failure();
    puts("PASS: actual stack queue, 100 blocking timeouts, unrelated wake, cancellation, detached lifetime and zero kernel allocations");
}
'''
with tempfile.TemporaryDirectory(prefix='stack-sleep-wait-') as directory:
    path = Path(directory)
    (path / 'host.c').write_text(host)
    (path / 'test.c').write_text(program)
    binary = str(path / 'test')
    subprocess.run(['cc', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                    '-pthread', '-DOS_WAIT_HOST_TEST', '-I' + str(root / 'include'),
                    '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
                    str(root / 'kernel/sync/wait.c'), str(root / 'kernel/object/object.c'),
                    str(path / 'host.c'), str(path / 'test.c'), '-o', binary], check=True)
    subprocess.run([binary], check=True, timeout=20)
