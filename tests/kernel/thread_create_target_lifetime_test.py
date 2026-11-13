"""Exercise actual syscall VM validation and failed-target cleanup blocks."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'kernel/syscall/process.c').read_text()
start = source.index('    /* The process reference does not retain a VM')
faults = source[start:source.index('    thread_t *thread = 0;', start)]
start = source.index('    g_thread_create_stage = 5U;', start)
cleanup = source[start:source.index('    status = handle_create(', start)]
fixture = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdatomic.h>
#include <stdarg.h>
#define K_OK 0
#define K_EBUSY -16
#define K_ECANCELED -125
#ifndef OS_DEBUG_SERIAL
#define OS_DEBUG_SERIAL 1
#endif
typedef struct {unsigned refs;} vm_space_t;
typedef struct {unsigned refs; vm_space_t *vm; atomic_uint state; unsigned thread_count; uint64_t pid;} process_t;
typedef struct {unsigned refs;} thread_t;
static bool held,detach,drop_owner;
static unsigned calls,fail_at,destroyed,process_destroyed,terminated,g_thread_create_stage;
static process_t *active;
static void process_thread_lock(process_t *p) {assert(p->refs && !held);held=true;}
static void process_thread_unlock(process_t *p) {assert(p->refs && held);held=false;}
static void vm_space_get(vm_space_t *s) {assert(held && s->refs);++s->refs;}
static void vm_space_put(vm_space_t *s) {assert(!held);if(s && !--s->refs){++destroyed;free(s);}}
static void object_put(void *p) {assert(!held);unsigned *refs=p;assert(*refs);if(!--*refs){++process_destroyed;free(p);}}
static int thread_create_diag_fail(unsigned step,int status) {assert(step>=5 && step<=7);return status;}
static int vm_handle_fault(vm_space_t *s,void *fault) {
 assert(!held && fault);++calls;
 if(!s)return -22;
 assert(s->refs);
 if(calls==1 && detach) {process_thread_lock(active);assert(active->vm==s);active->vm=NULL;process_thread_unlock(active);vm_space_put(s);}
 if(calls==1 && drop_owner)object_put(active);
 assert(s->refs);return calls==fail_at?-12:0;
}
static int thread_terminate(thread_t *t,int code) {assert(t->refs && code==K_ECANCELED);++terminated;return 0;}
static void kernel_serial_printf_serial_only(const char *fmt,...) {(void)fmt;}
static int validate(process_t *target,bool target_referenced) {
 int status;int entry_fault=1,stack_fault=2;
 @FAULTS@
 return status;
}
static int finish(process_t *target,process_t *caller,thread_t *thread,int status,bool target_referenced) {
 @CLEANUP@
 return status;
}
int main(void) {
 for(unsigned mode=0;mode<12;++mode) {
  calls=destroyed=process_destroyed=terminated=0;fail_at=mode%3;detach=mode>=3 && mode<9;drop_owner=mode>=6;
  active=calloc(1,sizeof(*active));assert(active);active->refs=2;active->pid=123;
  vm_space_t *space=malloc(sizeof(*space));assert(space);space->refs=1;active->vm=space;
  int result=validate(active,true);
  assert(result==(fail_at?-12:0));assert(calls==(fail_at==1?1:2));
  if(result==0) {process_t caller={.refs=1,.pid=1};assert(finish(active,&caller,NULL,K_EBUSY,true)==K_EBUSY);}
  if(!drop_owner) {if(active->vm){space=active->vm;active->vm=NULL;vm_space_put(space);}object_put(active);}
  else if(!detach)vm_space_put(space);
  assert(destroyed==1 && process_destroyed==1 && !held);
 }
 process_t p={.refs=1};calls=0;assert(validate(&p,false)==-22 && calls==1);
 for(unsigned mode=0;mode<2;++mode) {
  process_destroyed=terminated=0;
  active=calloc(1,sizeof(*active));assert(active);active->refs=1;
  thread_t *thread=malloc(sizeof(*thread));assert(thread);thread->refs=1;
  process_t caller={.refs=1};
  assert(finish(active,&caller,thread,-22,mode!=0)==-22);
  assert(terminated==1 && process_destroyed==1+mode && !held);
  if(!mode)object_put(active);
 }
 return 0;
}
'''

def run(directory, name, fault_block, cleanup_block, debug=1):
    path = directory / (name + '.c')
    path.write_text(fixture.replace('@FAULTS@', fault_block).replace('@CLEANUP@', cleanup_block))
    binary = directory / name
    subprocess.run(['cc', '-std=c11', '-g', '-fsanitize=address,undefined',
                    f'-DOS_DEBUG_SERIAL={debug}',
                    '-fno-pie', '-no-pie', str(path), '-o', str(binary)], check=True)
    return subprocess.run([str(binary)], capture_output=True, text=True)

with tempfile.TemporaryDirectory() as directory:
    directory = Path(directory)
    result = run(directory, 'retained', faults, cleanup)
    assert result.returncode == 0, result.stderr
    result = run(directory, 'retained_release', faults, cleanup, debug=0)
    assert result.returncode == 0, result.stderr
    # 缺少固定引用会复现退出操作分离所有者时访问已释放 VM 的问题。
    broken = faults.replace('if (space != 0) vm_space_get(space);', '').replace('vm_space_put(space);', '')
    result = run(directory, 'borrowed_vm', broken, cleanup)
    assert result.returncode != 0 and 'heap-use-after-free' in result.stderr, result.stderr
    # 在诊断前释放最后一个查找引用会复现目标对象 UAF。
    broken = cleanup.replace('        if (target_referenced) object_put(target);', '')
    broken = 'if (target_referenced) object_put(target);\n' + broken
    result = run(directory, 'early_target_put', faults, broken)
    assert result.returncode != 0 and 'heap-use-after-free' in result.stderr, result.stderr
print('thread create target lifetime: exit/fault failures and diagnostic UAF negatives passed')
