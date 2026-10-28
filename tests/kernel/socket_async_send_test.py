"""Production async submit/worker lifetime and ownership with controlled scheduling."""
from pathlib import Path
import resource
import signal
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root/'kernel/net/socket_io.c').read_text()
functions = source[source.index('static void socket_async_send_work('):
                   source.index('static void socket_receive_notify_peer(')]
code = r'''
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <kernel/deferred.h>
#include "socket_model.h"
static socket_t sock;
static socket_private_t state;
static completion_port_t port;
static unsigned socket_refs=1,port_refs=1,allocs,frees,fail_refs;
static bool oom,queue_fail,immediate,missing_private,post_fail;
static deferred_work_fn_t pending;
static void *pending_arg;
static uint64_t ids;
static unsigned posts,sends,expected_family;
static size_t expected_length;
static size_t allocation_length,allocated_bytes,cleared_bytes;
static unsigned char expected_payload[SOCKET_MAX_PAYLOAD];
static unsigned char input[SOCKET_MAX_PAYLOAD+32],address6[16];
static os_completion_entry_t last;
static kstatus_t send_status;
void *kmalloc(size_t n,uint32_t flags) {
    assert(!flags && n==sizeof(socket_async_send_t)+allocation_length);
    if(oom)return 0;
    void *p=malloc(n);assert(p);memset(p,0xA5,n);
    ++allocs;allocated_bytes+=n;return p;
}
void socket_zero(void *p,size_t n) {
    assert(n==sizeof(socket_async_send_t));
    cleared_bytes+=n;memset(p,0,n);
}
void kfree(void *p) { assert(p); ++frees; free(p); }
socket_private_t *socket_private(socket_t *s) { assert(s==&sock); return missing_private?0:&state; }
bool object_try_get(void *p) {
    assert(p==&sock || p==&port);
    if(p==&sock) { if(fail_refs&1)return false; ++socket_refs; }
    else { if(fail_refs&2)return false; ++port_refs; }
    return true;
}
void object_put(void *p) {
    if(p==&sock) { assert(socket_refs>1); --socket_refs; }
    else { assert(p==&port && port_refs>1); --port_refs; }
}
void socket_copy(void *out,const void *in,size_t n) { if(n)memmove(out,in,n); }
uint64_t socket_next_async_request_id(void) { return ++ids; }
bool deferred_schedule(deferred_work_fn_t fn,void *arg) {
    assert(!pending && socket_refs==2 && port_refs==2);
    if(queue_fail)return false;
    if(immediate)fn(arg);
    else { pending=fn; pending_arg=arg; }
    return true;
}
static kstatus_t sent(socket_t *s,const void *data,size_t n,uint16_t p,uint64_t *bytes) {
    assert(s==&sock && p==443 && n==expected_length && socket_refs==2 && port_refs==2);
    assert(!memcmp(data,expected_payload,n)); ++sends;
    *bytes=send_status==K_OK?n:0; return send_status;
}
kstatus_t socket_send(socket_t *s,const void *data,size_t n,uint32_t addr,uint16_t p,uint64_t *bytes) {
    assert(expected_family==4 && addr==0x01020304); return sent(s,data,n,p,bytes);
}
kstatus_t socket_send_ipv6(socket_t *s,const void *data,size_t n,const uint8_t addr[16],uint16_t p,uint64_t *bytes) {
    assert(expected_family==6 && !memcmp(addr,address6,16)); return sent(s,data,n,p,bytes);
}
kstatus_t completion_port_post(completion_port_t *p,const os_completion_entry_t *entry) {
    assert(p==&port && socket_refs==2 && port_refs==2); last=*entry; ++posts;
    return post_fail?K_EBUSY:K_OK;
}
''' + functions + r'''
static kstatus_t submit(unsigned family,const void *data,size_t n,uint64_t *id) {
    allocation_length=n;
    return family==4?socket_send_async(&sock,data,n,0x01020304,443,&port,0x12345678,id):
        socket_send_async_ipv6(&sock,data,n,address6,443,&port,0x12345678,id);
}
static void balanced(void) { assert(socket_refs==1 && port_refs==1 && allocs==frees && !pending); }
int main(void) {
    sock.object.type=KOBJECT_TYPE_SOCKET; sock.type=OS_SOCK_DGRAM;
    port.object.type=KOBJECT_TYPE_COMPLETION_PORT;
    for(unsigned i=0;i<16;++i)address6[i]=(unsigned char)(i+1);
    unsigned cases=0;
    for(unsigned family=4;family<=6;family+=2) {
        sock.family=family==4?OS_AF_INET4:OS_AF_INET6; expected_family=family;
        for(unsigned mode=0;mode<2;++mode)for(unsigned n=0;n<=SOCKET_MAX_PAYLOAD;++n) {
            immediate=mode!=0; expected_length=n;
            for(unsigned i=0;i<sizeof(input);++i)input[i]=(unsigned char)(i*17+n);
            memcpy(expected_payload,input+7,n);
            send_status=(n%3==0)?K_EAGAIN:K_OK; post_fail=n%5==0;
            uint64_t id=UINT64_MAX,previous_id=ids; unsigned previous_posts=posts;
            assert(submit(family,n?input+7:0,n,&id)==K_OK && id==previous_id+1);
            if(!immediate) {
                assert(posts==previous_posts && allocs==frees+1);
                memset(input,0xEE,sizeof(input));
                deferred_work_fn_t fn=pending; void *arg=pending_arg; pending=0; fn(arg);
            }
            assert(posts==previous_posts+1 && last.request_id==id && last.status==send_status);
            assert(last.user_key==0x12345678 && last.bytes_done==(send_status==K_OK?n:0));
            balanced(); ++cases;
        }
        immediate=false;
        for(unsigned mask=1;mask<=3;++mask) {
            fail_refs=mask; uint64_t id=UINT64_MAX;
            assert(submit(family,input,1,&id)==K_ECANCELED && id==UINT64_MAX); balanced();
        }
        fail_refs=0;
        uint64_t id=UINT64_MAX;
        oom=true; assert(submit(family,input,1,&id)==K_ENOMEM && id==UINT64_MAX); oom=false; balanced();
        queue_fail=true; assert(submit(family,input,1,&id)==K_EBUSY && id==UINT64_MAX); queue_fail=false; balanced();
        missing_private=true; assert(submit(family,input,1,&id)==K_EDEVREMOVED); missing_private=false;
        atomic_store(&state.closed,true); assert(submit(family,input,1,&id)==K_EDEVREMOVED); atomic_store(&state.closed,false);
        atomic_store(&state.send_shutdown,true); assert(submit(family,input,1,&id)==K_EPIPE); atomic_store(&state.send_shutdown,false);
        unsigned before=allocs;
        assert(submit(family,0,1,&id)==K_EINVAL);
        assert(submit(family,input,SOCKET_MAX_PAYLOAD+1,&id)==K_EINVAL);
        assert(submit(family,input,1,0)==K_EINVAL);
        port.object.type=0; assert(submit(family,input,1,&id)==K_EINVAL); port.object.type=KOBJECT_TYPE_COMPLETION_PORT;
        assert(before==allocs && id==UINT64_MAX); balanced();
    }
    uint64_t id=UINT64_MAX; unsigned before=allocs;
    assert(socket_send_async(0,input,1,0,0,&port,0,&id)==K_EINVAL);
    assert(socket_send_async(&sock,input,1,0,0,0,0,&id)==K_EINVAL);
    sock.object.type=0;
    assert(socket_send_async(&sock,input,1,0,0,&port,0,&id)==K_EINVAL);
    assert(socket_send_async_ipv6(&sock,input,1,address6,443,&port,0,&id)==K_EINVAL);
    sock.object.type=KOBJECT_TYPE_SOCKET;
    assert(socket_send_async_ipv6(&sock,input,1,0,443,&port,0,&id)==K_EINVAL);
    assert(socket_send_async_ipv6(&sock,input,1,address6,0,&port,0,&id)==K_EINVAL);
    sock.family=OS_AF_INET4;
    assert(socket_send_async_ipv6(&sock,input,1,address6,443,&port,0,&id)==K_EINVAL);
    sock.family=OS_AF_INET6; sock.type=OS_SOCK_STREAM;
    assert(socket_send_async_ipv6(&sock,input,1,address6,443,&port,0,&id)==K_EINVAL);
    assert(before==allocs && id==UINT64_MAX); balanced(); socket_async_send_work(0);
    assert(cleared_bytes==allocs*sizeof(socket_async_send_t));
    printf("Async storage: %zu-byte header, exact header+payload allocation, %zu allocated bytes and %zu header-only cleared bytes\n",
           sizeof(socket_async_send_t),allocated_bytes,cleared_bytes);
    printf("Socket async: %u full-length IPv4/IPv6 queued/immediate-worker cases, copied payload/completions and all reference/OOM/schedule failures balanced\n",cases);
}
'''
with tempfile.TemporaryDirectory(prefix='liteos-socket-async-') as directory:
    path=Path(directory)
    binary=str(path/'test')
    command=['cc','-std=c11','-O2','-Wall','-Wextra','-Werror',
             '-I'+str(root/'include'),'-I'+str(root/'kernel/net'),
             '-fsanitize=address,undefined','-fno-pie','-no-pie','-x','c','-','-o',binary]
    subprocess.run(command,input=code,text=True,check=True)
    subprocess.run([binary],check=True,timeout=30)
    # Reintroduce the old post-publication access: immediate execution frees it.
    bad=code.replace('*request_id = assigned_id;', '*request_id = request->request_id; (void)assigned_id;')
    subprocess.run(command,input=bad,text=True,check=True)
    failure=subprocess.run([binary],text=True,capture_output=True,timeout=30)
    assert failure.returncode!=0 and 'heap-use-after-free' in failure.stderr
    print('Regression control: post-publication request-ID read reproduces ASan heap-use-after-free')
    for name, old, new in (
        ('maximum-size allocation', 'kmalloc(sizeof(*request) + length, 0)',
         'kmalloc(sizeof(*request) + SOCKET_MAX_PAYLOAD, 0)'),
        ('payload clearing', 'socket_zero(request, sizeof(*request));',
         'socket_zero(request, sizeof(*request) + length);'),
    ):
        bad=code.replace(old,new)
        assert bad!=code,name
        subprocess.run(command,input=bad,text=True,check=True)
        failure=subprocess.run(
            [binary],capture_output=True,text=True,timeout=30,
            preexec_fn=lambda: resource.setrlimit(resource.RLIMIT_CORE,(0,0)))
        assert failure.returncode==-signal.SIGABRT and 'Assertion' in failure.stderr, (
            name,failure.returncode,failure.stderr)
        print('Negative control rejected:',name)
