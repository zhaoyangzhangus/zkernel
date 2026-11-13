"""Actual futex once publication and private/shared key resolution paths."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'kernel/sync/futex.c').read_text()
def function(name):
    start = source.index(name)
    return source[start:source.index('\n}', start) + 2]
layout = source[source.index('#define FUTEX_BUCKET_COUNT'):source.index('static void futex_bucket_lock(')]
initializer = function('static void futex_init(').replace(
    'atomic_compare_exchange_strong_explicit(&g_futex_init_state',
    'counted_cas(&g_futex_init_state')
resolver = function('static kstatus_t futex_resolve_key(')
program = r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include <sched.h>
#include <kernel/wait.h>
#define __user
#define X86_64_USER_TOP 0x00007fffffffffffULL
#define VM_PROT_READ 1U
typedef struct vm_space { paddr_t root_table; } vm_space_t;
typedef struct process { vm_space_t *vm; } process_t;
typedef struct { vaddr_t address;uint32_t access;uint64_t cpu_error; } vm_fault_info_t;
''' + layout + r'''
static atomic_uint attempts;
static bool parallel=true;
static bool counted_cas(atomic_uint *p,unsigned *expected,unsigned value,
                        memory_order success,memory_order failure) {
 atomic_fetch_add(&attempts,1);
 if(parallel)while(atomic_load(&attempts)<8)sched_yield();
 return atomic_compare_exchange_strong_explicit(p,expected,value,success,failure);
}
''' + initializer + r'''
static unsigned reads,translations,faults;
static kstatus_t first_status,second_status,fault_status;
static vaddr_t expected_address;
static vm_space_t *expected_space;
static uint64_t physical_word;
static kstatus_t get_user_u32(uint32_t *value,const uint32_t *address) {
 assert((vaddr_t)address==expected_address);++reads;*value=17;
 return reads==1?first_status:second_status;
}
static kstatus_t x86_translate_page(paddr_t root,vaddr_t address,paddr_t *physical,void *flags) {
 assert(root.value==expected_space->root_table.value && address==expected_address && !flags);
 ++translations;physical->value=physical_word;
 return translations==1?first_status:second_status;
}
static kstatus_t vm_handle_fault(vm_space_t *space,const vm_fault_info_t *info) {
 assert(space==expected_space && info->address==expected_address);
 assert(info->access==VM_PROT_READ && !info->cpu_error);++faults;return fault_status;
}
''' + resolver + r'''
static void verify_init(void) {
 assert(atomic_load(&g_futex_init_state)==2);
 for(unsigned i=0;i<FUTEX_BUCKET_COUNT;++i) {
  assert(!atomic_load(&g_futex_buckets[i].lock.state));
  assert(g_futex_buckets[i].entries.next==&g_futex_buckets[i].entries);
  assert(g_futex_buckets[i].entries.prev==&g_futex_buckets[i].entries);
 }
}
static void *initialize(void *unused) {
 (void)unused;futex_init();verify_init();return 0;
}
int main(void) {
 memset(g_futex_buckets,0x75,sizeof(g_futex_buckets));
 pthread_t threads[8];
 for(unsigned i=0;i<8;++i)assert(!pthread_create(&threads[i],0,initialize,0));
 for(unsigned i=0;i<8;++i)assert(!pthread_join(threads[i],0));
 assert(atomic_load(&attempts)==8);parallel=false;
 list_head_t marker;list_init(&marker);list_add_tail(&g_futex_buckets[17].entries,&marker);
 atomic_store(&g_futex_buckets[42].lock.state,1);
 for(unsigned i=0;i<100000;++i)futex_init();
 assert(atomic_load(&attempts)==8 && atomic_load(&g_futex_buckets[42].lock.state)==1);
 assert(g_futex_buckets[17].entries.next==&marker);
 list_del(&marker);atomic_store(&g_futex_buckets[42].lock.state,0);verify_init();
 const vaddr_t addresses[]={FUTEX_USER_BASE,FUTEX_USER_BASE+4,0x12345ffc,FUTEX_USER_END-4};
 vm_space_t space={.root_table={0x1000}};process_t process={&space};expected_space=&space;
 unsigned cases=0;
 for(unsigned a=0;a<sizeof(addresses)/sizeof(*addresses);++a)
 for(unsigned private=0;private<2;++private)
 for(unsigned scenario=0;scenario<4;++scenario) {
  expected_address=addresses[a];physical_word=0xabcdef000ULL+(addresses[a]&4095);
  reads=translations=faults=0;
  first_status=scenario?K_EACCES:K_OK;
  fault_status=scenario==2?K_ENOMEM:K_OK;
  second_status=scenario==3?K_EACCES:K_OK;
  futex_key_t key={.value=99,.space=&space,.private_key=true};
  kstatus_t expected=scenario==2?K_ENOMEM:scenario==3?K_EACCES:K_OK;
  assert(futex_resolve_key(&process,expected_address,private,&key)==expected);
  assert(faults==(scenario?1U:0U));
  unsigned calls=scenario==1 || scenario==3?2:1;
  assert(reads==(private?calls:0) && translations==(private?0:calls));
  if(expected==K_OK) {
   assert(key.value==(private?expected_address:physical_word));
   assert(key.space==(private?&space:0) && key.private_key==(bool)private);
  } else assert(key.value==99 && key.space==&space && key.private_key);
  ++cases;
 }
 for(unsigned private=0;private<2;++private) {
  futex_key_t key={.value=99,.space=&space,.private_key=true};
  reads=translations=faults=0;process_t no_vm={0};
  assert(futex_resolve_key(0,FUTEX_USER_BASE,private,&key)==K_EACCES);
  assert(futex_resolve_key(&no_vm,FUTEX_USER_BASE,private,&key)==K_EACCES);
  assert(futex_resolve_key(&process,FUTEX_USER_BASE,private,0)==K_EACCES);
  assert(futex_resolve_key(&process,FUTEX_USER_BASE-4,private,&key)==K_EACCES);
  assert(futex_resolve_key(&process,FUTEX_USER_END,private,&key)==K_EACCES);
  assert(futex_resolve_key(&process,UINTPTR_MAX,private,&key)==K_EACCES);
  assert(!reads && !translations && !faults && key.value==99);
 }
 assert(cases==32);
 puts("PASS: futex eight initializers, 100000 no-CAS fast calls without bucket reset, 32 private/shared key and fault cases, invalid address/output isolation");
}
'''
with tempfile.TemporaryDirectory(prefix='liteos-futex-key-') as directory:
    binary = str(Path(directory) / 'test')
    subprocess.run(['cc', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                    '-pthread', '-I' + str(root / 'include'),
                    '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
                    '-x', 'c', '-', '-o', binary], input=program, text=True, check=True)
    subprocess.run([binary], check=True, timeout=30)
