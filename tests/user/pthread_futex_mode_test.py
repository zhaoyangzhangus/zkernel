"""Production pthread futex helpers select matching private/shared ABI keys."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root/'user/libc/pthread.c').read_text()
helpers = source[source.index('static uint64_t linux_futex_timeout_pointer('):
                 source.index('static void key_lock(')]
code = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <time.h>
#include <uapi/syscall.h>
#include <uapi/linux_abi.h>
#define OS_WAIT_INFINITE UINT64_MAX
static bool linux_mode,shared_mode,waking;
static int return_error;
static volatile uint32_t word;
static uint64_t expected_timeout;
static bool pthread_linux_personality(void){return linux_mode;}
static int pthread_status_error(int64_t value){return value<0?(int)-value:0;}
static int64_t __libc_native_syscall6(uint64_t number,uint64_t a,uint64_t b,
 uint64_t c,uint64_t d,uint64_t e,uint64_t f){
 assert(!linux_mode && a==(uintptr_t)&word && b==7 && !e && !f);
 assert(number==(waking?OS_SYS_FUTEX_WAKE:OS_SYS_FUTEX_WAIT));
 if(waking){assert(c==(shared_mode?OS_FUTEX_FLAG_SHARED:0) && !d);}
 else{assert(c==expected_timeout && d==(shared_mode?OS_FUTEX_FLAG_SHARED:0));}
 return return_error;
}
static int64_t __libc_linux_syscall6(uint64_t number,uint64_t a,uint64_t b,
 uint64_t c,uint64_t d,uint64_t e,uint64_t f){
 assert(linux_mode && number==LINUX_SYS_FUTEX && a==(uintptr_t)&word && c==7 && !e && !f);
 assert(b==((waking?LINUX_FUTEX_WAKE:LINUX_FUTEX_WAIT)|(shared_mode?0:LINUX_FUTEX_PRIVATE_FLAG)));
 if(waking || expected_timeout==OS_WAIT_INFINITE){assert(!d);}
 else{struct timespec *t=(void *)(uintptr_t)d;assert(t && t->tv_sec==(int64_t)(expected_timeout/1000000000ULL) && t->tv_nsec==(int64_t)(expected_timeout%1000000000ULL));}
 return return_error;
}
''' + helpers + r'''
int main(void){
 const uint64_t deadlines[]={0,1,12345678901ULL,OS_WAIT_INFINITE};
 for(unsigned mode=0;mode<2;++mode)for(unsigned shared=0;shared<2;++shared){
  linux_mode=mode;shared_mode=shared;
  for(unsigned i=0;i<4;++i)for(unsigned fail=0;fail<2;++fail){
   expected_timeout=deadlines[i];return_error=fail?-110:0;waking=false;
   assert(futex_wait_mode(&word,7,expected_timeout,shared)==(fail?110:0));
   waking=true;assert(futex_wake_mode(&word,7,shared)==(fail?110:0));
  }
 }
 for(unsigned mode=0;mode<2;++mode){
  linux_mode=mode;shared_mode=false;return_error=0;expected_timeout=1;waking=false;
  assert(!futex_wait(&word,7,1));waking=true;assert(!futex_wake(&word,7));
 }
}
'''
with tempfile.TemporaryDirectory(prefix='pthread-futex-mode-') as directory:
    path = Path(directory)
    for negative in (False, True):
        candidate = code
        if negative:
            candidate = candidate.replace('shared ? OS_FUTEX_FLAG_SHARED : 0U','0U')
            assert candidate != code
        (path/'test.c').write_text(candidate)
        subprocess.run(['cc','-std=gnu11','-O2','-Wall','-Wextra','-Werror',
                        '-Wno-unused-parameter','-fsanitize=address,undefined',
                        '-fno-pie','-no-pie','-I',str(root/'include'),
                        str(path/'test.c'),'-o',str(path/'test')],check=True)
        result = subprocess.run([str(path/'test')],capture_output=True)
        assert (result.returncode != 0) == negative, result.stderr
print('Pthread futex ABI: native/Linux shared/private wait/wake, timeout layout, errno and private wrappers PASS; old native flags rejected')
