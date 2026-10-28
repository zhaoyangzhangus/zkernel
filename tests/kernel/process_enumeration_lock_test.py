"""Production enumeration releases registry lock before name/parent reads."""
from pathlib import Path
import subprocess
import tempfile
import resource

root = Path(__file__).resolve().parents[2]
source = (root / 'kernel/process/process.c').read_text()
name_helper = source[source.index('static void process_snapshot_name('):source.index('kstatus_t process_enumerate(')]
name_helper = name_helper.replace('    memcpy(name, process->name, copied);',
                                 '    assert(info && preemption && !registry && !threads);\n'
                                 '    memcpy(name, process->name, copied);')
functions = name_helper + source[source.index('kstatus_t process_enumerate('):
                   source.index('static process_t *process_monitor_find(')]
# Observe each list iteration, including skipped entries before list_entry.
iteration = '            if (remaining-- != 0U) continue;'
assert functions.count(iteration) == 1
functions = functions.replace(iteration, '            ++thread_visits;\n'+iteration)
monitor = source[source.index('static process_t *process_monitor_find('):
                 source.index('static bool process_monitor_protected(')]
monitor = monitor.replace('process_t *candidate = list_entry',
                          '++monitor_visits; process_t *candidate = list_entry')
functions += monitor
thread_monitor = source[source.index('static thread_t *process_monitor_thread_locked('):
                        source.index('uint64_t linux_thread_visible_tid(')]
thread_monitor = thread_monitor.replace('thread_t *thread = list_entry(node,',
                                        '++monitor_thread_visits; thread_t *thread = list_entry(node,')
