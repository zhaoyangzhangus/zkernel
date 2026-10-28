"""Production stream ring helpers against an independent byte/FIFO oracle."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root/'kernel/net/socket.c').read_text()
helpers = source[source.index('uint32_t socket_stream_write_locked('):
                 source.index('static uint32_t socket_active_ipv4_address(')]
code = r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "socket_model.h"
void *liteos_memmove(void *, const void *, size_t);
static unsigned copies;
void socket_copy(void *out, const void *in, size_t n) {
    ++copies; liteos_memmove(out, in, n);
}
''' + helpers + r'''
static socket_private_t actual, expected;
static unsigned char input[SOCKET_STREAM_BUFFER_SIZE+32];
static unsigned char output[SOCKET_STREAM_BUFFER_SIZE+32];
static unsigned char oracle[SOCKET_STREAM_BUFFER_SIZE+32];
static unsigned cases;
static void check(unsigned cursor, unsigned n) {
    memset(&actual, 0x75, sizeof(actual)); actual.stream_write=cursor; actual.stream_read=cursor;
    actual.stream_dynamic=NULL;actual.stream_capacity=0;
    memcpy(&expected,&actual,sizeof(actual));
    for(unsigned i=0;i<n;++i)expected.stream_buffer[(cursor+i)%SOCKET_STREAM_BUFFER_SIZE]=input[i+7];
    copies=0;
    unsigned next=socket_stream_write_locked(&actual,input+7,n);
    assert(next==(cursor+n)%SOCKET_STREAM_BUFFER_SIZE);
    assert(copies==(n==0 ? 0U : (cursor+n>SOCKET_STREAM_BUFFER_SIZE ? 2U : 1U)));
    assert(!memcmp(&actual,&expected,sizeof(actual)));
    memset(output,0x59,sizeof(output)); memset(oracle,0x59,sizeof(oracle));
    for(unsigned i=0;i<n;++i)oracle[i+3]=expected.stream_buffer[(cursor+i)%SOCKET_STREAM_BUFFER_SIZE];
    copies=0;
    next=socket_stream_read_locked(&actual,output+3,n);
    assert(next==(cursor+n)%SOCKET_STREAM_BUFFER_SIZE);
    assert(copies<=2 && !memcmp(output,oracle,sizeof(output)));
    assert(!memcmp(&actual,&expected,sizeof(actual))); /* Read/peek never commits cursors. */
    ++cases;
}
int main(void) {
    for(unsigned i=0;i<sizeof(input);++i)input[i]=(unsigned char)(i*17+9);
    for(unsigned cursor=0;cursor<SOCKET_STREAM_BUFFER_SIZE;++cursor) {
        const unsigned lengths[]={0,1,7,63,64,65,4095,4096,8191,8192,
                                  SOCKET_STREAM_BUFFER_SIZE-cursor};
        for(unsigned i=0;i<sizeof(lengths)/sizeof(lengths[0]);++i)check(cursor,lengths[i]);
    }
    for(unsigned n=0;n<=SOCKET_STREAM_BUFFER_SIZE;++n) {
        check(0,n); check(1,n); check(SOCKET_STREAM_BUFFER_SIZE-1,n);
    }
    memset(&actual,0,sizeof(actual));
    unsigned count=0,seed=7,reads=0,writes=0,peeks=0;
    for(unsigned step=0;step<20000;++step) {
        seed=seed*1664525U+1013904223U;
        unsigned n=(seed>>8)%(SOCKET_STREAM_BUFFER_SIZE+1);
        if(seed&1) {
            if(n>SOCKET_STREAM_BUFFER_SIZE-count)n=SOCKET_STREAM_BUFFER_SIZE-count;
            actual.stream_write=socket_stream_write_locked(&actual,input,n);
            memcpy(oracle+count,input,n); count+=n; ++writes;
        } else {
            if(n>count)n=count;
            unsigned next=socket_stream_read_locked(&actual,output,n);
            assert(!memcmp(output,oracle,n));
            if(seed&4) { ++peeks; }
            else { actual.stream_read=next; memmove(oracle,oracle+n,count-n); count-=n; ++reads; }
        }
        assert((actual.stream_read+count)%SOCKET_STREAM_BUFFER_SIZE==actual.stream_write);
    }
    copies=0;
    assert(socket_stream_read_locked(&actual,0,0)==actual.stream_read);
    assert(socket_stream_write_locked(&actual,0,0)==actual.stream_write && !copies);
    printf("Socket stream ring: %u boundary/oracle cases, %u writes/%u reads/%u peeks, two-copy bound and null-zero operations passed\n",cases,writes,reads,peeks);
}
'''
with tempfile.TemporaryDirectory(prefix='liteos-socket-ring-') as directory:
    path = Path(directory)
    obj = str(path/'string.o')
    subprocess.run(['cc','-std=c11','-O2','-fno-builtin','-mno-sse','-mno-mmx',
                    '-I'+str(root/'include'),'-fsanitize=address,undefined','-fno-pie',
                    '-Dmemcpy=liteos_memcpy','-Dmemmove=liteos_memmove',
                    '-Dmemset=liteos_memset','-Dmemcmp=liteos_memcmp',
                    '-c',str(root/'kernel/string.c'),'-o',obj],check=True)
    binary = str(path/'test')
    subprocess.run(['cc','-std=c11','-O2','-Wall','-Wextra','-Werror',
                    '-I'+str(root/'include'),'-I'+str(root/'kernel/net'),
                    '-fsanitize=address,undefined','-fno-pie','-no-pie',
                    '-x','c','-','-x','none',obj,'-o',binary],input=code,text=True,check=True)
    subprocess.run([binary],check=True,timeout=60)
