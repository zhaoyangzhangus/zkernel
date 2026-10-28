"""Characterize the old caller's discard policy using the real trim helper.

This is a deterministic limitation probe, not proof of physical NIC packet loss.
"""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root/'kernel/net/socket_protocol.c').read_text()
helper = source[source.index('static kstatus_t socket_tcp_trim_overlap('):
                source.index('static void socket_tcp_apply_mss(')]
program = r'''
#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
typedef int kstatus_t;
#define K_OK 0
#define K_EAGAIN -11
#define K_ENOENT -2
#define NET_TCP_FLAG_FIN 1
''' + helper + r'''
int main(void) {
 const uint32_t bases[]={1000,UINT32_MAX-1000};
 const unsigned sizes[]={536,1460};
 for(unsigned b=0;b<2;++b)for(unsigned m=0;m<2;++m) {
  uint8_t data[1460];unsigned size=sizes[m];uint32_t expected=bases[b];
  unsigned delivered=0,rejected=0;
  // Model the former caller: advance only for K_OK; future data has no
  // retained ownership here. Receiving the gap later cannot recover it.
  for(unsigned packet=1;packet<16;++packet) {
   uint32_t seq=bases[b]+packet*size;size_t length=size;const void* bytes=data;
   kstatus_t status=socket_tcp_trim_overlap(&seq,expected,&bytes,&length,0);
   assert(status==K_EAGAIN && seq==bases[b]+packet*size && length==size);
   ++rejected;
  }
  uint32_t seq=bases[b];size_t length=size;const void* bytes=data;
  assert(socket_tcp_trim_overlap(&seq,expected,&bytes,&length,0)==K_OK);
  expected=seq+(uint32_t)length;delivered+=(unsigned)length;
  assert(delivered==size && expected==bases[b]+size && rejected==15);
  // All discarded segments must return before the complete stream exists.
  for(unsigned packet=1;packet<16;++packet) {
   seq=bases[b]+packet*size;length=size;bytes=data;
   assert(socket_tcp_trim_overlap(&seq,expected,&bytes,&length,0)==K_OK);
   expected=seq+(uint32_t)length;delivered+=(unsigned)length;
  }
  assert(delivered==16*size && expected==bases[b]+16*size);
  printf("MSS=%u wrap=%u future_discarded=%u bytes_delivered_after_gap=%u retransmit_bytes_required=%u\n",
    size,b,rejected,size,15*size);
 }
}
'''
with tempfile.TemporaryDirectory(prefix='tcp-reorder-policy-') as directory:
    path=Path(directory)
    (path/'test.c').write_text(program)
    subprocess.run(['cc','-std=c11','-O2','-Wall','-Wextra','-Werror',
                    '-fsanitize=address,undefined','-fno-pie','-no-pie',
                    str(path/'test.c'),'-o',str(path/'test')],check=True)
    subprocess.run([str(path/'test')],check=True,timeout=10)
print('Old-policy characterization: discarding trim-rejected future segments requires retransmission; the new receiver uses reassembly instead')
