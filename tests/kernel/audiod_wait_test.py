"""执行真实自检等待路径，覆盖完成、截止、系统调用失败和句柄回收。"""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'user/services/audiod/service.c').read_text()
start = source.index('static int64_t audiod_wait_complete(')
end = source.index('\nstatic void audiod_debug_write(', start)
code = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdbool.h>
#include <uapi/audio.h>
#include <uapi/syscall.h>
#include <uapi/wait.h>
static unsigned polls, waits, creates, closes, ready_after, deadline_after;
static int64_t completion_error, create_error, wait_error;
static bool invalid_timer;
static os_audio_control_t control;
static int64_t audiod_syscall_one(uint64_t number,uint64_t arg0) {
 assert(number==OS_SYS_HANDLE_CLOSE && arg0==42 && creates==1 && !closes);
 ++closes;return 0;
}
static int64_t audiod_syscall_two(uint64_t number,uint64_t arg0,uint64_t arg1) {
 assert(number==OS_SYS_AUDIO_CONTROL && arg0==7 && arg1==(uint64_t)&control);
 assert(polls==0 || waits==polls);
 ++polls;
 if(completion_error)return completion_error;
 return polls>ready_after?0:-11;
}
static int64_t audiod_syscall_three(uint64_t number,uint64_t a,uint64_t b,uint64_t c) {
 if(number==OS_SYS_TIMER_CREATE) {
  assert(a==500000000 && b==0 && polls==1 && !creates);
  ++creates;
  if(create_error)return create_error;
  if(!invalid_timer)*(os_handle_t *)c=42;
  return 0;
 }
 assert(number==OS_SYS_WAIT_ONE && a==42 && b==1000000 && c);
 assert(creates==1 && !closes && polls==waits+1);
 assert(waits<1000);
 ++waits;
 if(wait_error)return wait_error;
 return waits>=deadline_after?0:-110;
}
''' + source[start:end] + r'''
static void reset(void) {
 polls=waits=creates=closes=0;ready_after=0;deadline_after=500;
 completion_error=create_error=wait_error=0;invalid_timer=false;
}
int main(void) {
 reset();assert(audiod_wait_complete(7,&control)==0);
 assert(polls==1 && !creates && !waits && !closes);
 reset();completion_error=-5;
 assert(audiod_wait_complete(7,&control)==-5 && !creates);
 for(unsigned n=1;n<=500;++n) {
  reset();ready_after=n;
  assert(audiod_wait_complete(7,&control)==0);
  assert(polls==n+1 && waits==n && creates==1 && closes==1);
 }
 reset();ready_after=1000;
 assert(audiod_wait_complete(7,&control)==-110);
 assert(waits==500 && polls==501 && creates==1 && closes==1);
 reset();ready_after=3;create_error=-12;
 assert(audiod_wait_complete(7,&control)==-12 && !waits && !closes);
 reset();ready_after=3;invalid_timer=true;
 assert(audiod_wait_complete(7,&control)==-5 && !waits && !closes);
 int errors[]={-4,-5,-9};
 for(unsigned i=0;i<3;++i) {
  reset();ready_after=3;wait_error=errors[i];
  assert(audiod_wait_complete(7,&control)==errors[i]);
  assert(polls==1 && waits==1 && closes==1);
 }
 puts("audiod wait: 500 delayed completions, exact deadline race, timeout, syscall failures and timer ownership passed");
}
'''
with tempfile.TemporaryDirectory(prefix='audiod-wait-') as directory:
    binary = str(Path(directory) / 'test')
    subprocess.run(['cc', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
                    '-I' + str(root / 'include'), '-x', 'c', '-', '-o', binary],
                   input=code, text=True, check=True)
    subprocess.run([binary], check=True, timeout=10)
