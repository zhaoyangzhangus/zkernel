"""Actual current fault entry retains one VM for resolution/diagnostics/prefetch."""
from pathlib import Path
import subprocess
import tempfile
root = Path(__file__).resolve().parents[2]
source = (root/'kernel/mm/fault.c').read_text()
body = source[source.index('bool vm_handle_current_fault('):source.index('static kstatus_t vm_handle_fault_impl(', source.index('bool vm_handle_current_fault('))]
selector = source[source.index('static bool vm_area_fault_around_eligible('):source.index('\n#if VM_FAULT_TELEMETRY_ENABLED', source.index('static bool vm_area_fault_around_eligible('))]
internal = (root/'kernel/process/internal.h').read_text()
helper = internal[internal.index('static inline vm_space_t *process_vm_reference('):]
fixture = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#define VM_USER_BASE 0x10000ULL
#define VM_USER_END 0x800000000000ULL
#define KOBJECT_TYPE_THREAD 3
#define K_OK 0
#define VM_PROT_READ 1
#define VM_PROT_WRITE 2
#define VM_PROT_EXEC 4
#define VM_OBJECT_FILE 7
#define VM_OBJECT_SHARED 8
#define VM_MAP_STACK 1
#define VM_OBJECT_FLAG_WINDOW_SURFACE 16
typedef uint64_t vaddr_t;
typedef int kstatus_t;
typedef struct {unsigned type,flags;union {struct {uint64_t file_offset;} file;} u;} vm_object_t;
typedef struct {uint64_t start,end,object_offset;unsigned prot,flags;vm_object_t *object;} vm_area_t;
typedef struct {unsigned refs;} vm_space_t;
typedef struct {vm_space_t *vm;uint64_t pid,linux_thread_group_leader_tid;} process_t;
typedef struct {struct {unsigned type;} object;process_t *process;uint64_t tid;} thread_t;
typedef struct {vaddr_t address;unsigned access,cpu_error;} vm_fault_info_t;
typedef struct {uint64_t start_tsc,end_tsc;} vm_fault_trace_t;
static process_t p;
static thread_t t;
static vm_space_t *expected;
static bool locked,map_locked,present;
static unsigned mode,resolves,diagnostics,prefetches,destroyed,gets,records,telemetry;
static int status;
static uint64_t g_vm_slow_fault_process=100;
static vm_object_t object={.type=7,.u.file.file_offset=8192};
static vm_area_t area={.start=0x10000,.end=0x20000,.object_offset=4096,.prot=3,.object=&object};
static vm_space_t *allocate(void){vm_space_t *v=calloc(1,sizeof(*v));assert(v);v->refs=1;return v;}
static void process_thread_lock(process_t *v){assert(v==&p && !locked && !map_locked);locked=true;}
static void process_thread_unlock(process_t *v){assert(v==&p && locked);locked=false;}
static void vm_space_get(vm_space_t *v){assert(locked && v->refs);++v->refs;++gets;}
static void vm_space_put(vm_space_t *v){assert(!locked && !map_locked);if(v && !--v->refs){++destroyed;free(v);}}
@HELPER@
static thread_t *sched_current_thread(void){return &t;}
static uint64_t x86_read_tsc(void){return 100;}
static uint64_t telemetry_timestamp(void){return 100;}
static void vm_fault_telemetry_record(const vm_fault_info_t *f,bool user,vm_fault_trace_t *trace){(void)user;assert(f && trace->start_tsc==100 && trace->end_tsc==100);++telemetry;}
@SELECTOR@
static int vm_handle_fault_impl(vm_space_t *v,const vm_fault_info_t *f,vm_fault_trace_t *trace,bool *surface_hint){
 assert(surface_hint);*surface_hint=vm_area_fault_around_eligible(&area);
 (void)trace;assert(!locked && !map_locked && v==expected && v->refs && f->address==0x12345);
 unsigned want=(f->cpu_error&16)?4:(f->cpu_error&2)?2:1;assert(f->access==want);++resolves;
 if(mode){process_thread_lock(&p);p.vm=mode==1?NULL:allocate();process_thread_unlock(&p);vm_space_put(v);}
 assert(v->refs);return status;
}
static void map_lock(vm_space_t *v){assert(!locked && !map_locked && v==expected && v->refs);map_locked=true;}
static void map_unlock(vm_space_t *v){assert(map_locked && v==expected && v->refs);map_locked=false;}
static vm_area_t *find_area(vm_space_t *v,uint64_t address){assert(map_locked && v==expected && address==0x12345);return present?&area:NULL;}
static void vm_fault_diag_emit(thread_t *thread,vm_fault_info_t *f,unsigned error,int result,vm_area_t *snapshot,uint64_t offset,unsigned type){
 assert(!locked && !map_locked && thread==&t && f->address==0x12345 && error==f->cpu_error && result==status);
 if(present)assert(snapshot && snapshot->start==area.start && snapshot->end==area.end && snapshot->prot==area.prot && offset==(object.type==VM_OBJECT_FILE?8192+4096+0x2345:0) && type==object.type);
 else assert(!snapshot && !offset && !type);
 ++diagnostics;
}
static void vm_fault_around(vm_space_t *v,vm_fault_info_t *f){assert(!locked && !map_locked && v==expected && v->refs && f->address==0x12345);++prefetches;}
static void vm_record_slow_fault(uint64_t pid,uint64_t tid,uint64_t address,unsigned error,uint64_t start,uint64_t resolved,uint64_t end){(void)error;assert(pid==p.pid && tid==t.tid && address==0x12345 && start==100 && resolved==100 && end==100);++records;}
@BODY@
static void setup(void){p.vm=allocate();expected=p.vm;p.pid=100;p.linux_thread_group_leader_tid=2;t=(thread_t){.object={3},.process=&p,.tid=2};locked=map_locked=false;resolves=diagnostics=prefetches=destroyed=gets=records=telemetry=0;}
int main(void){
 unsigned cases=0;
 for(unsigned user=0;user<2;++user)for(unsigned access=0;access<2;++access)
 for(unsigned error=0;error<32;++error)for(unsigned fail=0;fail<2;++fail)
 for(unsigned m=0;m<3;++m)for(unsigned exists=0;exists<2;++exists)
 for(unsigned surface=0;surface<2;++surface){
  setup();object.type=surface?VM_OBJECT_SHARED:VM_OBJECT_FILE;object.flags=surface?VM_OBJECT_FLAG_WINDOW_SURFACE:0;mode=m;status=fail?-5:0;present=exists;bool valid=user || access;
  assert(vm_handle_current_fault(0x12345,error,user,access)==(valid && !fail));
  assert(gets==valid && resolves==valid && diagnostics==(valid && user && fail) && prefetches==(valid && !fail && surface && !(error&1)));
  assert(records==valid && telemetry==(valid && VM_FAULT_TELEMETRY_ENABLED));
  bool detached=valid && m;assert(destroyed==detached);
  if(p.vm){assert(p.vm->refs==1);vm_space_put(p.vm);}
  assert(destroyed==(detached && m==2?2:1) && !locked && !map_locked);++cases;
 }
 setup();assert(!vm_handle_current_fault(1,0,true,false) && !gets);
 t.object.type=0;assert(!vm_handle_current_fault(0x12345,0,true,false) && !gets);
 t.object.type=3;t.process=NULL;assert(!vm_handle_current_fault(0x12345,0,true,false) && !gets);
 vm_space_put(p.vm);p.vm=NULL;t.process=&p;
 assert(!vm_handle_current_fault(0x12345,0,true,false) && !gets);
 assert(cases==3072);return 0;
}
'''
with tempfile.TemporaryDirectory() as directory:
    directory = Path(directory)
    def run(name, implementation=body, pin=helper, harness=fixture, trace=0):
        path = directory/(name+'.c')
        path.write_text(harness.replace('@SELECTOR@',selector).replace('@HELPER@',pin).replace('@BODY@',implementation))
        binary=directory/name
        subprocess.run(['cc','-std=c11','-fsanitize=address,undefined','-fno-pie','-no-pie',f'-DVM_FAULT_TELEMETRY_ENABLED={trace}',str(path),'-o',str(binary)],check=True)
        return subprocess.run([str(binary)],capture_output=True,text=True)
    for trace in (0,1):
        result=run('fault'+str(trace),trace=trace)
        assert result.returncode==0,result.stderr
    result=run('borrowed',pin=helper.replace('if (space != 0) vm_space_get(space);',''),harness=fixture.replace('unsigned m=0;','unsigned m=1;'))
    assert result.returncode!=0 and 'heap-use-after-free' in result.stderr,result.stderr
    result=run('wrong_prefetch',body.replace('vm_fault_around(space,','vm_fault_around(thread->process->vm,'))
    assert result.returncode!=0,result.stderr
    result=run('ordinary_prefetch',body.replace('status == K_OK && surface_hint &&','status == K_OK &&'))
    assert result.returncode!=0,result.stderr
    result=run('wrong_diagnostic',body.replace('map_lock(space);','map_lock(thread->process->vm);'))
    assert result.returncode!=0,result.stderr
print('Current fault snapshot: 3072 ordinary/surface/user/UAccess/error/status/exit/exec/area cases per telemetry mode, exact access/diagnostics/prefetch and balanced VM ownership passed; missing pin/UAF and wrong diagnostic/prefetch VM negatives rejected')
