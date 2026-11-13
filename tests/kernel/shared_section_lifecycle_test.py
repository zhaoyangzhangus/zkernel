"""Production shared-section and object-header code with VM allocation hooks."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
section = (root / 'kernel/ipc/shared_section.c').read_text()
objects = (root / 'kernel/object/object.c').read_text()
objects = objects[:objects.index('static bool object_pin_wait_set(')]
wait = (root / 'kernel/sync/wait.c').read_text()
start = wait.index('void wait_queue_init(')
queue_init = wait[start:wait.index('\n}', start)+2]
program = r'''
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <kernel/shared_section.h>
#include <kernel/kmem.h>
static atomic_uint allocations,frees,vm_creates,vm_frees,wakes;
static bool fail_section;
static kstatus_t fail_vm;
static void wait_global_init(void) {}
''' + queue_init + '\n' + objects + '\n' + section + r'''
void *kzalloc(size_t size,uint32_t flags) {
 assert(!flags);if(fail_section)return 0;
 void *p=calloc(1,size);if(p)atomic_fetch_add(&allocations,1);return p;
}
void kfree(void *p) { if(p)atomic_fetch_add(&frees,1);free(p); }
kstatus_t vm_object_create_shared(size_t size,vm_object_t **out) {
 assert(size && size%PAGE_SIZE==0 && out && !*out);
 if(fail_vm)return fail_vm;
 vm_object_t *p=calloc(1,sizeof(*p));assert(p);
 refcount_init(&p->refs,1);p->type=VM_OBJECT_SHARED;p->size=size;
 *out=p;atomic_fetch_add(&vm_creates,1);return K_OK;
}
void vm_object_get(vm_object_t *p) { assert(atomic_fetch_add(&p->refs.value,1)); }
kstatus_t vm_object_create_shared_populated(size_t size,vm_object_t **out) {
 kstatus_t status=vm_object_create_shared(size,out);
 if(status==K_OK)atomic_store(&(*out)->populated_pages,size/PAGE_SIZE);
 return status;
}
void vm_object_put(vm_object_t *p) {
 unsigned count=atomic_fetch_sub(&p->refs.value,1);assert(count);
 if(count==1) {atomic_fetch_add(&vm_frees,1);free(p);}
}
uint32_t wake_all(wait_queue_t *q) {
 assert(q && list_empty(&q->waiters) && q->waiters.prev==&q->waiters);
 assert(!atomic_load(&q->lock.state) && !atomic_load(&q->sequence));
 atomic_fetch_add(&wakes,1);return 0;
}
static void check_header(shared_section_t *s,uint64_t size) {
 assert(s->object.type==KOBJECT_TYPE_SHARED_SECTION && !s->object.flags);
 assert(atomic_load(&s->object.refs.value)==1);
 assert(s->size==size && s->vm_object && s->vm_object->size==size);
 assert(atomic_load(&s->vm_object->refs.value)==1);
 assert(list_empty(&s->object.signal_waiters.waiters));
 assert(s->object.signal_waiters.waiters.prev==&s->object.signal_waiters.waiters);
 assert(!atomic_load(&s->object.signal_waiters.lock.state));
 assert(!atomic_load(&s->object.signal_waiters.sequence));
 assert(!object_is_signaled(s) && object_wait_value(s)==0);
}
static void *worker(void *unused) {
 (void)unused;
 for(unsigned i=0;i<1000;++i) {
  shared_section_t *s=0;uint64_t size=(1+i%17)*PAGE_SIZE;
  assert((i%2?shared_section_create_populated(size,&s):shared_section_create(size,&s))==K_OK);
  check_header(s,size);
  assert(atomic_load(&s->vm_object->populated_pages)==(i%2?size/PAGE_SIZE:0));
  object_notify_changed(s);
  object_get(s);assert(object_try_get(s));
  assert(atomic_load(&s->object.refs.value)==3);
  object_put(s);object_put(s);object_put(s);
 }
 return 0;
}
int main(void) {
 shared_section_t *out=(void*)1;
 uint64_t invalid[]={0,1,PAGE_SIZE-1,PAGE_SIZE+1,UINT64_MAX};
 kstatus_t (*constructors[])(uint64_t,shared_section_t**)={
  shared_section_create,shared_section_create_populated};
 for(unsigned constructor=0;constructor<2;++constructor){
 kstatus_t (*create)(uint64_t,shared_section_t**)=constructors[constructor];
 for(unsigned i=0;i<sizeof(invalid)/sizeof(*invalid);++i) {
  assert(create(invalid[i],&out)==K_EINVAL && out==(void*)1);
 }
 assert(create(PAGE_SIZE,0)==K_EINVAL);
 fail_section=true;
 assert(create(PAGE_SIZE,&out)==K_ENOMEM && out==(void*)1);
 fail_section=false;
 kstatus_t errors[]={K_ENOMEM,K_EIO};
 for(unsigned i=0;i<2;++i) {
  fail_vm=errors[i];
  assert(create(PAGE_SIZE,&out)==errors[i] && out==(void*)1);
  assert(atomic_load(&allocations)==atomic_load(&frees));
 }
 fail_vm=K_OK;
 }
 shared_section_t *s=0;assert(shared_section_create(3*PAGE_SIZE,&s)==K_OK);
 check_header(s,3*PAGE_SIZE);vm_object_t *mapping=s->vm_object;vm_object_get(mapping);
 unsigned freed=atomic_load(&vm_frees);
 object_notify_signaled(s);assert(!atomic_load(&wakes));
 object_put(s);assert(atomic_load(&mapping->refs.value)==1 && atomic_load(&vm_frees)==freed);
 vm_object_put(mapping);assert(atomic_load(&vm_frees)==freed+1);
 pthread_t threads[4];
 for(unsigned i=0;i<4;++i)assert(!pthread_create(&threads[i],0,worker,0));
 for(unsigned i=0;i<4;++i)assert(!pthread_join(threads[i],0));
 assert(atomic_load(&allocations)==atomic_load(&frees));
 assert(atomic_load(&vm_creates)==atomic_load(&vm_frees));
 assert(atomic_load(&wakes)==4000);
 puts("PASS: shared-section production header/queue/refcounts, invalid/OOM/VM-error cleanup, retained VM mapping and 4000 four-thread lifecycles");
}
'''
with tempfile.TemporaryDirectory(prefix='liteos-shared-section-') as directory:
    path = Path(directory)
    (path / 'test.c').write_text(program)
    subprocess.run(['cc', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                    '-pthread', '-I' + str(root / 'include'),
                    '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
                    str(path / 'test.c'), '-o', str(path / 'test')], check=True)
    subprocess.run([str(path / 'test')], check=True)
