"""Actual pipe I/O, atomic threshold, wait-boundary races and object lifetime."""
from pathlib import Path
import re
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'kernel/ipc/pipe.c').read_text().replace(
    'static void pipe_lock(pipe_state_t *state) {',
    'static void pipe_lock(pipe_state_t *state) { ++locks;')
source = source.replace('spinlock_lock(&state->lock);',
                        'spinlock_lock(&state->lock); assert(!lock_depth); ++lock_depth;')
source = source.replace('spinlock_unlock(&state->lock);',
                        'assert(lock_depth==1); --lock_depth; spinlock_unlock(&state->lock);')
unix_source = (root / 'kernel/syscall/linux.c').read_text()
recvmsg = unix_source[unix_source.index('static int64_t linux_recvmsg('):
                       unix_source.index('static int64_t linux_unix_sendto(')]
assert 'pipe_read_limited(read_end, payload, request_length,' in recvmsg
assert 'linux_unix_channel_pipe_limit, channel)' in recvmsg
assert 'request_length = linux_unix_channel_read_limit(' not in recvmsg

def unix_function(name):
    match = re.search(r'static [^\n]+ ' + name + r'\(', unix_source)
    assert match, name
    opening = unix_source.index('{', match.start())
    depth, end = 1, opening + 1
    while depth:
        depth += (unix_source[end] == '{') - (unix_source[end] == '}')
        end += 1
    return unix_source[match.start():end]

ancillary = r'''
#define LINUX_UNIX_MAX_MESSAGES 8
typedef struct { uint64_t start_offset; uint32_t payload_length, fd_count; }
 linux_unix_ancillary_record_t;
typedef struct {
 spinlock_t lock;
 uint64_t bytes_written, bytes_read;
 unsigned head, tail, count;
 linux_unix_ancillary_record_t records[LINUX_UNIX_MAX_MESSAGES];
} linux_unix_channel_t;
static unsigned discarded_fds;
static void linux_unix_ancillary_put_record(linux_unix_ancillary_record_t *r) {
 discarded_fds+=r->fd_count;r->fd_count=0;
}
''' + '\n'.join(unix_function(name) for name in (
    'linux_unix_channel_push', 'linux_unix_channel_commit_write',
    'linux_unix_channel_read_limit', 'linux_unix_channel_pipe_limit',
    'linux_unix_channel_commit_read'))
