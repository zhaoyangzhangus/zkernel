"""Actual native window create/map handlers: failed publication cleanup."""
from pathlib import Path
import subprocess
import tempfile
root = Path(__file__).resolve().parents[2]
source = (root / 'kernel/syscall/graphics.c').read_text()
def extract(text, signature):
    start=text.index(signature)
    end=text.index('{',start)+1
    depth=1
    while depth:
        depth+=(text[end]=='{')-(text[end]=='}')
        end+=1
    return text[start:end]
create = extract(source, 'int64_t syscall_window_create(')
mapping = source[source.index('int64_t syscall_window_map('):
                 source.index('int64_t syscall_window_set(')]
internal = (root / 'kernel/process/internal.h').read_text()
helper = extract(internal, 'static inline vm_space_t *process_vm_reference(')
caption = extract((root/'include/uapi/window.h').read_text(), 'static inline bool os_window_caption_create_valid(')
fixture = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#define __user
#define K_OK 0
#define K_EINVAL -22
#define K_EPERM -1
#define OS_INVALID_HANDLE 0
#define WINDOW_RIGHT_ALL 3
#define OS_WINDOW_VISIBLE 1
#define OS_WINDOW_RESIZABLE 2
#define OS_WINDOW_CLIENT_DECORATIONS 4
#define OS_WINDOW_PREMULTIPLIED_ALPHA 8
#define OS_WINDOW_STANDARD_CAPTION_CONTROLS 16
#define OS_WINDOW_DARK_CAPTION 128
#define OS_WINDOW_CAPTION_HEIGHT_MASK (255U<<16U)
@CAPTION@
typedef int kstatus_t;
typedef unsigned handle_t;
typedef struct {unsigned refs;} vm_space_t;
typedef struct {vm_space_t *vm;unsigned handles;} process_t;
typedef struct {unsigned hdr,reserved,flags,width,height,background;int x,y;char title[32];uint64_t address,buffer_size;unsigned identifier,window;} os_window_create_t;
typedef struct {unsigned hdr,reserved,identifier;uint64_t address,length;} os_window_map_t;
typedef struct {unsigned refs;} window_server_window_t;
static process_t p;
static vm_space_t *expected;
static window_server_window_t *active;
static bool locked,scene,handle_owned,manager;
static unsigned failure,mode,stage,unmaps,vm_destroyed,window_destroyed,gets;
static os_window_create_t create_args;
static os_window_map_t map_args;
static bool mapping_call;
static process_t *current_process(void){return &p;}
static bool versioned_header_valid(unsigned *hdr,size_t size){(void)size;return *hdr==1;}
static int copy_from_user(void *to,const void *from,size_t n){memcpy(to,from,n);return 0;}
static void process_thread_lock(process_t *v){assert(v==&p && !locked);locked=true;}
static void process_thread_unlock(process_t *v){assert(v==&p && locked);locked=false;}
static vm_space_t *allocate(void){vm_space_t *v=calloc(1,sizeof(*v));assert(v);v->refs=1;return v;}
static void vm_space_get(vm_space_t *v){assert(locked && v->refs);++v->refs;++gets;}
static void vm_space_put(vm_space_t *v){assert(!locked);if(v && !--v->refs){++vm_destroyed;free(v);}}
@HELPER@
static void object_put(window_server_window_t *w){assert(!locked && w==active && w->refs);if(!--w->refs){++window_destroyed;free(w);}}
static void window_server_put(window_server_window_t *w){object_put(w);}
static void inject(unsigned where){
 if(!mode || stage!=where)return;
 process_thread_lock(&p);assert(p.vm==expected);p.vm=mode==1?NULL:allocate();process_thread_unlock(&p);
 vm_space_put(expected);
}
static int window_server_close_owned(process_t *v,window_server_window_t *w){assert(v==&p && w==active && w->refs);if(scene){scene=false;object_put(w);}return 0;}
static int handle_close(unsigned *table,handle_t h){assert(table==&p.handles && h==7 && handle_owned);handle_owned=false;window_server_close_owned(&p,active);object_put(active);return 0;}
static int handle_create(unsigned *table,window_server_window_t *w,unsigned rights,handle_t *h){assert(table==&p.handles && w==active && rights==3);if(failure==2)return -24;++w->refs;handle_owned=true;*h=7;return 0;}
static int window_server_create(process_t *v,int x,int y,unsigned w,unsigned h,unsigned f,unsigned bg,const char *title,window_server_window_t **out){
 assert(v==&p && x==10 && y==20 && w==640 && h==480 && f==create_args.flags && bg==0 && title);
 if(failure==1)return -12;
 active=calloc(1,sizeof(*active));assert(active);active->refs=2;scene=true;*out=active;return 0;
}
static uint64_t window_server_window_buffer_size(window_server_window_t *w){assert(w==active && w->refs);return 8192;}
static unsigned window_server_window_identifier(window_server_window_t *w){assert(w==active && w->refs);return 123;}
static int window_server_map_space(window_server_window_t *w,vm_space_t *v,uint64_t requested,uint64_t *out){
 assert(w==active && v==expected && v->refs==2 && !requested);inject(3);assert(v->refs);
 if(failure==3)return -12;*out=0x20000;return 0;
}
static int window_server_set_owner_address(window_server_window_t *w,uint64_t a){assert(w==active && w->refs && a==0x20000);inject(4);return failure==4?-5:0;}
static int window_server_set(window_server_window_t *w,int x,int y,unsigned visible){assert(w==active && w->refs && x==10 && y==20 && visible==1);inject(5);return failure==5?-5:0;}
static int copy_to_user(void *to,const void *from,size_t size){
 assert(to==(mapping_call?(void *)&map_args:(void *)&create_args));inject(6);
 if(failure==6)return -14;memcpy(to,from,size);return 0;
}
static int vm_unmap(vm_space_t *v,uint64_t address,size_t size){assert(!locked && v==expected && v->refs && address==0x20000 && size==8192);++unmaps;return -5;}
static bool window_server_is_manager(process_t *v){assert(v==&p);return manager;}
static int window_server_lookup(unsigned id,window_server_window_t **out){assert(id==123 && scene);++active->refs;*out=active;return 0;}
static struct {uint64_t tsc_hz;} x86_boot_cpu_features={100};
static uint64_t telemetry_timestamp(void){return 100;}
static uint64_t x86_tsc_to_ns(uint64_t t){return t;}
static void kernel_frame_trace_emit_scope(const char *s,uint64_t t){(void)s;(void)t;assert(!locked);}
static void kernel_frame_trace_emit_value(const char *s,uint64_t t){(void)s;(void)t;assert(!locked);}
@CREATE@
@MAP@
static void setup(unsigned f,unsigned m,unsigned s){
 failure=f;mode=m;stage=s;p.vm=allocate();expected=p.vm;active=NULL;scene=handle_owned=locked=false;manager=true;
 unmaps=vm_destroyed=window_destroyed=gets=0;
 create_args=(os_window_create_t){.hdr=1,.x=10,.y=20,.width=640,.height=480,.flags=1};
 map_args=(os_window_map_t){.hdr=1,.identifier=123};
}
static void cleanup_vm(bool detached,unsigned m){
 assert(vm_destroyed==detached && !locked);
 if(p.vm){assert(p.vm->refs==1);vm_space_put(p.vm);p.vm=NULL;}
 assert(vm_destroyed==(detached && m==2?2:1));
}
int main(void){
 unsigned cases=0;mapping_call=false;
 for(unsigned f=0;f<=6;++f)for(unsigned m=0;m<3;++m)for(unsigned s=3;s<=6;++s){
  setup(f,m,s);
  int want=f==1 || f==3?-12:f==2?-24:f==4 || f==5?-5:f==6?-14:0;
  assert(syscall_window_create((uint64_t)(uintptr_t)&create_args,0,0,0,0,0)==want);
  assert(unmaps==(f>=4) && gets==(f==0 || f>=3));
  bool detached=m && (f==0 || f>=s);
  cleanup_vm(detached,m);
  if(!f){assert(active->refs==2 && scene && handle_owned && create_args.buffer_size==8192 && create_args.address==0x20000);handle_close(&p.handles,7);}
  assert(window_destroyed==(f!=1) && !scene && !handle_owned);++cases;
 }
 for(unsigned caption_height=1;caption_height<256;++caption_height){
  setup(0,0,3);
  create_args.flags=1|OS_WINDOW_CLIENT_DECORATIONS|OS_WINDOW_STANDARD_CAPTION_CONTROLS|
   OS_WINDOW_DARK_CAPTION|(caption_height<<16U);
  bool valid=caption_height>=40&&caption_height<=128;
  assert(syscall_window_create((uint64_t)(uintptr_t)&create_args,0,0,0,0,0)==(valid?0:K_EINVAL));
  if(valid){assert(active&&scene&&handle_owned);handle_close(&p.handles,7);}
  else assert(!active&&!gets&&!scene&&!handle_owned);
  cleanup_vm(false,0);
 }
 for(unsigned invalid=0;invalid<3;++invalid){
  setup(0,0,3);create_args.flags=1|(46U<<16U);
  if(invalid==1){create_args.flags|=OS_WINDOW_STANDARD_CAPTION_CONTROLS;create_args.height=45;}
  if(invalid==2)create_args.flags|=OS_WINDOW_STANDARD_CAPTION_CONTROLS|0x80000000U;
  assert(syscall_window_create((uint64_t)(uintptr_t)&create_args,0,0,0,0,0)==K_EINVAL&&!active&&!gets);
  cleanup_vm(false,0);
 }
 mapping_call=true;
 const unsigned failures[]={0,3,6};
 for(unsigned a=0;a<3;++a)for(unsigned m=0;m<3;++m)for(unsigned s=3;s<=6;++s){
  unsigned f=failures[a];setup(f,m,s);active=calloc(1,sizeof(*active));assert(active);active->refs=1;scene=true;
  int want=f==3?-12:f==6?-14:0;
  assert(syscall_window_map((uint64_t)(uintptr_t)&map_args,0,0,0,0,0)==want);
  assert(unmaps==(f==6) && gets==1 && active->refs==1 && scene);
  cleanup_vm(m && (s==3 || (s==6 && f!=3)),m);
  window_server_close_owned(&p,active);assert(window_destroyed==1);++cases;
 }
 setup(0,0,3);active=calloc(1,sizeof(*active));assert(active);active->refs=1;scene=true;
 map_args.length=1024;
 assert(syscall_window_map((uint64_t)(uintptr_t)&map_args,0,0,0,0,0)==K_EINVAL && !gets && active->refs==1);
 map_args.length=0;map_args.hdr=0;
 assert(syscall_window_map((uint64_t)(uintptr_t)&map_args,0,0,0,0,0)==K_EINVAL && !gets && active->refs==1);
 map_args.hdr=1;manager=false;
 assert(syscall_window_map((uint64_t)(uintptr_t)&map_args,0,0,0,0,0)==K_EPERM && !gets && active->refs==1);
 cleanup_vm(false,0);window_server_close_owned(&p,active);
 setup(0,0,3);create_args.hdr=0;
 assert(syscall_window_create((uint64_t)(uintptr_t)&create_args,0,0,0,0,0)==K_EINVAL && !gets && !active);
 cleanup_vm(false,0);
 assert(cases==120);return 0;
}
'''
with tempfile.TemporaryDirectory() as directory:
    directory = Path(directory)
    def run(name, creator=create, mapper=mapping, pin=helper, harness=fixture, trace=0):
        path = directory / (name + '.c')
        path.write_text(harness.replace('@HELPER@', pin).replace('@CREATE@', creator).replace('@MAP@', mapper).replace('@CAPTION@', caption))
        binary = directory / name
        subprocess.run(['cc', '-std=c11', '-fsanitize=address,undefined', '-fno-pie',
                        '-no-pie', f'-DOS_ENABLE_FRAME_TRACE={trace}', str(path), '-o', str(binary)], check=True)
        return subprocess.run([str(binary)], capture_output=True, text=True)
    for trace in (0, 1):
        result = run('native'+str(trace), trace=trace)
        assert result.returncode == 0, result.stderr
    result = run('no_create_rollback', creator=create.replace('if (mapped) (void)vm_unmap(space, arguments.address, (size_t)buffer_size);', ''))
    assert result.returncode != 0, result.stderr
    result = run('no_map_rollback', mapper=mapping.replace('(void)vm_unmap(space, arguments.address, (size_t)arguments.length);', '(void)0;'))
    assert result.returncode != 0, result.stderr
    result = run('wrong_vm', creator=create.replace('vm_unmap(space,', 'vm_unmap(process->vm,'))
    assert result.returncode != 0, result.stderr
    result = run('unregistered_leak', creator=create.replace('(void)window_server_close_owned(process, window);', ''))
    assert result.returncode != 0, result.stderr
    no_metric_check = create.replace('!os_window_caption_create_valid(arguments.flags, arguments.height) ||', '')
    assert no_metric_check != create
    result = run('unvalidated_metric', creator=no_metric_check)
    assert result.returncode != 0, result.stderr
    no_metric_mask = create.replace('OS_WINDOW_CAPTION_HEIGHT_MASK', '0U')
    assert no_metric_mask != create
    result = run('metric_rejected', creator=no_metric_mask)
    assert result.returncode != 0, result.stderr
print('Native window cleanup: 120 rollback and 258 real create-metric cases per trace mode passed; rollback/ownership and missing/rejecting metric validation negatives rejected')
