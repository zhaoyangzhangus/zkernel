"""真实端口/管道与等待层：竞争传输、一次等待预算、超时和关闭。"""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
host = (root/'tests/kernel/wait_subscription_host.c').read_text()
start = host.index('    job->status = job->uninterruptible ?')
end = host.index(';', start) + 1
# 仅替换宿主线程的测试入口，调度/等待/超时实现继续复用原有夹具和生产代码。
previous = host[start:end].split('=', 1)[1].strip().removesuffix(';')
host = host[:start] + ('    job->status = job->count == 1U ? '
    'ipc_receive_job((void *)job->objects[0]) : (' + previous + ');') + host[end:]
host = host.replace('static void *host_wait_thread_main(void *opaque) {',
    'extern kstatus_t ipc_receive_job(void *);\nstatic void *host_wait_thread_main(void *opaque) {')
ports = '\n'.join((root/name).read_text().replace('wait_on_queue(', 'counted_wait_on_queue(')
    for name in ('kernel/ipc/port.c', 'kernel/ipc/completion.c', 'kernel/ipc/pipe.c'))
code = r'''
#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <stdio.h>
#include <kernel/message_port.h>
#include <kernel/completion_port.h>
#include <kernel/pipe.h>
typedef struct host_wait_job host_wait_job_t;
host_wait_job_t *host_wait_job_start(void *const *,size_t,bool,uint64_t);
bool host_wait_until_sleeping(host_wait_job_t *,uint64_t);
bool host_wait_until_done(host_wait_job_t *,uint64_t);
kstatus_t host_wait_job_status(const host_wait_job_t *);
void host_wait_job_join(host_wait_job_t *);
void host_wake_without_wait_notification(host_wait_job_t *);
bool host_wait_locks_held(void);
static atomic_uint wait_calls;
static kstatus_t counted_wait_on_queue(wait_queue_t *q,bool (*predicate)(void *),void *context,uint64_t timeout){
 atomic_fetch_add(&wait_calls,1);return wait_on_queue(q,predicate,context,timeout);
}
''' + ports + r'''
typedef struct receive_job {
 void *port;bool completion;uint64_t timeout;uint8_t byte;size_t size;
 os_completion_entry_t entry;
 pipe_endpoint_t *endpoint;bool write;uint64_t transferred;uint8_t data[8];
} receive_job_t;
kstatus_t ipc_receive_job(void *opaque){
 receive_job_t *job=opaque;
 assert(!host_wait_locks_held());
 if(job->endpoint)return job->write ?
  pipe_write(job->endpoint,job->data,sizeof(job->data),job->timeout,&job->transferred) :
  pipe_read(job->endpoint,job->data,sizeof(job->data),job->timeout,&job->transferred);
 return job->completion ? completion_port_wait(job->port,job->timeout,&job->entry) :
  message_port_receive(job->port,&job->byte,1,&job->size,job->timeout);
}
static void *new_port(bool completion){
 void *p=0;
 if(completion){completion_port_t *v=0;assert(completion_port_create(2,&v)==K_OK);p=v;}
 else {message_port_t *v=0;assert(message_port_create(2,&v)==K_OK);p=v;}
 return p;
}
static void post(void *p,bool completion,unsigned id){
 if(completion){os_completion_entry_t e={.request_id=id};assert(completion_port_post(p,&e)==K_OK);}
 else {uint8_t value=id;assert(message_port_send(p,&value,1)==K_OK);}
}
static void close_port(void *p,bool completion){
 assert((completion?completion_port_close(p):message_port_close(p))==K_OK);
}
static void verify(receive_job_t *job,unsigned id){
 if(job->completion)assert(job->entry.request_id==id);
 else assert(job->byte==id&&job->size==1);
}

static void verify_pipe_waits(void){
 static uint8_t fill[OS_PIPE_DEFAULT_SIZE],output[OS_PIPE_DEFAULT_SIZE];
 for(unsigned writing=0;writing<2;++writing)for(unsigned trial=0;trial<16;++trial){
  pipe_endpoint_t *reader=0,*writer=0;assert(pipe_create(0,&reader,&writer)==K_OK);
  uint64_t transferred;
  if(writing)assert(pipe_write(writer,fill,sizeof(fill),0,&transferred)==K_OK&&transferred==sizeof(fill));
  receive_job_t a={.endpoint=writing?writer:reader,.write=writing,.timeout=1000000000ULL};
  receive_job_t b=a;for(unsigned i=0;i<8;++i){a.data[i]=41;b.data[i]=42;}
  void *arguments_a[]={&a},*arguments_b[]={&b};
  unsigned before=atomic_load(&wait_calls);
  host_wait_job_t *first=host_wait_job_start(arguments_a,1,false,0);
  host_wait_job_t *second=host_wait_job_start(arguments_b,1,false,0);
  assert(first&&second&&host_wait_until_sleeping(first,1000)&&host_wait_until_sleeping(second,1000));
  if(writing){assert(pipe_read(reader,output,16,0,&transferred)==K_OK&&transferred==16);}
  else {uint8_t records[16];for(unsigned i=0;i<16;++i)records[i]=i<8?41:42;
   assert(pipe_write(writer,records,sizeof(records),0,&transferred)==K_OK&&transferred==sizeof(records));}
  assert(host_wait_until_done(first,1000)&&host_wait_until_done(second,1000));
  assert(host_wait_job_status(first)==K_OK&&host_wait_job_status(second)==K_OK);
  assert(a.transferred==8&&b.transferred==8&&atomic_load(&wait_calls)==before+2);
  host_wait_job_join(first);host_wait_job_join(second);
  if(writing){
   assert(pipe_read(reader,output,sizeof(output),0,&transferred)==K_OK&&transferred==sizeof(output));
   unsigned start=sizeof(output)-16;uint8_t one=output[start],two=output[start+8];
   assert((one==41&&two==42)||(one==42&&two==41));
   for(unsigned i=0;i<8;++i)assert(output[start+i]==one&&output[start+8+i]==two);
   assert(pipe_write(writer,fill,sizeof(fill),0,&transferred)==K_OK);
  }else{
   assert((a.data[0]==41&&b.data[0]==42)||(a.data[0]==42&&b.data[0]==41));
   for(unsigned i=0;i<8;++i)assert(a.data[i]==a.data[0]&&b.data[i]==b.data[0]);
  }
  a.transferred=99;a.timeout=20000000ULL;before=atomic_load(&wait_calls);
  first=host_wait_job_start(arguments_a,1,false,0);
  assert(first&&host_wait_until_sleeping(first,1000));
  for(unsigned wake=0;wake<8;++wake)host_wake_without_wait_notification(first);
  assert(host_wait_until_done(first,1000)&&host_wait_job_status(first)==K_ETIMEDOUT);
  assert(!a.transferred&&atomic_load(&wait_calls)==before+1);host_wait_job_join(first);
  a.timeout=UINT64_MAX;first=host_wait_job_start(arguments_a,1,false,0);
  assert(first&&host_wait_until_sleeping(first,1000));
  assert(pipe_shutdown(writing?reader:writer)==K_OK);
  assert(host_wait_until_done(first,1000)&&host_wait_job_status(first)==(writing?K_EPIPE:K_OK));
  assert(!a.transferred);host_wait_job_join(first);object_put(reader);object_put(writer);
 }
 puts("真实 pipe + wait/object：32 组竞争读写、原子记录、单次等待预算、超时和对端关闭通过");
}
int main(void){
 verify_pipe_waits();
 for(unsigned kind=0;kind<2;++kind)for(unsigned trial=0;trial<16;++trial){
  void *p=new_port(kind);
  receive_job_t a={.port=p,.completion=kind,.timeout=1000000000ULL};
  receive_job_t b=a;void *arguments_a[]={&a},*arguments_b[]={&b};
  unsigned before=atomic_load(&wait_calls);
  host_wait_job_t *first=host_wait_job_start(arguments_a,1,false,0);
  host_wait_job_t *second=host_wait_job_start(arguments_b,1,false,0);
  assert(first&&second&&host_wait_until_sleeping(first,1000)&&host_wait_until_sleeping(second,1000));
  post(p,kind,41);post(p,kind,42);
  assert(host_wait_until_done(first,1000)&&host_wait_until_done(second,1000));
  assert(host_wait_job_status(first)==K_OK&&host_wait_job_status(second)==K_OK);
  unsigned first_id=kind?a.entry.request_id:a.byte;
  assert(first_id==41||first_id==42);verify(&a,first_id);verify(&b,83-first_id);
  assert(atomic_load(&wait_calls)==before+2);
  host_wait_job_join(first);host_wait_job_join(second);
  /* 无数据通知不能重启预算；关闭会从同一个等待返回取消，输出保持原值。 */
  a=(receive_job_t){.port=p,.completion=kind,.timeout=20000000ULL,.byte=99,.size=99,.entry={.request_id=99}};
  before=atomic_load(&wait_calls);
  first=host_wait_job_start(arguments_a,1,false,0);assert(first&&host_wait_until_sleeping(first,1000));
  for(unsigned wake=0;wake<8;++wake)host_wake_without_wait_notification(first);
  assert(host_wait_until_done(first,1000)&&host_wait_job_status(first)==K_ETIMEDOUT);
  assert(atomic_load(&wait_calls)==before+1&&a.byte==99&&a.size==99&&a.entry.request_id==99);
  host_wait_job_join(first);
  a.timeout=UINT64_MAX;first=host_wait_job_start(arguments_a,1,false,0);
  assert(first&&host_wait_until_sleeping(first,1000));close_port(p,kind);
  assert(host_wait_until_done(first,1000)&&host_wait_job_status(first)==K_ECANCELED);
  host_wait_job_join(first);object_put(p);
 }
 puts("真实 message/completion + wait/object：32 组双消费者精确交付、单次等待预算、无数据唤醒、超时和关闭通过");
}
'''
with tempfile.TemporaryDirectory(prefix='wait-port-claim-') as directory:
    path = Path(directory)
    (path/'host.c').write_text(host)
    (path/'test.c').write_text(code)
    command = ['cc','-std=gnu11','-O2','-Wall','-Wextra','-Werror','-pthread',
               '-DOS_WAIT_HOST_TEST','-fsanitize=address,undefined','-fno-pie','-no-pie',
               '-I'+str(root/'include'),str(path/'test.c'),str(path/'host.c'),
               str(root/'kernel/sync/wait.c'),str(root/'kernel/object/object.c'),
               '-o',str(path/'test')]
    subprocess.run(command,check=True)
    subprocess.run([str(path/'test')],check=True,timeout=30)