code = r'''
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <pthread.h>
#include <sched.h>
#include <kernel/mm.h>
#include <kernel/pipe.h>
static atomic_uint locks,waits,allocations,frees,attempts,fail_at,one_wakes,all_wakes,notifications;
static atomic_uint read_wakes,write_wakes;
static _Thread_local unsigned lock_depth;
static void (*before_wait)(void *),(*after_ready)(void *);
static pipe_state_t *live_state;
static bool close_on_wake;
static void close_peer_during_wake(wait_queue_t *q);
static void record_wake(wait_queue_t *q);
void *kmalloc(size_t size,uint32_t flags) {
 (void)flags;if(++attempts==fail_at)return 0;
 void *p=malloc(size);if(p){++allocations;memset(p,0xa5,size);}return p;
}
void *kzalloc(size_t size,uint32_t flags) {
 void *p=kmalloc(size,flags);if(p)memset(p,0,size);return p;
}
void kfree(void *p) { if(p)++frees;free(p); }
void wait_queue_init(wait_queue_t *q) { memset(q,0,sizeof(*q)); }
void object_header_init(object_header_t *h,object_type_id_t type,const object_ops_t *ops) {
 memset(h,0,sizeof(*h));atomic_init(&h->refs.value,1);h->type=type;h->ops=ops;
}
void object_get(void *p) {
 object_header_t *h=p;assert(atomic_fetch_add(&h->refs.value,1)>0);
}
bool object_try_get(void *p) {
 object_header_t *h=p;unsigned count=atomic_load(&h->refs.value);
 while(count)if(atomic_compare_exchange_weak(&h->refs.value,&count,count+1))return true;
 return false;
}
void object_put(void *p) {
 if(!p)return;object_header_t *h=p;
 unsigned count=atomic_fetch_sub(&h->refs.value,1);assert(count);
 if(count==1)h->ops->destroy(p);
}
static void wake_check(wait_queue_t *q);
uint32_t wake_one(wait_queue_t *q) { wake_check(q);++one_wakes;return 1; }
uint32_t wake_all(wait_queue_t *q) {
 wake_check(q);++all_wakes;record_wake(q);
 if(close_on_wake) {close_on_wake=false;close_peer_during_wake(q);}
 return 1;
}
void object_notify_signaled(void *p);
kstatus_t wait_on_queue(wait_queue_t *q,bool (*ready)(void*),void *context,uint64_t timeout) {
 wake_check(q);assert(timeout==123);++waits;
 if(before_wait) {void (*hook)(void*)=before_wait;before_wait=0;hook(context);}
 bool value=ready(context);
 if(value && after_ready) {void (*hook)(void*)=after_ready;after_ready=0;hook(context);}
 return value?K_OK:K_ETIMEDOUT;
}
''' + source + r'''
static pipe_endpoint_t *reader,*writer;
static uint8_t input[OS_PIPE_DEFAULT_SIZE+32],output[OS_PIPE_DEFAULT_SIZE+32],fifo[OS_PIPE_DEFAULT_SIZE];
static void wake_check(wait_queue_t *q) {
 assert(live_state && (q==&live_state->read_waitq || q==&live_state->write_waitq));
 assert(!lock_depth);
}
static void record_wake(wait_queue_t *q) {
 if(q==&live_state->read_waitq)++read_wakes;
 else {assert(q==&live_state->write_waitq);++write_wakes;}
}
void object_notify_signaled(void *p) {
 pipe_endpoint_t *e=p;assert(!lock_depth);
 assert(atomic_load(&e->object.refs.value)>=1);++notifications;
}
static void close_peer_during_wake(wait_queue_t *q) {
 /* Last external reference disappears between data publication and notify. */
 if(q==&live_state->read_waitq) {
  assert(atomic_load(&reader->object.refs.value)==2);
  object_put(reader);reader=0;
 } else {
  assert(q==&live_state->write_waitq);
  assert(atomic_load(&writer->object.refs.value)==2);
  object_put(writer);writer=0;
 }
}
static void make_pipe(void) {
 unsigned before=allocations,tries=attempts;
 assert(pipe_create(0,&reader,&writer)==K_OK);
 assert(allocations==before+2 && attempts==tries+2);
 live_state=reader->state;
 assert(reader==&live_state->endpoints[0] && writer==&live_state->endpoints[1]);
 assert(!live_state->head && !live_state->tail && !live_state->active_destroyers && !live_state->released);
 assert(reader->state==writer->state && reader->state->count==0);
}
static void release_pair(void) { object_put(reader);object_put(writer);reader=writer=0;live_state=0; }
static void arrive(void *context) {
 assert(context==reader->state);uint8_t byte=42;uint64_t bytes=0;
 assert(pipe_write(writer,&byte,1,0,&bytes)==K_OK && bytes==1);
}
static void steal(void *context) {
 assert(context==reader->state);uint8_t byte=0;uint64_t bytes=0;
 assert(pipe_read(reader,&byte,1,0,&bytes)==K_OK && bytes==1 && byte==42);
}
static void free_one(void *context) {
 pipe_write_context_t *w=context;assert(w->state==reader->state);
 uint8_t byte;uint64_t bytes;
 assert(pipe_read(reader,&byte,1,0,&bytes)==K_OK && bytes==1);
}
static void take_free_space(void *context) {
 pipe_write_context_t *w=context;assert(w->state==reader->state);
 uint64_t bytes;assert(pipe_write(writer,input,OS_PIPE_BUF,0,&bytes)==K_OK && bytes==OS_PIPE_BUF);
}
static void close_writer(void *context) {
 assert(context==reader->state);object_put(writer);writer=0;
}
static void close_reader(void *context) {
 pipe_write_context_t *w=context;assert(w->state==reader->state);
 object_put(reader);reader=0;
}
static void shutdown_local_reader(void *context) {
 assert(context==reader->state);assert(pipe_shutdown(reader)==K_OK);
}
static void shutdown_local_writer(void *context) {
 pipe_write_context_t *w=context;assert(w->state==writer->state);
 assert(pipe_shutdown(writer)==K_OK);
}
static void *producer(void *arg) {
 unsigned id=(unsigned)(uintptr_t)arg;
 for(unsigned sequence=0;sequence<1000;++sequence) {
  uint8_t record[32];record[0]=(uint8_t)id;
  for(unsigned j=0;j<4;++j)record[1+j]=(uint8_t)(sequence>>(8*j));
  for(unsigned j=5;j<32;++j)record[j]=(uint8_t)(id*17+sequence+j);
  uint64_t bytes; kstatus_t status;
  while((status=pipe_write(writer,record,sizeof(record),0,&bytes))==K_ETIMEDOUT)sched_yield();
  assert(status==K_OK && bytes==sizeof(record));
 }
 object_put(writer);return 0;
}
static atomic_uint destroy_ready;
static atomic_bool destroy_go;
static void *destroy_endpoint(void *endpoint) {
 atomic_fetch_add(&destroy_ready,1);
 while(!atomic_load(&destroy_go))sched_yield();
 object_put(endpoint);return 0;
}
int main(void) {
 ancillary_boundary_tests();
 for(unsigned i=0;i<sizeof(input);++i)input[i]=(uint8_t)(i*17+3);
 for(unsigned stage=1;stage<=2;++stage) {
  reader=writer=0;fail_at=attempts+stage;
  assert(pipe_create(0,&reader,&writer)==K_ENOMEM && !reader && !writer);
  assert(allocations==frees);
 }
 fail_at=0;make_pipe();uint64_t bytes=99;
 // Only empty -> readable needs a direct-reader wake; poll notifications
 // must still be emitted for every write, including a nonempty ring.
 unsigned transition_wakes=read_wakes,transition_notify=notifications;
 for(unsigned i=0;i<1000;++i) {
  assert(pipe_write(writer,input,1,0,&bytes)==K_OK && bytes==1);
  assert(read_wakes==transition_wakes+1);
  assert(notifications==transition_notify+i+1);
 }
 assert(pipe_read(reader,output,1000,0,&bytes)==K_OK && bytes==1000);
 assert(pipe_write(writer,input,1,0,&bytes)==K_OK && bytes==1);
 assert(read_wakes==transition_wakes+2);
 assert(pipe_read(reader,output,1,0,&bytes)==K_OK && bytes==1);
 assert(pipe_read(reader,0,0,0,&bytes)==K_OK && !bytes);
 assert(pipe_write(writer,0,0,0,&bytes)==K_OK && !bytes);
 unsigned before=waits,start=locks;
 assert(pipe_write(writer,input,OS_PIPE_DEFAULT_SIZE,0,&bytes)==K_OK && bytes==OS_PIPE_DEFAULT_SIZE);
 assert(waits==before && locks==start+1); // transfer and referenced peer lookup share a lock.
 start=locks;bytes=99;
 assert(pipe_write(writer,input,1,0,&bytes)==K_ETIMEDOUT && !bytes);
 assert(waits==before && locks==start+1);
 start=locks;
 assert(pipe_read(reader,output,OS_PIPE_BUF-1,0,&bytes)==K_OK && bytes==OS_PIPE_BUF-1);
 assert(locks==start+1);
 assert(!memcmp(output,input,bytes));
 size_t old_count=reader->state->count;
 assert(pipe_write(writer,input,OS_PIPE_BUF,0,&bytes)==K_ETIMEDOUT && !bytes);
 assert(reader->state->count==old_count);
 before_wait=free_one;
 assert(pipe_write(writer,input,OS_PIPE_BUF,123,&bytes)==K_OK && bytes==OS_PIPE_BUF);
 assert(waits==before+1 && reader->state->count==OS_PIPE_DEFAULT_SIZE);
 assert(pipe_read(reader,output,1,0,&bytes)==K_OK && bytes==1);
 assert(pipe_write(writer,input,OS_PIPE_DEFAULT_SIZE+1,0,&bytes)==K_OK && bytes==1);
 assert(pipe_read(reader,output,sizeof(output),0,&bytes)==K_OK && bytes==OS_PIPE_DEFAULT_SIZE);
 before=waits;start=locks;
 assert(pipe_read(reader,output,1,0,&bytes)==K_ETIMEDOUT && !bytes);
 assert(waits==before && locks==start+1);
 before_wait=arrive;
 assert(pipe_read(reader,output,1,123,&bytes)==K_OK && bytes==1 && output[0]==42);
 assert(waits==before+1);
 before_wait=arrive;after_ready=steal;
 assert(pipe_read(reader,output,1,123,&bytes)==K_ETIMEDOUT && !bytes);
 assert(waits==before+3);
 // A competing atomic writer consumes newly available space before copying.
 assert(pipe_write(writer,input,OS_PIPE_DEFAULT_SIZE-OS_PIPE_BUF+1,0,&bytes)==K_OK);
 before_wait=free_one;after_ready=take_free_space;before=waits;
 assert(pipe_write(writer,input,OS_PIPE_BUF,123,&bytes)==K_ETIMEDOUT && !bytes);
 assert(waits==before+2 && reader->state->count==OS_PIPE_DEFAULT_SIZE);
 release_pair();
 make_pipe();before_wait=close_writer;
 unsigned reads=read_wakes,writes=write_wakes;
 assert(pipe_read(reader,output,1,123,&bytes)==K_OK && !bytes && !writer);
 assert(read_wakes==reads+1 && write_wakes==writes);
 assert(pipe_wait_value(reader)&PIPE_WAIT_HUP);object_put(reader);reader=0;
 assert(read_wakes==reads+1 && write_wakes==writes+1);
 make_pipe();assert(pipe_write(writer,input,OS_PIPE_DEFAULT_SIZE,0,&bytes)==K_OK);
 before_wait=close_reader;
 reads=read_wakes;writes=write_wakes;
 assert(pipe_write(writer,input,1,123,&bytes)==K_EPIPE && !bytes && !reader);
 assert(read_wakes==reads && write_wakes==writes+1);
 assert(pipe_wait_value(writer)&PIPE_WAIT_ERROR);object_put(writer);writer=0;
 assert(read_wakes==reads+1 && write_wakes==writes+1);
 // Endpoint references, EOF after draining and ordinary mixed FIFO I/O.
 make_pipe();object_get(writer);object_put(writer);
 assert(writer->state->writers==1);
 unsigned rng=7;size_t count=0;
 start=locks;
 for(unsigned step=0;step<10000;++step) {
  rng=rng*1664525U+1013904223U;size_t length=1+(rng%8192);
  if(rng&0x80000000U) {
   size_t free_space=OS_PIPE_DEFAULT_SIZE-count;
   kstatus_t status=pipe_write(writer,input,length,0,&bytes);
   if(free_space<(length<=OS_PIPE_BUF?length:1))assert(status==K_ETIMEDOUT && !bytes);
   else {
    size_t expected=free_space<length?free_space:length;
    assert(status==K_OK && bytes==expected);
    memcpy(fifo+count,input,expected);count+=expected;
   }
  } else {
   memset(output,0x69,sizeof(output));
   kstatus_t status=pipe_read(reader,output+16,length,0,&bytes);
   if(!count)assert(status==K_ETIMEDOUT && !bytes);
   else {
    size_t expected=count<length?count:length;
    assert(status==K_OK && bytes==expected && !memcmp(output+16,fifo,expected));
    count-=expected;memmove(fifo,fifo+expected,count);
   }
   for(unsigned i=0;i<16;++i)assert(output[i]==0x69);
   for(size_t i=16+bytes;i<sizeof(output);++i)assert(output[i]==0x69);
  }
  assert(reader->state->count==count);
 }
 assert(locks==start+10000); // One ring acquisition per nonblocking transfer/probe.
 object_put(writer);writer=0;
 assert(pipe_read(reader,output,sizeof(output),0,&bytes)==K_OK && bytes==count);
 assert(!memcmp(output,fifo,count));
 assert(pipe_read(reader,output,1,0,&bytes)==K_OK && !bytes);
 object_put(reader);reader=0;
 // Pinning before wake must keep either peer alive through the notification.
 make_pipe();unsigned notified=notifications;close_on_wake=true;
 assert(pipe_write(writer,input,1,0,&bytes)==K_OK && bytes==1 && !reader);
 assert(notifications==notified+2 && writer->state->readers==0);
 assert(pipe_write(writer,input,1,0,&bytes)==K_EPIPE && !bytes);
 object_put(writer);writer=0;
 make_pipe();assert(pipe_write(writer,input,2,0,&bytes)==K_OK);
 notified=notifications;close_on_wake=true;
 assert(pipe_read(reader,output,1,0,&bytes)==K_OK && bytes==1 && !writer);
 assert(notifications==notified+2 && reader->state->writers==0);
 assert(pipe_read(reader,output,1,0,&bytes)==K_OK && bytes==1);
 assert(pipe_read(reader,output,1,0,&bytes)==K_OK && !bytes);
 object_put(reader);reader=0;
 make_pipe();pthread_t producers[4];unsigned received[4]={0},total=0;
 for(unsigned i=0;i<4;++i) {
  object_get(writer);assert(!pthread_create(&producers[i],0,producer,(void*)(uintptr_t)i));
 }
 while(total<4000) {
  uint8_t records[256];
  kstatus_t status=pipe_read(reader,records,sizeof(records),0,&bytes);
  if(status==K_ETIMEDOUT) {sched_yield();continue;}
  assert(status==K_OK && bytes>0 && bytes%32==0);
  for(unsigned offset=0;offset<bytes;offset+=32) {
   uint8_t *record=records+offset;unsigned id=record[0];assert(id<4);
   unsigned sequence=0;
   for(unsigned j=0;j<4;++j)sequence|=(unsigned)record[1+j]<<(8*j);
   assert(sequence==received[id]++);
   for(unsigned j=5;j<32;++j)assert(record[j]==(uint8_t)(id*17+sequence+j));
   ++total;
  }
 }
 for(unsigned i=0;i<4;++i) {assert(!pthread_join(producers[i],0));assert(received[i]==1000);}
 assert(reader->state->count==0 && atomic_load(&writer->object.refs.value)==1);
 release_pair();
 // Nested peer destruction must not release the outer destructor's storage.
 make_pipe();close_on_wake=true;
 object_put(reader);reader=0;
 assert(!writer && !close_on_wake && allocations==frees);live_state=0;
 // Concurrent last-reference drops must broadcast once per direction and
 // release the shared ring exactly once, including nested peer destruction.
 for(unsigned cycle=0;cycle<2000;++cycle) {
  make_pipe();reads=read_wakes;writes=write_wakes;
  atomic_store(&destroy_ready,0);atomic_store(&destroy_go,false);
  pthread_t closers[2];
  assert(!pthread_create(&closers[0],0,destroy_endpoint,reader));
  assert(!pthread_create(&closers[1],0,destroy_endpoint,writer));
  while(atomic_load(&destroy_ready)!=2)sched_yield();
  atomic_store(&destroy_go,true);
  for(unsigned i=0;i<2;++i)assert(!pthread_join(closers[i],0));
  reader=writer=0;live_state=0;
  assert(read_wakes==reads+1 && write_wakes==writes+1);
  assert(allocations==frees);
 }
 make_pipe();object_get(reader);before_wait=shutdown_local_reader;
 unsigned shutdown_read_before=read_wakes,shutdown_write_before=write_wakes;
 assert(pipe_read(reader,output,1,123,&bytes)==K_OK && !bytes);
 assert(read_wakes==shutdown_read_before+1 && write_wakes==shutdown_write_before+1);
 assert(reader->state->readers==0 && atomic_load(&reader->object.refs.value)==2);
 unsigned shutdown_wakes=all_wakes;
 assert(pipe_shutdown(reader)==K_OK && all_wakes==shutdown_wakes);
 assert(pipe_write(writer,input,1,0,&bytes)==K_EPIPE && !bytes);
 assert(pipe_wait_value(reader)&PIPE_WAIT_HUP);
 object_put(reader);release_pair();
 make_pipe();object_get(writer);
 assert(pipe_write(writer,input,OS_PIPE_DEFAULT_SIZE,0,&bytes)==K_OK);
 shutdown_read_before=read_wakes;shutdown_write_before=write_wakes;
 before_wait=shutdown_local_writer;
 assert(pipe_write(writer,input,1,123,&bytes)==K_EPIPE && !bytes);
 assert(read_wakes==shutdown_read_before+1 && write_wakes==shutdown_write_before+1);
 assert(writer->state->writers==0 && atomic_load(&writer->object.refs.value)==2);
 assert(pipe_read(reader,output,OS_PIPE_DEFAULT_SIZE,0,&bytes)==K_OK && bytes==OS_PIPE_DEFAULT_SIZE);
 assert(pipe_read(reader,output,1,0,&bytes)==K_OK && !bytes);
 assert(pipe_wait_value(writer)&PIPE_WAIT_ERROR);
 object_put(writer);release_pair();
 assert(allocations==frees && !before_wait && !after_ready && !one_wakes && all_wakes && notifications);
 puts("PASS: two exact allocations per pipe with embedded endpoints/poisoned storage, one lock per transfer/probe, atomic threshold/short write, wait-boundary races, directional close wakes, close-during-notify/drain/refs, two OOM stages, 10000 FIFO operations, 4000 producer records and 2000 concurrent pair destructions");
}
'''
boundary_tests = ancillary + r'''
static linux_unix_channel_t ancillary_channel;
static void send_fragment(unsigned n, unsigned fds) {
 uint64_t bytes;
 if(fds) {
  linux_unix_ancillary_record_t record={.payload_length=n,.fd_count=fds};
  assert(linux_unix_channel_push(&ancillary_channel,&record)==K_OK);
  assert(!record.fd_count);
 }
 assert(pipe_write(writer,input,n,0,&bytes)==K_OK && bytes==n);
 linux_unix_channel_commit_write(&ancillary_channel,bytes);
}
static void ancillary_arrives_while_waiting(void *context) {
 assert(context==reader->state);
 send_fragment(3,0);send_fragment(4,1);send_fragment(2,2);
}
static size_t invalid_limit(void *context,size_t available) {
 return context?available+1:0;
}
static void ancillary_boundary_tests(void) {
 uint64_t bytes;linux_unix_ancillary_record_t record={0};
 memset(&ancillary_channel,0,sizeof(ancillary_channel));
 spinlock_init(&ancillary_channel.lock);make_pipe();
 /* The old pre-wait snapshot has no records and allows all nine bytes.
  * Arrival while the reader sleeps must not make it consume/close both FDs. */
 assert(linux_unix_channel_read_limit(&ancillary_channel,9)==9);
 before_wait=ancillary_arrives_while_waiting;
 assert(pipe_read_limited(reader,output,9,123,&bytes,
  linux_unix_channel_pipe_limit,&ancillary_channel)==K_OK && bytes==3);
 assert(!linux_unix_channel_commit_read(&ancillary_channel,bytes,true,&record));
 assert(!discarded_fds && ancillary_channel.count==2);
 assert(pipe_read_limited(reader,output,9,0,&bytes,
  linux_unix_channel_pipe_limit,&ancillary_channel)==K_OK && bytes==4);
 assert(linux_unix_channel_commit_read(&ancillary_channel,bytes,true,&record));
 assert(record.fd_count==1 && !discarded_fds && ancillary_channel.count==1);
 assert(pipe_read_limited(reader,output,1,0,&bytes,
  linux_unix_channel_pipe_limit,&ancillary_channel)==K_OK && bytes==1);
 assert(linux_unix_channel_commit_read(&ancillary_channel,bytes,true,&record));
 assert(record.fd_count==2 && !discarded_fds && !ancillary_channel.count);
 assert(pipe_read_limited(reader,output,9,0,&bytes,
  linux_unix_channel_pipe_limit,&ancillary_channel)==K_OK && bytes==1);
 assert(!linux_unix_channel_commit_read(&ancillary_channel,bytes,true,&record));
 assert(ancillary_channel.bytes_read==9);
 send_fragment(2,3);
 assert(pipe_read(reader,output,2,0,&bytes)==K_OK && bytes==2);
 assert(!linux_unix_channel_commit_read(&ancillary_channel,bytes,false,&record));
 assert(discarded_fds==3); /* Plain read still intentionally discards rights. */
 send_fragment(1,0);
 assert(pipe_read_limited(reader,output,1,0,&bytes,invalid_limit,0)==K_EINVAL && !bytes);
 assert(pipe_read_limited(reader,output,1,0,&bytes,invalid_limit,reader)==K_EINVAL && !bytes);
 assert(pipe_read(reader,output,1,0,&bytes)==K_OK && bytes==1);
 assert(pipe_read_limited(reader,output,1,0,&bytes,
  linux_unix_channel_pipe_limit,&ancillary_channel)==K_ETIMEDOUT && !bytes);
 release_pair();
 puts("PASS: production Unix ancillary boundaries rechecked after blocking wait; multiple FD batches, partial reads, plain-read discard, invalid limiter");
}
'''
code = code.replace('int main(void) {', boundary_tests + '\nint main(void) {', 1)
with tempfile.TemporaryDirectory(prefix='liteos-pipe-io-') as directory:
    binary = str(Path(directory) / 'test')
    def run(actual):
        subprocess.run(['cc', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                    '-I' + str(root / 'include'), '-pthread', '-fsanitize=address,undefined',
                    '-fno-pie', '-no-pie', '-x', 'c', '-', '-o', binary],
                   input=actual, text=True, check=True)
        return subprocess.run([binary],capture_output=True,text=True,timeout=60)
    result=run(code);assert result.returncode==0,result.stderr;print(result.stdout.strip())
    negative=code.replace('size_t bounded = limit(context, amount);',
                          'size_t bounded = amount; (void)context;')
    assert negative!=code
    result=run(negative);assert result.returncode!=0,result.stderr
    print('Negative control rejected: stale pre-wait boundary loses ancillary FDs')
    negative=code.replace('if (wake_readers) (void)wake_all(&state->read_waitq);',
                          '(void)wake_all(&state->read_waitq); (void)wake_readers;')
    assert negative!=code
    result=run(negative);assert result.returncode!=0,result.stderr
    print('Negative control rejected: redundant direct-reader wakes on nonempty writes')
    negative=code.replace('if (wake_readers) (void)wake_all(&state->read_waitq);',
                          '(void)wake_readers;')
    result=run(negative);assert result.returncode!=0,result.stderr
    print('Negative control rejected: missing empty-to-readable wake')
    negative=code.replace('    memset(state, 0, __builtin_offsetof(pipe_state_t, endpoints));','')
    result=run(negative);assert result.returncode!=0,result.stderr
    print('Negative control rejected: shared metadata remains poisoned')
    negative=code.replace('    }\n}\n\nstatic const object_ops_t g_pipe_endpoint_ops',
                          '    }\n    kfree(endpoint);\n}\n\nstatic const object_ops_t g_pipe_endpoint_ops')
    assert negative!=code
    result=run(negative);assert result.returncode!=0,result.stderr
    print('Negative control rejected: freeing an embedded endpoint separately')
    negative=code.replace('state->endpoint_refs == 0U && state->active_destroyers == 0U &&',
                          'state->endpoint_refs == 0U &&')
    result=run(negative);assert result.returncode!=0,result.stderr
    print('Negative control rejected: shared endpoint storage freed before all destructors finish')
