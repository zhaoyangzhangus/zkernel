"""Futex 候选条目锁外初始化、并发发布与引用回收。"""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'kernel/sync/futex.c').read_text()
def function(name):
    start = source.index(name)
    return source[start:source.index('\n}', start) + 2]
layout = source[source.index('#define FUTEX_BUCKET_COUNT'):source.index('/*\n * Futex')]
code = r'''
#define _XOPEN_SOURCE 700
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <kernel/wait.h>
#define __user
#define X86_64_USER_TOP 0x00007fffffffffffULL
typedef struct vm_space { atomic_uint refs; } vm_space_t;
typedef struct process { atomic_uint refs; } process_t;
''' + layout + r'''
static _Thread_local unsigned lock_depth;
static atomic_uint allocations, frees, initialized;
static bool synchronize_allocations, private_mode, fail_allocation;
static kstatus_t resolve_status;
static pthread_barrier_t allocated, acquired;
static vm_space_t space;
static process_t process;
static void sched_preempt_disable(void) { ++lock_depth; }
static void sched_preempt_enable(void) { assert(lock_depth); --lock_depth; }
static void *kzalloc(size_t bytes, unsigned flags) {
 assert(!lock_depth && !flags);
 if(fail_allocation)return 0;
 void *p=calloc(1,bytes);assert(p);atomic_fetch_add(&allocations,1);
 if(synchronize_allocations) {
  int status=pthread_barrier_wait(&allocated);
  assert(!status || status==PTHREAD_BARRIER_SERIAL_THREAD);
 }
 return p;
}
static void kfree(void *p) {
 assert(!lock_depth);
 if(p){atomic_fetch_add(&frees,1);free(p);}
}
void wait_queue_init(wait_queue_t *q) {
 assert(!lock_depth);
 spinlock_init(&q->lock);list_init(&q->waiters);atomic_init(&q->sequence,0);
 atomic_fetch_add(&initialized,1);
}
static void vm_space_get(vm_space_t *p) { assert(p==&space);atomic_fetch_add(&p->refs,1); }
static void vm_space_put(vm_space_t *p) { assert(p==&space);assert(atomic_fetch_sub(&p->refs,1)>1); }
static void object_get(void *raw) { process_t *p=raw;assert(p==&process);atomic_fetch_add(&p->refs,1); }
static void object_put(void *raw) { process_t *p=raw;assert(p==&process);assert(atomic_fetch_sub(&p->refs,1)>1); }
''' + '\n'.join(function(name) for name in [
    'static void futex_bucket_lock(', 'static void futex_bucket_unlock(',
    'static futex_entry_t *entry_from_node(', 'static void futex_init(',
    'static uint32_t futex_hash(', 'static futex_entry_t *find_locked(']) + r'''
static kstatus_t futex_resolve_key(process_t *p,vaddr_t address,bool private_key,futex_key_t *key) {
 assert(p==&process && address==0x10000 && private_key==private_mode);
 if(resolve_status!=K_OK)return resolve_status;
 *key=(futex_key_t){address,private_key?&space:0,private_key};return K_OK;
}
''' + function('static kstatus_t futex_get(') + '\n' + function('static void futex_put(') + r'''
static uintptr_t results[8];
static void *worker(void *raw) {
 unsigned index=(unsigned)(uintptr_t)raw;futex_entry_t *entry=0;
 assert(futex_get(&process,0x10000,true,private_mode,&entry)==K_OK);
 results[index]=(uintptr_t)entry;
 assert(entry->key==0x10000 && entry->process==&process);
 assert(entry->private_key==private_mode && entry->space==(private_mode?&space:0));
 assert(list_empty(&entry->waitq.waiters) && !atomic_load(&entry->waitq.sequence));
 int status=pthread_barrier_wait(&acquired);
 assert(!status || status==PTHREAD_BARRIER_SERIAL_THREAD);
 futex_put(entry);return 0;
}
static void verify_empty(void) {
 assert(!lock_depth && atomic_load(&space.refs)==1 && atomic_load(&process.refs)==1);
 assert(atomic_load(&allocations)==atomic_load(&frees));
 for(unsigned i=0;i<FUTEX_BUCKET_COUNT;++i)assert(list_empty(&g_futex_buckets[i].entries));
}
int main(void) {
 atomic_init(&space.refs,1);atomic_init(&process.refs,1);futex_init();
 assert(!pthread_barrier_init(&allocated,0,8));
 assert(!pthread_barrier_init(&acquired,0,8));
 for(unsigned mode=0;mode<2;++mode) {
  private_mode=mode;
  for(unsigned round=0;round<32;++round) {
   synchronize_allocations=true;
   unsigned before=atomic_load(&allocations), init_before=atomic_load(&initialized);
   pthread_t threads[8];
   for(unsigned i=0;i<8;++i)assert(!pthread_create(&threads[i],0,worker,(void *)(uintptr_t)i));
   for(unsigned i=0;i<8;++i)assert(!pthread_join(threads[i],0));
   for(unsigned i=1;i<8;++i)assert(results[i]==results[0]);
   assert(atomic_load(&allocations)==before+8 && atomic_load(&initialized)==init_before+8);
   verify_empty();
  }
  synchronize_allocations=false;
  futex_entry_t *entry=(void *)1;
  unsigned before=atomic_load(&allocations);
  assert(futex_get(&process,0x10000,false,private_mode,&entry)==K_ENOENT && entry==(void *)1);
  fail_allocation=true;
  assert(futex_get(&process,0x10000,true,private_mode,&entry)==K_ENOMEM && entry==(void *)1);
  fail_allocation=false;resolve_status=K_EACCES;
  assert(futex_get(&process,0x10000,true,private_mode,&entry)==K_EACCES && entry==(void *)1);
  resolve_status=K_OK;assert(atomic_load(&allocations)==before);verify_empty();
  assert(futex_get(&process,0x10000,true,private_mode,&entry)==K_OK);
  futex_entry_t *alias=0;before=atomic_load(&allocations);
  assert(futex_get(&process,0x10000,false,private_mode,&alias)==K_OK && alias==entry);
  assert(entry->refs==2 && atomic_load(&allocations)==before);
  futex_put(entry);assert(alias->refs==1);futex_put(alias);verify_empty();
 }
 pthread_barrier_destroy(&allocated);pthread_barrier_destroy(&acquired);
 puts("PASS: 64 eight-thread publication races, 512 initialized candidates, private/shared lifetime, existing/absent lookup, OOM and key failure; initialization never holds bucket lock");
}
'''
with tempfile.TemporaryDirectory(prefix='futex-entry-lifecycle-') as directory:
    path = Path(directory)
    (path / 'test.c').write_text(code)
    subprocess.run(['cc','-std=c11','-O2','-Wall','-Wextra','-Werror','-pthread',
                    '-I'+str(root/'include'),'-fsanitize=address,undefined',
                    '-fno-pie','-no-pie',str(path/'test.c'),'-o',str(path/'test')],check=True)
    subprocess.run([str(path/'test')],check=True,timeout=30)
    negative = code.replace('        wait_queue_init(&candidate->waitq);\n', '')
    negative = negative.replace('            list_add_tail(&bucket->entries, &candidate->node);',
                                '            wait_queue_init(&candidate->waitq);\n'
                                '            list_add_tail(&bucket->entries, &candidate->node);')
    assert negative != code
    (path / 'test.c').write_text(negative)
    subprocess.run(['cc','-std=c11','-O2','-Wall','-Wextra','-Werror','-pthread',
                    '-I'+str(root/'include'),'-fsanitize=address,undefined',
                    '-fno-pie','-no-pie',str(path/'test.c'),'-o',str(path/'test')],check=True)
    result = subprocess.run([str(path/'test')],capture_output=True,timeout=30)
    assert result.returncode != 0
    assert b'!lock_depth' in result.stderr
    print('Negative control rejected: wait queue initialization inside bucket lock')
