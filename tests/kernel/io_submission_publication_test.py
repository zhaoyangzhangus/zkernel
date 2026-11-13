"""Production submit publishes fully initialized ownership and telemetry."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root/'kernel/io/request.c').read_text()
objects = (root/'kernel/object/object.c').read_text()
object_init = objects[objects.index('void object_header_init('):objects.index('void object_get(')]
wait = (root/'kernel/sync/wait.c').read_text()
start = wait.index('void wait_queue_init(')
queue_init = wait[start:wait.index('\n}', start)+2]
source = source.replace('atomic_compare_exchange_strong_explicit(&req->state, &expected,',
                        'publication_cas(req, &req->state, &expected,')
source = source.replace('atomic_store_explicit(&req->state, IOREQ_QUEUED, memory_order_release);',
                        'publication_store(req);')
fixture = r'''
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <kernel/io.h>
#include <kernel/device.h>
#include <kernel/process.h>
static bool boundary_cancel;
static unsigned destroyed,boundaries,device_begins,latencies;
static bool publication_cas(io_request_t*,atomic_uint*,unsigned*,unsigned,
                            memory_order,memory_order);
static void publication_store(io_request_t*);
'''
mocks = r'''
void object_get(void *p){object_header_t *h=p;assert(atomic_fetch_add(&h->refs.value,1));}
bool object_try_get(void *p){
 object_header_t *h=p;unsigned n=atomic_load(&h->refs.value);
 while(n)if(atomic_compare_exchange_weak(&h->refs.value,&n,n+1))return true;
 return false;
}
void object_put(void *p){
 object_header_t *h=p;unsigned n=atomic_fetch_sub(&h->refs.value,1);assert(n);
 if(n==1){++destroyed;h->ops->destroy(p);}
}
void kfree(void *p){free(p);}
kstatus_t process_register_teardown_callback(void (*callback)(process_t*)){
 assert(callback==io_cancel_process);return K_OK;
}
bool device_io_begin(device_t *d,io_request_t *r){
 assert(d);++device_begins;
 if(boundary_cancel)assert(io_request_is_terminal(r));
 return false;
}
bool device_io_end(device_t *d,io_request_t *r){(void)d;assert(!r->device_ref_held);return false;}
uint64_t telemetry_timestamp(void){return 1234;}
kstatus_t telemetry_record_latency(uint32_t category,uint64_t id,uint64_t start){
 (void)category;assert(id && start==1234);++latencies;return K_OK;
}
kstatus_t completion_port_post(struct completion_port *p,const os_completion_entry_t *e){
 (void)p;(void)e;assert(false);return K_OK;
}
static bool publication_cas(io_request_t *r,atomic_uint *p,unsigned *old,unsigned value,
                            memory_order success,memory_order failure){
 bool won=atomic_compare_exchange_strong_explicit(p,old,value,success,failure);
 if(won && value==IOREQ_PREPARING){
  assert(r->submit_tsc==0);
  assert(io_cancel(r)==K_EBUSY);
  io_complete(r,K_EIO,0);
  assert(atomic_load(&r->state)==IOREQ_PREPARING);
  assert(io_submit(r)==K_EBUSY);
  assert(atomic_load(&r->object.refs.value)==3);
 }
 return won;
}
static void publication_store(io_request_t *r){
  assert(r->submit_tsc==1234);
  atomic_store_explicit(&r->state,IOREQ_QUEUED,memory_order_release);
  ++boundaries;
  /* 调用方、注册表和执行过程各持有引用；取消时可以释放后两者的引用。 */
  assert(atomic_load(&r->object.refs.value)==3);
  if(boundary_cancel){
   assert(io_cancel(r)==K_OK);
   assert(io_request_is_terminal(r) && atomic_load(&r->object.refs.value)==1);
  }
}
static void wait_global_init(void){}
'''
checks = r'''
static process_t process;
static device_t device;
static void verify_publication(void){
 unsigned cases=0;
 for(unsigned count=0;count<=64;++count)for(unsigned cancel=0;cancel<2;++cancel){
  io_vec_t vectors[64];uint8_t payload[64];
  for(unsigned i=0;i<count;++i)vectors[i]=(io_vec_t){&payload[i],1};
  io_request_t *r=malloc(sizeof(*r));assert(r);
  io_request_init(r,count?IO_READ:IO_FLUSH,&device,&process,vectors,count);
  r->internal_flags=IOREQ_INTERNAL_DYNAMIC;
  assert(io_request_register_user(r)==K_OK);
  unsigned before=destroyed;
  boundary_cancel=cancel;
  assert(io_submit(r)==(cancel?K_ECANCELED:K_EDEVREMOVED));
  assert(io_request_is_terminal(r) && atomic_load(&r->object.refs.value)==1);
  assert(r->submit_tsc==0);
  assert(destroyed==before);
  io_request_t *found=(void*)1;
  assert(io_request_lookup_user(&process,r->request_id,&found)==K_ENOENT && !found);
  object_put(r);assert(destroyed==before+1);++cases;
 }
 assert(boundaries==130 && device_begins==130 && latencies==130);
 printf("%u actual publication cases: execution reference before QUEUED CAS, forced cancellation before CAS returns, device rejection, exact registry/caller releases and dynamic destruction passed\n",cases);
}
static void verify_failed_cas(void){
 boundary_cancel=false;
 for(unsigned state=IOREQ_QUEUED;state<=IOREQ_PREPARING;++state){
  io_request_t r;io_request_init(&r,IO_FLUSH,&device,&process,NULL,0);
  atomic_store(&r.state,state);r.status=K_EIO;r.submit_tsc=9876;
  unsigned before=destroyed,begin=device_begins;
  for(unsigned i=0;i<10000;++i){
   assert(io_submit(&r)==(io_request_state_is_terminal(state)?K_EIO:K_EBUSY));
   assert(atomic_load(&r.object.refs.value)==1 && atomic_load(&r.state)==state);
   assert(r.submit_tsc==9876 && r.status==K_EIO);
  }
  assert(destroyed==before && device_begins==begin);object_put(&r);
 }
 io_request_t invalid;io_request_init(&invalid,IO_READ,&device,&process,NULL,1);
 assert(io_submit(&invalid)==K_EINVAL && atomic_load(&invalid.object.refs.value)==1);
 object_put(&invalid);
 assert(io_submit(NULL)==K_EINVAL);
 puts("60000 failed-CAS submits retain state/status and one caller reference; invalid vectors/null requests never publish execution ownership");
}
int main(void){verify_publication();verify_failed_cas();}
'''
with tempfile.TemporaryDirectory(prefix='liteos-io-publication-') as directory:
    binary = str(Path(directory)/'test')
    def run(actual):
        code = fixture+actual+mocks+queue_init+object_init+checks
        subprocess.run(['cc','-std=c11','-O2','-Wall','-Wextra','-Werror','-pthread',
                        '-I'+str(root/'include'),'-fsanitize=address,undefined',
                        '-fno-pie','-no-pie','-x','c','-','-o',binary],
                       input=code,text=True,check=True)
        return subprocess.run([binary],capture_output=True,text=True,timeout=30)
    result=run(source);assert result.returncode==0,result.stderr;print(result.stdout.strip())
    begin=source.index('kstatus_t io_submit(')
    end=source.index('kstatus_t io_cancel_with_status(',begin)
    submit=source[begin:end]
    late=submit.replace('    object_get(req);\n','',1).replace(
        '    req->submit_tsc = telemetry_timestamp();',
        '    object_get(req);\n    req->submit_tsc = telemetry_timestamp();',1)
    result=run(source[:begin]+late+source[end:])
    assert result.returncode!=0,result.stderr
    print('Negative control rejected: QUEUED becomes cancellable before execution ownership exists')
    leaked=submit.replace('        object_put(req);\n','',1)
    result=run(source[:begin]+leaked+source[end:])
    assert result.returncode!=0,result.stderr
    print('Negative control rejected: failed CAS leaks a temporary execution reference')
    late_timestamp=submit.replace('    req->submit_tsc = telemetry_timestamp();\n','',1).replace(
        '    publication_store(req);','    publication_store(req);\n    req->submit_tsc = telemetry_timestamp();',1)
    result=run(source[:begin]+late_timestamp+source[end:])
    assert result.returncode!=0,result.stderr
    print('Negative control rejected: cancellable publication precedes timestamp initialization')
