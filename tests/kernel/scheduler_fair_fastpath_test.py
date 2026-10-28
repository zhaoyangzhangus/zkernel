"""Compare the unchanged RB-tree selection with the accounted-current shortcut."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'kernel/sched/runqueue.c').read_text()
production = source[source.index('static thread_t *rt_thread_from_node'):
                    source.index('void initialize_thread(')]
core = (root / 'kernel/sched/core.c').read_text()
schedule = core[core.index('static void scheduler_schedule_internal(bool commands_already_drained) {'):]
assert schedule.index('scheduler_account_current(') < schedule.index('scheduler_fair_current_wins(')
assert schedule.index('scheduler_command_drain_local(') < schedule.index('scheduler_fair_current_wins(')
program = '#include "' + str(root / 'kernel/sched/internal.h') + '"\n'
program += '#include <assert.h>\n#include <stdio.h>\n' + production + r'''
static uint64_t seed=31337;
static uint64_t random_value(void) {
    seed=seed*UINT64_C(6364136223846793005)+1; return seed^(seed>>29);
}
int main(void) {
    unsigned shortcuts=0,full=0;
    for(unsigned trial=0;trial<30000;++trial) {
        run_queue_t queue={0}; thread_t current={0},idle={0},nodes[64]={0};
        current.sched_class=SCHED_CLASS_FAIR;
        atomic_init(&current.state,THREAD_RUNNING);
        queue.current=&current;queue.idle=&idle;
        current.tid=random_value()%8;
        current.sched.vruntime=trial%7==0?UINT64_MAX:random_value()%100000;
        unsigned count=1+random_value()%64;
        for(unsigned i=0;i<count;++i) {
            nodes[i].tid=random_value()%8;
            nodes[i].sched_class=SCHED_CLASS_FAIR;
            nodes[i].sched.vruntime=trial%3==0?current.sched.vruntime:random_value()%100000;
            fair_insert(&queue,&nodes[i]);
        }
        bool shortcut=scheduler_fair_current_wins(&queue);
        /* The original schedule path accounts first, enqueues current,
         * picks the leftmost and then removes it. Use those actual helpers. */
        fair_insert(&queue,&current);
        assert(shortcut==(queue.fair_leftmost==&current));
        if(shortcut)++shortcuts;else ++full;
        fair_remove(&queue,&current);
        thread_t *previous=0; uint32_t depth=UINT32_MAX,seen=0;
        assert(validate_fair_node(queue.fair_root.root,0,&previous,0,&depth,&seen));
        assert(seen==count);
        queue.rt_bitmap=1;assert(!scheduler_fair_current_wins(&queue));queue.rt_bitmap=0;
        current.migration_pending=true;assert(!scheduler_fair_current_wins(&queue));
        current.migration_pending=false;
        current.sched.flags=SCHED_ENTITY_ENQUEUED;assert(!scheduler_fair_current_wins(&queue));
        current.sched.flags=0;
        atomic_store(&current.state,THREAD_BLOCKED);assert(!scheduler_fair_current_wins(&queue));
        atomic_store(&current.state,THREAD_RUNNING);
        current.sched_class=SCHED_CLASS_RT;assert(!scheduler_fair_current_wins(&queue));
        current.sched_class=SCHED_CLASS_FAIR;
        queue.current=&idle;assert(!scheduler_fair_current_wins(&queue));
        queue.current=0;assert(!scheduler_fair_current_wins(&queue));
        queue.current=&current;
        for(unsigned i=0;i<count;++i)fair_remove(&queue,&nodes[i]);
        assert(!scheduler_fair_current_wins(&queue));
    }
    assert(shortcuts&&full);
    printf("Fair fast path: 30000 differential selections, ties/saturation and RT/block/migration guards; %u redundant tree insert/remove pairs avoided in fixture\n",shortcuts);
}
'''
with tempfile.TemporaryDirectory(prefix='scheduler-fair-fastpath-') as directory:
    binary = str(Path(directory) / 'test')
    subprocess.run(['cc', '-std=gnu11', '-O2', '-Wall', '-Wextra', '-Werror',
                    '-Wno-unused-function', '-fsanitize=address,undefined',
                    '-fno-pie', '-no-pie', '-I' + str(root / 'include'),
                    '-x', 'c', '-', '-o', binary], input=program, text=True, check=True)
    subprocess.run([binary], check=True)
