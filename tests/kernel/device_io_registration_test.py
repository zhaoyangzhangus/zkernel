"""Actual request finalization versus actual in-lock device registration."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
request_source = (root/'kernel/io/request.c').read_text()
device_source = (root/'kernel/drivers/core/device.c').read_text()
device_io = device_source[device_source.index('bool device_io_begin('):
                          device_source.index('kstatus_t device_reset(')]
device_drain = device_source[device_source.index('static void device_cancel_pending_io('):
                             device_source.index('void device_unregister(')]
objects = (root/'kernel/object/object.c').read_text()
object_init = objects[objects.index('void object_header_init('):objects.index('void object_get(')]
wait = (root/'kernel/sync/wait.c').read_text()
start = wait.index('void wait_queue_init(')
queue_init = wait[start:wait.index('\n}',start)+2]
request_source = request_source.replace('    req->bytes_done = bytes_done;',
                                        '    req->bytes_done = bytes_done; finalize_hook(req);')
fixture = r'''
#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <kernel/io.h>
#include <kernel/device.h>
#include <kernel/process.h>
static device_t device;
static process_t process;
static io_request_t *active_request;
static atomic_uint registration_entered,cancellation_owned,destroyed,callbacks;
static atomic_uint end_attempted,cancel_done;
static atomic_uint driver_cancels;
static atomic_uint progress_completions;
static atomic_uint finish_allowed,drain_locks;
static bool delay_finalizer;
static bool race_dispatch;
static atomic_uint dispatch_entered,competing_done;
static bool inject_registration,registered=true;
static pthread_mutex_t device_mutex=PTHREAD_MUTEX_INITIALIZER;
static _Thread_local unsigned lock_depth;
static void finalize_hook(io_request_t *r){
 if(delay_finalizer && r==active_request){
  atomic_store(&cancellation_owned,1);
  while(!atomic_load(&finish_allowed))sched_yield();
 }
 if(inject_registration && r==active_request){
  assert(atomic_load(&registration_entered));
  assert(atomic_load(&r->state)==IOREQ_COMPLETING);
  atomic_store(&cancellation_owned,1);
 }
}
'''
mocks = r'''
void object_get(void *p){
 object_header_t *h=p;
 if(delay_finalizer && p==active_request)assert(atomic_load(&h->refs.value)<5);
 assert(atomic_fetch_add(&h->refs.value,1));
}
bool object_try_get(void *p){
 if(p==&device && inject_registration){
  assert(lock_depth==2);atomic_store(&registration_entered,1);
  while(!atomic_load(&cancellation_owned) ||
        (!atomic_load(&end_attempted) && !atomic_load(&cancel_done)))sched_yield();
 }
 object_get(p);return true;
}
void object_put(void *p){
 if(p==&device)assert(!lock_depth);
 object_header_t *h=p;unsigned n=atomic_fetch_sub(&h->refs.value,1);assert(n);
 if(n==1){atomic_fetch_add(&destroyed,1);h->ops->destroy(p);}
}
void kfree(void *p){free(p);}
static void device_initialize(void){}
static void device_lock(void){
 if(inject_registration && atomic_load(&registration_entered))atomic_store(&end_attempted,1);
 assert(!pthread_mutex_lock(&device_mutex));assert(!lock_depth);++lock_depth;
 if(delay_finalizer)atomic_fetch_add(&drain_locks,1);
}
static void device_unlock(void){
 assert(lock_depth==1);--lock_depth;assert(!pthread_mutex_unlock(&device_mutex));
 if(race_dispatch && !atomic_exchange(&dispatch_entered,1)){
  while(!atomic_load(&competing_done))sched_yield();
 }
}
static void device_state_lock(device_t *d){assert(d==&device && lock_depth==1);++lock_depth;}
static void device_state_unlock(device_t *d){assert(d==&device && lock_depth==2);--lock_depth;}
static int find_device_slot(device_t *d){assert(d==&device && lock_depth==1);return registered?0:-1;}
kstatus_t process_register_teardown_callback(void (*callback)(process_t*)){
 assert(callback==io_cancel_process);return K_OK;
}
uint64_t telemetry_timestamp(void){return 1234;}
kstatus_t telemetry_record_latency(uint32_t category,uint64_t id,uint64_t start){
 (void)category;assert(id && start==1234);return K_OK;
}
kstatus_t completion_port_post(struct completion_port *p,const os_completion_entry_t *e){
 (void)p;(void)e;assert(false);return K_OK;
}
static void wait_global_init(void){}
'''
checks = r'''
static void callback(io_request_t *r){
 assert(!lock_depth && !r->device_ref_held && list_empty(&r->device_node));
 assert(atomic_load(&device.io_inflight)==0);atomic_fetch_add(&callbacks,1);
}
static void driver_cancel(io_request_t *r){
 assert(!lock_depth && atomic_load(&r->state)==IOREQ_COMPLETING);
 assert(r->status==K_EDEVREMOVED);
 atomic_fetch_add(&driver_cancels,1);
 io_complete(r,K_OK,999);assert(r->status==K_EDEVREMOVED);
 assert(io_cancel(r)==K_EBUSY);
}
static void *canceller(void *argument){
 io_request_t *r=argument;
 while(!atomic_load(&registration_entered))sched_yield();
 assert(io_cancel(r)==K_OK);atomic_store(&cancel_done,1);
 object_put(r);assert(!lock_depth);return NULL;
}
static void initialize_device(void){
 memset(&device,0,sizeof(device));atomic_init(&device.object.refs.value,1);
 atomic_init(&device.state,DEVICE_ACTIVE);atomic_init(&device.io_inflight,0);
 list_init(&device.io_requests);
}
static void race_registration(void){
 for(unsigned i=0;i<1000;++i){
  initialize_device();atomic_store(&registration_entered,0);atomic_store(&cancellation_owned,0);
  atomic_store(&end_attempted,0);atomic_store(&cancel_done,0);
  io_request_t *r=malloc(sizeof(*r));assert(r);
  io_request_init(r,IO_FLUSH,&device,&process,NULL,0);
  r->internal_flags=IOREQ_INTERNAL_DYNAMIC;r->complete=callback;
  assert(io_request_register_user(r)==K_OK);
  object_get(r); /* cancellation observer */
  active_request=r;inject_registration=true;
  pthread_t thread;assert(!pthread_create(&thread,NULL,canceller,r));
  kstatus_t status=io_submit(r);
  assert(status==K_EBUSY || status==K_ECANCELED);
  assert(!pthread_join(thread,NULL));inject_registration=false;
  assert(io_request_is_terminal(r) && r->status==K_ECANCELED);
  assert(!r->device_ref_held && list_empty(&r->device_node));
  assert(list_empty(&device.io_requests) && atomic_load(&device.io_inflight)==0);
  assert(atomic_load(&device.object.refs.value)==1 && atomic_load(&r->object.refs.value)==1);
  assert(!lock_depth);object_put(r);
 }
 assert(atomic_load(&callbacks)==1000 && atomic_load(&destroyed)==1000);
 puts("1000 forced in-lock registration/cancel races: completion waits for device ownership, unlinks before callbacks/terminal publication, zero inflight, exact device/request references and unlocked releases passed");
}
static void begin_end_cases(void){
 initialize_device();
 for(unsigned i=0;i<40000;++i){
  io_request_t r;io_request_init(&r,IO_FLUSH,&device,&process,NULL,0);
  atomic_store(&r.state,IOREQ_QUEUED);
  assert(device_io_begin(&device,&r));
  assert(r.device_ref_held && atomic_load(&device.object.refs.value)==2);
  assert(atomic_load(&device.io_inflight)==1);
  assert(!device_io_begin(&device,&r));
  assert(device_io_end(&device,&r));object_put(&device);
  assert(!device_io_end(&device,&r) && !r.device_ref_held);
  assert(atomic_load(&device.object.refs.value)==1 && atomic_load(&device.io_inflight)==0);
  assert(list_empty(&r.device_node) && list_empty(&device.io_requests));object_put(&r);
 }
 io_request_t r;io_request_init(&r,IO_FLUSH,&device,&process,NULL,0);
 assert(!device_io_begin(&device,&r));
 atomic_store(&r.state,IOREQ_QUEUED);registered=false;
 assert(!device_io_begin(&device,&r));registered=true;
 atomic_store(&device.state,DEVICE_REMOVING);assert(!device_io_begin(&device,&r));
 atomic_store(&device.state,DEVICE_ACTIVE);
 assert(!device_io_end(NULL,&r) && !device_io_end(&device,NULL));
 assert(!device_io_end(&device,&r));
 /* 移除真实链表节点时，损坏的计数不能发生下溢。 */
 assert(device_io_begin(&device,&r));atomic_store(&device.io_inflight,0);
 assert(device_io_end(&device,&r));object_put(&device);
 assert(atomic_load(&device.io_inflight)==0);object_put(&r);
 puts("40000 begin/end/repeat/duplicate cases, invalid/nonqueued/unregistered/removing inputs and zero-count underflow guard passed");
}
static void *finish_delayed(void *p){
 if(race_dispatch)while(!atomic_load(&dispatch_entered))sched_yield();
 io_complete(p,K_OK,17);
 if(race_dispatch){
  assert(atomic_load(&((io_request_t*)p)->state)==IOREQ_COMPLETING);
  atomic_store(&competing_done,1);
 }
 return NULL;
}
static void *drain_device(void *p){device_cancel_pending_io(p);return NULL;}
static void drain_cases(void){
 io_request_t stack;
 for(unsigned mode=0;mode<3;++mode){
 io_request_t *r=mode==2?malloc(sizeof(*r)):&stack;assert(r);
 unsigned before=atomic_load(&destroyed);
 initialize_device();
 io_request_init(r,IO_FLUSH,&device,&process,NULL,0);
 if(mode==2)r->internal_flags=IOREQ_INTERNAL_DYNAMIC;
 assert(io_request_register_user(r)==K_OK);object_get(r);
 atomic_store(&r->state,IOREQ_QUEUED);assert(device_io_begin(&device,r));
 atomic_store(&r->state,IOREQ_SUBMITTED);r->complete=callback;
 if(mode)r->cancel=driver_cancel;
 active_request=r;delay_finalizer=true;
 race_dispatch=mode!=0;atomic_store(&dispatch_entered,0);atomic_store(&competing_done,0);
 atomic_store(&cancellation_owned,0);atomic_store(&finish_allowed,0);atomic_store(&drain_locks,0);
 pthread_t finish,drain;assert(!pthread_create(&finish,NULL,finish_delayed,r));
 if(!mode)while(!atomic_load(&cancellation_owned))sched_yield();
 assert(!pthread_create(&drain,NULL,drain_device,&device));
 if(!mode)while(atomic_load(&drain_locks)<1000)sched_yield();
 else while(!atomic_load(&cancellation_owned))sched_yield();
 assert(atomic_load(&r->object.refs.value)==3);
 if(mode==2){object_put(r);assert(atomic_load(&r->object.refs.value)==2);}
 atomic_store(&finish_allowed,1);
 assert(!pthread_join(finish,NULL));assert(!pthread_join(drain,NULL));delay_finalizer=false;race_dispatch=false;
 if(mode!=2){
  assert(io_request_is_terminal(r) && r->status==(mode?K_EDEVREMOVED:K_OK) && r->bytes_done==(mode?0:17));
  assert(atomic_load(&r->object.refs.value)==1);object_put(r);
 }
 assert(atomic_load(&device.object.refs.value)==1 && list_empty(&device.io_requests));
 assert(atomic_load(&destroyed)==before+1);
 }
 io_request_t r;
 /* 普通的排队取消仍应完成，且不额外保留观察者引用。 */
 io_request_init(&r,IO_FLUSH,&device,&process,NULL,0);object_get(&r);
 atomic_store(&r.state,IOREQ_QUEUED);assert(device_io_begin(&device,&r));r.complete=callback;
 r.cancel=driver_cancel;
 device_cancel_pending_io(&device);
 assert(io_request_is_terminal(&r) && r.status==K_EDEVREMOVED);
 assert(atomic_load(&r.object.refs.value)==1);
 assert(atomic_load(&device.object.refs.value)==1 && list_empty(&device.io_requests));object_put(&r);
 assert(atomic_load(&driver_cancels)==3);
 puts("Device removal: 1000 busy retries add no references; in-lock claim defeats a competing completion before unlocked dispatch; dynamic request survives caller release until claimed cancellation finishes; exact destruction/device refs and normal queued cancel passed");
}
static void progress_callback(io_request_t *r){
 assert(!lock_depth && !r->device_ref_held && list_empty(&r->device_node));
 assert(r->status==K_EDEVREMOVED);atomic_fetch_add(&progress_completions,1);
}
static void blocked_head_cases(void){
 enum {COUNT=65};io_request_t requests[COUNT];initialize_device();
 for(unsigned i=0;i<COUNT;++i){
  io_request_init(&requests[i],IO_FLUSH,&device,&process,NULL,0);object_get(&requests[i]);
  atomic_store(&requests[i].state,IOREQ_QUEUED);assert(device_io_begin(&device,&requests[i]));
  if(i&1)atomic_store(&requests[i].state,IOREQ_SUBMITTED);
  requests[i].complete=i?progress_callback:callback;
 }
 active_request=&requests[0];delay_finalizer=true;
 atomic_store(&cancellation_owned,0);atomic_store(&finish_allowed,0);
 atomic_store(&drain_locks,0);atomic_store(&progress_completions,0);
 pthread_t finish,drain;assert(!pthread_create(&finish,NULL,finish_delayed,&requests[0]));
 while(!atomic_load(&cancellation_owned))sched_yield();
 assert(!pthread_create(&drain,NULL,drain_device,&device));
 while(atomic_load(&drain_locks)<1000)sched_yield();
 assert(atomic_load(&progress_completions)==COUNT-1);
 for(unsigned i=1;i<COUNT;++i){
  assert(io_request_is_terminal(&requests[i]) && requests[i].status==K_EDEVREMOVED);
  assert(atomic_load(&requests[i].object.refs.value)==1);
 }
 assert(atomic_load(&requests[0].state)==IOREQ_COMPLETING);
 assert(atomic_load(&requests[0].object.refs.value)==2);
 atomic_store(&finish_allowed,1);
 assert(!pthread_join(finish,NULL));assert(!pthread_join(drain,NULL));delay_finalizer=false;
 assert(requests[0].status==K_OK && requests[0].bytes_done==17);
 assert(atomic_load(&device.io_inflight)==0 && atomic_load(&device.object.refs.value)==1);
 assert(list_empty(&device.io_requests));
 for(unsigned i=0;i<COUNT;++i)object_put(&requests[i]);
 puts("Paused queue head: all 64 later QUEUED/SUBMITTED requests cancelled before head release; original completion result, exact refs, empty list and zero inflight preserved");
}
int main(void){race_registration();begin_end_cases();drain_cases();blocked_head_cases();}
'''
with tempfile.TemporaryDirectory(prefix='liteos-device-io-') as directory:
    binary = str(Path(directory)/'test')
    def run(actual, drain_source=device_drain):
        code=fixture+actual+mocks+device_io+drain_source+queue_init+object_init+checks
        subprocess.run(['cc','-std=c11','-O2','-Wall','-Wextra','-Werror','-pthread',
                        '-I'+str(root/'include'),'-fsanitize=address,undefined',
                        '-fno-pie','-no-pie','-x','c','-','-o',binary],
                       input=code,text=True,check=True)
        return subprocess.run([binary],capture_output=True,text=True,timeout=30)
    result=run(request_source);assert result.returncode==0,result.stderr;print(result.stdout.strip())
    negative=request_source.replace('if (execution_ref && device_io_end(req->device, req))',
                                    'if (req->device_ref_held != 0U && device_io_end(req->device, req))')
    result=run(negative);assert result.returncode!=0,result.stderr
    print('Negative control rejected: unlocked reference-marker check skips unfinished device registration')
    negative=device_drain.replace('io_request_claim_device_cancel(candidate)',
                                  'atomic_load(&candidate->state)!=IOREQ_COMPLETING').replace(
        'io_request_finish_device_cancel(request, K_EDEVREMOVED);',
        '(void)io_cancel_with_status(request, K_EDEVREMOVED);')
    result=run(request_source,negative)
    assert result.returncode!=0,result.stderr
    print('Negative control rejected: unlocked cancellation loses ownership before dispatch')
    negative=device_drain.replace('node != &dev->io_requests; node = node->next',
                                  'node != &dev->io_requests; node = &dev->io_requests')
    result=run(request_source,negative);assert result.returncode!=0,result.stderr
    print('Negative control rejected: an owned queue head blocks later cancellable requests')
