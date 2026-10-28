"""Transport timeouts must not become transient EAGAIN in the Linux ABI."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root/'kernel/syscall/linux.c').read_text()
def function(name):
    start=source.index('static int64_t '+name+'(')
    end=source.index('{',start)+1
    depth=1
    while depth:
        depth+=(source[end]=='{')-(source[end]=='}')
        end+=1
    return source[start:end]

code=r'''
#include <assert.h>
#include <stdint.h>
#define K_OK 0
#define K_EAGAIN -11
#define K_ETIMEDOUT -110
#define K_EDEVREMOVED -2001
#define K_EDEVLOST -2002
#define LINUX_ECONNRESET -104
typedef int kstatus_t;
#define OS_SOCKET_OPTION_ERROR 7
typedef struct {int error,query_status;unsigned queries;} socket_t;
static int socket_get_option(socket_t *s,unsigned option,int32_t *out){
 assert(s && option==OS_SOCKET_OPTION_ERROR);++s->queries;
 *out=s->error;return s->query_status;
}
''' + function('linux_socket_status') + function('linux_socket_receive_status') + r'''
int main(void){
 assert(linux_socket_status(K_ETIMEDOUT)==K_ETIMEDOUT);
 assert(linux_socket_status(K_EAGAIN)==K_EAGAIN);
 assert(linux_socket_status(K_EDEVREMOVED)==LINUX_ECONNRESET);
 assert(linux_socket_status(K_EDEVLOST)==LINUX_ECONNRESET);
 assert(linux_socket_status(-111)==-111);
 assert(linux_socket_status(K_OK)==K_OK);
 socket_t socket={0};
 const int errors[]={110,104,111,5,101};
 for(unsigned i=0;i<sizeof(errors)/sizeof(errors[0]);++i){
  socket.error=errors[i];
  assert(linux_socket_receive_status(&socket,K_ETIMEDOUT)==-errors[i]);
  assert(socket.error==errors[i]); /* SO_ERROR remains available. */
 }
 socket.error=0;
 assert(linux_socket_receive_status(&socket,K_ETIMEDOUT)==K_EAGAIN);
 socket.query_status=-22;
 assert(linux_socket_receive_status(&socket,K_ETIMEDOUT)==-22);
 unsigned before=socket.queries;
 assert(linux_socket_receive_status(&socket,K_EAGAIN)==K_EAGAIN);
 assert(linux_socket_receive_status(&socket,K_OK)==K_OK);
 assert(linux_socket_receive_status(&socket,-104)==-104);
 assert(socket.queries==before);
}
'''
with tempfile.TemporaryDirectory(prefix='linux-socket-timeout-') as directory:
    path=Path(directory)
    (path/'test.c').write_text(code)
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror',
                    '-fsanitize=address,undefined','-fno-pie','-no-pie',
                    str(path/'test.c'),'-o',str(path/'test')],check=True)
    subprocess.run([str(path/'test')],check=True)
    # Original broad conversion reproduces the browser's endless retry.
    broken=code.replace('static int64_t linux_socket_status(kstatus_t status) {',
        'static int64_t linux_socket_status(kstatus_t status) {\n'
        '    if (status == K_ETIMEDOUT) return K_EAGAIN;')
    assert broken!=code
    (path/'test.c').write_text(broken)
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror',
                    str(path/'test.c'),'-o',str(path/'old')],check=True)
    result=subprocess.run([str(path/'old')],capture_output=True)
    assert result.returncode!=0 and b'Assertion' in result.stderr
# Cover every ABI call site, not just the conversion helper in isolation.
assert 'return linux_socket_receive_status(socket, status);' in function('linux_socket_receive_payload')
assert 'linux_socket_status(status)' in function('linux_socket_send_payload')
assert 'return linux_socket_status(status);' in function('linux_socket_connect')
assert 'if (status == K_ETIMEDOUT) status = K_EAGAIN;' in function('linux_socket_accept')
print('Linux socket: terminal timeout preserved, wait-only timeout EAGAIN, errno/query failures and unchanged fast path PASS')
print('Negative control rejected: transport timeout disguised as transient EAGAIN')
