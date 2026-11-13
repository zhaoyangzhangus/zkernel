"""Production checksum against a per-word carry oracle and page boundaries."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root/'kernel/net/core/core.c').read_text()
helpers = source[source.index('static uint32_t net_checksum_add('):
                 source.index('void net_ndp_solicited_node_address(')]
stores = source[source.index('static void net_store_be32('):
                source.index('static uint16_t net_load_be16(')]
code = r'''
#define _GNU_SOURCE
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
''' + stores + helpers + r'''
static unsigned cases;
static uint32_t oracle(uint32_t sum,const uint8_t *p,size_t n) {
    while (sum>>16) sum=(sum&65535)+(sum>>16);
    for (size_t i=0;i<n;i+=2) {
        sum+=(uint32_t)p[i]<<8;
        if (i+1<n) sum+=p[i+1];
        sum=(sum&65535)+(sum>>16);
    }
    while (sum>>16) sum=(sum&65535)+(sum>>16);
    return sum;
}
static void check(const uint8_t *p,size_t n) {
    assert(net_checksum16(p,n)==(uint16_t)~oracle(0,p,n));
    const uint32_t seeds[]={0,1,65535,65536,UINT32_MAX};
    for (unsigned i=0;i<5;++i)
        assert(net_checksum_add(seeds[i],p,n)==oracle(seeds[i],p,n));
    ++cases;
}
static void check_transport(const uint8_t *source,const uint8_t *destination,
                            const uint8_t *payload,uint16_t n,uint8_t protocol) {
    uint8_t header[40]={0};
    memcpy(header,source,16);memcpy(header+16,destination,16);
    header[34]=(uint8_t)(n>>8);header[35]=(uint8_t)n;header[39]=protocol;
    uint16_t expected=(uint16_t)~oracle(oracle(0,header,40),payload,n);
    assert(net_transport_checksum_ipv6(source,destination,protocol,payload,n)==expected);
    uint32_t from=0,to=0;
    for (unsigned i=0;i<4;++i) {from=(from<<8)|source[i];to=(to<<8)|destination[i];}
    memset(header,0,sizeof(header));memcpy(header,source,4);memcpy(header+4,destination,4);
    header[9]=protocol;header[10]=(uint8_t)(n>>8);header[11]=(uint8_t)n;
    expected=(uint16_t)~oracle(oracle(0,header,12),payload,n);
    assert(net_transport_checksum_ipv4(from,to,protocol,payload,n)==expected);
    ++cases;
}
int main(void) {
    assert(net_checksum16(0,0)==65535 && net_checksum16(0,1)==0);
    const size_t capacity=4*1024*1024;
    uint8_t *p=malloc(capacity+32);assert(p);uint32_t seed=17;
    for (size_t i=0;i<capacity+32;++i) {
        seed=seed*1664525U+1013904223U;p[i]=(uint8_t)(seed>>24);
    }
    for (unsigned offset=0;offset<16;++offset)
        for (unsigned n=0;n<=2048;++n)check(p+offset,n);
    const uint16_t transport_lengths[]={0,1,7,8,9,20,32,40,1472,4096,65534,65535};
    const uint8_t protocols[]={0,6,17,58,255};
    for (unsigned offset=0;offset<16;++offset)
        for (unsigned i=0;i<sizeof(transport_lengths)/sizeof(transport_lengths[0]);++i)
            for (unsigned protocol=0;protocol<sizeof(protocols);++protocol)
                check_transport(p+offset,p+31-offset,p+64+offset,
                                transport_lengths[i],protocols[protocol]);
    const size_t lengths[]={65527,65528,65529,65535,65536,65537,65543,
                           131070,131071,131072,131073,262144,capacity};
    for (unsigned pattern=0;pattern<3;++pattern) {
        if (pattern) memset(p,pattern==1 ? 0 : 255,capacity+32);
        for (unsigned offset=0;offset<16;++offset)
            for (unsigned i=0;i<sizeof(lengths)/sizeof(lengths[0]);++i)
                check(p+offset,lengths[i]);
    }
    /* 对足够大的缓冲区使用旧 32 位累加时会丢失进位。 */
    uint32_t old=0;
    for (size_t i=0;i<capacity;i+=2) old+=65535;
    while (old>>16) old=(old&65535)+(old>>16);
    assert(old!=oracle(0,p,capacity));
    free(p);
    size_t page=(size_t)sysconf(_SC_PAGESIZE);
    uint8_t *guard=mmap(0,page*3,PROT_NONE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    assert(guard!=MAP_FAILED && !mprotect(guard+page,page,PROT_READ|PROT_WRITE));
    memset(guard+page,255,page);
    for (unsigned n=0;n<=page;++n) {
        check(guard+page,n);check(guard+2*page-n,n);
    }
    assert(!munmap(guard,page*3));
    printf("Checksums: %u independent carry-oracle cases, all alignment/tails, 64KiB batches, 4MiB overflow and guard pages passed; old overflow rejected\n",cases);
}
'''
with tempfile.TemporaryDirectory(prefix='liteos-net-checksum-') as directory:
    binary = str(Path(directory)/'test')
    subprocess.run(['cc','-std=c11','-O2','-Wall','-Wextra','-Werror',
                    '-fno-builtin','-mno-sse','-mno-mmx',
                    '-fsanitize=address,undefined','-fno-pie','-no-pie',
                    '-x','c','-','-o',binary],input=code,text=True,check=True)
    subprocess.run([binary],check=True,timeout=60)
