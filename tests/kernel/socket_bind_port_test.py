"""Production explicit/implicit port binding and collision/lifetime guards."""
from pathlib import Path
import resource
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'kernel/net/socket.c').read_text()


def function(signature):
    start = source.index(signature)
    end = source.index('{', start) + 1
    depth = 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


code = r'''
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include "socket_model.h"
static socket_binding_t bindings[32];
static socket_binding_t *g_bindings=bindings;
static uint32_t g_registry_capacity=32;
static uint32_t g_udp_lookup_hint,g_udp6_lookup_hint;
static atomic_uint g_next_ephemeral_port;
static spinlock_t g_binding_lock;
static pthread_mutex_t mutex=PTHREAD_MUTEX_INITIALIZER;
static _Thread_local bool held;
static unsigned calls,forced;
static socket_t sockets[32];
static socket_private_t states[32];
static bool close_on_lock;
void socket_initialize_globals(void){assert(!held);}
void socket_lock(spinlock_t *lock){
 assert(lock==&g_binding_lock&&!held);assert(!pthread_mutex_lock(&mutex));held=true;
 if(close_on_lock){atomic_store(&states[0].closed,true);close_on_lock=false;}
}
void socket_unlock(spinlock_t *lock){
 assert(lock==&g_binding_lock&&held);held=false;assert(!pthread_mutex_unlock(&mutex));
}
void socket_copy(void *d,const void *s,size_t n){memcpy(d,s,n);}
socket_private_t *socket_private(socket_t *s){return s->protocol_state;}
static uint32_t socket_active_ipv4_address(void){assert(!held);return 0xc0a80102;}
''' + '\n'.join(function(s) for s in (
    'bool socket_address6_equal(', 'bool socket_address6_zero(',
    'static bool socket_port_available_locked(',
    'static bool socket_port6_available_locked(',
    'static kstatus_t socket_bind_locked(',
    'static kstatus_t socket_bind6_locked(')).replace(
        'socket_bind_locked(', 'socket_bind_actual_locked(').replace(
        'socket_bind6_locked(', 'socket_bind6_actual_locked(') + r'''
static kstatus_t socket_bind_locked(socket_t *s,socket_private_t *p,
                                    uint32_t a,uint16_t port){
 assert(held);++calls;if(forced)return forced==1?K_EBUSY:K_ENOMEM;
 return socket_bind_actual_locked(s,p,a,port);
}
static kstatus_t socket_bind6_locked(socket_t *s,socket_private_t *p,
                                     const uint8_t a[16],uint16_t port){
 assert(held);++calls;if(forced)return forced==1?K_EBUSY:K_ENOMEM;
 return socket_bind6_actual_locked(s,p,a,port);
}
''' + '\n'.join(function(s) for s in (
    'static kstatus_t socket_bind_port_locked(',
    'static kstatus_t socket_bind_ephemeral(',
    'static kstatus_t socket_bind6_ephemeral(',
    'kstatus_t socket_bind(', 'kstatus_t socket_bind_ipv6(')) + r'''
static const uint8_t loop[16]={[15]=1};
static void reset(unsigned family,unsigned type){
 assert(!held);memset(bindings,0,sizeof(bindings));
 memset(sockets,0,sizeof(sockets));memset(states,0,sizeof(states));
 for(unsigned i=0;i<32;++i){
  sockets[i].object.type=KOBJECT_TYPE_SOCKET;sockets[i].family=family;
  sockets[i].type=type;sockets[i].protocol_state=&states[i];
 }
 forced=calls=0;close_on_lock=false;
 atomic_store(&g_next_ephemeral_port,SOCKET_EPHEMERAL_FIRST);
}
static kstatus_t bind_port(unsigned i,uint16_t port,bool wildcard){
 uint8_t zero[16]={0};
 return sockets[i].family==OS_AF_INET4?
  socket_bind(&sockets[i],wildcard?0:0x7f000001,port):
  socket_bind_ipv6(&sockets[i],wildcard?zero:loop,port);
}
static void *parallel_bind(void *index){
 assert(bind_port((unsigned)(uintptr_t)index,0,false)==K_OK);return NULL;
}
int main(void){
 const unsigned span=SOCKET_EPHEMERAL_LAST-SOCKET_EPHEMERAL_FIRST+1;
 for(unsigned family=OS_AF_INET4;family<=OS_AF_INET6;++family){
  if(family!=OS_AF_INET4&&family!=OS_AF_INET6)continue;
  for(unsigned type=OS_SOCK_STREAM;type<=OS_SOCK_DGRAM;++type){
   for(unsigned wildcard=0;wildcard<2;++wildcard){
    reset(family,type);
    assert(bind_port(0,SOCKET_EPHEMERAL_FIRST,wildcard)==K_OK);
    assert(bind_port(1,SOCKET_EPHEMERAL_FIRST+1,wildcard)==K_OK);
    calls=0;assert(bind_port(2,0,wildcard)==K_OK&&calls==3);
    assert(states[2].bound&&states[2].local_port==SOCKET_EPHEMERAL_FIRST+2);
    if(family==OS_AF_INET4)assert(states[2].local_address==(wildcard?0:0x7f000001));
    else assert(wildcard?socket_address6_zero(states[2].local_address6):
                        socket_address6_equal(states[2].local_address6,loop));
    calls=0;assert(bind_port(2,0,wildcard)==K_EBUSY&&!calls);
    assert(states[2].local_port==SOCKET_EPHEMERAL_FIRST+2);
   }
   reset(family,type);atomic_store(&g_next_ephemeral_port,SOCKET_EPHEMERAL_LAST);
   assert(bind_port(0,0,false)==K_OK&&states[0].local_port==SOCKET_EPHEMERAL_LAST);
   assert(bind_port(1,0,false)==K_OK&&states[1].local_port==SOCKET_EPHEMERAL_FIRST);
   reset(family,type);forced=1;
   assert(bind_port(0,0,false)==K_EBUSY&&calls==span&&!states[0].bound);
   forced=2;calls=0;
   assert(bind_port(0,0,false)==K_ENOMEM&&calls==1&&!states[0].bound);
   forced=0;calls=0;atomic_store(&states[0].closed,true);
   assert(bind_port(0,0,false)==K_EDEVREMOVED&&!calls&&!states[0].bound);
   reset(family,type);close_on_lock=true;
   assert(bind_port(0,0,false)==K_EDEVREMOVED&&!calls&&!states[0].bound);
   assert(!bindings[0].socket);
   reset(family,type);
   kstatus_t status=family==OS_AF_INET4?socket_bind_ephemeral(&sockets[0],&states[0]):
                                      socket_bind6_ephemeral(&sockets[0],&states[0],loop);
   assert(status==K_OK&&states[0].bound);
   if(family==OS_AF_INET4)assert(states[0].local_address==0xc0a80102);
   else assert(socket_address6_equal(states[0].local_address6,loop));
   reset(family,type);pthread_t threads[32];
   for(unsigned i=0;i<32;++i)assert(!pthread_create(&threads[i],0,parallel_bind,(void*)(uintptr_t)i));
   for(unsigned i=0;i<32;++i)assert(!pthread_join(threads[i],0));
   for(unsigned i=0;i<32;++i)for(unsigned j=0;j<i;++j)
    assert(states[i].local_port!=states[j].local_port);
  }
 }
 puts("Socket bind: both families/types, wildcard/loopback preservation, collisions, wrap, bounded exhaustion, OOM, closed/rebound guards and 32 concurrent clients PASS");
}
'''


