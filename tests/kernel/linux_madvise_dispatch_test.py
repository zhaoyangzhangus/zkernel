"""Actual Linux madvise forwarding: preserve errno and VM reference ownership."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'kernel/syscall/linux.c').read_text()
case = source[source.index('    case LINUX_SYS_MADVISE: {'):
              source.index('    case LINUX_SYS_DUP:', source.index('    case LINUX_SYS_MADVISE: {'))]
assert 'OS_CHROME_MADVISE_FAIL' not in source
assert 'OS_DIAG_MADVISE_FAIL' not in source
program = r'''
#include <assert.h>
#include <stdint.h>
#include <stddef.h>
#define LINUX_SYS_MADVISE 28U
typedef uint64_t vaddr_t;
typedef int kstatus_t;
typedef struct {unsigned refs;} vm_space_t;
typedef struct {unsigned unused;} process_t;
typedef struct {uint64_t rdi,rsi,rdx;} frame_t;
static vm_space_t space;
static int result;
static unsigned puts,calls;
static uint64_t address,length;
static uint32_t advice;
static vm_space_t *process_vm_reference(process_t *p){assert(p);++space.refs;return &space;}
static int vm_advise(vm_space_t *s,vaddr_t a,size_t n,uint32_t f){
 assert(s==&space && space.refs==1);++calls;address=a;length=n;advice=f;return result;
}
static void vm_space_put(vm_space_t *s){assert(s==&space && space.refs==1);--space.refs;++puts;}
static int64_t dispatch(process_t *process,frame_t *frame){switch(LINUX_SYS_MADVISE){
''' + case + r'''
}return -38;}
int main(void){
 process_t process={0};
 const uint64_t values[]={0,1,4096,UINT32_MAX,UINT64_MAX};
 const int statuses[]={0,-22,-2,-12,-5,-75};
 for(unsigned a=0;a<5;++a)for(unsigned n=0;n<5;++n)
 for(unsigned f=0;f<5;++f)for(unsigned e=0;e<6;++e){
  frame_t frame={values[a],values[n],values[f]};result=statuses[e];
  assert(dispatch(&process,&frame)==result && !space.refs);
  assert(address==frame.rdi && length==frame.rsi && advice==(uint32_t)frame.rdx);
 }
 assert(calls==750 && puts==calls);
}
'''
with tempfile.TemporaryDirectory(prefix='linux-madvise-dispatch-') as directory:
    path = Path(directory)
    (path / 'test.c').write_text(program)
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
                    str(path / 'test.c'), '-o', str(path / 'test')], check=True)
    subprocess.run([str(path / 'test')], check=True)
    assert 'return status;' in program
    (path / 'test.c').write_text(program.replace('return status;', 'return 0;'))
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra',
                    str(path / 'test.c'), '-o', str(path / 'test')], check=True,
                   capture_output=True)
    result = subprocess.run([str(path / 'test')], capture_output=True)
    assert result.returncode != 0, 'Negative control swallowed madvise errors'
print('Linux madvise: 750 address/length/advice/errno cases, balanced VM references, no UART side effects and swallowed-error negative control PASS')