functions += thread_monitor
code = r'''
#include <assert.h>
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <string.h>
#include <stdio.h>
#include <uapi/process.h>
#include <uapi/syscall.h>
#define K_OK 0
#define K_EINVAL -22
#define K_ENOENT -2
typedef int kstatus_t;
typedef struct list_head { struct list_head *next,*prev; } list_head_t;
#define list_entry(n,t,m) ((t *)((char *)(n)-offsetof(t,m)))
typedef struct process {
    list_head_t global_node,threads;
    list_head_t *monitor_thread_cursor,*enumeration_thread_cursor;
    uint32_t enumeration_thread_index;
    struct process *parent;
    uint64_t pid,create_time_ns;
    atomic_uint state;
    uint32_t thread_count;
    int64_t exit_code;
    unsigned info_lock,refs;
    char name[OS_PROCESS_NAME_MAX];
} process_t;
typedef struct {
    list_head_t process_node;
    uint64_t tid;
    atomic_uint state;
    uint32_t current_cpu;
} thread_t;
static list_head_t g_process_registry;
static list_head_t *g_process_enumeration_cursor;
static uint32_t g_process_enumeration_index;
static list_head_t *g_process_monitor_cursor;
static process_t process,parent,prefix,empty;
static thread_t thread;
static thread_t prefix_threads[128];
static unsigned thread_visits;
static unsigned monitor_visits;
static unsigned monitor_thread_visits;
static bool registry,threads,parent_locked,info,detach_on_unlock;
static unsigned preemption,name_updates;
static bool kernel_workers;
static kstatus_t sched_monitor_process(os_process_snapshot_t *s) {
 assert(!registry&&!threads&&!preemption);
 if(!kernel_workers)return K_ENOENT;
 *s=(os_process_snapshot_t){0};s->pid=OS_KERNEL_WORKERS_PID;return K_OK;
}
static kstatus_t sched_monitor_thread(uint32_t index,os_thread_snapshot_t *s) {
 assert(!registry&&!threads&&!preemption);
 if(!kernel_workers||index!=0)return K_ENOENT;
 *s=(os_thread_snapshot_t){0};s->tid=999;s->process_pid=OS_KERNEL_WORKERS_PID;return K_OK;
}
static void object_get(process_t *p) { assert(registry && (p==&process || p==&prefix)); ++p->refs; }
static void object_put(process_t *p) {
    assert(!registry && !threads && !info && !parent_locked && p->refs);
    --p->refs;
}
static void process_registry_lock(void) { assert(!registry); registry=true; }
static void process_registry_unlock(void) {
    assert(registry); registry=false;
    if(detach_on_unlock) {
        assert(process.refs==2);
        g_process_registry.next=&g_process_registry;
        g_process_enumeration_cursor=0;
        g_process_monitor_cursor=0;
        --process.refs; // concurrent removal releases the registry reference
    }
}
static void process_thread_lock(process_t *p) { assert(p && !threads); threads=true; }
static void process_thread_unlock(process_t *p) { assert(p && threads); threads=false; }
static void process_parent_lock(process_t *p) { assert(p==&process && p->refs && !registry); parent_locked=true; }
static void process_parent_unlock(process_t *p) { assert(p==&process && parent_locked); parent_locked=false; }
static void sched_preempt_disable(void) { ++preemption; }
static void sched_preempt_enable(void) { assert(preemption); --preemption; }
static void spinlock_lock(unsigned *p) {
    assert(((p==&process.info_lock && process.refs) ||
            (p==&prefix.info_lock && prefix.refs)) && !registry && !threads && preemption);
    info=true;
}
static void spinlock_unlock(unsigned *p) { assert((p==&process.info_lock || p==&prefix.info_lock) && info); info=false; }
static void process_set_name(process_t *destination,const char *name){
    assert(destination==&parent && !registry && !threads && !info && !preemption);
    memcpy(destination->name,name,sizeof(destination->name));++name_updates;
}
''' + functions + r'''
static void setup(void) {
    g_process_enumeration_cursor=0;
    g_process_monitor_cursor=0;
    process.enumeration_thread_cursor=0;
    prefix.enumeration_thread_cursor=0;
    g_process_registry.next=&process.global_node;
    process.global_node.next=&g_process_registry;
    process.threads.next=&thread.process_node;
    process.threads.prev=&thread.process_node;
    thread.process_node.next=&process.threads;
    thread.process_node.prev=&process.threads;
    process.refs=1;
    detach_on_unlock=false;
}
int main(void) {
    process.pid=20;parent.pid=10;process.parent=&parent;
    process.create_time_ns=123;process.thread_count=1;process.exit_code=-7;
    atomic_init(&process.state,OS_PROCESS_RUNNING);
    strcpy(process.name,"enumeration-test");
    setup();
    process_copy_name(0,&process);process_copy_name(&parent,0);assert(!name_updates);
    process_copy_name(&parent,&process);
    assert(name_updates==1 && !memcmp(parent.name,process.name,sizeof(parent.name)));
    thread.tid=21;thread.current_cpu=3;atomic_init(&thread.state,OS_THREAD_BLOCKED);
    /* Snapshot layouts may differ in size; copy all source bytes, including
     * embedded NULs, and zero only the larger destination's suffix. */
    setup();
    for(unsigned pattern=0;pattern<3;++pattern)for(unsigned capacity=0;capacity<=64;++capacity){
        unsigned char arena[80];memset(arena,0xa5,sizeof(arena));
        for(unsigned i=0;i<OS_PROCESS_NAME_MAX;++i)
            process.name[i]=pattern==0?0:pattern==1?(char)('a'+i%26):(char)(i%5?i+1:0);
        process_snapshot_name(&process,(char*)arena+8,capacity);
        for(unsigned i=0;i<sizeof(arena);++i){
            unsigned char expected=0xa5;
            if(i>=8 && i<8+capacity)expected=i-8<OS_PROCESS_NAME_MAX?(unsigned char)process.name[i-8]:0;
            assert(arena[i]==expected);
        }
        assert(!preemption && !info && process.refs==1);
    }
    strcpy(process.name,"enumeration-test");
    for(unsigned detach=0;detach<2;++detach) {
        os_process_snapshot_t p; os_thread_snapshot_t t;
        setup();detach_on_unlock=detach;
        assert(process_enumerate(0,&p)==0);
        assert(p.pid==20 && p.parent_pid==10 && p.create_time_ns==123 && p.exit_code==-7);
        assert(p.thread_count==1 && !strcmp(p.name,"enumeration-test"));
        assert(process.refs==(detach?0:1) && !registry && !preemption);
        setup();detach_on_unlock=detach;
        assert(process_enumerate_thread(0,&t)==0);
        assert(t.tid==21 && t.process_pid==20 && t.current_cpu==3 && t.state==OS_THREAD_BLOCKED);
        assert(!strcmp(t.name,"enumeration-test"));
        assert(process.refs==(detach?0:1) && !registry && !threads && !preemption);
    }
    setup();
    os_process_snapshot_t p;os_thread_snapshot_t t;
    assert(process_enumerate(1,&p)==K_ENOENT);
    assert(process_enumerate_thread(1,&t)==K_ENOENT);
    assert(process_enumerate(0,0)==K_EINVAL);
    assert(process_enumerate_thread(0,0)==K_EINVAL);
    assert(process.refs==1 && !registry && !threads && !preemption);
    g_process_registry.next=&g_process_registry;
    g_process_enumeration_cursor=0;
    assert(process_enumerate(0,&p)==K_ENOENT);
    assert(process_enumerate_thread(0,&t)==K_ENOENT);
    setup();
    prefix.pid=19;prefix.refs=1;prefix.thread_count=128;
    strcpy(prefix.name,"prefix-process");
    empty.thread_count=0;empty.threads.next=&empty.threads;
    g_process_registry.next=&empty.global_node;
    empty.global_node.next=&prefix.global_node;
    prefix.global_node.next=&process.global_node;
    prefix.threads.next=&prefix_threads[0].process_node;
    prefix.threads.prev=&prefix_threads[127].process_node;
    for(unsigned i=0;i<128;++i) {
        prefix_threads[i].tid=1000+i;
        atomic_init(&prefix_threads[i].state,OS_THREAD_RUNNING);
        prefix_threads[i].process_node.next=i==127 ? &prefix.threads : &prefix_threads[i+1].process_node;
        prefix_threads[i].process_node.prev=i?&prefix_threads[i-1].process_node:&prefix.threads;
    }
    thread_visits=0;
    assert(process_enumerate_thread(128,&t)==K_OK && t.tid==21 && t.process_pid==20);
    assert(thread_visits==1); // no traversal of the 128 preceding threads
    unsigned queries=0,visits=0,nearest_end_visits=0;
    for(unsigned count=1;count<=128;++count) {
        prefix.thread_count=count;
        /* 此夹具直接改链表，模拟真实增删时的缓存失效。 */
        prefix.enumeration_thread_cursor=0;
        prefix.threads.prev=&prefix_threads[count-1].process_node;
        for(unsigned i=0;i<count;++i) {
            prefix_threads[i].process_node.next=i+1==count?&prefix.threads:&prefix_threads[i+1].process_node;
            prefix_threads[i].process_node.prev=i?&prefix_threads[i-1].process_node:&prefix.threads;
        }
        for(unsigned i=0;i<count;++i) {
            thread_visits=0;
            assert(process_enumerate_thread(i,&t)==K_OK && t.tid==1000+i && t.process_pid==19);
            assert(!strcmp(t.name,"prefix-process"));
            monitor_visits=0;
            process_t *owner=process_monitor_find(t.process_pid);
            assert(owner==&prefix && !monitor_visits && prefix.refs==2);
            object_put(owner);
            monitor_thread_visits=0;
            process_thread_lock(&prefix);
            assert(process_monitor_thread_locked(&prefix,t.tid)==&prefix_threads[i]);
            process_thread_unlock(&prefix);
            assert(!monitor_thread_visits);
            unsigned expected=(i<count-1-i?i:count-1-i)+1;
            nearest_end_visits+=expected;
            if(i && expected>2)expected=2; /* 连续查询仅移动一个节点。 */
            assert(thread_visits==expected);
            visits+=thread_visits;++queries;
        }
        thread_visits=0;
        assert(process_enumerate_thread(count,&t)==K_OK && t.tid==21 && thread_visits==1);
        monitor_visits=0;
        process_t *owner=process_monitor_find(t.process_pid);
        assert(owner==&process && !monitor_visits && process.refs==2);
        object_put(owner);
    }
    assert(visits*5U<nearest_end_visits);
    printf("Thread enumeration: %u identities, %u list visits versus %u previous nearest-end visits\n",queries,visits,nearest_end_visits);
    /* 任意顺序仍按原始索引返回；从首、尾、缓存三个位置选最短距离。 */
    unsigned seed=19,previous=128;
    prefix.enumeration_thread_cursor=0;
    for(unsigned n=0;n<20000;++n) {
        seed=seed*1664525U+1013904223U;unsigned index=(seed>>8)%128U;
        unsigned distance=index<127U-index?index:127U-index;
        if(previous<128U) {
            unsigned cached=index>previous?index-previous:previous-index;
            if(cached<distance)distance=cached;
        }
        thread_visits=0;
        assert(process_enumerate_thread(index,&t)==K_OK && t.tid==1000+index);
        assert(thread_visits==distance+1U);previous=index;
    }
    thread_visits=0;
    assert(process_enumerate_thread(129,&t)==K_ENOENT);
    assert(process_enumerate_thread(UINT32_MAX,&t)==K_ENOENT);
    assert(thread_visits==0 && process.refs==1 && prefix.refs==1);
    assert(!registry && !threads && !info && !preemption);
    kernel_workers=true;
    assert(process_enumerate(3,&p)==K_OK&&p.pid==OS_KERNEL_WORKERS_PID);
    assert(process_enumerate(4,&p)==K_ENOENT);
    assert(process_enumerate_thread(129,&t)==K_OK&&t.tid==999&&t.process_pid==OS_KERNEL_WORKERS_PID);
    assert(process_enumerate_thread(130,&t)==K_ENOENT);
}
'''
with tempfile.TemporaryDirectory(prefix='liteos-enumeration-lock-') as directory:
    path = Path(directory)
    (path / 'test.c').write_text(code)
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
                    '-I'+str(root/'include'), str(path/'test.c'), '-o', str(path/'test')], check=True)
    subprocess.run([str(path/'test')], check=True)
    condition = 'if (process->enumeration_thread_cursor != 0) {'
    cache_index = 'process->enumeration_thread_index = target;'
    padding = 'if (capacity > copied) memset(name + copied, 0, capacity - copied);'
    assert padding in code
    assert condition in code and cache_index in code
    for label, negative in (
            ('unused enumeration cursor', code.replace(condition, 'if (false && process->enumeration_thread_cursor != 0) {')),
            ('wrong cursor index', code.replace(cache_index, 'process->enumeration_thread_index = 0;')),
            ('missing thread monitor hint', code.replace(
                'g_process_monitor_cursor = &process->global_node;', '')),
            ('missing enumerated TID hint', code.replace(
                'process->monitor_thread_cursor = thread_node;', '')),
            ('missing snapshot padding', code.replace(padding, '')),
            ('unlocked name read', code.replace('spinlock_lock(&process->info_lock);',
                                                'if (false) spinlock_lock(&process->info_lock);'))):
        (path/'negative.c').write_text(negative)
        subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                        '-I'+str(root/'include'), str(path/'negative.c'),
                        '-o', str(path/'negative')], check=True)
        result = subprocess.run([str(path/'negative')], capture_output=True, text=True,
                                preexec_fn=lambda: resource.setrlimit(resource.RLIMIT_CORE, (0, 0)))
        assert result.returncode != 0 and 'Assertion' in result.stderr, result.stderr
        print('Negative control rejected: '+label)
print('Process enumeration: short registry locks, concurrent removal references, counted list skipping, nearest cached/head/tail walks across 1..128 threads and empty/error cleanup passed')
