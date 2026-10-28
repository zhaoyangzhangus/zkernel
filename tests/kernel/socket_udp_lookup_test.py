"""Actual UDP binding hints against independent first-exact/last-wildcard lookup."""
from pathlib import Path
import resource
import signal
import subprocess
import tempfile

root=Path(__file__).resolve().parents[2]
source=(root/'kernel/net/socket.c').read_text()


def function(signature):
    start=source.index(signature);end=source.index('{',start)+1;depth=1
    while depth:
        depth+=(source[end]=='{')-(source[end]=='}');end+=1
    return source[start:end]


functions='\n'.join(function(name) for name in (
    'bool socket_address6_equal(', 'bool socket_address6_zero(',
    'static void socket_unbind_locked(', 'int32_t socket_find_binding_locked(',
    'int32_t socket_find_binding6_locked(', 'static bool socket_port_available_locked(',
    'static bool socket_port6_available_locked(', 'static kstatus_t socket_bind_locked(',
    'static kstatus_t socket_bind6_locked('))
functions=functions.replace('const socket_binding_t *binding =',
                            '++probes; const socket_binding_t *binding =')
code=r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "socket_model.h"
static socket_binding_t tables[2][512];
static socket_binding_t *g_bindings=tables[0];
static uint32_t g_registry_capacity=512;
static uint32_t g_udp_lookup_hint=UINT32_MAX,g_udp6_lookup_hint=UINT32_MAX;
static socket_t sockets[513];
static socket_private_t states[513];
static unsigned probes,cases;
void socket_zero(void *p,size_t n){memset(p,0,n);}
void socket_copy(void *d,const void *s,size_t n){memmove(d,s,n);}
const socket_private_t *socket_private_const(const socket_t *s){return s->protocol_state;}
''' + functions + r'''
static int check(unsigned family,unsigned address,unsigned port,unsigned type,bool listening){
 int expected=-1,wildcard=-1;uint8_t a6[16]={address};
 for(unsigned i=0;i<g_registry_capacity;++i){
  const socket_binding_t *b=&g_bindings[i];
  if(!b->socket || b->family!=(family?OS_AF_INET6:OS_AF_INET4) || b->port!=port || b->type!=type)continue;
  const socket_private_t *p=b->socket->protocol_state;
  if(listening && (!p || !p->listening))continue;
  bool equal=family?!memcmp(b->address6,a6,16):b->address==address;
  uint8_t zero[16]={0};bool any=family?!memcmp(b->address6,zero,16):!b->address;
  if(equal){expected=(int)i;break;}if(any)wildcard=(int)i;
 }
 if(expected<0)expected=wildcard;
 probes=0;
 int actual=family?socket_find_binding6_locked(a6,port,type,listening):
                   socket_find_binding_locked(address,port,type,listening);
 assert(actual==expected);++cases;return actual;
}
int main(void){
 for(unsigned family=0;family<2;++family){
  g_bindings=tables[0];g_registry_capacity=512;
  memset(tables,0,sizeof(tables));memset(sockets,0,sizeof(sockets));memset(states,0,sizeof(states));
  g_udp_lookup_hint=g_udp6_lookup_hint=UINT32_MAX;
  for(unsigned i=0;i<512;++i){
   sockets[i].family=family?OS_AF_INET6:OS_AF_INET4;sockets[i].type=OS_SOCK_DGRAM;
   sockets[i].protocol_state=&states[i];uint8_t a[16]={1};
   assert((family?socket_bind6_locked(&sockets[i],&states[i],a,4000+i):
                  socket_bind_locked(&sockets[i],&states[i],1,4000+i))==K_OK);
   assert(g_udp_lookup_hint==UINT32_MAX && g_udp6_lookup_hint==UINT32_MAX);
  }
  assert(check(family,1,4511,OS_SOCK_DGRAM,false)==511 && probes==512);
  for(unsigned i=0;i<10000;++i)assert(check(family,1,4511,OS_SOCK_DGRAM,false)==511 && probes==1);
  memcpy(tables[1],tables[0],sizeof(tables[0]));g_bindings=tables[1];
  memset(tables[0],0xa5,sizeof(tables[0]));
  assert(check(family,1,4511,OS_SOCK_DGRAM,false)==511 && probes==1);
  socket_unbind_locked(&sockets[0]);
  assert(g_udp_lookup_hint==UINT32_MAX && g_udp6_lookup_hint==UINT32_MAX);
  states[0].bound=false;
  uint8_t a[16]={2};
  assert((family?socket_bind6_locked(&sockets[0],&states[0],a,4999):
                 socket_bind_locked(&sockets[0],&states[0],2,4999))==K_OK);
  check(family,2,4999,OS_SOCK_DGRAM,false);
  /* Synthetic duplicate/wildcard tables exercise precedence even when the
   * normal bind policy prevents duplicate live endpoints. */
  g_bindings[0].port=g_bindings[511].port;
  for(unsigned mode=0;mode<3;++mode){
   g_udp_lookup_hint=g_udp6_lookup_hint=UINT32_MAX;
   g_bindings[0].address=mode?0:1;g_bindings[0].address6[0]=mode?0:1;
   g_bindings[511].address=mode==2?0:1;g_bindings[511].address6[0]=mode==2?0:1;
   for(unsigned i=0;i<10;++i){check(family,1,4511,OS_SOCK_DGRAM,false);check(family,2,4511,OS_SOCK_DGRAM,false);}
  }
  g_bindings[1].type=OS_SOCK_STREAM;g_bindings[1].port=4511;
  assert(check(family,1,4511,OS_SOCK_STREAM,true)==-1);
  states[1].listening=true;
  assert(check(family,1,4511,OS_SOCK_STREAM,true)==1);
  assert(check(family,1,4511,OS_SOCK_STREAM,false)==1);
  g_bindings[1].type=OS_SOCK_DGRAM;g_bindings[1].port=4001;
  g_udp_lookup_hint=g_udp6_lookup_hint=UINT32_MAX;
  unsigned seed=17;
  for(unsigned i=0;i<20000;++i){seed=seed*1664525U+1013904223U;
   check(family,seed%3,4000+(seed>>8)%520,i%7?OS_SOCK_DGRAM:OS_SOCK_STREAM,(i%11)==0);
  }
  g_udp_lookup_hint=g_udp6_lookup_hint=511;
  g_registry_capacity=64;check(family,1,4511,OS_SOCK_DGRAM,false);
  check(family,1,4001,OS_SOCK_DGRAM,false);
 }
 printf("UDP lookup: %u independent oracle cases; hot exact slot 512->1 probes, wildcard precedence, mutation invalidation, relocated tables and shrink bounds PASS\n",cases);
}
'''
with tempfile.TemporaryDirectory(prefix='socket-udp-lookup-') as directory:
    binary=directory+'/test'
    command=['cc','-std=c11','-O2','-Wall','-Wextra','-Werror',
             '-I'+str(root/'include'),'-I'+str(root/'kernel/net'),
             '-fsanitize=address,undefined','-fno-pie','-no-pie',
             '-x','c','-','-o',binary]
    subprocess.run(command,input=code,text=True,check=True)
    subprocess.run([binary],check=True,timeout=30)
    for name,broken in (
        ('missing mutation invalidation',code.replace(
            '    g_udp_lookup_hint = g_udp6_lookup_hint = UINT32_MAX;', '')),
        ('full scan on hot endpoint',code.replace(
            'if (cacheable && g_udp', 'if (false && cacheable && g_udp')),
    ):
        assert broken!=code,name
        subprocess.run(command,input=broken,text=True,check=True)
        result=subprocess.run([binary],text=True,capture_output=True,timeout=30,
                              preexec_fn=lambda: resource.setrlimit(resource.RLIMIT_CORE,(0,0)))
        assert result.returncode==-signal.SIGABRT and 'Assertion' in result.stderr,(name,result)
        print('Negative control rejected:',name)
