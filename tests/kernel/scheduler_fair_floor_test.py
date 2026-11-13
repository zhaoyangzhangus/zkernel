"""Actual fair-tree insertion/removal: newborns must not starve old threads."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root/'kernel/sched/runqueue.c').read_text()
begin = source.index('static thread_t *rt_thread_from_node')
end = source.index('void initialize_thread(')
program = '#include "' + str(root/'kernel/sched/internal.h') + '"\n'
program += '#include <assert.h>\n' + source[begin:end] + r'''
int main(void) {
    run_queue_t queue = {0};
    thread_t browser = {0}, idle = {0}, renderer = {0}, sleeper = {0};
    browser.tid=126; browser.sched_class=SCHED_CLASS_FAIR;
    browser.sched.vruntime=300000000;
    atomic_init(&browser.state, THREAD_RUNNING);
    queue.current=&browser; queue.idle=&idle;
    renderer.tid=200; renderer.sched_class=SCHED_CLASS_FAIR;
    fair_insert(&queue,&renderer);
    assert(renderer.sched.vruntime==300000000);
    assert(queue.min_vruntime==300000000);
    fair_remove(&queue,&renderer);
    assert(queue.min_vruntime==300000000);
    queue.current=&idle;
    scheduler_update_min_vruntime(&queue);
    assert(queue.min_vruntime==300000000);
    sleeper.tid=135; sleeper.sched_class=SCHED_CLASS_FAIR;
    sleeper.sched.vruntime=100;
    fair_insert(&queue,&sleeper);
    assert(sleeper.sched.vruntime==300000000);
    fair_insert(&queue,&browser);
    assert(queue.fair_leftmost==&browser);
    fair_remove(&queue,&browser); fair_remove(&queue,&sleeper);
    /* 已领先的线程保留其权重；反复让出 CPU 不会提高其权重。 */
    browser.sched.vruntime=400000000;
    fair_insert(&queue,&browser);
    assert(browser.sched.vruntime==400000000);
    fair_remove(&queue,&browser);
    /* 当前实体和排队实体都参与最低值计算。 */
    browser.sched.vruntime=500000000; queue.current=&browser;
    renderer.sched.vruntime=450000000;
    fair_insert(&queue,&renderer);
    assert(renderer.sched.vruntime==500000000);
    browser.sched.vruntime=600000000;
    scheduler_update_min_vruntime(&queue);
    assert(queue.min_vruntime==500000000);
    fair_remove(&queue,&renderer);
    assert(queue.min_vruntime==600000000);
    queue.current=&idle;
    thread_t nodes[200]={0};
    for (unsigned i=0;i<200;++i) {
        nodes[i].tid=1000+i; nodes[i].sched_class=SCHED_CLASS_FAIR;
        nodes[i].sched.vruntime=(i%3==0)?700000000:0;
        fair_insert(&queue,&nodes[i]);
    }
    thread_t *previous=0; uint32_t black_depth=UINT32_MAX, count=0;
    assert(validate_fair_node(queue.fair_root.root,0,&previous,0,&black_depth,&count));
    assert(count==200);
    for (unsigned i=0;i<200;++i) fair_remove(&queue,&nodes[i]);
    assert(!queue.fair_root.root && !queue.fair_count);
    assert(queue.min_vruntime==700000000);
    queue.current=&browser; browser.sched.vruntime=UINT64_MAX;
    scheduler_update_min_vruntime(&queue);
    assert(queue.min_vruntime==UINT64_MAX);
    renderer.sched.vruntime=0; fair_insert(&queue,&renderer);
    assert(renderer.sched.vruntime==UINT64_MAX);
    assert(rt_thread_from_node(&renderer.sched.rt_node)==&renderer);
}
'''
with tempfile.TemporaryDirectory(prefix='liteos-fair-floor-') as directory:
    path = Path(directory)
    (path/'test.c').write_text(program)
    subprocess.run(['cc','-std=gnu11','-Wall','-Wextra','-Werror',
                    '-fsanitize=address,undefined','-fno-pie','-no-pie',
                    '-I'+str(root/'include'),str(path/'test.c'),
                    '-o',str(path/'test')],check=True)
    subprocess.run([str(path/'test')],check=True)
print('fair floor: newborn placement, idle monotonicity, sleeper clamp, no debt reset, RB tree and saturation passed')
