"""Execute real ring reassembly: reordered/overlapping bytes, FIN and wrap."""
from pathlib import Path
import os
import subprocess
import sys
import tempfile

root=Path(__file__).resolve().parents[2]
source=(root/'kernel/net/socket.c').read_text()
ring=source[source.index('uint32_t socket_stream_write_locked('):
            source.index('static uint32_t socket_active_ipv4_address(')]
protocol=(root/'kernel/net/socket_protocol.c').read_text()
trim=protocol[protocol.index('static kstatus_t socket_tcp_trim_overlap('):
              protocol.index('static void socket_tcp_apply_mss(')]
assert protocol.count('kstatus_t reassembly = socket_tcp_reassemble_locked(')==2
reassembly=(root/'kernel/net/tcp_reassembly.h').read_text()
instrumented=reassembly.replace('while (contiguous < available) {',
                               'while (contiguous < available) { ++scan_steps;')
instrumented=instrumented.replace('while (index < length) {',
                                'while (index < length) { ++insert_steps;')
assert instrumented != reassembly
program=r'''
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <kernel/net_core.h>
#include "kernel/net/socket_model.h"
static unsigned bulk_calls,scan_steps,insert_steps;
#include "kernel/net/tcp_reassembly.h"
void socket_copy(void *d,const void *s,size_t n){++bulk_calls;memmove(d,s,n);}
''' + ring + trim + r'''
static socket_private_t state;
static uint8_t bits[(SOCKET_STREAM_BUFFER_SIZE+7)/8];
static uint8_t data[SOCKET_STREAM_BUFFER_SIZE],output[SOCKET_STREAM_BUFFER_SIZE];
static void reset(uint32_t base,unsigned position) {
 memset(&state,0,sizeof(state));memset(bits,0,sizeof(bits));
 state.tcp_rx_present=bits;state.tcp_sequence_valid=true;state.tcp_next_sequence=base;
 state.stream_write=state.stream_read=position;
 for(unsigned i=0;i<sizeof(data);++i)data[i]=(uint8_t)(i*37+(i>>8));
}
static int inject(uint32_t seq,const void* input,size_t length,unsigned flags,bool *notify) {
 uint32_t expected=state.tcp_next_sequence;
 kstatus_t status=socket_tcp_trim_overlap(&seq,expected,&input,&length,flags);
 if(status!=K_OK && !(status==K_EAGAIN && state.tcp_rx_present && !atomic_load(&state.receive_shutdown)))
  return status==K_ENOENT?K_OK:status;
 return socket_tcp_reassemble_locked(&state,seq,expected,flags,input,length,notify);
}
static void verify(unsigned size) {
 assert(atomic_load(&state.stream_count)==size);
 unsigned next=socket_stream_read_locked(&state,output,size);
 assert(!memcmp(output,data,size));
 state.stream_read=next;atomic_store(&state.stream_count,0);
 assert(!state.tcp_rx_future_count);
 for(unsigned i=0;i<sizeof(bits);++i)assert(!bits[i]);
}
int main(void) {
 unsigned max_scan_steps=0;
 // Retain the in-window prefix of a segment crossing the right edge.
 // A FIN at/outside that edge must be retransmitted after space opens;
 // it must not discard valid payload or close the stream prematurely.
 unsigned edge_cases=0;
 for(unsigned position=0;position<16;++position)
 for(unsigned wrap=0;wrap<2;++wrap)
 for(unsigned mode=0;mode<3;++mode)
 for(unsigned ending=0;ending<3;++ending){
  bool notify;uint32_t base=wrap?UINT32_MAX-100:1000;
  reset(base,SOCKET_STREAM_BUFFER_SIZE-16+position);
  if(mode==2)state.tcp_rx_present=NULL;
  unsigned prefix=SOCKET_STREAM_BUFFER_SIZE-4;
  assert(inject(base,data,prefix,0,&notify)==K_OK && notify);
  uint32_t expected=state.tcp_next_sequence;
  const uint8_t tail[8]={91,92,93,94,95,96,97,98};
  unsigned flags=ending?NET_TCP_FLAG_FIN:0;
  unsigned length=ending==1?4:8;
  if(mode==1){
   assert(inject(expected+1,tail+1,length-1,flags,&notify)==K_OK && !notify);
   assert(state.tcp_rx_future_count==3 && state.tcp_next_sequence==expected);
   assert(!state.tcp_rx_fin_pending);
   assert(inject(expected,tail,1,0,&notify)==K_OK && notify);
  }else{
   assert(inject(expected,tail,length,flags,&notify)==K_OK && notify);
  }
  assert(atomic_load(&state.stream_count)==SOCKET_STREAM_BUFFER_SIZE);
  assert(state.tcp_next_sequence==expected+4 && !state.tcp_rx_future_count);
  assert(!state.tcp_rx_fin_pending && !atomic_load(&state.peer_closed));
  assert(inject(expected+4,tail,1,0,&notify)==K_EAGAIN && !notify);
  assert(inject(expected+4,NULL,0,NET_TCP_FLAG_FIN,&notify)==K_EAGAIN);
  state.stream_read=socket_stream_read_locked(&state,output,sizeof(output));
  assert(!memcmp(output,data,prefix) && !memcmp(output+prefix,tail,4));
  atomic_store(&state.stream_count,0);
  assert(inject(expected+4,NULL,0,NET_TCP_FLAG_FIN,&notify)==K_OK && notify);
  assert(state.tcp_next_sequence==expected+5 && atomic_load(&state.peer_closed));
  ++edge_cases;
 }
 printf("Receive right edge: %u partial payload/FIN, zero-window, ring/sequence wrap and OOM fallback cases passed\n",edge_cases);
 // Exhaust every ownership byte, not only empty/full fast-path cases.
 // Conflicting retransmissions must preserve all first-accepted bytes.
 for(unsigned mask=0;mask<256;++mask)
 for(unsigned position=0;position<16;++position){
  bool notify;reset(UINT32_MAX-4,SOCKET_STREAM_BUFFER_SIZE-16+position);
  uint32_t base=state.tcp_next_sequence;
  uint8_t incoming[8],expected[16];memcpy(expected,data,8);
  for(unsigned i=0;i<8;++i){
   incoming[i]=(uint8_t)(data[8+i]^0xff);
   expected[8+i]=(mask&(1U<<i))?data[8+i]:incoming[i];
   if(mask&(1U<<i))assert(inject(base+8+i,data+8+i,1,0,&notify)==K_OK);
  }
  assert(inject(base+8,incoming,8,0,&notify)==K_OK && !notify);
  assert(state.tcp_rx_future_count==8);
  assert(inject(base,data,8,0,&notify)==K_OK && notify);
  assert(atomic_load(&state.stream_count)==16 && !state.tcp_rx_future_count);
  socket_stream_read_locked(&state,output,16);assert(!memcmp(output,expected,16));
 }
 // Insertion and duplicate skipping both have a bounded lock-held loop.
 for(unsigned position=0;position<16;++position){
  bool notify;reset(1000,SOCKET_STREAM_BUFFER_SIZE-16+position);
  unsigned before=insert_steps;
  unsigned copies=bulk_calls;
  assert(inject(1001,data+1,4095,0,&notify)==K_OK && !notify);
  assert(insert_steps-before<=530);
  // 连续空闲区最多在环尾拆成两次复制，重复包不应再复制负载。
  assert(bulk_calls-copies<=2);
  copies=bulk_calls;
  before=insert_steps;
  assert(inject(1001,data+1,4095,0,&notify)==K_OK && !notify);
  assert(insert_steps-before<=530);
  assert(bulk_calls==copies);
  assert(inject(1000,data,1,0,&notify)==K_OK && notify);verify(4096);
 }
 // A filled gap releases a large retained run. Its aligned bitmap bytes
 // should advance in groups, not hold the socket lock once per data byte.
 for(unsigned position=0;position<8;++position){
  bool notify;reset(UINT32_MAX-1000,SOCKET_STREAM_BUFFER_SIZE-8+position);
  uint32_t base=state.tcp_next_sequence;
  assert(inject(base+1,data+1,4095,0,&notify)==K_OK && !notify);
  unsigned before=scan_steps;
  assert(inject(base,data,1,0,&notify)==K_OK && notify);
  assert(scan_steps-before<=530);
  if(scan_steps-before>max_scan_steps)max_scan_steps=scan_steps-before;
  assert(state.tcp_next_sequence==base+4096);verify(4096);
 }
 // Every FIN bit position must stop grouped advancement exactly at EOF,
 // including unaligned cursors and both ring/sequence wrap.
 for(unsigned position=0;position<16;++position)
 for(unsigned end=1;end<32;++end){
  bool notify;reset(UINT32_MAX-15,SOCKET_STREAM_BUFFER_SIZE-16+position);
  uint32_t base=state.tcp_next_sequence;
  assert(inject(base+end,NULL,0,NET_TCP_FLAG_FIN,&notify)==K_OK && !notify);
  assert(inject(base+1,data+1,end-1,0,&notify)==K_OK && !notify);
  assert(!atomic_load(&state.peer_closed));
  assert(inject(base,data,1,0,&notify)==K_OK && notify);
  assert(state.tcp_next_sequence==base+end+1 && atomic_load(&state.peer_closed));
  verify(end);
 }
 // Data may have been retained beyond an earlier FIN before FIN arrived.
 // Even a full bitmap byte must not publish those bytes past EOF.
 for(unsigned end=1;end<16;++end){
  bool notify;reset(UINT32_MAX-7,SOCKET_STREAM_BUFFER_SIZE-8);
  uint32_t base=state.tcp_next_sequence;
  assert(inject(base+1,data+1,31,0,&notify)==K_OK && !notify);
  assert(inject(base+end,NULL,0,NET_TCP_FLAG_FIN,&notify)==K_OK && !notify);
  assert(inject(base,data,1,0,&notify)==K_OK && notify);
  assert(state.tcp_next_sequence==base+end+1 && atomic_load(&state.peer_closed));
  assert(atomic_load(&state.stream_count)==end && state.tcp_rx_future_count==32-end);
  socket_stream_read_locked(&state,output,end);assert(!memcmp(output,data,end));
 }
 const uint32_t bases[]={1000,UINT32_MAX-1000};
 const unsigned sizes[]={536,1460};unsigned cases=0;
 for(unsigned b=0;b<2;++b)for(unsigned m=0;m<2;++m)
 for(unsigned position=0;position<2;++position) {
  uint32_t base=bases[b];unsigned size=sizes[m];bool notify;
  reset(base,position?SOCKET_STREAM_BUFFER_SIZE-17:0);
  for(unsigned packet=1;packet<16;++packet) {
   assert(inject(base+packet*size,data+packet*size,size,0,&notify)==K_OK);
   assert(!notify && !atomic_load(&state.stream_count));
   assert(state.tcp_next_sequence==base);
  }
  assert(state.tcp_rx_future_count==15*size);
  assert(inject(base,data,size,0,&notify)==K_OK && notify);
  assert(state.tcp_next_sequence==base+16*size);verify(16*size);++cases;
 }
 // Repeated windows with shuffled arrivals and application reads exercise
 // bitmap reuse; partial duplicates must not alter first-accepted bytes.
 uint32_t random=0x9215;
 for(unsigned trial=0;trial<100;++trial) {
  uint32_t base=UINT32_MAX-trial*100;bool notify;unsigned order[64];
  reset(base,(trial*977)%SOCKET_STREAM_BUFFER_SIZE);
  for(unsigned i=0;i<64;++i)order[i]=i;
  for(unsigned i=63;i>0;--i){random=random*1664525+1013904223;unsigned j=random%(i+1),tmp=order[j];order[j]=order[i];order[i]=tmp;}
  for(unsigned i=0;i<64;++i)assert(inject(base+order[i]*73,data+order[i]*73,73,0,&notify)==K_OK);
  verify(64*73);
  base+=64*73;
  assert(inject(base+73,data+73,73,0,&notify)==K_OK && !notify);
  assert(inject(base,data,73,0,&notify)==K_OK && notify);verify(146);++cases;
 }
 bool notify;reset(1000,0);
 assert(inject(1008,data+8,8,0,&notify)==K_OK && !notify);
 uint8_t conflicting[16];memcpy(conflicting,data,16);memset(conflicting+8,0,8);
 assert(inject(1000,conflicting,16,0,&notify)==K_OK && notify);verify(16);
 reset(UINT32_MAX-5,SOCKET_STREAM_BUFFER_SIZE-3);
 uint32_t base=state.tcp_next_sequence;
 assert(inject(base+8,data+8,2,NET_TCP_FLAG_FIN,&notify)==K_OK && !notify);
 assert(!atomic_load(&state.peer_closed));
 assert(inject(base+11,data,1,0,&notify)==K_EAGAIN);
 assert(inject(base,data,8,0,&notify)==K_OK && notify);
 assert(state.tcp_next_sequence==base+11 && atomic_load(&state.peer_closed));verify(10);
 assert(inject(base+11,data,8,0,&notify)==K_OK && !notify);
 assert(!atomic_load(&state.stream_count));
 reset(1000,0);
 atomic_store(&state.stream_count,SOCKET_STREAM_BUFFER_SIZE-4);
 assert(inject(1004,data,1,0,&notify)==K_EAGAIN);
 assert(!state.tcp_rx_future_count && state.tcp_next_sequence==1000);
 atomic_store(&state.stream_count,0);
 unsigned before=bulk_calls;
 assert(inject(1000,data,1460,0,&notify)==K_OK && notify);
 assert(bulk_calls>before);verify(1460);
 reset(1000,0);state.tcp_rx_present=NULL;
 assert(inject(1001,data,1,0,&notify)==K_EAGAIN);
 assert(inject(1000,data,32,0,&notify)==K_OK);verify(32);
 reset(1000,0);atomic_store(&state.receive_shutdown,true);
 assert(inject(1001,data,1,0,&notify)==K_EAGAIN);
 assert(inject(1000,data,32,0,&notify)==K_OK && !notify);
 assert(state.tcp_next_sequence==1032 && !atomic_load(&state.stream_count));
 // Dynamic rings exercise the variable divisor path, including a capacity
 // not divisible by eight. No advance may skip the exact wrap boundary.
 const unsigned capacities[]={32771,262144,4194304};
 for(unsigned c=0;c<3;++c)for(unsigned tail=1;tail<=16;++tail){
  unsigned capacity=capacities[c];reset(UINT32_MAX-1000,0);
  state.stream_dynamic=calloc(capacity,1);assert(state.stream_dynamic);
  state.stream_capacity=capacity;
  state.tcp_rx_present=calloc((capacity+7)/8,1);assert(state.tcp_rx_present);
  state.stream_write=state.stream_read=capacity-tail;
  uint32_t base=state.tcp_next_sequence;
  assert(socket_tcp_reassemble_locked(&state,base+1,base,0,data+1,4095,&notify)==K_OK && !notify);
  assert(socket_tcp_reassemble_locked(&state,base+1,base,0,data+1,4095,&notify)==K_OK && !notify);
  assert(state.tcp_rx_future_count==4095);
  assert(socket_tcp_reassemble_locked(&state,base,base,0,data,1,&notify)==K_OK && notify);
  assert(state.tcp_next_sequence==base+4096 && !state.tcp_rx_future_count);
  assert(atomic_load(&state.stream_count)==4096);
  socket_stream_read_locked(&state,output,4096);assert(!memcmp(data,output,4096));
  for(unsigned i=0;i<(capacity+7)/8;++i)assert(!state.tcp_rx_present[i]);
  free(state.stream_dynamic);free(state.tcp_rx_present);
 }
 printf("Dynamic rings: 48 capacity/wrap/duplicate/gap cases passed\n");
 printf("TCP reassembly: %u gap/shuffle cases plus overlap, FIN, capacity, OOM fallback, shutdown and fast path passed\n",cases);
 printf("Bitmap scan: 4096-byte gap release max %u iterations; 496 FIN/alignment and 15 retained-beyond-FIN boundaries passed\n",max_scan_steps);
}
'''
receivers=protocol[protocol.index('kstatus_t socket_inject_tcp_ipv4_reply('):
                   protocol.index('kstatus_t socket_inject_tcp_ipv4(')]
