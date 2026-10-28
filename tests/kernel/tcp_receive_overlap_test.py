"""Production retransmission trimming: exact suffix, FIN and sequence wrap."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'kernel/net/socket_protocol.c').read_text()
helper = source[source.index('static kstatus_t socket_tcp_trim_overlap('):
                source.index('static void socket_tcp_apply_mss(')]
for family in ('4', '6'):
    body = source.split('kstatus_t socket_inject_tcp_ipv' + family + '_reply(', 1)[1].split('\nkstatus_t ', 1)[0]
    assert 'socket_tcp_trim_overlap(&sequence, expected, &buffer, &length, flags)' in body
code = r'''
#include <assert.h>
#include <stdint.h>
#include <stddef.h>
typedef int kstatus_t;
#define K_OK 0
#define K_EAGAIN -11
#define K_ENOENT -2
#define NET_TCP_FLAG_FIN 1
''' + helper + r'''
int main(void){
 uint8_t data[4096];
 const uint32_t starts[]={0,1,UINT32_MAX-2048,UINT32_MAX-3,0x7ffffffe};
 for(unsigned i=0;i<5;++i)for(unsigned n=0;n<=4096;n+=17){
  for(unsigned skip=0;skip<=n+1;++skip)for(unsigned fin=0;fin<2;++fin){
   uint32_t seq=starts[i],expected=seq+skip;
   size_t length=n;const void *buffer=data;
   int want=skip==0?K_OK:skip>n||(skip==n&&!fin)?K_ENOENT:K_OK;
   assert(socket_tcp_trim_overlap(&seq,expected,&buffer,&length,fin)==want);
   if(want==K_OK){assert(seq==expected&&length==n-skip&&buffer==data+skip);}
   else assert(seq==starts[i]&&length==n&&buffer==data);
  }
  uint32_t seq=starts[i]+1;size_t length=n;const void *buffer=data;
  assert(socket_tcp_trim_overlap(&seq,starts[i],&buffer,&length,1)==K_EAGAIN);
  assert(seq==starts[i]+1&&length==n&&buffer==data);
 }
}
'''
with tempfile.TemporaryDirectory(prefix='tcp-overlap-') as directory:
    path = Path(directory)
    (path / 'test.c').write_text(code)
    subprocess.run(['cc', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
                    str(path / 'test.c'), '-o', str(path / 'test')], check=True)
    subprocess.run([str(path / 'test')], check=True)
print('TCP receive overlap: exact suffix, duplicates, future data, FIN and wrap PASS; both families integrated')
