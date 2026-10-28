"""Execute actual pending-flip retirement against a fault-injected KAPI."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root/'kernel/drivers/display/nvidia/nv_nvkms_backend.c').read_text()
def function(name):
    start = source.index(name)
    opening = source.index('{', start)
    depth, end = 1, opening+1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]

code = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>
#define K_OK 0
#define K_ETIMEDOUT -110
#define NV_TRUE 1
#define NV_FALSE 0
#define NVKMS_KAPI_LAYER_PRIMARY_IDX 0
typedef int NvBool;
typedef int kstatus_t;
typedef void (*display_scanout_complete_fn)(void *, kstatus_t);
typedef struct { int id; } nvkms_scanout_surface_t;
struct functions { NvBool (*getFlipPendingStatus)(void *, unsigned, unsigned, NvBool *); };
typedef struct {
 bool lock_initialized,pending,pending_timed_out;
 int lock;
 uint64_t pending_started_ns,submitted_flips,completed_flips,recovered_flips;
 uint64_t timed_out_flips,pending_query_failures;
 struct functions *functions;
 void *device;
 unsigned head;
 nvkms_scanout_surface_t current,pending_surface,spare;
 display_scanout_complete_fn pending_complete;
 void *pending_context;
} nvkms_display_state_t;
static uint64_t now;
static bool held,contended,query_success=true,hw_pending;
static unsigned callbacks,reports;
static int result;
static kstatus_t nv_port_mutex_lock(int *lock,uint64_t timeout) {
 (void)lock; assert(!timeout);
 if(contended)return -16;
 assert(!held);held=true;return K_OK;
}
static int nv_port_mutex_unlock(int *lock) {(void)lock;assert(held);held=false;return 0;}
static uint64_t nv_port_time_now_ns(void){return now;}
static void nvkms_trace(const char *s){assert(!held&&s);++reports;}
static int kernel_serial_printf_serial_only(const char *s,...) {
 assert(!held&&s);return 0;
}
static NvBool query(void *d,unsigned h,unsigned l,NvBool *pending){
 (void)d;(void)h;(void)l;assert(held);*pending=hw_pending;return query_success;
}
static void complete(void *ctx,kstatus_t status){assert(!held&&ctx);++callbacks;result=status;}
''' + function('static void nvkms_pending_progress(') + r'''
int main(void){
 struct functions f={query};
 nvkms_display_state_t s={.lock_initialized=true,.pending=true,
  .functions=&f,.current={1},.pending_surface={2},
  .pending_complete=complete,.pending_context=&s};
 hw_pending=true;now=249999999;
 nvkms_pending_progress(&s,false);assert(!callbacks&&s.pending);
 now=250000000;nvkms_pending_progress(&s,false);
 assert(callbacks==1&&result==K_ETIMEDOUT&&reports==1&&s.pending);
 assert(s.current.id==1&&s.pending_surface.id==2&&!s.spare.id);
 nvkms_pending_progress(&s,true);assert(callbacks==1&&reports==1);
 hw_pending=false;nvkms_pending_progress(&s,false);
 assert(!s.pending&&s.current.id==2&&s.spare.id==1&&!s.pending_surface.id);
 assert(callbacks==1&&s.recovered_flips==1&&s.completed_flips==1);
 /* Late duplicate event must not retire a NEW hardware-pending flip. */
 s.pending=true;s.pending_surface.id=3;s.spare.id=0;s.pending_started_ns=now;
 s.pending_complete=complete;s.pending_context=&s;hw_pending=true;
 nvkms_pending_progress(&s,true);assert(callbacks==1&&s.pending&&s.current.id==2);
 contended=true;hw_pending=false;
 nvkms_pending_progress(&s,false);assert(s.pending&&callbacks==1);contended=false;
 query_success=false;nvkms_pending_progress(&s,false);
 assert(s.pending&&s.pending_query_failures==1&&callbacks==1);
 query_success=true;nvkms_pending_progress(&s,false);
 assert(callbacks==2&&result==K_OK&&!s.pending&&s.current.id==3&&s.spare.id==2);
 nvkms_pending_progress(&s,true);assert(callbacks==2);
 /* Failed/unsupported queries never recycle surfaces, even after timeout. */
 s.pending=true;s.pending_surface.id=4;s.spare.id=0;
 s.pending_complete=complete;s.pending_context=&s;s.pending_started_ns=now;
 f.getFlipPendingStatus=NULL;now+=250000000;
 nvkms_pending_progress(&s,false);
 assert(s.pending&&s.current.id==3&&s.pending_surface.id==4&&callbacks==3);
 assert(result==K_ETIMEDOUT&&s.timed_out_flips==2&&reports==2);
}
'''
with tempfile.TemporaryDirectory(prefix='nvkms-pending-') as temporary:
    binary = str(Path(temporary)/'test')
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror',
                    '-fsanitize=address,undefined','-fno-pie','-no-pie',
                    '-x','c','-','-o',binary],input=code,text=True,check=True)
    subprocess.run([binary],check=True)
print('NVKMS pending: lost event recovery, timeout quarantine, late/duplicate event, contention/query failure, exactly-once completion PASS')
