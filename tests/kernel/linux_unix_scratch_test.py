"""Execute real Unix sendto/recvfrom bodies, including every scratch cleanup."""
from pathlib import Path
import resource
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
source = (ROOT / 'kernel/syscall/linux.c').read_text()

def function(name):
    start = source.index('static ', source.index(name + '(') - 32)
    brace = source.index('{', start)
    end, depth = brace + 1, 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]

prefix = r'''
#include <kernel/base.h>
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#define LINUX_MSG_DONTWAIT 0x40U
#define LINUX_MSG_NOSIGNAL 0x4000U
#define LINUX_O_NONBLOCK 0x800U
#define LINUX_PIPE_COPY_MAX 4096U
#define KOBJECT_TYPE_LINUX_UNIX_SOCKET 12U
#define LINUX_EFAULT -14
#define LINUX_EAFNOSUPPORT -97
typedef struct { struct { unsigned type; } object; } linux_unix_socket_t;
typedef struct { unsigned read_mutex, write_mutex; } linux_unix_channel_t;
typedef struct { int unused; } pipe_endpoint_t;
typedef struct { void *private_data; uint32_t flags; } linux_fd_entry_t;
static linux_unix_channel_t channel;
static pipe_endpoint_t endpoint;
static unsigned allocations, frees, pins, unpins, locks, unlocks, calls, commits;
static unsigned scenario, requested;
static uint64_t expected_timeout;
static void *heap;
static bool no_channel;
static unsigned char user[8192], wire[4096];
static void *kmalloc(size_t n, unsigned flags) {
    (void)flags; ++allocations;
    if (scenario == 1) return NULL;
    assert(!heap); heap = malloc(n); assert(heap); return heap;
}
static void kfree(void *p) { assert(p && p == heap); free(p); heap=NULL; ++frees; }
static bool linux_user_pointer(uint64_t p, size_t n) {
    return p == (uintptr_t)user && n <= sizeof(user) && scenario != 2;
}
static kstatus_t copy_from_user(void *d,const void *s,size_t n) {
    if (scenario == 3) return K_EACCES;
    memcpy(d,s,n);return K_OK;
}
static kstatus_t copy_to_user(void *d,const void *s,size_t n) {
    if (scenario == 8) return K_EACCES;
    memcpy(d,s,n);return K_OK;
}
static void linux_unix_socket_pin_io(linux_unix_socket_t *s,bool write,
                                    pipe_endpoint_t **e,linux_unix_channel_t **c) {
    (void)write;assert(s);++pins;
    *e=scenario==4?NULL:&endpoint;*c=no_channel?NULL:&channel;
}
static void linux_unix_socket_unpin_io(pipe_endpoint_t *e,linux_unix_channel_t *c) {
    assert(e==&endpoint && (c==&channel || !c));++unpins;
}
static kstatus_t kmutex_lock(unsigned *m,uint64_t timeout) {
    assert(m);assert(timeout==expected_timeout);++locks;
    return scenario==5?K_ETIMEDOUT:K_OK;
}
static kstatus_t kmutex_unlock(unsigned *m) { assert(m);++unlocks;return K_OK; }
static kstatus_t pipe_write(pipe_endpoint_t *e,const void *p,size_t n,
                            uint64_t timeout,uint64_t *bytes) {
    assert(e==&endpoint && n==requested && timeout==expected_timeout);++calls;
    if(scenario==6)return K_EIO;
    *bytes=scenario==7?n/2:n;memcpy(wire,p,*bytes);return K_OK;
}
static kstatus_t pipe_read(pipe_endpoint_t *e,void *p,size_t n,
                           uint64_t timeout,uint64_t *bytes) {
    assert(e==&endpoint && n==requested && timeout==expected_timeout);++calls;
    if(scenario==6)return K_EIO;
    *bytes=scenario==7?n/2:n;memcpy(p,wire,*bytes);return K_OK;
}
static void linux_unix_channel_commit_write(linux_unix_channel_t *c,uint64_t n) {
    assert(c==&channel || !c);assert(n<=requested);++commits;
}
static kstatus_t linux_unix_channel_commit_read(linux_unix_channel_t *c,
                                               uint64_t n,bool records,void *out) {
    assert(!records && !out);linux_unix_channel_commit_write(c,n);return K_OK;
}
static int64_t linux_copy_status(kstatus_t s) { return s; }
'''
bodies = '\n'.join(function(n) for n in (
    'linux_pipe_status', 'linux_socket_timeout',
    'linux_unix_sendto', 'linux_unix_recvfrom'))
