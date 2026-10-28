"""执行生产休眠适配和分发，验证 glibc 230 号调用实际等待以及相对/绝对截止语义。"""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'kernel/syscall/linux.c').read_text()
adapter = source[source.index('static int64_t linux_nanosleep('):
                 source.index('static int64_t linux_proc_getdents64(')]
start = source.index('    case LINUX_SYS_NANOSLEEP:')
end = source.index('    case LINUX_SYS_SCHED_YIELD:', start)
dispatch = source[start:end]
program = r'''
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <uapi/linux_abi.h>
#define __user
#define K_OK 0
#define K_EINVAL -22
#define K_EOVERFLOW -75
#define K_ENOSYS -38
#define LINUX_EFAULT -14
typedef int kstatus_t;
typedef struct {int64_t seconds,nanoseconds;} linux_timespec_t,os_timespec_t;
typedef struct {bool initialized;} wait_queue_t;
static uint64_t delay;
static unsigned waits,clock_reads,last_clock;
static int wait_error,clock_error;
static os_timespec_t now={10,900000000};
static int copy_from_user(void *to,const void *from,size_t n){
 if((uintptr_t)from<4096)return -14;
 memcpy(to,from,n);return 0;
}
static int copy_to_user(void *to,const void *from,size_t n){
 if((uintptr_t)to<4096)return -14;
 memcpy(to,from,n);return 0;
}
static int kernel_clock_read(uint32_t clock,os_timespec_t *value){
 ++clock_reads;last_clock=clock;*value=now;return clock_error;
}
static void wait_queue_init(wait_queue_t *queue){queue->initialized=true;}
static bool linux_poll_never_ready(void *context){assert(!context);return false;}
static int wait_on_queue(wait_queue_t *queue,bool (*predicate)(void*),void *context,uint64_t ns){
 assert(queue->initialized&&ns&&ns<=3600000000000ULL&&!predicate(context));
 ++waits;delay=ns;
 /* 极长请求只验证首段，随后模拟取消，不能在测试中循环数百万次。 */
 return wait_error?wait_error:-110;
}
#define K_ETIMEDOUT -110
''' + adapter + r'''
typedef struct {uint64_t rdi,rsi,rdx,r10;} frame_t;
static int64_t dispatch_sleep(uint64_t number,frame_t *frame){
 switch(number){
''' + dispatch + r'''
 default:return K_ENOSYS;
 }
}
int main(void){
 linux_timespec_t request={0,500000},remain={77,88};
 frame_t frame={0,0,(uintptr_t)&request,(uintptr_t)&remain};
 /* 对应部署 libc 的真实参数：clock_nanosleep(CLOCK_REALTIME,0,req,rem)。 */
 assert(dispatch_sleep(230,&frame)==0&&delay==500000&&waits==1);
 assert(!remain.seconds&&!remain.nanoseconds&&!clock_reads);
 frame=(frame_t){(uintptr_t)&request,(uintptr_t)&remain,0,0};
 assert(dispatch_sleep(35,&frame)==0&&waits==2);
 for(unsigned clock=0;clock<=1;++clock){
  unsigned old_waits=waits;
  frame=(frame_t){clock,1,(uintptr_t)&request,1};
  assert(dispatch_sleep(230,&frame)==0&&waits==old_waits); /* 过去的绝对截止 */
  request=(linux_timespec_t){11,100000000};
  assert(dispatch_sleep(230,&frame)==0&&delay==200000000&&last_clock==clock);
  request=(linux_timespec_t){10,900000000};old_waits=waits;
  assert(dispatch_sleep(230,&frame)==0&&waits==old_waits);
  request=(linux_timespec_t){0,500000};
 }
 unsigned before=waits;
 assert(linux_nanosleep(4,0,(uintptr_t)&request,0)==K_EINVAL);
 assert(linux_nanosleep(2,0,(uintptr_t)&request,0)==K_EINVAL);
 assert(linux_nanosleep(0,2,(uintptr_t)&request,0)==K_EINVAL);
 assert(linux_nanosleep(0,0,1,0)==LINUX_EFAULT);
 request=(linux_timespec_t){-1,0};assert(linux_nanosleep(0,0,(uintptr_t)&request,0)==K_EINVAL);
 request=(linux_timespec_t){0,-1};assert(linux_nanosleep(0,0,(uintptr_t)&request,0)==K_EINVAL);
 request=(linux_timespec_t){0,1000000000};assert(linux_nanosleep(0,0,(uintptr_t)&request,0)==K_EINVAL);
 request=(linux_timespec_t){INT64_MAX,0};assert(linux_nanosleep(0,0,(uintptr_t)&request,0)==K_EOVERFLOW);
 request=(linux_timespec_t){0,0};assert(linux_nanosleep(0,0,(uintptr_t)&request,1)==LINUX_EFAULT);
 assert(linux_nanosleep(0,1,(uintptr_t)&request,1)==0&&waits==before);
 request=(linux_timespec_t){1,5};wait_error=-12;
 assert(linux_nanosleep(0,0,(uintptr_t)&request,0)==-12);wait_error=0;
 wait_error=-125;remain=(linux_timespec_t){0,0};
 assert(linux_nanosleep(0,0,(uintptr_t)&request,(uintptr_t)&remain)==-125);
 assert(remain.seconds==request.seconds&&remain.nanoseconds==request.nanoseconds);
 assert(linux_nanosleep(0,0,(uintptr_t)&request,1)==LINUX_EFAULT);wait_error=0;
 request=(linux_timespec_t){11,100000000};clock_error=-5;before=waits;
 assert(linux_nanosleep(1,1,(uintptr_t)&request,1)==-5&&waits==before);clock_error=0;
 request=(linux_timespec_t){18446744073LL,709551615};
 wait_error=-125;
 assert(linux_nanosleep(0,0,(uintptr_t)&request,0)==-125&&delay==3600000000000ULL);
 wait_error=0;
 request=(linux_timespec_t){3601,0};before=waits;
 assert(linux_nanosleep(0,0,(uintptr_t)&request,0)==0&&waits==before+2&&delay==1000000000);
}
'''
with tempfile.TemporaryDirectory(prefix='linux-clock-nanosleep-') as directory:
    base = Path(directory)
    fixture, binary = base / 'test.c', base / 'test'
    command = ['cc', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
               '-fsanitize=address,undefined',
               '-fno-pie', '-no-pie', '-I', str(root / 'include'), str(fixture), '-o', str(binary)]
    fixture.write_text(program)
    subprocess.run(command, check=True)
    subprocess.run([str(binary)], check=True, timeout=20)
    broken = program.replace(dispatch[dispatch.index('    case LINUX_SYS_CLOCK_NANOSLEEP:'):], '')
    assert broken != program
    fixture.write_text(broken)
    subprocess.run(command, check=True)
    result = subprocess.run([str(binary)], capture_output=True)
    assert result.returncode != 0 and b'dispatch_sleep(230' in result.stderr
print('Linux sleep: real 35/230 dispatch, blocking relative waits, absolute deadlines, ignored absolute remaining pointer, finite long-sleep chunks, zero timer objects, validation/cancellation and missing-230 negative control PASS')
