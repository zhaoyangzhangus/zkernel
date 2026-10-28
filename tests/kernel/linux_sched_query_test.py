"""Actual task lookup and scheduler adapters, including Linux leader aliases."""
from pathlib import Path
import subprocess
import tempfile
import resource

root=Path(__file__).resolve().parents[2]
process=(root/'kernel/process/process.c').read_text()
lookup=process[process.index('uint64_t linux_thread_visible_tid('):process.index('kstatus_t process_task_query(')]
linux=(root/'kernel/syscall/linux.c').read_text()
adapter=linux[linux.index('static int64_t linux_sched_read('):linux.index('static int64_t linux_sigaction(')]
identity='''static int64_t identity_syscall(uint64_t number, uint64_t pointer) {
    thread_t *thread=current;
    process_t *process=thread ? thread->process : 0;
    struct {uint64_t rdi;} storage={pointer}, *frame=&storage;
    switch(number) {
'''+linux[linux.index('    case LINUX_SYS_GETTID:'):linux.index('    case LINUX_SYS_GETUID:')]+linux[linux.index('    case LINUX_SYS_SET_TID_ADDRESS:'):linux.index('    case LINUX_SYS_SET_ROBUST_LIST:')]+'''
    default:return K_ENOSYS;
    }
}
'''
assert 'case LINUX_SYS_SCHED_GETPARAM:' in linux
assert 'case LINUX_SYS_SCHED_GETSCHEDULER:' in linux
fixture=r'''
#include <assert.h>
#include <string.h>
#include <kernel/process.h>
#include <kernel/linux_abi.h>
#include <uapi/linux_abi.h>
#define LINUX_EFAULT -14
static list_head_t g_process_registry;
static thread_t *current;
static unsigned gets,puts,registry_depth,process_depth;
static bool fault;
static uint32_t x86_smp_discovered_count(void){return 4;}
static void linux_zero(void *p,size_t size){memset(p,0,size);}
thread_t *sched_current_thread(void) { return current; }
bool linux_process_is(process_t *p) { return p->personality==OS_PROCESS_PERSONALITY_LINUX_X86_64; }
void object_get(void *p) { assert(p);++gets; }
void object_put(void *p) { assert(p&&!registry_depth&&!process_depth);++puts; }
static void process_registry_lock(void) { assert(!process_depth);++registry_depth; }
static void process_registry_unlock(void) { assert(registry_depth&&!process_depth);--registry_depth; }
static void process_thread_lock(process_t *p) {
    assert(registry_depth || (current&&current->process==p));++process_depth;
}
static void process_thread_unlock(process_t *p) { (void)p;assert(process_depth);--process_depth; }
static kstatus_t copy_to_user(void *dst,const void *src,size_t size) {
    assert(!registry_depth&&!process_depth);
    if(fault||!dst)return -14;
    memcpy(dst,src,size);return 0;
}
'''
program=r'''
static process_t own,foreign;
static thread_t leader,sibling,other,dead;
static void add(thread_t *t,process_t *p,uint64_t tid) {
    t->process=p;t->tid=tid;t->base_sched_class=SCHED_CLASS_FAIR;
    t->affinity.bits[0]=15;
    atomic_init(&t->state,THREAD_RUNNING);list_add_tail(&p->threads,&t->process_node);
}
int main(void) {
    _Static_assert(LINUX_SYS_SCHED_GETPARAM==143&&LINUX_SYS_SCHED_GETSCHEDULER==145,"syscall ABI");
    list_init(&g_process_registry);list_init(&own.threads);list_init(&foreign.threads);
    own.pid=10;own.linux_thread_group_leader_tid=11;own.personality=OS_PROCESS_PERSONALITY_LINUX_X86_64;
    foreign.pid=20;foreign.linux_thread_group_leader_tid=21;foreign.personality=own.personality;
    list_add_tail(&g_process_registry,&own.global_node);list_add_tail(&g_process_registry,&foreign.global_node);
    add(&leader,&own,11);add(&sibling,&own,12);add(&other,&foreign,21);add(&dead,&foreign,22);
    atomic_store(&dead.state,THREAD_DEAD);current=&leader;
    assert(identity_syscall(LINUX_SYS_GETTID,0)==10);
    assert(identity_syscall(LINUX_SYS_SET_TID_ADDRESS,1234)==10);
    assert(leader.linux_clear_tid==1234&&leader.linux_clear_tid_pending);
    assert(identity_syscall(LINUX_SYS_SET_TID_ADDRESS,0)==10&&!leader.linux_clear_tid_pending);
    struct { uint32_t before;int32_t priority;uint32_t after; } result={0xaabbccdd,99,0x11223344};
    assert(linux_sched_read(0,false,(uintptr_t)&result.priority)==0&&result.priority==0);
    assert(result.before==0xaabbccdd&&result.after==0x11223344);
    assert(linux_sched_read(10,true,0)==0&&linux_sched_read(12,true,0)==0);
    assert(linux_sched_read(11,true,0)==K_ESRCH); /* Native leader ID is not its Linux TID. */
    assert(linux_sched_read(20,true,0)==0&&linux_sched_read(21,true,0)==K_ESRCH);
    assert(linux_sched_read(22,true,0)==K_ESRCH&&linux_sched_read(999,true,0)==K_ESRCH);
    uint64_t mask=0;
    assert(linux_sched_getaffinity(10,sizeof(mask),(uintptr_t)&mask)==8&&mask==15);
    sibling.affinity.bits[0]=5;
    assert(linux_sched_getaffinity(12,sizeof(mask),(uintptr_t)&mask)==8&&mask==5);
    assert(linux_sched_getaffinity(20,sizeof(mask),(uintptr_t)&mask)==8&&mask==15);
    assert(linux_sched_getaffinity(11,sizeof(mask),(uintptr_t)&mask)==K_ESRCH);
    assert(linux_sched_getaffinity(22,sizeof(mask),(uintptr_t)&mask)==K_ESRCH);
    assert(linux_sched_getaffinity((42ULL<<32)|10,sizeof(mask),(uintptr_t)&mask)==8);
    assert(linux_sched_read(UINT64_MAX,true,0)==K_EINVAL);
    assert(linux_sched_read((42ULL<<32)|10,true,0)==0); /* pid_t is 32-bit. */
    current=&sibling;assert(linux_sched_read(0,true,0)==0&&linux_sched_read(10,true,0)==0);
    assert(identity_syscall(LINUX_SYS_GETTID,0)==12&&identity_syscall(LINUX_SYS_SET_TID_ADDRESS,0)==12);
    sibling.base_sched_class=SCHED_CLASS_RT;sibling.base_rt_priority=0;
    assert(linux_sched_read(0,true,0)==2);
    assert(linux_sched_read(0,false,(uintptr_t)&result.priority)==0&&result.priority==99);
    sibling.base_rt_priority=31;
    assert(linux_sched_read(0,false,(uintptr_t)&result.priority)==0&&result.priority==68);
    sibling.base_sched_class=SCHED_CLASS_IDLE;assert(linux_sched_read(0,true,0)==5);
    sibling.base_sched_class=99;assert(linux_sched_read(0,true,0)==K_ENOSYS);
    sibling.base_sched_class=SCHED_CLASS_FAIR;
    fault=true;assert(linux_sched_read(0,false,(uintptr_t)&result.priority)==-14);
    fault=false;assert(linux_sched_read(0,false,0)==-14);
    assert(gets==puts&&!registry_depth&&!process_depth);
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='linux-scheduler-query-') as directory:
    base=Path(directory);source=base/'test.c';binary=base/'test'
    source.write_text(fixture+lookup+adapter+identity+program)
    subprocess.run(['cc','-std=gnu11','-Wall','-Wextra','-Werror','-I',str(root/'include'),str(source),'-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True,timeout=5)
    old_case=identity[identity.index('    case LINUX_SYS_SET_TID_ADDRESS:'):identity.index('    default:')]
    broken=identity.replace(old_case,old_case.replace(
        'return (int64_t)linux_thread_visible_tid(thread);',
        'return (int64_t)thread->tid;'))
    assert broken!=identity
    source.write_text(fixture+lookup+adapter+broken+program)
    subprocess.run(['cc','-std=gnu11','-Wall','-Wextra','-Werror','-I',str(root/'include'),str(source),'-o',str(binary)],check=True)
    def no_core():resource.setrlimit(resource.RLIMIT_CORE,(0,0))
    negative=subprocess.run([str(binary)],capture_output=True,timeout=5,preexec_fn=no_core)
    assert negative.returncode!=0 and b'LINUX_SYS_SET_TID_ADDRESS' in negative.stderr
print('Linux scheduler query: real lookup, leader aliases, sibling/foreign/dead tasks, priorities, 32-bit IDs, copy bounds and references passed')
