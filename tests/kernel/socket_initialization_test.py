"""Actual socket global initializer and accessors: once publication and IDs."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root/'kernel/net/socket.c').read_text()
globals_source = source[source.index('static socket_binding_t g_initial_bindings['):
                        source.index('static socket_tcp_ipv4_output_fn g_tcp_ipv4_output;')]
functions = source[source.index('void socket_initialize_globals('):
                   source.index('void socket_set_tcp_ipv4_output(')]
code = r'''
#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <string.h>
#include "socket_model.h"
static atomic_uint attempts,clears;
static bool parallel;
''' + globals_source + r'''
void socket_zero(void *p,size_t n) {atomic_fetch_add(&clears,1);memset(p,0,n);}
static bool counted_cas(atomic_uint *p,unsigned *expected,unsigned value,
                        memory_order success,memory_order failure) {
 atomic_fetch_add(&attempts,1);
 if(parallel)while(atomic_load(&attempts)<8)sched_yield();
 return atomic_compare_exchange_strong_explicit(p,expected,value,success,failure);
}
''' + functions.replace('atomic_compare_exchange_strong_explicit(', 'counted_cas(') + r'''
static void *initializer(void *unused) {
 (void)unused;socket_initialize_globals();
 assert(atomic_load(&g_socket_init_state)==2 && !atomic_load(&g_binding_lock.state));
 assert(g_tcp_lookup_hint==UINT32_MAX && g_tcp6_lookup_hint==UINT32_MAX);
 for(unsigned i=0;i<SOCKET_BINDING_COUNT;++i) {
  socket_binding_t binding={0};socket_tcp_connection_t tcp={0};socket_tcp6_connection_t tcp6={0};
  assert(!memcmp(socket_binding_at(i),&binding,sizeof(binding)));
  assert(!memcmp(socket_tcp_connection_at(i),&tcp,sizeof(tcp)));
  assert(!memcmp(socket_tcp6_connection_at(i),&tcp6,sizeof(tcp6)));
 }
 return 0;
}
static uint64_t ids[4][1000];static unsigned indexes[4];
static void *id_worker(void *opaque) {
 unsigned index=*(unsigned*)opaque;
 for(unsigned i=0;i<1000;++i)ids[index][i]=socket_next_async_request_id();return 0;
}
int main(void) {
 memset(g_bindings,0x75,sizeof(g_initial_bindings));memset(g_tcp_connections,0x75,sizeof(g_initial_tcp_connections));memset(g_tcp6_connections,0x75,sizeof(g_initial_tcp6_connections));
 pthread_t threads[8];parallel=true;
 for(unsigned i=0;i<8;++i)assert(!pthread_create(&threads[i],0,initializer,0));
 for(unsigned i=0;i<8;++i)assert(!pthread_join(threads[i],0));
 parallel=false;assert(atomic_load(&attempts)==8 && atomic_load(&clears)==3);
 assert(atomic_load(&g_next_ephemeral_port)==SOCKET_EPHEMERAL_FIRST);
 assert(!socket_binding_at(SOCKET_BINDING_COUNT) && !socket_tcp_connection_at(UINT32_MAX) && !socket_tcp6_connection_at(SOCKET_BINDING_COUNT));
 g_bindings[7].port=1234;
 for(unsigned i=0;i<10000;++i) {
  assert(socket_binding_lock()==&g_binding_lock);
  assert(socket_next_async_request_id()==i+1 && socket_binding_at(7)->port==1234);
 }
 for(unsigned i=0;i<4;++i){indexes[i]=i;assert(!pthread_create(&threads[i],0,id_worker,&indexes[i]));}
 for(unsigned i=0;i<4;++i)assert(!pthread_join(threads[i],0));
 bool seen[4000]={0};
 for(unsigned i=0;i<4;++i)for(unsigned j=0;j<1000;++j) {
  uint64_t id=ids[i][j];assert(id>10000 && id<=14000 && !seen[id-10001]);seen[id-10001]=true;
 }
 assert(atomic_load(&attempts)==8 && atomic_load(&clears)==3);
 puts("Socket initialization: eight concurrent initializers, exact table publication/accessor bounds, preserved state over 10000 calls and 4000 unique concurrent IDs without repeat CAS/clearing passed");
}
'''
with tempfile.TemporaryDirectory(prefix='liteos-socket-init-') as directory:
    binary = str(Path(directory)/'test')
    subprocess.run(['cc','-std=c11','-O2','-Wall','-Wextra','-Werror','-pthread',
                    '-I'+str(root/'include'),'-I'+str(root/'kernel/net'),
                    '-fsanitize=address,undefined','-fno-pie','-no-pie',
                    '-x','c','-','-o',binary],input=code,text=True,check=True)
    subprocess.run([binary],check=True,timeout=30)
