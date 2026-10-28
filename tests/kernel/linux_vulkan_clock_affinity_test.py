"""Production RAW/coarse clocks and oversized glibc CPU-set queries."""
from pathlib import Path
import subprocess
import tempfile
import resource

root=Path(__file__).resolve().parents[2]
source=(root/'kernel/syscall/linux.c').read_text()
clock=source[source.index('static int64_t linux_clock_gettime('):source.index('static int64_t linux_gettimeofday(')]
affinity=source[source.index('static int64_t linux_sched_getaffinity('):source.index('static int64_t linux_sigaction(')]
code=r'''
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <assert.h>
#include <stdbool.h>
#define __user
#define K_OK 0
#define K_EINVAL -22
#define K_EPERM -1
#define K_ESRCH -3
#define LINUX_EFAULT -14
#define OS_LINUX_CLOCK_REALTIME 0
#define OS_LINUX_CLOCK_MONOTONIC 1
typedef int kstatus_t;
typedef struct {int64_t seconds,nanoseconds;} os_timespec_t,linux_timespec_t;
typedef struct {uint64_t pid;} process_t;
typedef struct {process_t *process;struct {uint64_t runtime_ns;} sched;struct{uint64_t bits[4];}affinity;} thread_t;
typedef struct {uint64_t runtime_ns;} os_task_info_t;
static process_t process={7};
static thread_t thread={&process,{2000000003ULL},{{UINT64_MAX,UINT64_MAX,UINT64_MAX,UINT64_MAX}}};
static unsigned gets,puts;
static thread_t *linux_thread_reference_by_tid(uint64_t tid){if(tid&&tid!=7&&tid!=9)return 0;++gets;return &thread;}
static void object_put(thread_t *t){assert(t==&thread);++puts;}
static bool current_available=true;
static thread_t *sched_current_thread(void){return current_available?&thread:0;}
static int process_task_query(uint64_t pid,uint64_t tid,os_task_info_t *info){
 assert(pid==7&&!tid);info->runtime_ns=7000000009ULL;return 0;
}
static unsigned cpus=4,clock_id; static int fault,clock_error;
static void linux_zero(void *p,size_t n) {memset(p,0,n);}
static uint32_t x86_smp_discovered_count(void) {return cpus;}
static int kernel_clock_read(uint32_t id,os_timespec_t *v) {clock_id=id;v->seconds=7;v->nanoseconds=8;return clock_error;}
static int copy_to_user(void *p,const void *v,size_t n) {if(fault)return -14;memcpy(p,v,n);return 0;}
''' + clock + affinity + r'''
int main(void) {
    unsigned char mask[136]; memset(mask,0x77,sizeof(mask));
    assert(linux_sched_getaffinity(0,128,(uintptr_t)mask)==8);
    assert(*(uint64_t*)mask==15 && mask[8]==0x77 && mask[135]==0x77);
    assert(linux_sched_getaffinity(0,7,(uintptr_t)mask)==-22);
    cpus=65; assert(linux_sched_getaffinity(0,128,(uintptr_t)mask)==16);
    assert(((uint64_t*)mask)[0]==UINT64_MAX && ((uint64_t*)mask)[1]==1 && mask[16]==0x77);
    cpus=256; assert(linux_sched_getaffinity(0,128,(uintptr_t)mask)==32);
    assert(mask[31]==255 && mask[32]==0x77);
    assert(linux_sched_getaffinity(1,128,(uintptr_t)mask)==K_ESRCH);
    assert(linux_sched_getaffinity(7,128,(uintptr_t)mask)==32);
    assert(linux_sched_getaffinity(9,128,(uintptr_t)mask)==32);
    thread.affinity.bits[0]=5;
    assert(linux_sched_getaffinity(9,128,(uintptr_t)mask)==32&&*(uint64_t*)mask==5);
    assert(linux_sched_getaffinity(UINT64_MAX,128,(uintptr_t)mask)==K_EINVAL);
    linux_timespec_t tv;
    assert(!linux_clock_gettime(4,(uintptr_t)&tv) && clock_id==1 && tv.seconds==7 && tv.nanoseconds==8);
    assert(!linux_clock_gettime((45ULL<<32)|4,(uintptr_t)&tv) && clock_id==1);
    clock_error=-5;
    assert(linux_clock_gettime(4,(uintptr_t)&tv)==-5);clock_error=0;
    assert(!linux_clock_gettime(5,(uintptr_t)&tv) && clock_id==0 && tv.seconds==7);
    assert(!linux_clock_gettime(6,(uintptr_t)&tv) && clock_id==1 && tv.nanoseconds==8);
    assert(!linux_clock_gettime(3,(uintptr_t)&tv) && tv.seconds==2 && tv.nanoseconds==3);
    assert(!linux_clock_gettime(2,(uintptr_t)&tv) && tv.seconds==7 && tv.nanoseconds==9);
    assert(!linux_clock_gettime((45ULL<<32)|3,(uintptr_t)&tv) && tv.seconds==2);
    current_available=false;
    assert(linux_clock_gettime(3,(uintptr_t)&tv)==K_EPERM);current_available=true;
    assert(linux_clock_gettime(99,(uintptr_t)&tv)==-22);
    fault=1;
    assert(linux_clock_gettime(4,(uintptr_t)&tv)==-14);
    assert(linux_clock_gettime(5,(uintptr_t)&tv)==-14);
    assert(linux_sched_getaffinity(0,128,(uintptr_t)mask)==-14);
    assert(gets==puts);
}
'''
with tempfile.TemporaryDirectory(prefix='vulkan-clock-affinity-') as directory:
    base=Path(directory); (base/'test.c').write_text(code)
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror',str(base/'test.c'),'-o',str(base/'test')],check=True)
    subprocess.run([str(base/'test')],check=True)
    # Prove the new RAW assertions detect the exact missing-adapter regression,
    # rather than passing because the fixture accepts every clock identifier.
    raw_adapter='    if (clock_id == 4U) clock_id = OS_LINUX_CLOCK_MONOTONIC;'
    assert code.count(raw_adapter)==1
    (base/'old.c').write_text(code.replace(raw_adapter,''))
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror',str(base/'old.c'),'-o',str(base/'old')],check=True)
    def no_core():
        resource.setrlimit(resource.RLIMIT_CORE,(0,0))
    old=subprocess.run([str(base/'old')],capture_output=True,preexec_fn=no_core)
    assert old.returncode!=0 and b'linux_clock_gettime(4' in old.stderr
print('RAW/coarse clocks and CPU affinity: production adapters, RAW uses unadjusted monotonic source, errors propagate, 128-byte glibc query and exact write guards passed')
