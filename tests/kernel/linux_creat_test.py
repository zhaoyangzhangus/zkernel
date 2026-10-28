"""验证生产 creat 分派的参数、截断标志及错误透传。"""
from pathlib import Path
import subprocess
import tempfile
root = Path(__file__).resolve().parents[2]
source = (root / 'kernel/syscall/linux.c').read_text()
start = source.index('    case LINUX_SYS_CREAT:')
case = source[start:source.index('    case LINUX_SYS_OPENAT:', start)]
program = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <uapi/linux_abi.h>
typedef struct { int unused; } process_t;
typedef struct {uint64_t rax,rdi,rsi,rdx;} arch_trap_frame_t;
static int64_t expected, calls;
static uint64_t saved_path,saved_flags,saved_mode;
static int64_t linux_openat(process_t *p,int64_t dir,uint64_t path,uint64_t flags,uint64_t mode){
 assert(p && dir==LINUX_AT_FDCWD);++calls;
 saved_path=path;saved_flags=flags;saved_mode=mode;return expected;
}
static int64_t dispatch(process_t *process,arch_trap_frame_t *frame){
 switch(frame->rax){
''' + case + r'''
 default:return -38;
 }
}
int main(void){
 process_t process={0};
 arch_trap_frame_t frame={85,0x12345678,0666,0xdeadbeef};
 assert(LINUX_SYS_CREAT==85);
 const int64_t results[]={0,7,255,-14,-2,-13,-28,-24,-5};
 for(unsigned i=0;i<sizeof(results)/sizeof(results[0]);++i){
  expected=results[i];assert(dispatch(&process,&frame)==expected);
  assert(saved_path==frame.rdi && saved_mode==0666);
  assert(saved_flags==(LINUX_O_WRONLY|LINUX_O_CREAT|LINUX_O_TRUNC));
  assert(!(saved_flags&LINUX_O_EXCL));
 }
 assert(calls==9);
 frame.rsi=0600;expected=4;assert(dispatch(&process,&frame)==4&&saved_mode==0600);
 puts("creat: syscall 85, mode register, create/truncate flags and success/error propagation passed");
}
'''
with tempfile.TemporaryDirectory(prefix='linux-creat-') as directory:
    binary = str(Path(directory) / 'test')
    subprocess.run(['cc', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                    '-I'+str(root/'include'), '-x', 'c', '-', '-o', binary],
                   input=program, text=True, check=True)
    subprocess.run([binary], check=True)