mocks=r'''
static socket_t target;
static socket_tcp_connection_t connection4;
static socket_tcp6_connection_t connection6;
static spinlock_t binding;
static unsigned wakes,refs;
void socket_initialize_globals(void) {}
spinlock_t* socket_binding_lock(void) {return &binding;}
void socket_lock(spinlock_t* lock) {(void)lock;}
void socket_unlock(spinlock_t* lock) {(void)lock;}
void socket_zero(void* p,size_t n) {memset(p,0,n);}
socket_private_t* socket_private(socket_t* s) {assert(s==&target);return &state;}
bool object_try_get(void* s) {assert(s==&target);++refs;return true;}
void object_put(void* s) {assert(s==&target && refs);--refs;}
void object_notify_signaled(void* s) {assert(s==&target);}
void object_notify_changed(void* s) {assert(s==&target);}
uint32_t wake_all(wait_queue_t* q) {assert(q==&target.waitq);++wakes;return 0;}
int32_t socket_find_tcp_locked(uint32_t s,uint16_t sp,uint32_t d,uint16_t dp) {
 (void)s;(void)sp;(void)d;(void)dp;return 0;
}
int32_t socket_find_tcp6_locked(const uint8_t s[16],uint16_t sp,const uint8_t d[16],uint16_t dp) {
 (void)s;(void)sp;(void)d;(void)dp;return 0;
}
const socket_tcp_connection_t* socket_tcp_connection_at(uint32_t i) {
 assert(!i);connection4.socket=&target;return &connection4;
}
const socket_tcp6_connection_t* socket_tcp6_connection_at(uint32_t i) {
 assert(!i);connection6.socket=&target;return &connection6;
}
bool socket_address6_zero(const uint8_t a[16]) {return !a[0];}
'''
wrapper=r'''
static int inject(uint32_t seq,const void* input,size_t length,unsigned flags,bool *notify) {
 state.tcp_state=SOCKET_TCP_ESTABLISHED;
 unsigned before=wakes;socket_tcp_reply_t reply={0};int result;
#if TEST_FAMILY == 4
 result=socket_inject_tcp_ipv4_reply(1,443,2,50000,seq,
   flags|NET_TCP_FLAG_ACK,input,length,&reply);
#else
 uint8_t source[16]={0x20,1},dest[16]={0x20,2};
 result=socket_inject_tcp_ipv6_reply(source,443,dest,50000,seq,
   flags|NET_TCP_FLAG_ACK,input,length,&reply);
#endif
 *notify=wakes!=before;
 assert(!refs && reply.valid);
 assert(reply.acknowledgement==state.tcp_next_sequence);
 assert(reply.window==SOCKET_STREAM_BUFFER_SIZE-atomic_load(&state.stream_count));
 return result;
}
'''
start=program.index('static int inject(')
end=program.index('static void verify(',start)
program=program[:start]+mocks+receivers+wrapper+program[end:]
with tempfile.TemporaryDirectory(prefix='tcp-reassembly-') as directory:
    path=Path(directory)
    (path/'instrumented.h').write_text(instrumented)
    (path/'test.c').write_text(program.replace('"kernel/net/tcp_reassembly.h"','"instrumented.h"'))
    for family in (4,6):
        subprocess.run(['cc','-std=gnu11','-O2','-Wall','-Wextra','-Werror',
                    '-DTEST_FAMILY='+str(family),
                    '-DOS_KERNEL_BUILD','-I'+str(root),'-I'+str(root/'include'),
                    '-fsanitize=address,undefined','-fno-pie','-no-pie',
                    str(path/'test.c'),'-o',str(path/'test')],check=True)
        subprocess.run([str(path/'test')],check=True,timeout=20)
        print('Actual IPv%d receiver: cumulative ACK/window, notification and reference balance passed'%family)
    # An implementation that rejects the whole crossing segment must fail
    # even if its ordinary in-window, reorder and FIN tests still pass.
    old_edge=reassembly.replace(
        'if (distance > available) return K_EAGAIN;',
        'if (distance > available || length > available - distance || '
        '(fin && distance + length >= available)) return K_EAGAIN;')
    assert old_edge != reassembly
    (path/'old-edge.h').write_text(old_edge)
    (path/'test.c').write_text(program.replace('"kernel/net/tcp_reassembly.h"','"old-edge.h"'))
    subprocess.run(['cc','-std=gnu11','-O2','-Wall','-Wextra','-Werror',
                    '-DTEST_FAMILY=4','-DOS_KERNEL_BUILD',
                    '-I'+str(root),'-I'+str(root/'include'),
                    str(path/'test.c'),'-o',str(path/'old-edge')],check=True)
    result=subprocess.run([str(path/'old-edge')],capture_output=True,timeout=20)
    assert result.returncode!=0 and b'inject(expected,tail,length,flags' in result.stderr
    print('Negative control rejected: all-or-nothing window edge discards valid payload')
    broken=(root/'kernel/net/tcp_reassembly.h').read_text().replace(
        '*bits |= mask;', '(void)mask;')
    assert broken!=(root/'kernel/net/tcp_reassembly.h').read_text()
    (path/'broken.h').write_text(broken)
    (path/'test.c').write_text(program.replace('"kernel/net/tcp_reassembly.h"','"broken.h"'))
    subprocess.run(['cc','-std=gnu11','-O2','-Wall','-Wextra','-Werror',
                    '-DTEST_FAMILY=4','-DOS_KERNEL_BUILD',
                    '-I'+str(root),'-I'+str(root/'include'),
                    '-fsanitize=address,undefined','-fno-pie','-no-pie',
                    str(path/'test.c'),'-o',str(path/'test')],check=True)
    result=subprocess.run([str(path/'test')],capture_output=True,timeout=20)
    assert result.returncode!=0 and b'Assertion' in result.stderr
    print('Negative control rejected: failing to retain future-byte ownership breaks gap recovery')
    # Disabling only grouped advancement retains correct data but must fail
    # the independent loop-count bound. This gate detects a performance revert.
    slow=instrumented.replace('if ((slot & 7U) == 0U && *bits == UINT8_MAX &&',
                              'if (false && (slot & 7U) == 0U && *bits == UINT8_MAX &&')
    assert slow != instrumented
    (path/'instrumented.h').write_text(slow)
    (path/'test.c').write_text(program.replace('"kernel/net/tcp_reassembly.h"','"instrumented.h"'))
    subprocess.run(['cc','-std=gnu11','-O2','-Wall','-Wextra','-Werror',
                    '-DTEST_FAMILY=4','-DOS_KERNEL_BUILD',
                    '-I'+str(root),'-I'+str(root/'include'),
                    str(path/'test.c'),'-o',str(path/'slow')],check=True)
    result=subprocess.run([str(path/'slow')],capture_output=True,timeout=20)
    assert result.returncode!=0 and b'scan_steps-before<=530' in result.stderr
    print('Negative control rejected: byte-at-a-time gap release exceeds scan budget')
    slow_insert=instrumented.replace(
        'if ((slot & 7U) == 0U && length - index >= 8U &&',
        'if (false && (slot & 7U) == 0U && length - index >= 8U &&')
    assert slow_insert != instrumented
    (path/'instrumented.h').write_text(slow_insert)
    subprocess.run(['cc','-std=gnu11','-O2','-Wall','-Wextra','-Werror',
                    '-DTEST_FAMILY=4','-DOS_KERNEL_BUILD',
                    '-I'+str(root),'-I'+str(root/'include'),
                    str(path/'test.c'),'-o',str(path/'slow-insert')],check=True)
    result=subprocess.run([str(path/'slow-insert')],capture_output=True,timeout=20)
    assert result.returncode!=0 and b'insert_steps-before<=530' in result.stderr
    print('Insertion: 4096 ownership/alignment cases and bounded new/duplicate batches passed; bytewise negative control rejected')
    # 禁用合并后数据仍正确，但必须被复制次数门槛识别。
    unmerged=instrumented.replace('while (run < groups && bits[run] == *bits)',
                                  'while (false && run < groups && bits[run] == *bits)')
    assert unmerged != instrumented
    (path/'instrumented.h').write_text(unmerged)
    subprocess.run(['cc','-std=gnu11','-O2','-Wall','-Wextra','-Werror',
                    '-DTEST_FAMILY=4','-DOS_KERNEL_BUILD',
                    '-I'+str(root),'-I'+str(root/'include'),
                    str(path/'test.c'),'-o',str(path/'unmerged')],check=True)
    result=subprocess.run([str(path/'unmerged')],capture_output=True,timeout=20)
    assert result.returncode!=0 and b'bulk_calls-copies<=2' in result.stderr
    print('连续乱序负载合并及重复包零复制通过；逐八字节复制负面对照被拒绝')
    if '--benchmark' in sys.argv:
        # Same production receiver, fixed reordered payload, actual dynamic
        # capacity. Host CPU timing is not guest NIC throughput evidence.
        baseline=reassembly.replace(
            'slot += 8U;\n            if (slot == capacity) slot = 0U;',
            'slot = (slot + 8U) % capacity;').replace(
            'if (++slot == capacity) slot = 0U;',
            'slot = (slot + 1U) % capacity;')
        assert baseline != reassembly
        if os.environ.get('TCP_REASSEMBLY_BENCH_BASELINE'):
            baseline=Path(os.environ['TCP_REASSEMBLY_BENCH_BASELINE']).read_text()
        bench=r'''
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <kernel/net_core.h>
#include "kernel/net/socket_model.h"
#include "bench.h"
void socket_copy(void *d,const void *s,size_t n){memcpy(d,s,n);}
''' + ring + r'''
int main(int argc,char **argv){
 unsigned capacity=argc>1?(unsigned)strtoul(argv[1],0,10):262144;
 socket_private_t *s=calloc(1,sizeof(*s));assert(s);
 s->stream_capacity=capacity;s->stream_dynamic=calloc(capacity,1);
 s->tcp_rx_present=calloc((capacity+7)/8,1);
 assert(s->stream_dynamic && s->tcp_rx_present);
 unsigned char payload[4096],output[4096];memset(payload,91,sizeof(payload));
 struct timespec a,b;clock_gettime(CLOCK_MONOTONIC,&a);
 for(unsigned i=0;i<100000;++i){
  uint32_t base=s->tcp_next_sequence;bool notify;
  assert(socket_tcp_reassemble_locked(s,base+1,base,0,payload+1,4095,&notify)==K_OK && !notify);
  assert(socket_tcp_reassemble_locked(s,base,base,0,payload,1,&notify)==K_OK && notify);
  assert(atomic_load(&s->stream_count)==4096 && !s->tcp_rx_future_count);
  s->stream_read=socket_stream_read_locked(s,output,4096);
  assert(!memcmp(payload,output,4096));atomic_store(&s->stream_count,0);
 }
 clock_gettime(CLOCK_MONOTONIC,&b);
 printf("%.6f\n",(b.tv_sec-a.tv_sec)+(b.tv_nsec-a.tv_nsec)*1e-9);
 free(s->stream_dynamic);free(s->tcp_rx_present);free(s);
}
'''
        (path/'bench.c').write_text(bench)
        for name,header in (('baseline',baseline),('current',reassembly)):
            (path/'bench.h').write_text(header)
            subprocess.run(['cc','-std=gnu11','-O2','-Wall','-Wextra','-Werror',
                            '-DOS_KERNEL_BUILD','-I'+str(root),'-I'+str(root/'include'),
                            str(path/'bench.c'),'-o',str(path/name)],check=True)
        for capacity in (32771,262144,4194304):
            times={name:[] for name in ('baseline','current')}
            for repeat in range(3):
                for name in (('baseline','current') if repeat%2==0 else ('current','baseline')):
                    result=subprocess.check_output([str(path/name),str(capacity)],timeout=20,text=True)
                    times[name].append(float(result))
            print('Host reassembly benchmark capacity=%d seconds=%s (not NIC throughput)'%(capacity,times))
