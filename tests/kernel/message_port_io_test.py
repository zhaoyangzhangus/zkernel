"""Production port implementation and kernel copies, queue/lifecycle boundaries."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root/'kernel/ipc/port.c').read_text().replace('memmove(', 'liteos_memmove(')
source = source.replace('static void message_port_lock(message_port_t *port) {',
                        'static void message_port_lock(message_port_t *port) { ++lock_calls;')
code = r'''
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <pthread.h>
#include <kernel/mm.h>
#include <kernel/message_port.h>
void *liteos_memmove(void *,const void *,size_t);
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
void kfree(void *p){
 if(!p)return;
 ++frees;
 allocation_t *a=(void*)((unsigned char*)p-16-__builtin_offsetof(allocation_t,bytes));
 for(unsigned i=0;i<16;++i)assert(a->bytes[i]==0x5a && a->bytes[16+a->size+i]==0x5a);
 free(a);
}
static void check_storage(message_port_t *p);
void wait_queue_init(wait_queue_t *q) { memset(q,0,sizeof(*q)); }
void object_header_init(object_header_t *h,object_type_id_t type,const object_ops_t *ops) {
 memset(h,0,sizeof(*h));h->type=type;h->ops=ops;
}
void object_put(void *p) { if(p)((object_header_t*)p)->ops->destroy(p); }
bool object_is_signaled(const void *p) { return ((const object_header_t*)p)->ops->is_signaled(p); }
void object_notify_signaled(void *p) {
 message_port_t *port=p;assert(!atomic_load(&port->lock.state));++notifications;
}
static void wake_check(wait_queue_t *q) {
 message_port_t *p=(void*)((uint8_t*)q - offsetof(message_port_t,receive_waitq));
 assert(!atomic_load(&p->lock.state));
}
uint32_t wake_one(wait_queue_t *q) { wake_check(q);++one_wakes;return 1; }
uint32_t wake_all(wait_queue_t *q) {
 wake_check(q);++all_wakes;atomic_fetch_add(&q->sequence,1);return 1;
}
kstatus_t wait_on_queue(wait_queue_t *q,bool (*ready)(void*),void *ctx,uint64_t timeout) {
 void *port=(void*)((uint8_t*)q - offsetof(message_port_t,receive_waitq));
 assert(timeout==123 || timeout==0);++wait_calls;
 if(before_wait) { void (*hook)(void*)=before_wait;before_wait=0;hook(port); }
 bool value=ready(ctx);
 if(value && after_ready) { void (*hook)(void*)=after_ready;after_ready=0;hook(port); }
 return value?K_OK:K_ETIMEDOUT;
}
''' + source.replace('    *out = port;', '    check_storage(port);\n    *out = port;') + r'''
static void check_storage(message_port_t *p){
 allocation_t *a=(void*)((unsigned char*)p-16-__builtin_offsetof(allocation_t,bytes));
 assert(a->size==sizeof(*p)+(size_t)p->capacity*sizeof(p->messages[0]));
 assert(!p->head && !p->tail && !p->count && !atomic_load(&p->closed));
 const unsigned char *slots=(const void*)p->messages;
 for(size_t i=0;i<(size_t)p->capacity*sizeof(p->messages[0]);++i)assert(slots[i]==0xa5);
}
static void post_between_check_and_wait(void *p) {
 unsigned char data=42;assert(message_port_send(p,&data,1)==K_OK);
}
static void close_between_check_and_wait(void *p) { assert(message_port_close(p)==K_OK); }
static void compete_after_claim(void *p) {
 unsigned char data=99;size_t size=123;
 assert(message_port_receive(p,&data,1,&size,0)==K_ETIMEDOUT && data==99 && size==123);
}
static void *close_many(void *p) {
 for(unsigned i=0;i<10000;++i)assert(message_port_close(p)==K_OK);
 return 0;
}
int main(void) {
 assert(message_port_self_test());
 assert(message_port_create(1,0)==K_EINVAL);
 message_port_t *p=0;assert(message_port_create(33,&p)==K_EINVAL && !p);
 assert(message_port_create(UINT32_MAX,&p)==K_EINVAL && !p);
 fail_alloc=true;assert(message_port_create(1,&p)==K_ENOMEM && !p);fail_alloc=false;
 assert(message_port_create(0,&p)==K_OK && p->capacity==OS_PORT_DEFAULT_CAPACITY);
 object_put(p);
 uint8_t data[256],out[288];size_t size;
 for(unsigned capacity=1;capacity<=32;++capacity) {
  assert(message_port_create(capacity,&p)==K_OK);
  for(unsigned length=1;length<=256;++length) {
   for(unsigned i=0;i<256;++i)data[i]=(uint8_t)(i*17+length);
   unsigned before=one_wakes;
   for(unsigned i=0;i<capacity;++i)assert(message_port_send(p,data,length)==K_OK);
   assert(one_wakes==before+capacity && p->count==capacity);
   assert(message_port_send(p,data,length)==K_EAGAIN && one_wakes==before+capacity);
   if(length>1) {
    size=123;assert(message_port_receive(p,out,length-1,&size,0)==K_EINVAL);
    assert(size==123 && p->count==capacity);
   }
   for(unsigned i=0;i<capacity;++i) {
    memset(out,0x69,sizeof(out));size=0;
    unsigned locks=lock_calls,waits=wait_calls;
    assert(message_port_receive(p,out+16,length,&size,0)==K_OK && size==length);
    assert(lock_calls==locks+1 && wait_calls==waits);
    assert(!memcmp(out+16,data,length));
    for(unsigned j=0;j<16;++j)assert(out[j]==0x69);
    for(unsigned j=16+length;j<sizeof(out);++j)assert(out[j]==0x69);
   }
   assert(!p->count && !atomic_load(&p->lock.state));
   assert(message_port_receive(p,out,sizeof(out),&size,0)==K_ETIMEDOUT);
  }
  /* 队列顺序、关闭队列的排空以及幂等的关闭通知。 */
  for(unsigned i=0;i<capacity;++i) {data[0]=(uint8_t)i;assert(message_port_send(p,data,1)==K_OK);}
  unsigned notify=notifications,wakes=all_wakes;
  assert(message_port_close(p)==K_OK && notifications==notify+1 && all_wakes==wakes+1);
  assert(message_port_close(p)==K_OK && notifications==notify+1 && all_wakes==wakes+1);
  assert(message_port_send(p,data,1)==K_ECANCELED);
  for(unsigned i=0;i<capacity;++i) {
   assert(message_port_receive(p,out,sizeof(out),&size,0)==K_OK && size==1 && out[0]==i);
  }
  assert(message_port_receive(p,out,sizeof(out),&size,0)==K_ECANCELED);
  assert(p->object.ops->wait_value(p)==K_ECANCELED && object_is_signaled(p));
  object_put(p);
  assert(notifications==notify+1 && all_wakes==wakes+1);
 }
 assert(message_port_create(1,&p)==K_OK);
 /* 内核内调用者可能使用别名消息存储：两个方向都要测试。 */
 for(unsigned direction=0;direction<2;++direction) {
  for(unsigned i=0;i<256;++i)p->messages[0].data[i]=(uint8_t)i;
  memcpy(data,p->messages[0].data,256);
  unsigned from=direction?0:1,to=direction?1:0;
  if(to==0) {
   memmove(data,p->messages[0].data+from,255);
   assert(message_port_send(p,p->messages[0].data+from,255)==K_OK);
  } else {
   p->messages[0].size=255;p->count=1;
   memmove(data+to,data,255);size=0;
   assert(message_port_receive(p,p->messages[0].data+to,255,&size,0)==K_OK);
  }
  assert(!memcmp(data,p->messages[0].data,256));p->count=0;
 }
 assert(message_port_send(p,0,1)==K_EINVAL && message_port_send(p,data,0)==K_EINVAL);
 assert(message_port_send(p,data,257)==K_EINVAL);
 assert(message_port_receive(p,0,1,&size,0)==K_EINVAL);
 assert(message_port_receive(p,out,1,0,0)==K_EINVAL);
 object_put(p);
 assert(message_port_create(1,&p)==K_OK);
 unsigned waits=wait_calls,locks=lock_calls;
 size=123;assert(message_port_receive(p,out,1,&size,0)==K_ETIMEDOUT && size==123);
 assert(wait_calls==waits && lock_calls==locks+1);
 before_wait=post_between_check_and_wait;
 assert(message_port_receive(p,out,1,&size,123)==K_OK && out[0]==42 && size==1);
 assert(wait_calls==waits+1);
 before_wait=post_between_check_and_wait;after_ready=compete_after_claim;
 size=123;
 assert(message_port_receive(p,out,1,&size,123)==K_OK && out[0]==42 && size==1);
 assert(wait_calls==waits+2 && !p->count);
 before_wait=close_between_check_and_wait;
 assert(message_port_receive(p,out,1,&size,123)==K_ECANCELED);
 assert(wait_calls==waits+3 && !before_wait && !after_ready && !atomic_load(&p->lock.state));
 object_put(p);
 /* 并发 close 共享同一次发布；重复销毁操作也必须安全。 */
 assert(message_port_create(1,&p)==K_OK);
 unsigned notify=notifications,wakes=all_wakes;
 pthread_t workers[8];
 for(unsigned i=0;i<8;++i)assert(!pthread_create(&workers[i],0,close_many,p));
 for(unsigned i=0;i<8;++i)assert(!pthread_join(workers[i],0));
 assert(notifications==notify+1 && all_wakes==wakes+1);
 assert(message_port_receive(p,out,sizeof(out),&size,123)==K_ECANCELED);
 assert(object_is_signaled(p) && p->object.ops->wait_value(p)==K_ECANCELED);
 object_put(p);assert(notifications==notify+1 && all_wakes==wakes+1);
 assert(allocations==frees);
 printf("Message storage: exact single allocations for all 32 capacities; one-slot port saves %zu bytes versus 32 slots; poisoned unread slots and allocation guards passed\n",
        (size_t)(OS_PORT_MAX_CAPACITY-1)*sizeof(message_port_message_t));
 puts("Message ports: 32 capacities x 256 sizes, full/wrap/FIFO, short buffers, close/drain/wakes, overlap and allocation lifetime passed");
}
'''
with tempfile.TemporaryDirectory(prefix='liteos-message-port-') as directory:
    binary = str(Path(directory)/'test')
    obj = str(Path(directory)/'string.o')
    subprocess.run(['cc','-O2','-fno-builtin','-mno-sse','-mno-mmx',
                    '-I'+str(root/'include'),'-Dmemmove=liteos_memmove',
                    '-Dmemcpy=liteos_memcpy','-Dmemset=liteos_memset','-Dmemcmp=liteos_memcmp',
                    '-c',str(root/'kernel/string.c'),'-o',obj],check=True)
    def run(actual):
        subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-I'+str(root/'include'),
                    '-pthread','-fsanitize=address,undefined','-fno-pie','-no-pie',
                    '-x','c','-','-x','none',obj,'-o',binary],input=actual,text=True,check=True)
        return subprocess.run([binary],capture_output=True,text=True,timeout=60)
    result=run(code);assert result.returncode==0,result.stderr;print(result.stdout.strip())
    negative=code.replace('(size_t)capacity * sizeof(port->messages[0])',
                          '(size_t)OS_PORT_MAX_CAPACITY * sizeof(port->messages[0])')
    result=run(negative);assert result.returncode!=0,result.stderr
    print('Negative control rejected: all message ports allocate maximum capacity')
    negative=code.replace('message_port_t *)kmalloc(', 'message_port_t *)kzalloc(')
    result=run(negative);assert result.returncode!=0,result.stderr
    print('Negative control rejected: clearing unpublished message slots')
    for field in ('head','tail','count'):
        negative=code.replace(f'    port->{field} = 0U;', '')
        result=run(negative);assert result.returncode!=0,result.stderr
    print('Negative controls rejected: uninitialized head/tail/count')
    negative=code.replace('liteos_memmove(receive->data, message->data, message->size);',
                          'liteos_memmove(receive->data, message->data, OS_PORT_MAX_MESSAGE_SIZE);')
    result=run(negative);assert result.returncode!=0,result.stderr
    print('Negative control rejected: unused message bytes copied into receiver buffer')
