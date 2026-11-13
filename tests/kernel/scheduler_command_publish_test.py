"""Production publisher capacities, rejection isolation and overflow routing."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'kernel/sched/core.c').read_text()
begin = source.index('static inline bool scheduler_command_publish_snapshot(')
end = source.index('static bool scheduler_remote_command_push(', begin)
program = '#include "' + str(root / 'kernel/sched/internal.h') + '"\n' + r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
static unsigned loads;
static unsigned uncounted_load(const atomic_uint *index,memory_order order) {
    return atomic_load_explicit(index,order);
}
static unsigned counted_load(const atomic_uint *index,memory_order order) {
    ++loads;return uncounted_load(index,order);
}
#undef atomic_load_explicit
#define atomic_load_explicit(index,order) counted_load(index,order)
''' + source[begin:end] + r'''
#undef atomic_load_explicit
#define atomic_load_explicit(index,order) uncounted_load(index,order)
static thread_t thread;
static sched_cmd_ring_t primary;
static sched_cmd_overflow_ring_t overflow;
static void fixture(bool large,uint32_t start,uint32_t count) {
    atomic_uint *head=large?&overflow.head:&primary.head;
    atomic_uint *tail=large?&overflow.tail:&primary.tail;
    sched_cmd_t *slots=large?overflow.slots:primary.slots;
    unsigned capacity=large?SCHED_CMD_OVERFLOW_SLOTS:SCHED_CMD_SLOTS;
    memset(slots,0xa5,capacity*sizeof(*slots));
    sched_cmd_t before[SCHED_CMD_OVERFLOW_SLOTS];
    memcpy(before,slots,capacity*sizeof(*slots));
    atomic_store(head,start);atomic_store(tail,start+count);loads=0;
    bool accepted=large?scheduler_command_overflow_try_push(&overflow,&thread,9U,UINT32_MAX):
                        scheduler_command_ring_try_push(&primary,&thread,9U,UINT32_MAX);
    assert(loads==2&&accepted==(count<capacity));
    assert(atomic_load(head)==start);
    assert(atomic_load(tail)==start+count+accepted);
    if(accepted) {
        unsigned slot=(start+count)&(capacity-1U);
        before[slot]=(sched_cmd_t){.object=(uintptr_t)&thread,.op=9U,.generation=UINT32_MAX};
    }
    assert(!memcmp(before,slots,capacity*sizeof(*slots)));
}
int main(void) {
    unsigned cases=0;
    for(unsigned large=0;large<2;++large) {
        unsigned capacity=large?SCHED_CMD_OVERFLOW_SLOTS:SCHED_CMD_SLOTS;
        for(unsigned offset=0;offset<capacity;++offset)
        for(unsigned count=0;count<=capacity+1U;++count) {
            fixture(large,offset,count);fixture(large,UINT32_MAX-offset,count);cases+=2;
        }
    }
    loads=0;
    assert(!scheduler_command_ring_try_push(0,&thread,1,0));
    assert(!scheduler_command_ring_try_push(&primary,0,1,0));
    assert(!scheduler_command_overflow_try_push(0,&thread,1,0));
    assert(!scheduler_command_overflow_try_push(&overflow,0,1,0));
    assert(!loads);
    sched_cmd_channel_t channel={0};
    atomic_init(&channel.primary.head,UINT32_MAX-3U);
    atomic_init(&channel.primary.tail,UINT32_MAX-3U);
    atomic_init(&channel.overflow.head,UINT32_MAX-31U);
    atomic_init(&channel.overflow.tail,UINT32_MAX-31U);
    for(unsigned round=0;round<1000;++round) {
        uint32_t p=atomic_load(&channel.primary.tail),o=atomic_load(&channel.overflow.tail);
        for(unsigned n=0;n<SCHED_CMD_SLOTS+SCHED_CMD_OVERFLOW_SLOTS;++n) {
            bool overflowed=true;loads=0;
            assert(scheduler_command_channel_try_push(&channel,&thread,1U,n,&overflowed));
            assert(overflowed==(n>=SCHED_CMD_SLOTS));
            assert(loads==(n==SCHED_CMD_SLOTS?4U:2U));
        }
        bool overflowed=false;loads=0;
        assert(!scheduler_command_channel_try_push(&channel,&thread,1U,999,&overflowed));
        assert(overflowed&&loads==2&&channel.overflow_active);
        /* 溢出队列仍非空时释放主队列槽位：即使主队列已有空位，也要继续留在溢出队列中
         * 以保持顺序。 */
        atomic_store(&channel.primary.head,atomic_load(&channel.primary.tail));loads=0;
        assert(!scheduler_command_channel_try_push(&channel,&thread,1U,999,0));
        assert(loads==2&&channel.overflow_active);
        for(unsigned n=0;n<SCHED_CMD_SLOTS;++n) {
            sched_cmd_t entry=channel.primary.slots[(p+n)&SCHED_CMD_MASK];
            assert(entry.object==(uintptr_t)&thread&&entry.op==1&&entry.generation==n);
        }
        for(unsigned n=0;n<SCHED_CMD_OVERFLOW_SLOTS;++n) {
            sched_cmd_t entry=channel.overflow.slots[(o+n)&SCHED_CMD_OVERFLOW_MASK];
            assert(entry.object==(uintptr_t)&thread&&entry.op==1&&entry.generation==n+SCHED_CMD_SLOTS);
        }
        atomic_store(&channel.overflow.head,atomic_load(&channel.overflow.tail));
        loads=0;overflowed=true;
        assert(scheduler_command_channel_try_push(&channel,&thread,1U,123,&overflowed));
        assert(loads==4&&!overflowed&&!channel.overflow_active);
        atomic_store(&channel.primary.head,atomic_load(&channel.primary.tail));
    }
    bool untouched=true;loads=0;
    assert(!scheduler_command_channel_try_push(0,&thread,1,0,&untouched));
    assert(!scheduler_command_channel_try_push(&channel,0,1,0,&untouched));
    assert(untouched&&!loads);
    puts("Active overflow publication: 4 index reads -> 2; full rejection, primary reuse and FIFO routing preserved");
    printf("Publishers: %u offset/capacity/full/wrap cases and 1000 channel cycles; invalid inputs and rejected slots unchanged\n",cases);
}
'''
with tempfile.TemporaryDirectory(prefix='liteos-command-publish-') as directory:
    binary = str(Path(directory) / 'test')
    subprocess.run(['cc', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
                    '-I' + str(root / 'include'), '-x', 'c', '-', '-o', binary],
                   input=program, text=True, check=True)
    subprocess.run([binary], check=True, timeout=60)
