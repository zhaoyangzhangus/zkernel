"""Run the boot fixture's actual IPv6 wire-peer setup and cleanup ownership."""
from pathlib import Path
import resource
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'kernel/net/socket_test.c').read_text()


def function(signature):
    start = source.index(signature)
    end = source.index('{', start) + 1
    depth = 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


setup = function('static bool socket_test_open_active6_wire(')
cleanup = '\n'.join(function('if (' + name + ' != 0)')
                    for name in ('active_client6', 'active_listener6'))
assert 'if (!socket_test_closed_loopback6()) return false;' in source
assert '!socket_test_open_active6_wire(&active_listener6, &active_client6,' in source
program = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define K_OK 0
#define K_EIO -5
#define OS_AF_INET6 10
#define OS_SOCK_STREAM 1
typedef int kstatus_t;
typedef struct {bool bound,listening,connected,closed;unsigned refs;} socket_t;
static socket_t objects[2];
static unsigned calls,fail_at,created,released;
static socket_t *listener;
static const uint8_t loop[16]={[15]=1};
static bool step(void){return ++calls!=fail_at;}
static kstatus_t socket_create(unsigned family,unsigned type,unsigned protocol,socket_t **out){
 assert(family==OS_AF_INET6&&type==OS_SOCK_STREAM&&!protocol&&!*out);
 if(!step())return K_EIO;
 assert(created<2);*out=&objects[created++];(*out)->refs=1;
 if(created==1)listener=*out;
 return K_OK;
}
static kstatus_t socket_bind_ipv6(socket_t *s,const uint8_t address[16],uint16_t port){
 assert(s==listener&&s->refs==1&&!s->closed&&!s->bound&&port==16001&&!memcmp(address,loop,16));
 if(!step())return K_EIO;
 s->bound=true;return K_OK;
}
static kstatus_t socket_listen(socket_t *s,unsigned backlog){
 assert(s==listener&&s->bound&&!s->closed&&backlog==1);
 if(!step())return K_EIO;
 s->listening=true;return K_OK;
}
static kstatus_t socket_connect_ipv6(socket_t *s,const uint8_t address[16],uint16_t port){
 assert(s!=listener&&s->refs==1&&!s->closed&&port==16001&&!memcmp(address,loop,16));
 if(!step()||!listener->bound||!listener->listening)return K_EIO;
 s->connected=true;return K_OK;
}
static kstatus_t socket_close(socket_t *s){
 assert(s&&s->refs==1&&!s->closed);s->closed=true;return K_OK;
}
static void object_put(socket_t *s){assert(s->closed&&s->refs==1);--s->refs;++released;}
''' + setup + r'''
int main(void){
 for(fail_at=0;fail_at<=5;++fail_at){
  memset(objects,0,sizeof(objects));calls=created=released=0;listener=0;
  socket_t *active_client6=0,*active_listener6=0;
  bool ok=socket_test_open_active6_wire(&active_listener6,&active_client6,loop,16001);
  assert(ok==(fail_at==0));
  assert(calls==(fail_at?fail_at:5));
  if(ok)assert(created==2&&listener->listening&&active_client6->connected);
''' + cleanup + r'''
  assert(released==created);
  for(unsigned i=0;i<created;++i)assert(objects[i].closed&&!objects[i].refs);
 }
 puts("IPv6 boot wire probe: actual setup and cleanup, listening precondition, five failure boundaries and balanced ownership PASS");
}
'''
resource.setrlimit(resource.RLIMIT_CORE, (0, resource.getrlimit(resource.RLIMIT_CORE)[1]))
with tempfile.TemporaryDirectory(prefix='ipv6-boot-probe-') as directory:
    path = Path(directory)
    for negative in (False, True):
        candidate = program
        if negative:
            candidate = candidate.replace('socket_listen(*listener_out, 1U) == K_OK',
                                          '((void)&socket_listen, true)')
            # Keep the function syntactically used without opening a listener.
            assert candidate != program
        (path / 'test.c').write_text(candidate)
        subprocess.run(['cc', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                        '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
                        str(path / 'test.c'), '-o', str(path / 'test')], check=True)
        result = subprocess.run([str(path / 'test')], capture_output=True, text=True, timeout=10)
        if negative:
            assert result.returncode != 0 and 'ok==(fail_at==0)' in result.stderr, result.stderr
            print('Negative control rejected: synthetic ::1 peer has no listener')
        else:
            assert result.returncode == 0, result.stderr
            print(result.stdout, end='')
