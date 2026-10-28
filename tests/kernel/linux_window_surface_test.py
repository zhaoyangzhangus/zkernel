"""Run the production ioctl adapter, including failed lease-token return."""
from pathlib import Path
import resource
import signal
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'kernel/syscall/linux.c').read_text()
start = source.index('static kstatus_t linux_window_surface_ioctl(')
end = source.index('static int64_t linux_ioctl(', start)
program = r'''
#include <assert.h>
#include <stdbool.h>
#include <string.h>
#include "uapi/window_surface.h"
#define __user
#define K_OK 0
#define K_EINVAL -22
#define LINUX_EFAULT -14
typedef int kstatus_t;
typedef struct {int id;} process_t;
typedef struct {int id;} window_server_window_t;
static os_surface_command_t user;
static unsigned begins,cancels,commits,writes;
static int read_error,write_error,begin_error;
static bool linux_user_pointer(uint64_t p,unsigned n){return p==(uintptr_t)&user&&n==sizeof(user);}
static int linux_copy_status(int s){return s;}
static int copy_from_user(void *d,const void *s,unsigned n){if(read_error)return read_error;memcpy(d,s,n);return 0;}
static int copy_to_user(void *d,const void *s,unsigned n){++writes;if(write_error)return write_error;memcpy(d,s,n);return 0;}
static int window_server_surface_begin(process_t *p,window_server_window_t *w,os_surface_frame_t *f){
 assert(p&&w);++begins;if(begin_error)return begin_error;
 *f=(os_surface_frame_t){.token=7,.address=0x20000,.capacity=4096,.width=16,.height=16,.stride=64};return 0;
}
static int window_server_surface_end(process_t *p,window_server_window_t *w,const os_surface_frame_t *f,bool commit){
 assert(p&&w&&f->token==7);if(commit)++commits;else ++cancels;return 0;
}
''' + source[start:end] + r'''
int main(void){
 process_t p={1};window_server_window_t w={1};uint64_t ptr=(uintptr_t)&user;
 assert(linux_window_surface_ioctl(&p,&w,0)==LINUX_EFAULT);
 user.operation=OS_SURFACE_BEGIN;read_error=LINUX_EFAULT;
 assert(linux_window_surface_ioctl(&p,&w,ptr)==LINUX_EFAULT&&!begins);
 read_error=0;user.reserved=1;
 assert(linux_window_surface_ioctl(&p,&w,ptr)==K_EINVAL&&!begins);
 user.reserved=0;user.frame.token=9;
 assert(linux_window_surface_ioctl(&p,&w,ptr)==K_EINVAL&&!begins);
 user.frame.token=0;
 int32_t *damage_signed[]={&user.frame.damage_x,&user.frame.damage_y};
 for(unsigned i=0;i<2;++i){*damage_signed[i]=-1;assert(linux_window_surface_ioctl(&p,&w,ptr)==K_EINVAL&&!begins);*damage_signed[i]=0;}
 uint32_t *damage_unsigned[]={&user.frame.damage_width,&user.frame.damage_height};
 for(unsigned i=0;i<2;++i){*damage_unsigned[i]=1;assert(linux_window_surface_ioctl(&p,&w,ptr)==K_EINVAL&&!begins);*damage_unsigned[i]=0;}
 begin_error=-110;
 assert(linux_window_surface_ioctl(&p,&w,ptr)==-110&&!writes&&!cancels);
 begin_error=0;write_error=LINUX_EFAULT;
 assert(linux_window_surface_ioctl(&p,&w,ptr)==LINUX_EFAULT&&cancels==1);
 assert(!user.frame.token); // Token never reached the client.
 write_error=0;assert(linux_window_surface_ioctl(&p,&w,ptr)==K_OK);
 assert(user.frame.token==7&&user.frame.capacity==4096);
 unsigned before=writes;write_error=LINUX_EFAULT;
 user.operation=OS_SURFACE_COMMIT;
 assert(linux_window_surface_ioctl(&p,&w,ptr)==K_OK&&commits==1&&writes==before);
 user.operation=OS_SURFACE_CANCEL;
 assert(linux_window_surface_ioctl(&p,&w,ptr)==K_OK&&cancels==2&&writes==before);
 user.operation=999;assert(linux_window_surface_ioctl(&p,&w,ptr)==K_EINVAL);
}
'''
with tempfile.TemporaryDirectory(prefix='linux-window-surface-') as directory:
    path = Path(directory)
    (path / 'test.c').write_text(program)
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
                    '-I', str(root / 'include'), str(path / 'test.c'),
                    '-o', str(path / 'test')], check=True)
    subprocess.run([str(path / 'test')], check=True, timeout=20)
    broken = program.replace(
        '(void)window_server_surface_end(process, window, &command.frame, false);',
        '(void)0;')
    assert broken != program
    (path / 'broken.c').write_text(broken)
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                    '-I', str(root / 'include'), str(path / 'broken.c'),
                    '-o', str(path / 'broken')], check=True)
    def no_core():
        resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
    result = subprocess.run([str(path / 'broken')], capture_output=True,
                            preexec_fn=no_core, timeout=20)
    assert result.returncode == -signal.SIGABRT
print('Surface ioctl: pointer/reserved validation, BEGIN failure, lost-token rollback, '
      'valid acquisition, commit/cancel and omitted-rollback negative control passed')
