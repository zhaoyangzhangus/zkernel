"""Execute actual OS event escape forwarding, preserving vendor status packets."""
from pathlib import Path
import subprocess
import tempfile

root=Path(__file__).resolve().parents[2]
source=(root/'kernel/drivers/gpu/nvidia/rm/nv_rm_vendor_bridge.c').read_text()
begin=source.index('    case NV_ESC_RM_GET_EVENT_DATA:')
end=source.index('    case NV_ESC_CHECK_VERSION_STR:',begin)
code=r'''
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <assert.h>
#define NV_ESC_ALLOC_OS_EVENT 206
#define NV_ESC_FREE_OS_EVENT 207
#define NV_ESC_RM_GET_EVENT_DATA 41
#define NV_OK 0
#define K_OK 0
#define K_EINVAL -22
#define K_ENOMEM -12
#define K_EIO -5
typedef unsigned NV_STATUS;
typedef struct {unsigned hClient,hDevice,fd,Status;} nv_ioctl_alloc_os_event_t,nv_ioctl_free_os_event_t;
typedef struct {uint64_t pEvent;unsigned MoreEvents,status;} NVOS41_PARAMETERS;
typedef int nvidia_stack_t;
typedef struct {bool control; int vendor_private;} file_t;
static nvidia_stack_t stack;
static int state_value,*state=&state_value;
static bool no_stack; static unsigned calls,frees,vendor_status;
static nvidia_stack_t *nv_rm_vendor_frontend_stack_create(void) {return no_stack?NULL:&stack;}
static void kfree(void *p) {assert(p==&stack);++frees;}
static NV_STATUS rm_ioctl(nvidia_stack_t *s,int *nv,int *fp,unsigned cmd,void *argument,unsigned size) {
    assert(s==&stack && nv==state && fp && size==16 && (cmd==206 || cmd==207 || cmd==41));
    if(cmd==41) {
        NVOS41_PARAMETERS *api=argument;assert(api->pEvent==0x12345000);
        api->MoreEvents=1;api->status=42;++calls;return vendor_status;
    }
    nv_ioctl_alloc_os_event_t *api=argument;
    assert(api->hClient==123 && api->fd==19);
    api->Status=42; ++calls; return vendor_status;
}
static int dispatch(file_t *file,unsigned number,void *argument,unsigned argument_size) {
    switch(number) {
''' + source[begin:end] + r'''
    default:return -38;
    }
}
int main(void) {
    file_t f={true,0}; nv_ioctl_alloc_os_event_t api={123,7,19,0};
    for(unsigned n=206;n<=207;++n) {
        api.Status=0;
        assert(!dispatch(&f,n,&api,sizeof(api)) && api.Status==42);
    }
    assert(calls==2 && frees==2);
    f.control=false;assert(!dispatch(&f,206,&api,16) && calls==3 && frees==3);
    f.control=true;assert(dispatch(&f,206,&api,15)==-22 && calls==3);
    no_stack=true;assert(dispatch(&f,206,&api,16)==-12 && calls==3);
    no_stack=false;vendor_status=7;
    assert(dispatch(&f,206,&api,16)==-5 && calls==4 && frees==4);
    vendor_status=0;NVOS41_PARAMETERS event={.pEvent=0x12345000};
    assert(!dispatch(&f,41,&event,sizeof(event))&&event.MoreEvents==1&&event.status==42&&calls==5&&frees==5);
    assert(dispatch(&f,41,&event,15)==-22&&calls==5);
    no_stack=true;assert(dispatch(&f,41,&event,16)==-12&&calls==5);
    no_stack=false;vendor_status=7;assert(dispatch(&f,41,&event,16)==-5&&calls==6&&frees==6);
    /* 即使 RM 自身拒绝请求，共享路径也必须保留数据包状态，并且只释放一个临时栈。 */
    for(unsigned i=0;i<10000;++i) {
        f.control=(i&1)!=0;
        vendor_status=i%3?0:7;
        unsigned number=i%3==0?41:(i%3==1?206:207);
        unsigned old_calls=calls,old_frees=frees;
        int status=dispatch(&f,number,number==41?(void*)&event:(void*)&api,16);
        assert(status==(vendor_status?-5:0)&&calls==old_calls+1&&frees==old_frees+1);
        assert(number==41?event.status==42:api.Status==42);
        assert(dispatch(&f,number,number==41?(void*)&event:(void*)&api,0)==-22);
        assert(calls==old_calls+1&&frees==old_frees+1);
    }
}
'''
with tempfile.TemporaryDirectory(prefix='nvidia-events-') as directory:
    base=Path(directory);(base/'test.c').write_text(code)
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror',str(base/'test.c'),'-o',str(base/'test')],check=True)
    subprocess.run([str(base/'test')],check=True)
print('NVIDIA event escapes: actual dispatch, real RM call boundary, preserved status, size/node checks and stack/error cleanup passed')
