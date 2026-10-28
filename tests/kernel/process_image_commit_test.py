"""Actual initial ELF commit cannot resurrect a dead/released process VM."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'kernel/process/exec.c').read_text()
body = source[source.index('kstatus_t process_load_elf_image('):
              source.index('kstatus_t process_exec_from_vfs(')]
process_source = (root / 'kernel/process/process.c').read_text()
release = process_source[process_source.index('void process_release_runtime_resources('):
                         process_source.index('static bool process_destruction_self_test_round(')]
fixture = r'''
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#define K_OK 0
#define K_EINVAL -22
#define K_ENOMEM -12
#define K_EBUSY -16
#define PROCESS_RUNNING 1U
#define PROCESS_FLAG_RESOURCES_RELEASED 2U
#define OS_PROCESS_PERSONALITY_LINUX_X86_64 7U
#define OS_PROCESS_PERSONALITY_NATIVE 3U
typedef int kstatus_t;
typedef uint64_t vaddr_t;
typedef struct { unsigned owned; } linux_fd_entry_t;
typedef struct { unsigned identity; } vm_space_t;
typedef struct { unsigned dummy; } elf_exec_arguments_t;
typedef struct { bool linux_abi; uint64_t interpreter_base,brk_base; } user_elf_image_info_t;
typedef struct { uint64_t pid; uint32_t thread_count,flags,personality; atomic_uint state;
 pthread_mutex_t lock; vm_space_t *vm; bool linux_loader_pending;
 vaddr_t linux_brk_base,linux_brk;unsigned handles,linux_fds; } process_t;
static _Thread_local unsigned held;
static unsigned allocations,frees,new_puts,old_puts,stdio_calls,stdio_releases,lock_releases;
static int prepare_error;
static bool alloc_fail,pause_prepare,entered,allow;
static pthread_mutex_t gate=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t condition=PTHREAD_COND_INITIALIZER;
static vm_space_t *space(unsigned identity) {
 vm_space_t *v=malloc(sizeof(*v));assert(v);v->identity=identity;return v;
}
static void *kzalloc(size_t size,unsigned flags) { assert(!held && !flags);++allocations;return alloc_fail?NULL:calloc(1,size); }
static void kfree(void *p) { assert(!held && p);++frees;free(p); }
static void vm_space_put(vm_space_t *v) { assert(!held);if(!v)return;if(v->identity==1)++old_puts;else { assert(v->identity==2);++new_puts; } free(v); }
static void process_thread_lock(process_t *p) { assert(!held);pthread_mutex_lock(&p->lock);++held; }
static void process_thread_unlock(process_t *p) { assert(held==1);--held;pthread_mutex_unlock(&p->lock); }
static int copy_exec_arguments(void *a,void *b,elf_exec_arguments_t *out) { assert(!a && !b && out);return 0; }
static int prepare_elf_space(const void *image,size_t size,void *file,uint64_t file_size,
 vm_space_t **out,user_elf_image_info_t *info,const elf_exec_arguments_t *arguments,
 void *map,vaddr_t *stack,void *diag) {
 assert(!held && image==(void*)1 && size==17 && file_size==17 && !file && info && arguments && !map && stack && !diag);
 if(pause_prepare) {
  pthread_mutex_lock(&gate);entered=true;pthread_cond_broadcast(&condition);
  while(!allow)pthread_cond_wait(&condition,&gate);
  pthread_mutex_unlock(&gate);
 }
 if(prepare_error)return prepare_error;
 *out=space(2);*stack=0x4000;return 0;
}
static void linux_fd_table_reset_stdio(unsigned *table,linux_fd_entry_t previous[3]) {
 assert(held && table);++stdio_calls;
 for(unsigned i=0;i<3;++i)previous[i].owned=i+1;
}
static void linux_fd_table_release_stdio(linux_fd_entry_t previous[3]) {
 assert(!held && stdio_calls==1 && !stdio_releases);
 for(unsigned i=0;i<3;++i){assert(previous[i].owned==i+1);previous[i].owned=0;}
 ++stdio_releases;
}
static void handle_table_destroy(unsigned *table) { assert(!held && table); }
static void file_lock_release_owner(uint64_t pid) { assert(!held && pid==42); ++lock_releases; }
static void linux_fd_table_destroy(unsigned *table) { assert(!held && table); }
'''
checks = r'''
typedef struct { process_t *p; user_elf_image_info_t info; vaddr_t stack; int status; } job_t;
static void *load_job(void *opaque) {
 job_t *job=opaque;job->status=process_load_elf_image(job->p,(void*)1,17,&job->info,&job->stack);return NULL;
}
static void reset(void) { allocations=frees=new_puts=old_puts=stdio_calls=stdio_releases=lock_releases=0;alloc_fail=false;prepare_error=0;pause_prepare=false; }
int main(void) {
 unsigned cases=0;
 for(unsigned state=0;state<4;++state)for(unsigned flags=0;flags<2;++flags)
 for(unsigned count=0;count<3;++count)for(unsigned image=0;image<3;++image) {
  reset();process_t p={.thread_count=count,.flags=flags?2U:0,.personality=99,
   .vm=space(1),.linux_brk=0xaaaa,.linux_brk_base=0xbbbb};
  atomic_init(&p.state,state);pthread_mutex_init(&p.lock,NULL);
  user_elf_image_info_t info={.linux_abi=image!=0,.interpreter_base=image==2?0x9000:0,.brk_base=0x8000};vaddr_t stack=0;
  bool valid=state==PROCESS_RUNNING && !flags && !count;
  assert(process_load_elf_image(&p,(void*)1,17,&info,&stack)==(valid?0:K_EBUSY));
  assert(allocations==1 && frees==1 && !held);
  if(valid) {
   assert(p.vm->identity==2 && old_puts==1 && !new_puts && stdio_calls==1 && stdio_releases==1);
   assert(p.personality==(image==1?7U:3U) && p.linux_loader_pending==(image==2));
   assert(p.linux_brk==0x8000 && p.linux_brk_base==0x8000);
  } else {
   assert(p.vm->identity==1 && new_puts==1 && !old_puts && !stdio_calls);
   assert(p.personality==99 && p.linux_brk==0xaaaa && p.linux_brk_base==0xbbbb);
  }
  vm_space_put(p.vm);pthread_mutex_destroy(&p.lock);++cases;
 }
 // Real interleaving: prepared image returns only after owner VM teardown.
 for(unsigned round=0;round<100;++round) {
  reset();pause_prepare=true;entered=allow=false;
  process_t p={.pid=42,.vm=space(1)};atomic_init(&p.state,PROCESS_RUNNING);pthread_mutex_init(&p.lock,NULL);
  job_t job={.p=&p};pthread_t worker;assert(!pthread_create(&worker,NULL,load_job,&job));
  pthread_mutex_lock(&gate);while(!entered)pthread_cond_wait(&condition,&gate);pthread_mutex_unlock(&gate);
  atomic_store(&p.state,3);process_release_runtime_resources(&p);
  assert(!p.vm && old_puts==1 && !new_puts && lock_releases==1);
  pthread_mutex_lock(&gate);allow=true;pthread_cond_broadcast(&condition);pthread_mutex_unlock(&gate);
  assert(!pthread_join(worker,NULL));
  assert(job.status==K_EBUSY && !p.vm && new_puts==1 && !stdio_calls && frees==1);
  pthread_mutex_destroy(&p.lock);
 }
 reset();process_t p={.vm=space(1)};atomic_init(&p.state,PROCESS_RUNNING);pthread_mutex_init(&p.lock,NULL);
 user_elf_image_info_t info={0};vaddr_t stack=0;
 assert(process_load_elf_image(NULL,(void*)1,17,&info,&stack)==K_EINVAL);
 assert(process_load_elf_image(&p,(void*)1,17,NULL,&stack)==K_EINVAL);
 assert(process_load_elf_image(&p,(void*)1,17,&info,NULL)==K_EINVAL && !allocations);
 alloc_fail=true;assert(process_load_elf_image(&p,(void*)1,17,&info,&stack)==K_ENOMEM && !frees);
 alloc_fail=false;prepare_error=-5;assert(process_load_elf_image(&p,(void*)1,17,&info,&stack)==-5);
 assert(p.vm->identity==1 && !stdio_calls && !old_puts && !new_puts);
 vm_space_put(p.vm);pthread_mutex_destroy(&p.lock);
 printf("%u commit states and 100 loading/exit races: no resurrection, exact cleanup and image metadata passed\n",cases);
}
'''
with tempfile.TemporaryDirectory(prefix='liteos-image-commit-') as directory:
    path = Path(directory)
    def run(actual):
        (path / 'test.c').write_text(fixture + actual + release + checks)
        subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-pthread',
                        '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
                        str(path / 'test.c'), '-o', str(path / 'test')], check=True)
        return subprocess.run([str(path / 'test')], capture_output=True, text=True)
    result = run(body)
    assert result.returncode == 0, result.stdout + result.stderr
    print(result.stdout.strip())
    start = body.index('if (process->thread_count != 0 ||')
    end = body.index(' {', start)
    negative = body[:start] + 'if (process->thread_count != 0)' + body[end:]
    result = run(negative)
    assert result.returncode != 0 and 'Assertion' in result.stderr
    print('Negative control rejected: old thread-count-only gate commits into a dead process')
    negative=body.replace('    process_thread_unlock(process);\n    linux_fd_table_release_stdio(previous_stdio);',
                          '    linux_fd_table_release_stdio(previous_stdio);\n    process_thread_unlock(process);')
    result=run(negative)
    assert result.returncode != 0 and '!held' in result.stderr,result.stderr
    print('Negative control rejected: stdio releases run under the process lock')