def no_core():
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))


with tempfile.TemporaryDirectory(prefix='socket-bind-port-') as directory:
    path = Path(directory)
    mutations = {
        'normal': lambda text: text,
        'collision_retry': lambda text: text.replace('if (status != K_EBUSY) break;', 'break;'),
        'zero_port': lambda text: text.replace(
            'if (private->bound) return K_EBUSY;',
            'if (!requested_port) return K_EINVAL;\n    if (private->bound) return K_EBUSY;'),
        'rebound_guard': lambda text: text.replace(
            'if (private->bound) return K_EBUSY;', ''),
        'close_race': lambda text: text.replace(
            'if (atomic_load_explicit(&private->closed, memory_order_acquire))\n'
            '        return K_EDEVREMOVED;', ''),
    }
    for name, mutate in mutations.items():
        candidate = mutate(code)
        (path / 'test.c').write_text(candidate)
        subprocess.run(['cc', '-std=gnu11', '-O2', '-Wall', '-Wextra', '-Werror',
                        '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
                        '-pthread', '-I', str(root / 'include'),
                        '-I', str(root / 'kernel/net'), str(path / 'test.c'),
                        '-o', str(path / 'test')], check=True)
        result = subprocess.run([str(path / 'test')], capture_output=True,
                                text=True, timeout=15, preexec_fn=no_core)
        assert result.returncode == 0 if name == 'normal' else result.returncode != 0, result.stderr
        if name == 'normal':
            print(result.stdout, end='')
        else:
            print('Negative control rejected:', name)