main = r'''
int main(void) {
    linux_unix_socket_t socket={.object={KOBJECT_TYPE_LINUX_UNIX_SOCKET}};
    linux_fd_entry_t entry={.private_data=&socket};
    const unsigned sizes[]={0,1,255,256,257,4096,8192};
    for(unsigned receive=0;receive<2;++receive)
    for(unsigned i=0;i<sizeof(sizes)/sizeof(sizes[0]);++i)
    for(unsigned mode=0;mode<3;++mode)
    for(unsigned nc=0;nc<2;++nc)
    for(scenario=0;scenario<9;++scenario) {
        unsigned n=sizes[i];requested=n>4096?4096:n;
        allocations=frees=pins=unpins=locks=unlocks=calls=commits=0;
        no_channel=nc;entry.flags=mode==1?LINUX_O_NONBLOCK:0;
        unsigned flags=mode==2?LINUX_MSG_DONTWAIT:0;
        expected_timeout=mode?0:UINT64_MAX;
        memset(user,receive?0x55:0x3a,sizeof(user));memset(wire,0x3a,sizeof(wire));
        int64_t expected=requested;
        bool early=!n || scenario==2 || (scenario==1 && requested>256);
        if(!n)expected=0;
        else if(scenario==2)expected=LINUX_EFAULT;
        else if(scenario==1 && requested>256)expected=K_ENOMEM;
        else if(!receive && scenario==3)expected=LINUX_EFAULT;
        else if(scenario==4)expected=receive?0:K_EPIPE;
        else if(scenario==5 && !nc)expected=K_EAGAIN;
        else if(scenario==6)expected=K_EIO;
        else if(receive && scenario==8)expected=K_EACCES;
        else if(scenario==7)expected=requested/2;
        int64_t result=receive?
          linux_unix_recvfrom(&entry,(uintptr_t)user,n,flags,0,0):
          linux_unix_sendto(&entry,(uintptr_t)user,n,flags,0,0);
        assert(result==expected && !heap);
        unsigned expected_alloc=n && scenario!=2 && requested>256;
        assert(allocations==expected_alloc);
        assert(frees==(expected_alloc && scenario!=1));
        assert(unpins==(pins && scenario!=4));
        assert(unlocks==(locks && scenario!=5));
        if(early || (!receive && scenario==3) || scenario==4)
            assert(!calls && !locks && !commits);
        if(result>0)for(int64_t j=0;j<result;++j)
            assert((receive?user[j]:wire[j])==0x3a);
    }
    return 0;
}
'''
resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
with tempfile.TemporaryDirectory(prefix='unix-scratch-') as directory:
    path = Path(directory)
    for negative in (False, True):
        changed = bodies
        if negative:
            changed = changed.replace(
                'request <= sizeof(inline_payload) ? inline_payload :',
                'request < 1U ? inline_payload :')
            assert changed != bodies
        code = path / 'test.c'
        code.write_text(prefix + changed + main)
        exe = path / 'test'
        subprocess.run(['cc', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                        '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
                        '-I'+str(ROOT/'include'), str(code), '-o', str(exe)], check=True)
        result = subprocess.run([str(exe)], capture_output=True)
        assert (result.returncode != 0) == negative, result.stderr.decode()
print('Unix scratch: 756 production-path cases, inline/heap boundary, failures, '
      'short I/O, ownership and nonblocking PASS; negative control rejected')
