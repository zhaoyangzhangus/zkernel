"""Exercise real packet builders, parsers and boot TCP self-test."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
self_test = (root / 'kernel/net/core/self_test.c').read_text()
self_test = self_test[self_test.index('bool net_tcp_self_test(void)'):]
self_test = self_test[:self_test.index('\n}', self_test.index('{')) + 2]
program = r'''
#include <assert.h>
#include <string.h>
#include "kernel/net/core/core.c"
''' + self_test + r'''
int main(void) {
    uint8_t frame[256], mac[6]={2,1,2,3,4,5}, address[16]={0x20,1};
    const uint8_t payload[]={0,1,2,3,0xff};
    assert(net_tcp_self_test());
    for(unsigned family=0; family<2; ++family)
    for(unsigned mode=0; mode<4; ++mode)
    for(unsigned data=0; data<2; ++data) {
        uint8_t flags = mode==0 ? NET_TCP_FLAG_SYN : mode==1 ?
            NET_TCP_FLAG_SYN|NET_TCP_FLAG_ACK : mode==2 ? NET_TCP_FLAG_ACK :
            NET_TCP_FLAG_RST|NET_TCP_FLAG_ACK;
        unsigned header=mode<2?24:20, ip=family?40:20;
        unsigned bytes=data?sizeof(payload):0, expected=14+ip+header+bytes;
        for(unsigned capacity=expected-1; capacity<=expected; ++capacity) {
            memset(frame,0xa5,sizeof(frame));
            size_t length=123;
            kstatus_t status=family ? net_tcp_build_ipv6(frame,capacity,mac,mac,
                address,address,443,50000,1,2,flags,32768,payload,bytes,&length) :
                net_tcp_build_ipv4(frame,capacity,mac,mac,1,2,443,50000,
                    1,2,flags,32768,payload,bytes,&length);
            if(capacity<expected) {
                assert(status==K_EINVAL && length==123);
                for(unsigned i=0;i<sizeof(frame);++i)assert(frame[i]==0xa5);
                continue;
            }
            assert(status==K_OK && length==expected && frame[expected]==0xa5);
            net_tcp_view_t view;
            assert((family?net_tcp_parse_ipv6(frame,length,&view):
                           net_tcp_parse_ipv4(frame,length,&view))==K_OK);
            assert(view.header_length==header && view.flags==flags);
            assert(view.peer_mss==(mode<2?(family?1440:1460):0));
            assert(view.payload_length==bytes && !memcmp(view.payload,payload,bytes));
            assert(view.window==32768 && view.sequence==1 && view.acknowledgement==2);
        }
    }
}
'''
with tempfile.TemporaryDirectory(prefix='tcp-syn-mss-') as directory:
    path = Path(directory)
    (path / 'test.c').write_text(program)
    subprocess.run(['cc', '-std=gnu11', '-O2', '-Wall', '-Wextra', '-Werror',
                    '-ffunction-sections', '-fdata-sections', '-Wl,--gc-sections',
                    '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
                    '-I', str(root), '-I', str(root / 'include'),
                    str(path / 'test.c'), '-o', str(path / 'test')], check=True)
    subprocess.run([str(path / 'test')], check=True)
print('TCP SYN MSS: IPv4/IPv6 SYN/SYNACK, unchanged ACK/RST, payload, exact capacity, checksums and boot self-test PASS')
