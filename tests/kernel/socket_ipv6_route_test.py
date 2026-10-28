"""Real IPv6 connect/source selection, including Chromium-style UDP probes."""
from pathlib import Path
import resource
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
resource.setrlimit(resource.RLIMIT_CORE, (0, resource.getrlimit(resource.RLIMIT_CORE)[1]))
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
#include <string.h>
#include "kernel/net_core.h"
#include "kernel/net_manager.h"
#include "kernel/net/socket_model.h"
static socket_t target;
static socket_private_t state;
static spinlock_t g_binding_lock;
static net_manager_status_t network;
static bool have_network,fail_bind,registered=true,have_listener,missing_output;
static unsigned depth,binds,outputs,listener_lookups,reserves;
kstatus_t socket_stream_reserve(socket_t *s,uint32_t cap){
 assert(s==&target && !depth && cap==SOCKET_STREAM_TCP_CAPACITY);++reserves;return K_ENOMEM;
}
void socket_initialize_globals(void){}
int32_t socket_find_binding6_locked(const uint8_t address[16],uint16_t port,
                                   uint16_t type,bool listening){
 const uint8_t loop[16]={[15]=1};
 assert(depth && !memcmp(address,loop,16) && port==443 &&
        type==OS_SOCK_STREAM && listening);
 ++listener_lookups;return have_listener?0:-1;
}
socket_private_t *socket_private(socket_t *s){return s==&target?&state:0;}
void socket_copy(void *d,const void *s,size_t n){memcpy(d,s,n);}
void socket_lock(spinlock_t *l){(void)l;++depth;}
void socket_unlock(spinlock_t *l){(void)l;assert(depth);--depth;}
uint32_t wake_all(wait_queue_t *q){assert(q==&target.waitq && !depth);return 0;}
void object_notify_changed(void *p){assert(p==&target && !depth);}
bool net_manager_get_status(net_manager_status_t *out){
 assert(!depth);*out=network;return have_network;
}
static kstatus_t socket_bind6_ephemeral(socket_t *s,socket_private_t *p,
                                       const uint8_t address[16]) {
 assert(s==&target&&!depth);++binds;
 if(fail_bind)return K_ENOMEM;
 p->bound=true;memcpy(p->local_address6,address,16);p->local_port=49152;return K_OK;
}
static kstatus_t output(void *ctx,const uint8_t src[16],uint16_t sport,
 const uint8_t dst[16],uint16_t dport,uint32_t seq,uint32_t ack,uint8_t flags,
 uint16_t window,const void *payload,size_t bytes){
 (void)ctx;(void)seq;(void)ack;(void)payload;
 assert(!depth&&sport==49152&&dport==443&&flags==NET_TCP_FLAG_SYN);
 assert(window==SOCKET_STREAM_BUFFER_SIZE&&!bytes);
 assert(!memcmp(src,state.local_address6,16)&&!memcmp(dst,state.peer_address6,16));
 ++outputs;return K_EAGAIN;
}
void socket_tcp6_output_get(socket_tcp_ipv6_output_fn *fn,void **context){
 *fn=missing_output?0:output;*context=0;
}
bool socket_register_tcp_locked(socket_t *s,socket_private_t *p){
 assert(depth&&s==&target&&p==&state);return registered;
}
static uint64_t x86_read_tsc(void){return 1;}
static uint64_t x86_timeout_ns_to_tsc(uint64_t ns){return ns;}
uint64_t socket_tcp_retransmit_deadline(uint64_t now){return now+100;}
''' + '\n'.join(line for line in (root/'kernel/net/tcp_retry.h').read_text().splitlines()
                if not line.startswith(('#include','#pragma'))) + '\n'.join(function(s) for s in (
    'bool socket_address6_equal(', 'bool socket_address6_zero(',
    'static kstatus_t socket_ipv6_connect_source(', 'kstatus_t socket_connect_ipv6(')) + r'''
