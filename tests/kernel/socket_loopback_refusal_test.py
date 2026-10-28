"""Execute production IPv4 connect: closed loopback ports never reach the NIC."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'kernel/net/socket.c').read_text()
start = source.index('kstatus_t socket_connect(socket_t *socket,')
end = source.index('\nkstatus_t socket_connect_ipv6(', start)
connect = source[start:end]
boot = (root / 'kernel/net/socket_test.c').read_text()
start = boot.index('static bool socket_test_closed_loopback(void)')
end = boot.index('\nstatic bool socket_test_open_local_pair(', start)
boot_loopback = boot[start:end]
assert 'if (!socket_test_closed_loopback()) return false;' in boot
wire_definitions = '\n'.join(line for line in boot.splitlines()
                            if line.startswith(('#define SOCKET_TEST_WIRE_PEER_IPV4 ',
                                                '#define SOCKET_TEST_TIMEOUT_PEER_IPV4 ')))
assert len(wire_definitions.splitlines()) == 2
code = wire_definitions + r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <arch/x86_64/cpu.h>
#include <kernel/net_core.h>
#include "socket_model.h"
#include "tcp_retry.h"
static socket_t client,listener,child;
static socket_private_t cstate,lstate,hstate;
static socket_binding_t bindings[1];
static socket_binding_t *g_bindings=bindings;
static spinlock_t g_binding_lock;
static unsigned held,outputs,registrations,binds,gets,releases;
static spinlock_t *locks[3];
static bool listening,bind_fail;
kstatus_t socket_stream_reserve(socket_t *s,uint32_t cap){
 assert(s==&client && !held && cap==SOCKET_STREAM_TCP_CAPACITY);return K_ENOMEM;
}
socket_private_t *socket_private(socket_t *s){
 return s==&client?&cstate:s==&listener?&lstate:s==&child?&hstate:0;
}
void socket_lock(spinlock_t *p){
 assert(held<3);for(unsigned i=0;i<held;++i)assert(locks[i]!=p);locks[held++]=p;
}
void socket_unlock(spinlock_t *p){assert(held&&locks[held-1]==p);--held;}
void socket_initialize_globals(void){}
static uint32_t socket_active_ipv4_address(void){return 0xc0a80106;}
static kstatus_t socket_bind_ephemeral(socket_t *s,socket_private_t *p){
 assert(s==&client&&!held);++binds;if(bind_fail)return K_ENOMEM;
 p->bound=true;p->local_address=socket_active_ipv4_address();p->local_port=49152;return K_OK;
}
int32_t socket_find_binding_locked(uint32_t a,uint16_t p,uint16_t t,bool l){
 (void)a;assert(held&&(p==9229||p==17011)&&t==OS_SOCK_STREAM&&l);return listening?0:-1;
}
bool object_try_get(void *s){assert(held&&s==&listener);++gets;return true;}
void object_get(void *s){assert(s==&client||s==&child);++gets;}
void object_put(void *s){assert(!held&&s==&listener);++releases;}
bool socket_tcp_slots_available_locked(uint32_t n){assert(held&&n==2);return true;}
bool socket_register_tcp_locked(socket_t *s,socket_private_t *p){
 assert(held&&socket_private(s)==p);++registrations;return true;
}
kstatus_t socket_allocate(uint16_t f,uint16_t t,uint16_t p,socket_t **out){
 assert(!held&&f==OS_AF_INET4&&t==OS_SOCK_STREAM&&p==SOCKET_PROTOCOL_TCP);
 *out=&child;return K_OK;
}
void socket_abort_child(socket_t *s){(void)s;assert(0);}
static kstatus_t output(void *ctx,uint32_t a,uint16_t ap,uint32_t b,uint16_t bp,
 uint32_t seq,uint32_t ack,uint8_t flags,uint16_t window,const void *payload,size_t n){
 (void)ctx;(void)seq;(void)payload;
 assert(!held&&a==0xc0a80106&&ap==49152&&bp==9229&&ack==0);
 assert((b>>24)!=127&&flags==NET_TCP_FLAG_SYN&&window==SOCKET_STREAM_BUFFER_SIZE&&!n);
 ++outputs;return K_OK;
}
void socket_tcp_output_get(socket_tcp_ipv4_output_fn *fn,void **ctx){*fn=output;*ctx=0;}
uint64_t x86_read_tsc(void){return 1000;}
uint64_t x86_timeout_ns_to_tsc(uint64_t ns){return ns;}
uint64_t socket_tcp_retransmit_deadline(uint64_t t){return t+100;}
uint32_t wake_all(wait_queue_t *q){assert(!held&&q==&listener.waitq);return 0;}
void object_notify_changed(void *s){(void)s;assert(0);}
void object_notify_signaled(void *s){assert(!held&&s==&listener);}
''' + connect + r'''
static void reset(unsigned type){
 assert(!held);memset(&client,0,sizeof(client));memset(&listener,0,sizeof(listener));
 memset(&child,0,sizeof(child));memset(&cstate,0,sizeof(cstate));
 memset(&lstate,0,sizeof(lstate));memset(&hstate,0,sizeof(hstate));
 client.object.type=KOBJECT_TYPE_SOCKET;client.family=OS_AF_INET4;client.type=type;
 bindings[0].socket=&listener;lstate.listening=true;lstate.local_port=9229;
 listening=bind_fail=false;outputs=registrations=binds=gets=releases=0;
}
static kstatus_t socket_test_create(unsigned family,unsigned type,unsigned protocol,
 socket_t **out){
 assert(family==OS_AF_INET4&&type==OS_SOCK_STREAM&&!protocol);
 reset(type);*out=&client;return K_OK;
}
static void socket_test_release(socket_t *s){assert(s==&client&&!held);}
#define socket_create socket_test_create
''' + boot_loopback + r'''
#undef socket_create
int main(void){
 assert(socket_test_closed_loopback());
 const uint32_t local[]={0x7f000001,0x7f000002,0x7f123456,0x7fffffff};
 for(unsigned i=0;i<4;++i)for(unsigned bound=0;bound<2;++bound){
  reset(OS_SOCK_STREAM);cstate.bound=bound;
  assert(socket_connect(&client,local[i],9229)==K_ECONNREFUSED);
  assert(!cstate.connected&&!cstate.tcp_send_pending&&!cstate.tcp_send_deadline_tsc);
  assert(!outputs&&!registrations&&!gets&&!releases&&binds==!bound);
  /* Refusal must not poison the socket's later successful connect. */
  listening=true;
  assert(socket_connect(&client,local[i],9229)==K_OK);
  assert(cstate.connected&&cstate.peer==&child&&hstate.peer==&client);
  assert(lstate.accept_count==1&&lstate.accept_queue[0]==&child);
  assert(!outputs&&registrations==2&&gets==3&&releases==1);
 }
 reset(OS_SOCK_DGRAM);assert(socket_connect(&client,local[0],9229)==K_OK);
 assert(cstate.connected&&!outputs&&!registrations);
 reset(OS_SOCK_STREAM);cstate.connected=true;
 assert(socket_connect(&client,local[0],9229)==K_EBUSY&&!outputs&&!registrations);
 const uint32_t remote[]={0x7e000001,0x80000001,0xc0a80101,
  SOCKET_TEST_WIRE_PEER_IPV4,SOCKET_TEST_TIMEOUT_PEER_IPV4};
 for(unsigned i=0;i<sizeof(remote)/sizeof(remote[0]);++i){
  reset(OS_SOCK_STREAM);assert(socket_connect(&client,remote[i],9229)==K_OK);
  assert(outputs==1&&registrations==1&&cstate.tcp_state==SOCKET_TCP_SYN_SENT);
 }
 reset(OS_SOCK_STREAM);bind_fail=true;
 assert(socket_connect(&client,local[0],9229)==K_ENOMEM&&!outputs);
 reset(OS_SOCK_STREAM);atomic_store(&cstate.closed,true);
 assert(socket_connect(&client,local[0],9229)==K_EDEVREMOVED&&!binds&&!outputs);
 puts("IPv4 loopback: immediate refusal, all 127/8 boundaries, listener/reconnect, UDP and external SYN PASS");
}
'''
with tempfile.TemporaryDirectory(prefix='socket-loopback-refusal-') as directory:
    path = Path(directory)
    for negative in (False, True):
        candidate = code
        if negative:
            candidate = candidate.replace(
                'if ((address >> 24U) == 127U)', 'if (false)')
            assert candidate != code, 'Production loopback refusal is missing'
        (path / 'test.c').write_text(candidate)
        subprocess.run(['cc', '-std=gnu11', '-O2', '-Wall', '-Wextra', '-Werror',
                        '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
                        '-I', str(root / 'include'), '-I', str(root / 'kernel/net'),
                        str(path / 'test.c'), '-o', str(path / 'test')], check=True)
        result = subprocess.run([str(path / 'test')], capture_output=True, text=True)
        if negative:
            assert result.returncode != 0, 'Loopback NIC fallback must fail'
            print('Negative control rejected: closed loopback port reaches external TCP')
        else:
            assert result.returncode == 0, result.stderr
            print(result.stdout, end='')
