"""Real socket registries: growth, publication races, OOM and slot reuse."""
from pathlib import Path
import subprocess
import tempfile

root=Path(__file__).resolve().parents[2]
source=(root/'kernel/net/socket.c').read_text()
def function(signature):
    start=source.index(signature);end=source.index('{',start)+1;depth=1
    while depth:
        depth+=(source[end]=='{')-(source[end]=='}');end+=1
    return source[start:end]
globals_=source[source.index('static socket_binding_t g_initial_bindings['):
                source.index('static socket_tcp_ipv4_output_fn g_tcp_ipv4_output;')]
registry=source[source.index('static uint32_t g_registry_reservations;'):
                source.index('static void socket_destroy(')]
code=r'''
#define _GNU_SOURCE
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "socket_model.h"
static pthread_mutex_t mutex=PTHREAD_MUTEX_INITIALIZER;
static pthread_barrier_t barrier;
static _Thread_local bool held;
static bool fail_allocation,race;
static atomic_uint live,allocations;
void socket_lock(spinlock_t *p){(void)p;assert(!held);assert(!pthread_mutex_lock(&mutex));held=true;}
void socket_unlock(spinlock_t *p){(void)p;assert(held);held=false;assert(!pthread_mutex_unlock(&mutex));}
void socket_zero(void *p,size_t n){memset(p,0,n);}
void socket_copy(void *d,const void *s,size_t n){memcpy(d,s,n);}
static void *kzalloc(size_t n,unsigned flags){
 assert(!held && !flags);if(fail_allocation)return NULL;
 void *p=calloc(1,n);assert(p);++live;++allocations;
 if(race){int r=pthread_barrier_wait(&barrier);assert(!r||r==PTHREAD_BARRIER_SERIAL_THREAD);}
 return p;
}
static void kfree(void *p){assert(!held);if(p){assert(live);--live;free(p);}}
void vfree(void *p){assert(!p);}
''' + globals_ + function('void socket_initialize_globals(') + registry
code += '\n'.join(function(s) for s in (
    'const socket_binding_t *socket_binding_at(',
    'const socket_tcp_connection_t *socket_tcp_connection_at(',
    'const socket_tcp6_connection_t *socket_tcp6_connection_at(',
    'uint32_t socket_registry_capacity_locked(',
    'const socket_private_t *socket_private_const(',
    'bool socket_address6_equal(', 'bool socket_address6_zero(',
    'int32_t socket_find_binding_locked(', 'int32_t socket_find_binding6_locked(',
    'static bool socket_tcp_matches(', 'static bool socket_tcp6_matches(',
    'int32_t socket_find_tcp_locked(', 'int32_t socket_find_tcp6_locked(',
    'static void socket_unbind_locked(', 'bool socket_register_tcp_locked(',
    'static uint32_t socket_unregister_tcp_locked(',
    'static bool socket_port_available_locked(', 'static bool socket_port6_available_locked(',
    'static kstatus_t socket_bind_locked(', 'static kstatus_t socket_bind6_locked('))
