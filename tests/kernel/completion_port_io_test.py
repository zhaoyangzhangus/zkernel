"""Actual completion-port code: arbitrary capacities, FIFO and lifecycle."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'kernel/ipc/completion.c').read_text()
source = source.replace('static void completion_port_lock(completion_port_t *port) {',
                        'static void completion_port_lock(completion_port_t *port) { ++lock_calls;')
code = r'''
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <pthread.h>
#include <kernel/mm.h>
#include <kernel/completion_port.h>
static atomic_uint notifications,all_wakes;
static unsigned one_wakes,allocations,frees;
static unsigned lock_calls,wait_calls;
static void (*before_wait)(void *),(*after_ready)(void *);
static bool fail_alloc;
typedef struct {size_t size;max_align_t alignment;unsigned char bytes[];} allocation_t;
static void *allocate(size_t n,bool zero){
 if(fail_alloc)return NULL;
 ++allocations;
 allocation_t *a=malloc(sizeof(*a)+n+32);assert(a);a->size=n;
 memset(a->bytes,0x5a,16);memset(a->bytes+16,zero?0:0xa5,n);
 memset(a->bytes+16+n,0x5a,16);return a->bytes+16;
}
void *kmalloc(size_t n,uint32_t flags){(void)flags;return allocate(n,false);}
void *kzalloc(size_t n,uint32_t flags){(void)flags;return allocate(n,true);}
void kfree(void *p) {
 if(!p)return;
 ++frees;
 allocation_t *a=(void*)((unsigned char*)p-16-__builtin_offsetof(allocation_t,bytes));
 for(unsigned i=0;i<16;++i)assert(a->bytes[i]==0x5a && a->bytes[16+a->size+i]==0x5a);
 free(a);
}
static void check_storage(completion_port_t *p);
void wait_queue_init(wait_queue_t *q) {memset(q,0,sizeof(*q));}
void object_header_init(object_header_t *h,object_type_id_t type,const object_ops_t *ops) {
 memset(h,0,sizeof(*h));h->type=type;h->ops=ops;
}
void object_put(void *p) {if(p)((object_header_t*)p)->ops->destroy(p);}
bool object_is_signaled(const void *p) {return ((const object_header_t*)p)->ops->is_signaled(p);}
void object_notify_signaled(void *p) {
 completion_port_t *port=p;assert(!atomic_load(&port->lock.state));++notifications;
}
static void wake_check(wait_queue_t *q) {
 completion_port_t *p=(void*)((uint8_t*)q - offsetof(completion_port_t,waitq));
 assert(!atomic_load(&p->lock.state));
}
uint32_t wake_one(wait_queue_t *q) {wake_check(q);++one_wakes;return 1;}
uint32_t wake_all(wait_queue_t *q) {
 wake_check(q);++all_wakes;atomic_fetch_add(&q->sequence,1);return 1;
}
kstatus_t wait_on_queue(wait_queue_t *q,bool (*ready)(void*),void *ctx,uint64_t timeout) {
 void *port=(void*)((uint8_t*)q - offsetof(completion_port_t,waitq));
 assert(timeout==123 || timeout==0);++wait_calls;
 if(before_wait) { void (*hook)(void*)=before_wait;before_wait=0;hook(port); }
 bool value=ready(ctx);
 if(value && after_ready) { void (*hook)(void*)=after_ready;after_ready=0;hook(port); }
 return value?K_OK:K_ETIMEDOUT;
}
''' + source.replace('    *out = port;', '    check_storage(port);\n    *out = port;') + r'''
static void check_storage(completion_port_t *p){
 allocation_t *a=(void*)((unsigned char*)p-16-__builtin_offsetof(allocation_t,bytes));
 assert(a->size==sizeof(*p)+(size_t)p->capacity*sizeof(p->entries[0]));
 assert(p->head==0 && p->tail==0 && p->count==0 && !atomic_load(&p->closed));
 const unsigned char *slots=(const void*)p->entries;
 for(size_t i=0;i<(size_t)p->capacity*sizeof(p->entries[0]);++i)assert(slots[i]==0xa5);
}
static os_completion_entry_t entry_for(unsigned id) {
 return (os_completion_entry_t){.user_key=(uint64_t)id*17,
  .request_id=id,.bytes_done=id*3,.status=(id&1)?K_EIO:K_OK};
}
static void check_entry(os_completion_entry_t actual,unsigned id) {
 os_completion_entry_t expected=entry_for(id);
 assert(actual.user_key==expected.user_key && actual.request_id==expected.request_id);
 assert(actual.bytes_done==expected.bytes_done && actual.status==expected.status);
}
static void post_between_check_and_wait(void *p) {
 os_completion_entry_t e=entry_for(42);assert(completion_port_post(p,&e)==K_OK);
}
static void close_between_check_and_wait(void *p) { assert(completion_port_close(p)==K_OK); }
static void compete_after_claim(void *p) {
 os_completion_entry_t e=entry_for(99);
 assert(completion_port_wait(p,0,&e)==K_ETIMEDOUT);check_entry(e,99);
}
static void *close_many(void *p) {
 for(unsigned i=0;i<10000;++i)assert(completion_port_close(p)==K_OK);
 return 0;
}
int main(void) {
 assert(completion_port_self_test());
 completion_port_t *p=0;
 assert(completion_port_create(1,0)==K_EINVAL);
 assert(completion_port_create(257,&p)==K_EINVAL && !p);
 assert(completion_port_create(UINT32_MAX,&p)==K_EINVAL && !p);
 fail_alloc=true;assert(completion_port_create(1,&p)==K_ENOMEM && !p);fail_alloc=false;
 assert(completion_port_create(0,&p)==K_OK && p->capacity==COMPLETION_PORT_DEFAULT_CAPACITY);
 object_put(p);
 for(unsigned cap=1;cap<=256;++cap) {
  assert(completion_port_create(cap,&p)==K_OK);
  unsigned sent=0,received=0,rng=cap;
  os_completion_entry_t out;
  /* 从每个起始位置执行填充/排空循环，并跨越环形缓冲区的边界。 */
  for(unsigned cycle=0;cycle<cap+2;++cycle) {
   unsigned wakes=one_wakes,notify=notifications;
   for(unsigned i=0;i<cap;++i) {
    os_completion_entry_t e=entry_for(sent++);
    assert(completion_port_post(p,&e)==K_OK);
   }
   os_completion_entry_t e=entry_for(sent);
   assert(completion_port_post(p,&e)==K_EAGAIN);
   assert(one_wakes==wakes+cap && notifications==notify+cap);
   for(unsigned i=0;i<cap;++i) {
    assert(p->object.ops->wait_value(p)==(int64_t)(cap-i));
    unsigned locks=lock_calls,waits=wait_calls;
    assert(completion_port_wait(p,0,&out)==K_OK);check_entry(out,received++);
    assert(lock_calls==locks+1 && wait_calls==waits);
   }
   assert(completion_port_wait(p,0,&out)==K_ETIMEDOUT && !object_is_signaled(p));
   e=entry_for(sent++);assert(completion_port_post(p,&e)==K_OK);
   assert(completion_port_wait(p,0,&out)==K_OK);check_entry(out,received++);
  }
  /* 参考 FIFO 的索引不设上限；交错执行部分排空和填充。 */
  for(unsigned step=0;step<8192;++step) {
   rng=rng*1664525U+1013904223U;
   if(rng&0x80000000U) {
    os_completion_entry_t e=entry_for(sent);
    kstatus_t status=completion_port_post(p,&e);
    if(sent-received==cap)assert(status==K_EAGAIN);
    else {assert(status==K_OK);++sent;}
   } else {
    kstatus_t status=completion_port_wait(p,0,&out);
    if(sent==received)assert(status==K_ETIMEDOUT);
    else {assert(status==K_OK);check_entry(out,received++);}
   }
   assert(p->count==sent-received && p->head<cap && p->tail<cap);
   assert(p->head==received%cap && p->tail==sent%cap);
  }
  unsigned notify=notifications,wakes=all_wakes;
  assert(completion_port_close(p)==K_OK && notifications==notify+1 && all_wakes==wakes+1);
  assert(completion_port_close(p)==K_OK && notifications==notify+1 && all_wakes==wakes+1);
  os_completion_entry_t e=entry_for(sent);assert(completion_port_post(p,&e)==K_ECANCELED);
  while(received<sent) {assert(completion_port_wait(p,0,&out)==K_OK);check_entry(out,received++);}
  assert(completion_port_wait(p,0,&out)==K_ECANCELED);
  assert(object_is_signaled(p) && p->object.ops->wait_value(p)==K_ECANCELED);
  assert(completion_port_post(p,0)==K_EINVAL && completion_port_wait(p,0,0)==K_EINVAL);
  object_put(p);
  assert(notifications==notify+1 && all_wakes==wakes+1);
 }
 assert(completion_port_create(1,&p)==K_OK);
 os_completion_entry_t out=entry_for(99);
 unsigned waits=wait_calls,locks=lock_calls;
 assert(completion_port_wait(p,0,&out)==K_ETIMEDOUT);check_entry(out,99);
 assert(wait_calls==waits && lock_calls==locks+1);
 before_wait=post_between_check_and_wait;
 assert(completion_port_wait(p,123,&out)==K_OK);check_entry(out,42);
 assert(wait_calls==waits+1);
 before_wait=post_between_check_and_wait;after_ready=compete_after_claim;
 out=entry_for(99);
 assert(completion_port_wait(p,123,&out)==K_OK);check_entry(out,42);
 assert(wait_calls==waits+2 && !p->count);
 before_wait=close_between_check_and_wait;
 assert(completion_port_wait(p,123,&out)==K_ECANCELED);
 assert(wait_calls==waits+3 && !before_wait && !after_ready && !atomic_load(&p->lock.state));
 object_put(p);
 /* 并发 close 共享同一次发布；重复销毁操作也必须安全。 */
 assert(completion_port_create(1,&p)==K_OK);
 unsigned notify=notifications,wakes=all_wakes;
 pthread_t workers[8];
 for(unsigned i=0;i<8;++i)assert(!pthread_create(&workers[i],0,close_many,p));
 for(unsigned i=0;i<8;++i)assert(!pthread_join(workers[i],0));
 assert(notifications==notify+1 && all_wakes==wakes+1);
 assert(completion_port_wait(p,123,&out)==K_ECANCELED);
 assert(object_is_signaled(p) && p->object.ops->wait_value(p)==K_ECANCELED);
 object_put(p);assert(notifications==notify+1 && all_wakes==wakes+1);
 assert(allocations==frees);
 printf("Completion storage: one exact allocation for all 256 capacities; default saves %zu bytes versus 256 slots; poisoned unread slots, complete publication and boundary guards passed\n",
        (size_t)(COMPLETION_PORT_MAX_CAPACITY-COMPLETION_PORT_DEFAULT_CAPACITY)*sizeof(os_completion_entry_t));
 puts("Completion ports: all 256 capacities, FIFO/wrap/mixed operations, full/timeout/closed drains, notification and lifetime passed");
}
'''
with tempfile.TemporaryDirectory(prefix='liteos-completion-port-') as directory:
    binary = str(Path(directory)/'test')
    def run(actual):
        subprocess.run(['cc','-std=c11','-O2','-Wall','-Wextra','-Werror',
                    '-pthread','-I'+str(root/'include'),'-fsanitize=address,undefined',
                    '-fno-pie','-no-pie','-x','c','-','-o',binary],
                   input=actual,text=True,check=True)
        return subprocess.run([binary],capture_output=True,text=True,timeout=60)
    result=run(code);assert result.returncode==0,result.stderr;print(result.stdout.strip())
    negative=code.replace('(size_t)capacity * sizeof(port->entries[0])',
                          '(size_t)COMPLETION_PORT_MAX_CAPACITY * sizeof(port->entries[0])')
    result=run(negative);assert result.returncode!=0,result.stderr
    print('Negative control rejected: every completion port allocates maximum capacity')
    negative=code.replace('completion_port_t *)kmalloc(', 'completion_port_t *)kzalloc(')
    result=run(negative);assert result.returncode!=0,result.stderr
    print('Negative control rejected: clearing unread completion slots')
    negative=code.replace('    port->count = 0U;', '')
    result=run(negative);assert result.returncode!=0,result.stderr
    print('Negative control rejected: poison escapes through uninitialized readable count')
    negative=code.replace('port->entries[port->tail] = *entry;',
                          'port->entries[port->tail].user_key = entry->user_key;')
    result=run(negative);assert result.returncode!=0,result.stderr
    print('Negative control rejected: partially initialized entry is published to a receiver')
