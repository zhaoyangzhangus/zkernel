"""Whole production I/O requests: registry ownership and cancellation lifecycle."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root/'kernel/io/request.c').read_text()
user_io = (root/'kernel/syscall/io.c').read_text()
user_context = user_io[user_io.index('typedef struct user_io_context {'):
                       user_io.index('int64_t syscall_io_submit(')]
object_source = (root/'kernel/object/object.c').read_text()
object_init = object_source[object_source.index('void object_header_init('):
                            object_source.index('void object_get(')]
wait_source = (root/'kernel/sync/wait.c').read_text()
start = wait_source.index('void wait_queue_init(')
queue_init = wait_source[start:wait_source.index('\n}',start)+2]
code = r'''
#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include <arch/x86_64/uaccess.h>
#include <uapi/io.h>
static atomic_uint init_attempts,destroyed;
static atomic_uint freed_staging;
static atomic_uint registry_visits;
static atomic_uint busy_phase,finish_busy,drain_done;
struct io_request;
static struct io_request *busy_request;
static void publication_hook(struct io_request *request);
static _Bool cancel_on_publication;
static _Bool reject_teardown;
static _Bool parallel_init;
static _Bool counted_cas(atomic_uint *p,unsigned *expected,unsigned value,
                        memory_order success,memory_order failure) {
 atomic_fetch_add(&init_attempts,1);
 if(parallel_init)while(atomic_load(&init_attempts)<8)sched_yield();
 return atomic_compare_exchange_strong_explicit(p,expected,value,success,failure);
}
''' + source.replace('void io_complete(', 'void actual_io_complete(').replace('atomic_compare_exchange_strong_explicit(&g_io_registry_state',
                     'counted_cas(&g_io_registry_state').replace(
                         'list_entry(node, io_request_t, queue_node)',
                         '(atomic_fetch_add(&registry_visits,1),list_entry(node, io_request_t, queue_node))').replace(
                             '    req->internal_flags |= IOREQ_INTERNAL_USER_REQUEST;\n    io_registry_unlock();',
                             '    req->internal_flags |= IOREQ_INTERNAL_USER_REQUEST;\n    io_registry_unlock();\n    publication_hook(req);') + r'''
void io_complete(io_request_t *request,kstatus_t status,uint64_t bytes){
 assert(!atomic_load(&busy_phase));
 actual_io_complete(request,status,bytes);
}
void object_get(void *p) {object_header_t *h=p;assert(atomic_fetch_add(&h->refs.value,1));}
bool object_try_get(void *p) {
 if(atomic_load(&busy_phase) && p==busy_request){
  unsigned state=atomic_load(&busy_request->state);
  assert(state!=IOREQ_COMPLETING && state!=IOREQ_PREPARING);
 }
 object_header_t *h=p;unsigned count=atomic_load(&h->refs.value);
 while(count)if(atomic_compare_exchange_weak(&h->refs.value,&count,count+1))return true;
 return false;
}
void object_put(void *p) {
 object_header_t *h=p;unsigned count=atomic_fetch_sub(&h->refs.value,1);assert(count);
 if(count==1){atomic_fetch_add(&destroyed,1);h->ops->destroy(p);}
}
void kfree(void *p) {atomic_fetch_add(&freed_staging,1);free(p);}
kstatus_t copy_to_user(void *out,const void *in,size_t n){memcpy(out,in,n);return K_OK;}
kstatus_t process_register_teardown_callback(void (*callback)(process_t *)) {
 assert(callback==io_cancel_process);return reject_teardown?K_ENOSPC:K_OK;
}
bool device_io_begin(device_t *device,io_request_t *request) {(void)device;(void)request;assert(!"unexpected device submit");return false;}
bool device_io_end(device_t *device,io_request_t *request) {(void)device;assert(!request->device_ref_held);return false;}
uint64_t telemetry_timestamp(void) {return 1;}
kstatus_t telemetry_record_latency(uint32_t category,uint64_t id,uint64_t start) {
 (void)category;(void)id;(void)start;assert(!"unexpected submitted request");return K_OK;
}
kstatus_t completion_port_post(completion_port_t *port,const os_completion_entry_t *entry) {
 (void)port;(void)entry;assert(!"unexpected port");return K_OK;
}
''' + 'static void wait_global_init(void) {}\n' + queue_init + object_init + user_context + r'''
static process_t processes[2];
static void publication_hook(io_request_t *request){
 if(!cancel_on_publication)return;
 assert(!atomic_load(&g_io_registry_lock.state));
 io_cancel_process(request->process);
 assert(io_request_is_terminal(request) && atomic_load(&request->object.refs.value)==1);
}
static void assert_registry_empty(void) {
 for(unsigned i=0;i<IO_REGISTRY_BUCKETS;++i)
  assert(g_io_registry[i].next==&g_io_registry[i] && g_io_registry[i].prev==&g_io_registry[i]);
}
static void *initializer(void *unused) {
 (void)unused;io_registry_initialize();
 assert_registry_empty();
 return 0;
}
static void register_request(io_request_t *request,process_t *process) {
 io_request_init(request,IO_FLUSH,0,process,0,0);
 assert(list_empty(&request->object.signal_waiters.waiters));
 assert(request->object.signal_waiters.waiters.next==&request->object.signal_waiters.waiters);
 assert(!atomic_load(&request->object.signal_waiters.lock.state) && !atomic_load(&request->object.signal_waiters.sequence));
 /* 注册必须获取自己的引用，并保留初始引用。 */
 assert(io_request_register_user(request)==K_OK);
 assert(atomic_load(&request->object.refs.value)==2);
}
static void verify_removed(io_request_t *request,kstatus_t status) {
 io_request_t *found=(void*)1;
 assert(io_request_is_terminal(request) && request->status==status);
 assert(io_request_lookup_user(request->process,request->request_id,&found)==K_ENOENT && !found);
 assert(request->queue_node.next==&request->queue_node && request->queue_node.prev==&request->queue_node);
 assert(atomic_load(&request->object.refs.value)==1);
 object_put(request);assert(!atomic_load(&request->object.refs.value));
}
static void busy_completion(io_request_t *r){
 assert(atomic_load(&r->state)==IOREQ_COMPLETING);
 atomic_store(&busy_phase,1);
 while(!atomic_load(&finish_busy))sched_yield();
}
static void *finish_request(void *p){actual_io_complete(p,K_OK,23);return NULL;}
static void *drain_owner(void *p){io_cancel_process(p);atomic_store(&drain_done,1);return NULL;}
static void verify_busy_owner(void){
 for(unsigned preparing=0;preparing<2;++preparing){
 io_request_t r,tail[64];pthread_t finish,drain;
 register_request(&r,&processes[0]);
 object_get(&r);atomic_store(&r.state,preparing?IOREQ_PREPARING:IOREQ_SUBMITTED);r.complete=busy_completion;
 for(unsigned i=0;i<64;++i){
  register_request(&tail[i],&processes[0]);
  if(i%3){object_get(&tail[i]);atomic_store(&tail[i].state,i%3==1?IOREQ_QUEUED:IOREQ_SUBMITTED);}
 }
 busy_request=&r;
 atomic_store(&drain_done,0);
 atomic_store(&finish_busy,0);atomic_store(&registry_visits,0);
 if(preparing)atomic_store(&busy_phase,1);
 else assert(!pthread_create(&finish,NULL,finish_request,&r));
 while(!atomic_load(&busy_phase))sched_yield();
 assert(!pthread_create(&drain,NULL,drain_owner,&processes[0]));
 while(atomic_load(&registry_visits)<1000){assert(!atomic_load(&drain_done));sched_yield();}
 for(unsigned i=0;i<64;++i){
  assert(io_request_is_terminal(&tail[i]) && tail[i].status==K_ECANCELED);
  assert(atomic_load(&tail[i].object.refs.value)==1);
 }
 assert(atomic_load(&r.object.refs.value)==3);
 atomic_store(&finish_busy,1);
 if(preparing){
  atomic_store(&busy_phase,0);
  atomic_store_explicit(&r.state,IOREQ_QUEUED,memory_order_release);
  actual_io_complete(&r,K_OK,23);
 }else assert(!pthread_join(finish,NULL));
 assert(!pthread_join(drain,NULL));
 atomic_store(&busy_phase,0);
 if(!preparing)assert(r.bytes_done==23 && r.status==K_OK);
 else assert(r.status==K_OK || r.status==K_ECANCELED);
 verify_removed(&r,r.status);
 for(unsigned i=0;i<64;++i)verify_removed(&tail[i],K_ECANCELED);
 assert_registry_empty();
 }
 puts("Owner drain: paused COMPLETING/PREPARING request acquires no extra observers; 64 later NEW/QUEUED/SUBMITTED requests cancelled before release; 1000 busy scans per mode, preserved results and exact refs passed");
}
static void *worker(void *unused) {
 (void)unused;io_request_t request;
 for(unsigned i=0;i<1000;++i) {
  register_request(&request,&processes[0]);io_request_t *found=0;
  assert(io_request_lookup_user(request.process,request.request_id,&found)==K_OK && found==&request);
  assert(atomic_load(&request.object.refs.value)==3);object_put(found);
  assert(io_cancel(&request)==K_OK);verify_removed(&request,K_ECANCELED);
 }
 return 0;
}
enum {RACE_COUNT=512};
static io_request_t race_requests[RACE_COUNT],callback_request;
static atomic_uint observers_ready,observer_go;
static void *observer(void *argument){
 unsigned seed=(unsigned)(uintptr_t)argument;
 atomic_fetch_add(&observers_ready,1);
 while(!atomic_load(&observer_go))sched_yield();
 for(unsigned i=0;i<10000;++i){
  io_request_t *r=&race_requests[(i*17+seed)%RACE_COUNT],*found=NULL;
  kstatus_t status=io_request_lookup_user(&processes[0],r->request_id,&found);
  assert(status==K_OK || status==K_ENOENT);
  if(status==K_OK){assert(found==r);object_put(found);}else assert(!found);
 }
 return NULL;
}
static void register_from_completion(io_request_t *unused){
 (void)unused;assert(!atomic_load(&g_io_registry_lock.state));
 io_request_init(&callback_request,IO_FLUSH,0,&processes[0],0,0);
 callback_request.request_id=1ULL<<40;
 assert(io_request_register_user(&callback_request)==K_OK);
}
static atomic_uint staging_reader_ready,staging_reader_go;
static void *staging_reader(void *argument){
 io_request_t *r=argument;
 atomic_store(&staging_reader_ready,1);
 while(!atomic_load(&staging_reader_go))sched_yield();
 for(unsigned i=0;i<50000;++i){
  unsigned slot=i%64;
  assert(r->vecs[slot].length==17 && ((uint8_t*)r->vecs[slot].base)[i%17]==slot+1);
 }
 object_put(r);return NULL;
}
static void verify_staging_lifetime(void){
 unsigned cases=0;
 device_t device={0};
 for(unsigned count=0;count<=64;++count)for(unsigned mode=0;mode<3;++mode){
  unsigned before=atomic_load(&freed_staging);
  user_io_context_t *c=malloc(sizeof(*c)+count*(sizeof(os_io_vec_t)+sizeof(io_vec_t)+sizeof(void*)));
  io_request_t *r=malloc(sizeof(*r));assert(c && r);
  c->vector_count=count;c->kernel_vectors=(io_vec_t*)(c->user_vectors+count);
  c->buffers=(void**)(c->kernel_vectors+count);
  uint8_t output[64][17];memset(output,0x69,sizeof(output));
  for(unsigned i=0;i<count;++i){
   c->buffers[i]=malloc(17);assert(c->buffers[i]);memset(c->buffers[i],i+1,17);
   c->user_vectors[i]=(os_io_vec_t){(uintptr_t)output[i],17};
   c->kernel_vectors[i]=(io_vec_t){c->buffers[i],17};
  }
  io_request_init(r,count?IO_READ:IO_IOCTL,&device,&processes[0],c->kernel_vectors,count);
  r->private_data=c;r->private_release=user_io_context_release;r->complete=user_io_complete;
  r->internal_flags=IOREQ_INTERNAL_DYNAMIC;
  assert(io_request_register_user(r)==K_OK);
  object_get(r); /* pending DMA/observer owns a reference independently of caller */
  uint64_t bytes=count*17/2;
  if(mode){object_get(r);atomic_store(&r->state,IOREQ_SUBMITTED);} /* execution owner */
  if(mode==1)io_complete(r,K_OK,bytes);else assert(io_cancel(r)==K_OK);
  assert(io_request_is_terminal(r) && r->private_data==c);
  assert(atomic_load(&r->object.refs.value)==2 && atomic_load(&freed_staging)==before);
  /* 即使请求已取消，真实提交验证器也必须保持安全。
   * 不会执行设备操作：请求已经进入终态。 */
  assert(io_submit(r)==(mode==1?K_OK:K_ECANCELED));
  for(unsigned i=0;i<count;++i)for(unsigned j=0;j<17;++j){
   assert(r->vecs[i].base==c->buffers[i] && r->vecs[i].length==17);
   assert(((uint8_t*)r->vecs[i].base)[j]==i+1);
   assert(output[i][j]==(mode==1 && i*17+j<bytes?i+1:0x69));
  }
  object_put(r); /* syscall returns; the pending owner still pins all buffers */
  assert(atomic_load(&r->object.refs.value)==1 && atomic_load(&freed_staging)==before);
  for(unsigned i=0;i<count;++i)memset(c->buffers[i],0x73,17); /* late DMA remains valid */
  object_put(r);
  assert(atomic_load(&freed_staging)-before==count+2);++cases;
 }
 assert_registry_empty();
 printf("%u actual request/staging lifetime cases: pre-submit cancellation, submitted cancellation/completion, partial copy-out, terminal resubmit validation, late DMA access and final-reference-only frees passed\n",cases);
 unsigned before=atomic_load(&freed_staging);
 user_io_context_t *c=malloc(sizeof(*c)+64*(sizeof(os_io_vec_t)+sizeof(io_vec_t)+sizeof(void*)));
 io_request_t *r=malloc(sizeof(*r));assert(c && r);
 c->vector_count=64;c->kernel_vectors=(io_vec_t*)(c->user_vectors+64);
 c->buffers=(void**)(c->kernel_vectors+64);
 for(unsigned i=0;i<64;++i){
  c->buffers[i]=malloc(17);assert(c->buffers[i]);memset(c->buffers[i],i+1,17);
  c->kernel_vectors[i]=(io_vec_t){c->buffers[i],17};
 }
 io_request_init(r,IO_READ,&device,&processes[0],c->kernel_vectors,64);
 r->private_data=c;r->private_release=user_io_context_release;r->complete=user_io_complete;
 r->internal_flags=IOREQ_INTERNAL_DYNAMIC;
 assert(io_request_register_user(r)==K_OK);
 object_get(r);atomic_store(&r->state,IOREQ_SUBMITTED); /* execution reference */
 object_get(r); /* reader reference */
 pthread_t reader;assert(!pthread_create(&reader,NULL,staging_reader,r));
 while(!atomic_load(&staging_reader_ready))sched_yield();
 atomic_store(&staging_reader_go,1);
 assert(io_cancel(r)==K_OK);object_put(r);
 assert(!pthread_join(reader,NULL));
 assert(atomic_load(&freed_staging)-before==66);assert_registry_empty();
 puts("50000 concurrent vector reads versus cancellation and caller release retain staging through the reader's final reference");
}
int main(void) {
 pthread_t threads[8];parallel_init=true;
 for(unsigned i=0;i<IO_REGISTRY_BUCKETS;++i)
  g_io_registry[i].next=g_io_registry[i].prev=(void*)1;
 for(unsigned i=0;i<8;++i)assert(!pthread_create(&threads[i],0,initializer,0));
 for(unsigned i=0;i<8;++i)assert(!pthread_join(threads[i],0));
 parallel_init=false;assert(atomic_load(&init_attempts)==8);
 io_request_t rejected;
 io_request_init(&rejected,IO_FLUSH,0,&processes[0],0,0);
 reject_teardown=true;
 assert(io_request_register_user(&rejected)==K_ENOSPC);
 reject_teardown=false;
 assert(atomic_load(&rejected.object.refs.value)==1 && !rejected.internal_flags);
 assert(list_empty(&rejected.queue_node));object_put(&rejected);assert_registry_empty();
 io_request_t *publication=malloc(sizeof(*publication));assert(publication);
 io_request_init(publication,IO_FLUSH,0,&processes[0],0,0);
 publication->internal_flags=IOREQ_INTERNAL_DYNAMIC;
 cancel_on_publication=true;
 assert(io_request_register_user(publication)==K_OK);
 cancel_on_publication=false;
 assert(io_request_is_terminal(publication) && publication->status==K_ECANCELED);
 assert(atomic_load(&publication->object.refs.value)==1);
 object_put(publication);assert_registry_empty();
 puts("Registration publication: forced owner cancellation before return preserves caller reference and balances dynamic destruction");
 enum {COUNT=96};io_request_t requests[COUNT];
 for(unsigned i=0;i<COUNT;++i)register_request(&requests[i],&processes[i&1]);
 for(unsigned i=0;i<COUNT;++i) {
  io_request_t *found=(void*)1;
  assert(io_request_register_user(&requests[i])==K_EINVAL);
  assert(io_request_lookup_user(&processes[(i&1)^1],requests[i].request_id,&found)==K_ENOENT && !found);
 }
 io_cancel_process(&processes[0]);
 for(unsigned i=0;i<COUNT;i+=2)verify_removed(&requests[i],K_ECANCELED);
 for(unsigned i=1;i<COUNT;i+=2) {
  assert(!io_request_is_terminal(&requests[i]));
  assert(io_cancel_with_status(&requests[i],K_EIO)==K_OK);
  assert(io_cancel(&requests[i])==K_OK);verify_removed(&requests[i],K_EIO);
 }
 assert_registry_empty();
 enum {LOOKUPS=2048};io_request_t *indexed=calloc(LOOKUPS,sizeof(*indexed));assert(indexed);
 for(unsigned i=0;i<LOOKUPS;++i)register_request(&indexed[i],&processes[i&1]);
 atomic_store(&registry_visits,0);
 for(unsigned i=0;i<LOOKUPS;++i){
  io_request_t *found=NULL;
  assert(io_request_lookup_user(indexed[i].process,indexed[i].request_id,&found)==K_OK && found==&indexed[i]);
  object_put(found);
 }
 unsigned indexed_visits=atomic_load(&registry_visits);
 assert(indexed_visits==LOOKUPS*(LOOKUPS/IO_REGISTRY_BUCKETS+1)/2);
 atomic_store(&registry_visits,0);
 for(unsigned i=0;i<LOOKUPS;++i){
  io_request_t *found=(void*)1;
  assert(io_request_lookup_user(&processes[(i&1)^1],indexed[i].request_id,&found)==K_ENOENT && !found);
  assert(io_request_lookup_user(indexed[i].process,indexed[i].request_id+(1ULL<<32),&found)==K_ENOENT && !found);
 }
 assert(atomic_load(&registry_visits)==2*LOOKUPS*(LOOKUPS/IO_REGISTRY_BUCKETS));
 io_cancel_process(&processes[0]);
 for(unsigned i=0;i<LOOKUPS;i+=2)verify_removed(&indexed[i],K_ECANCELED);
 for(unsigned i=1;i<LOOKUPS;i+=2){
  assert(!io_request_is_terminal(&indexed[i]));
  io_request_t *found=NULL;
  assert(io_request_lookup_user(indexed[i].process,indexed[i].request_id,&found)==K_OK && found==&indexed[i]);
  object_put(found);
 }
 io_cancel_process(&processes[1]);
 for(unsigned i=1;i<LOOKUPS;i+=2)verify_removed(&indexed[i],K_ECANCELED);
 free(indexed);assert_registry_empty();
 printf("Bucketed lookup: %u visits for %u successful lookups (full-list baseline %u); exact collision/miss/owner isolation and per-process bucket drain passed\n",
        indexed_visits,LOOKUPS,LOOKUPS*(LOOKUPS+1)/2);
 io_request_t trigger;
 io_request_init(&trigger,IO_FLUSH,0,&processes[0],0,0);
 trigger.request_id=(1ULL<<40)|63;trigger.complete=register_from_completion;
 assert(io_request_register_user(&trigger)==K_OK);
 io_cancel_process(&processes[0]);
 verify_removed(&trigger,K_ECANCELED);verify_removed(&callback_request,K_ECANCELED);
 assert_registry_empty();
 for(unsigned i=0;i<RACE_COUNT;++i)register_request(&race_requests[i],&processes[0]);
 for(unsigned i=0;i<4;++i)assert(!pthread_create(&threads[i],0,observer,(void*)(uintptr_t)i));
 while(atomic_load(&observers_ready)!=4)sched_yield();
 atomic_store(&observer_go,1);io_cancel_process(&processes[0]);
 for(unsigned i=0;i<4;++i)assert(!pthread_join(threads[i],0));
 for(unsigned i=0;i<RACE_COUNT;++i)verify_removed(&race_requests[i],K_ECANCELED);
 assert_registry_empty();
 puts("40000 concurrent lookups versus owner drain, observer reference balance, and callback registration into an earlier bucket passed");
 for(unsigned i=0;i<4;++i)assert(!pthread_create(&threads[i],0,worker,0));
 for(unsigned i=0;i<4;++i)assert(!pthread_join(threads[i],0));
 assert_registry_empty();
 verify_staging_lifetime();
 verify_busy_owner();
 assert(atomic_load(&destroyed)==COUNT+LOOKUPS+RACE_COUNT+4+196+4000+130 && atomic_load(&init_attempts)==8);
 assert(!atomic_load(&g_io_registry_lock.state));
 puts("I/O registry: eight concurrent initializers/publication, 96 process-owned requests/cross-process isolation/exit cancellation, 4000 concurrent register/lookup/cancel cycles and balanced references without repeated init CAS passed");
}
'''
with tempfile.TemporaryDirectory(prefix='liteos-io-registry-') as directory:
    binary = str(Path(directory)/'test')
    def run(actual):
        subprocess.run(['cc','-std=c11','-O2','-Wall','-Wextra','-Werror','-pthread',
                        '-I'+str(root/'include'),'-fsanitize=address,undefined',
                        '-fno-pie','-no-pie','-x','c','-','-o',binary],
                       input=actual,text=True,check=True)
        return subprocess.run([binary],capture_output=True,text=True,timeout=30)
    result=run(code);assert result.returncode==0,result.stderr;print(result.stdout.strip())
    negative=code.replace('request_id & (IO_REGISTRY_BUCKETS - 1U)', '0')
    # 显式保留未使用参数，确保 -Werror 下也能通过。
    negative=negative.replace('return &g_io_registry[0];', '(void)request_id; return &g_io_registry[0];')
    result=run(negative);assert result.returncode!=0,result.stderr
    print('Negative control rejected: all requests collapse into one full-scan bucket')
    negative=code.replace('list_head_t *bucket = &g_io_registry[index];',
                           'if (index < bucket_index) break; list_head_t *bucket = &g_io_registry[index];')
    result=run(negative);assert result.returncode!=0,result.stderr
    print('Negative control rejected: owner cancellation misses callback registrations in an earlier bucket')
    negative=code.replace('    object_get(req);\n    io_registry_insert_tail(',
                           '    io_registry_insert_tail(')
    result=run(negative);assert result.returncode!=0 and 'heap-use-after-free' in result.stderr,result.stderr
    print('Negative control rejected: transferring the caller reference frees the request before registration returns')
    negative=code.replace('    /* Submission and pending DMA may still hold request references after a',
                           '    user_io_context_release(request);\n    /* Submission and pending DMA may still hold request references after a')
    result=run(negative);assert result.returncode!=0,result.stderr
    print('Negative control rejected: completion frees staging while caller/DMA references remain live')
    negative=code.replace('                    busy = true;',
                          '                    io_complete(candidate,K_ECANCELED,candidate->bytes_done);\n                    busy = true;')
    result=run(negative);assert result.returncode!=0,result.stderr
    print('Negative control rejected: owner drain attempts redundant completion while another finalizer owns the results')
    negative=code.replace('                    busy = true;\n                    continue;',
                          '                    busy = true;')
    result=run(negative);assert result.returncode!=0,result.stderr
    print('Negative control rejected: selecting a busy owner churns observer references and blocks later requests')
    negative=code.replace('            if (!busy) return;', '            if (busy) return;\n            return;')
    result=run(negative);assert result.returncode!=0,result.stderr
    print('Negative control rejected: owner drain returns before its busy request is unregistered')
