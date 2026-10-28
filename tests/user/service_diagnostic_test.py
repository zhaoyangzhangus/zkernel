"""Run production service debug helpers with only the syscall sink replaced."""
from pathlib import Path
import subprocess
import tempfile
import re

ROOT = Path(__file__).resolve().parents[2]
audio = (ROOT/'user/services/audiod/service.c').read_text()
network = (ROOT/'user/services/networkd/netd.c').read_text()
assert 'LITEOS_' not in audio and 'LITEOS_' not in network

def function(source, name):
    definition=re.search(r'static void '+re.escape(name)+r'\([^;{}]*\)\s*\{',source)
    assert definition,name
    start=definition.start()
    opening=definition.end()-1
    depth,end=1,opening+1
    while depth:
        depth+=(source[end]=='{')-(source[end]=='}');end+=1
    return source[start:end]

code = network[:network.index('static int64_t netd_syscall_one(')] + r'''
#include <assert.h>
#include <stdlib.h>
#include <string.h>
static char output[241];
static size_t calls,last_length;
static int64_t sink_result;
static unsigned file_opens;
static int64_t netd_syscall_four(uint64_t number,uint64_t path,
                               uint64_t flags,uint64_t mode,uint64_t handle) {
    assert(number==OS_SYS_FILE_OPEN && path && handle && mode==0644);
    assert(!strcmp((const char *)(uintptr_t)path,"/log/netd.log"));
    assert(flags==(OS_FILE_OPEN_WRITE|OS_FILE_OPEN_CREATE|OS_FILE_OPEN_TRUNCATE));
    ++file_opens; return -5;
}
static int64_t netd_syscall_one(uint64_t number,uint64_t argument) {
    (void)number;(void)argument;assert(!"fsync after failed open");return -5;
}
static int64_t capture(uint64_t number,uint64_t address,uint64_t length) {
    assert(number==OS_SYS_DEBUG_WRITE && address && length && length<=240);
    ++calls;last_length=length;
    memcpy(output,(void *)(uintptr_t)address,length);output[length]=0;
    return sink_result;
}
#define audiod_syscall_two capture
#define netd_syscall_two capture
''' + '\n'.join((function(network,'netd_persist_debug'),
                  function(audio,'audiod_debug_write'),
                  function(network,'netd_debug'),
                  function(network,'netd_debug_attempt'),
                  function(network,'netd_debug_packet'))) + r'''
int main(void) {
    void (*writers[])(const char *)={audiod_debug_write,netd_debug};
    for (unsigned i=0;i<2;++i) {
        calls=0;writers[i](NULL);writers[i]("");assert(!calls);
        const char *marker=i==0?"OS_AUDIOD_READY\r\n":"OS_NETD_START\r\n";
        writers[i](marker);assert(calls==1 && !strcmp(output,marker));
        for (size_t n=1;n<=241;++n) {
            char *text=malloc(n);assert(text);memset(text,'x',n);
            if (n<240) text[n-1]=0;
            calls=0;writers[i](text);
            size_t expected=n<240?n-1:240;
            assert(calls==(expected!=0));
            if (expected) {
                assert(last_length==expected && strlen(output)==expected);
                for (size_t j=0;j<expected;++j) assert(output[j]=='x');
            }
            free(text);
        }
        sink_result=-5;calls=0;writers[i](marker);assert(calls==1);sink_result=0;
    }
    netd_context_t context={0};
    calls=0;
    netd_debug_attempt(NULL,"ignored");netd_debug_packet(NULL,"ignored");
    netd_debug_attempt(&context,NULL);netd_debug_packet(&context,NULL);
    netd_debug_attempt(&context,"");netd_debug_packet(&context,"");
    assert(!calls && !context.debug_attempts && !context.debug_packets);
    for (unsigned i=0;i<1000;++i) {
        netd_debug_attempt(&context,"OS_NETD_DISCOVER_SEND_OK\r\n");
        netd_debug_packet(&context,"OS_NETD_PACKET_RX\r\n");
    }
    assert(calls==12 && context.debug_attempts==6 && context.debug_packets==6);
    context.debug_attempts=context.debug_packets=UINT32_MAX;calls=0;
    netd_debug_attempt(&context,"ignored");netd_debug_packet(&context,"ignored");
    assert(!calls && context.debug_attempts==UINT32_MAX && context.debug_packets==UINT32_MAX);
    assert(file_opens==1);
}
'''
with tempfile.TemporaryDirectory(prefix='service-diagnostics-') as directory:
    binary=Path(directory)/'test'
    subprocess.run(['cc','-std=gnu11','-Wall','-Wextra','-Werror',
                    '-fsanitize=address,undefined','-fno-pie','-no-pie',
                    '-I',str(ROOT/'include'),'-x','c','-','-o',str(binary)],
                   input=code,text=True,check=True)
    subprocess.run([str(binary)],check=True)
print('Service diagnostics: actual audio/network helpers, null/empty guards, exact 240-byte unterminated bounds, quotas and failed sinks passed')