code+=r'''
static socket_t sockets[300];
static socket_private_t states[300];
static void scenario(unsigned family){
 unsigned count=family?180:300;
 memset(sockets,0,sizeof(sockets));memset(states,0,sizeof(states));
 for(unsigned i=0;i<count;++i){
  if(i==64){
   fail_allocation=true;
   assert(socket_registry_reserve()==K_ENOMEM);
   assert(g_registry_reservations==64 && g_registry_capacity==64 && !live);
   assert(socket_binding_at(63)->socket==&sockets[63]);
   fail_allocation=false;
  }
  assert(!socket_registry_reserve());
  unsigned f=family?family:(i%2?OS_AF_INET6:OS_AF_INET4);
  sockets[i].family=f;sockets[i].type=(!family && i%3==0)?OS_SOCK_DGRAM:OS_SOCK_STREAM;
  socket_private_t *p=&states[i];
  sockets[i].protocol_state=p;
  socket_lock(&g_binding_lock);
  uint8_t address[16]={0x20,1};
  assert(!(f==OS_AF_INET4?socket_bind_locked(&sockets[i],p,1,4000+i):
   socket_bind6_locked(&sockets[i],p,address,4000+i)));
  assert(socket_binding_at(i)->socket==&sockets[i]);
  if(sockets[i].type==OS_SOCK_STREAM){
   p->connected=true;p->peer_port=443;p->peer_address=2;p->peer_address6[0]=0x20;
   assert(socket_register_tcp_locked(&sockets[i],p));
   assert(socket_register_tcp_locked(&sockets[i],p)); /* Idempotent. */
  }
  assert(socket_registry_capacity_locked()>=i+1);
  socket_unlock(&g_binding_lock);
 }
 assert(g_registry_capacity==(count>256?512:256) && live==1);
 unsigned bound=0,tcp=0;
 for(unsigned i=0;i<g_registry_capacity;++i){
  const socket_binding_t *b=socket_binding_at(i);
  if(b->socket){assert(i<count && b->socket==&sockets[i] && b->port==4000+i);++bound;}
  const socket_tcp_connection_t *v4=socket_tcp_connection_at(i);
  if(v4->socket){
   unsigned id=(unsigned)(v4->socket-sockets);
   assert(id<count && sockets[id].family==OS_AF_INET4 && v4->local_port==4000+id &&
    v4->peer_port==443 && v4->local_address==1 && v4->peer_address==2);++tcp;
  }
  const socket_tcp6_connection_t *v6=socket_tcp6_connection_at(i);
  if(v6->socket){
   unsigned id=(unsigned)(v6->socket-sockets);
   assert(id<count && sockets[id].family==OS_AF_INET6 && v6->local_port==4000+id &&
    v6->peer_port==443 && v6->local_address[0]==0x20 && v6->peer_address[0]==0x20);++tcp;
  }
 }
 assert(bound==count && tcp==(family?count:count-count/3));
 socket_lock(&g_binding_lock);
 for(unsigned i=0;i<count;++i){
  socket_private_t *p=&states[i];unsigned f=sockets[i].family;
  int32_t b=f==OS_AF_INET4?socket_find_binding_locked(1,4000+i,sockets[i].type,false):
   socket_find_binding6_locked(p->local_address6,4000+i,sockets[i].type,false);
  assert(b==(int32_t)i && socket_binding_at(b)->socket==&sockets[i]);
  if(sockets[i].type==OS_SOCK_STREAM){
   int32_t t=f==OS_AF_INET4?socket_find_tcp_locked(2,443,1,4000+i):
    socket_find_tcp6_locked(p->peer_address6,443,p->local_address6,4000+i);
   assert(t>=0 && (f==OS_AF_INET4?socket_tcp_connection_at(t)->socket:
    socket_tcp6_connection_at(t)->socket)==&sockets[i]);
  }
 }
 socket_unlock(&g_binding_lock);
 assert(!socket_binding_at(g_registry_capacity));
 // Release holes before high slots; growth must preserve all live entries.
 for(unsigned parity=0;parity<2;++parity)for(unsigned i=parity;i<count;i+=2){
  socket_lock(&g_binding_lock);
  socket_unbind_locked(&sockets[i]);
  assert(socket_unregister_tcp_locked(&sockets[i])==(sockets[i].type==OS_SOCK_STREAM?1U:0U));
  socket_unlock(&g_binding_lock);socket_registry_release();
 }
 assert(!g_registry_reservations && !g_registry_storage && !live);
 assert(g_registry_capacity==SOCKET_BINDING_COUNT && g_bindings==g_initial_bindings);
 for(unsigned i=0;i<g_registry_capacity;++i)
  assert(!g_bindings[i].socket && !g_tcp_connections[i].socket && !g_tcp6_connections[i].socket);
}
static void *reserve_worker(void *unused){(void)unused;assert(!socket_registry_reserve());return NULL;}
int main(void){
 socket_initialize_globals();scenario(OS_AF_INET4);scenario(OS_AF_INET6);scenario(0);
 for(unsigned i=0;i<64;++i)assert(!socket_registry_reserve());
 unsigned before=allocations;
 assert(!pthread_barrier_init(&barrier,NULL,8));race=true;
 pthread_t threads[8];
 for(unsigned i=0;i<8;++i)assert(!pthread_create(&threads[i],NULL,reserve_worker,NULL));
 for(unsigned i=0;i<8;++i)assert(!pthread_join(threads[i],NULL));
 race=false;assert(!pthread_barrier_destroy(&barrier));
 assert(g_registry_reservations==72 && g_registry_capacity==128 && live==1 && allocations-before==8);
 for(unsigned i=0;i<72;++i)socket_registry_release();
 assert(!live && !g_registry_reservations && g_registry_capacity==64);
 g_registry_capacity=g_registry_reservations=1U<<30;
 before=allocations;assert(socket_registry_reserve()==K_ENOMEM && allocations==before);
 g_registry_capacity=64;g_registry_reservations=0;
 puts("Socket registry: 180 IPv4/180 IPv6/300 mixed bindings, preserved TCP entries, OOM rollback, hole teardown, eight racing growers, redundant allocation reclamation and overflow guard PASS");
}
'''
allocation=function('kstatus_t socket_allocate(')
assert allocation.index('socket_registry_reserve()')<allocation.index('object_header_init(')
destroy=function('static void socket_destroy(')
assert destroy.index('socket_close(socket)')<destroy.index('socket_registry_release()')
with tempfile.TemporaryDirectory(prefix='socket-registry-growth-') as directory:
    path=Path(directory);(path/'test.c').write_text(code)
    subprocess.run(['cc','-std=gnu11','-O2','-Wall','-Wextra','-Werror','-pthread',
                    '-fsanitize=address,undefined','-fno-pie','-no-pie',
                    '-I'+str(root/'include'),'-I'+str(root/'kernel/net'),
                    str(path/'test.c'),'-o',str(path/'test')],check=True)
    subprocess.run([str(path/'test')],check=True,timeout=30)
    broken=code.replace('        if (replacement != 0 && capacity > g_registry_capacity) {',
        '        socket_unlock(&g_binding_lock); kfree(replacement); return K_ENOMEM;\n'
        '        if (replacement != 0 && capacity > g_registry_capacity) {')
    assert broken!=code
    (path/'test.c').write_text(broken)
    subprocess.run(['cc','-std=gnu11','-O2','-Wall','-Wextra','-Werror','-pthread',
                    '-I'+str(root/'include'),'-I'+str(root/'kernel/net'),
                    str(path/'test.c'),'-o',str(path/'old')],check=True)
    result=subprocess.run([str(path/'old')],capture_output=True,timeout=30)
    assert result.returncode!=0 and b'Assertion' in result.stderr
    print('Negative control rejected: fixed 64-slot registry cannot admit the next socket')