static void reset(unsigned type){
 memset(&target,0,sizeof(target));memset(&state,0,sizeof(state));
 target.object.type=KOBJECT_TYPE_SOCKET;target.family=OS_AF_INET6;target.type=type;
 binds=outputs=listener_lookups=reserves=0;
 fail_bind=have_listener=missing_output=false;registered=true;assert(!depth);
}
int main(void){
 const uint8_t loop[16]={[15]=1},remote[16]={0x20,1,0x48,0x60},
   neighbour[16]={0xfe,0x80,[15]=2},zero[16]={0};
 uint8_t selected[16];
 for(unsigned type=OS_SOCK_STREAM;type<=OS_SOCK_DGRAM;++type){
  have_network=false;memset(&network,0,sizeof(network));reset(type);
  assert(socket_connect_ipv6(&target,remote,443)==K_ENETUNREACH);
  assert(!state.bound&&!state.connected&&!binds&&!outputs);
  assert(socket_connect_ipv6(&target,loop,443)==
         (type==OS_SOCK_STREAM?K_ECONNREFUSED:K_OK));
  assert(!memcmp(state.local_address6,loop,16)&&binds==1&&!outputs&&!reserves);
  assert(state.connected==(type==OS_SOCK_DGRAM));
  assert(listener_lookups==(type==OS_SOCK_STREAM));
  assert(!state.tcp_send_pending&&!state.tcp_send_deadline_tsc);
  reset(type);assert(socket_connect_ipv6(&target,zero,443)==K_EINVAL);
  have_network=true;network.link_up=true;network.ipv6_configured=true;
  memcpy(network.ipv6_address,neighbour,16);network.ipv6_address[15]=3;
  reset(type);assert(socket_connect_ipv6(&target,remote,443)==K_ENETUNREACH);
  assert(!state.bound&&!state.connected&&!binds&&!outputs);
  reset(type);assert(socket_connect_ipv6(&target,neighbour,443)==K_OK);
  assert(!memcmp(state.local_address6,network.ipv6_address,16));
  reset(type);state.bound=true;state.local_port=49152;
  assert(socket_connect_ipv6(&target,neighbour,443)==K_OK&&!binds);
  assert(!memcmp(state.local_address6,network.ipv6_address,16));
  reset(type);state.bound=true;memcpy(state.local_address6,loop,16);
  assert(socket_connect_ipv6(&target,neighbour,443)==K_ENETUNREACH);
  assert(!state.connected&&!binds&&!outputs&&!memcmp(state.local_address6,loop,16));
  reset(type);fail_bind=true;
  assert(socket_connect_ipv6(&target,neighbour,443)==K_ENOMEM);
  assert(!state.bound&&!state.connected&&!outputs);
  reset(type);atomic_store(&state.closed,true);
  assert(socket_connect_ipv6(&target,loop,443)==K_EDEVREMOVED&&!binds);
 }
 /* Closed local probes are refused without an output backend or a dynamic
  * receive allocation; an existing socket may subsequently connect elsewhere.
  * A present listener retains its old path, not an unconditional ::1 reject. */
 reset(OS_SOCK_STREAM);missing_output=true;
 assert(socket_connect_ipv6(&target,loop,443)==K_ECONNREFUSED);
 assert(!state.connected&&!reserves&&!outputs&&listener_lookups==1);
 reset(OS_SOCK_STREAM);state.bound=true;state.local_port=49152;
 memcpy(state.local_address6,loop,16);state.connected=true;
 assert(socket_connect_ipv6(&target,loop,443)==K_EBUSY&&!outputs&&!reserves);
 reset(OS_SOCK_STREAM);have_listener=true;
 assert(socket_connect_ipv6(&target,loop,443)==K_OK&&outputs==1&&reserves==1);
 reset(OS_SOCK_STREAM);
 assert(socket_connect_ipv6(&target,loop,443)==K_ECONNREFUSED);
 have_listener=true;
 assert(socket_connect_ipv6(&target,loop,443)==K_OK&&outputs==1&&reserves==1);
 /* Every fe80::/10 boundary is on-link; fec0:: and other scopes are not. */
 for(unsigned octet=0;octet<256;++octet){
  uint8_t dest[16]={0xfe,octet,[15]=7};memset(selected,0xa5,16);
  kstatus_t result=socket_ipv6_connect_source(dest,selected);
  assert(result==((octet>=0x80&&octet<=0xbf)?K_OK:K_ENETUNREACH));
  if(result==K_OK)assert(!memcmp(selected,network.ipv6_address,16));
  else for(unsigned i=0;i<16;++i)assert(selected[i]==0xa5);
 }
 assert(socket_ipv6_connect_source(network.ipv6_address,selected)==K_OK);
 network.link_up=false;
 assert(socket_ipv6_connect_source(neighbour,selected)==K_ENETUNREACH);
 assert(socket_ipv6_connect_source(loop,selected)==K_OK);
 network.link_up=true;network.ipv6_configured=false;
 assert(socket_ipv6_connect_source(neighbour,selected)==K_ENETUNREACH);
 network.ipv6_configured=true;memset(network.ipv6_address,0,16);
 assert(socket_ipv6_connect_source(neighbour,selected)==K_ENETUNREACH);
}
'''
with tempfile.TemporaryDirectory(prefix='socket-ipv6-route-') as directory:
    path = Path(directory)
    for negative in ('', 'route', 'closed-loopback'):
        candidate = code
        if negative == 'route':
            candidate = candidate.replace('if (!local && !neighbour) return K_ENETUNREACH;',
                                          '(void)local; (void)neighbour;')
        if negative == 'closed-loopback':
            candidate = candidate.replace('if (socket_address6_equal(address, loopback)) {',
                                          'if (socket_address6_equal(address, loopback) && false) {')
        if negative:
            assert candidate != code
        (path / 'test.c').write_text(candidate)
        subprocess.run(['cc', '-std=gnu11', '-O2', '-Wall', '-Wextra', '-Werror',
                        '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
                        '-I', str(root), '-I', str(root / 'include'),
                        str(path / 'test.c'), '-o', str(path / 'test')], check=True)
        result = subprocess.run([str(path / 'test')], capture_output=True)
        assert (result.returncode != 0) if negative else (result.returncode == 0), result.stderr
        if negative:
            assert b'Assertion' in result.stderr, result.stderr
            print('Negative control rejected: ' + negative)
print('IPv6 source/connect: closed local TCP refusal without output/allocation/SYN, listener retry, UDP, missing route, link-local boundaries, binds, errors and negative control PASS')
