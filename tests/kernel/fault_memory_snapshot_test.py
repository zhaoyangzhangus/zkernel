"""Exercise actual fatal-fault formatting without UART or live guest access."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'kernel/mm/fault.c').read_text()
function = source[source.index('static void vm_fault_diag_emit('):
                  source.index('static void vm_write_repair_diag(')]
program = r'''
#include <assert.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef long kstatus_t;
typedef struct {uint64_t pid;} process_t;
typedef struct {process_t *process;} thread_t;
typedef struct {uint64_t address,access;} vm_fault_info_t;
typedef struct {uint64_t start,end,prot;} vm_area_t;
static atomic_flag g_vm_fault_diag_lock=ATOMIC_FLAG_INIT;
static char text[2048];static unsigned used,snapshots;
static kstatus_t stats_status;
static kstatus_t page_memory_get_stats(uint64_t *total,uint64_t *free){
 assert(!used);++snapshots;
 if(stats_status)return stats_status;
 *total=1048576;*free=524288;return 0;
}
static void vm_fault_diag_write_text(const char *s){
 assert(used+strlen(s)<sizeof(text));used+=sprintf(text+used,"%s",s);
}
static void vm_fault_diag_write_char(char c){assert(used+1<sizeof(text));text[used++]=c;text[used]=0;}
static void vm_fault_diag_write_hex(uint64_t n){
 assert(used+18<sizeof(text));used+=sprintf(text+used,"0x%016llx",(unsigned long long)n);
}
''' + function + r'''
int main(void){
 process_t process={251};thread_t thread={&process};
 vm_fault_info_t fault={0x12345000,2};vm_area_t area={0x12340000,0x12350000,11};
 for(unsigned unavailable=0;unavailable<2;++unavailable){
  used=0;text[0]=0;stats_status=unavailable?-1:0;
  vm_fault_diag_emit(&thread,&fault,6,-12,unavailable?0:&area,0,1);
  assert(snapshots==unavailable+1);
  assert(strstr(text,"OS_VM_USER_FAULT pid=0x00000000000000fb"));
  assert(strstr(text," status=0xfffffffffffffff4"));
  if(unavailable){
   assert(strstr(text," memory_status=0xffffffffffffffff total_pages=0x0000000000000000 free_pages=0x0000000000000000"));
   assert(strstr(text," area=0x0000000000000000-0x0000000000000000"));
  }else{
   assert(strstr(text," memory_status=0x0000000000000000 total_pages=0x0000000000100000 free_pages=0x0000000000080000"));
   assert(strstr(text," area=0x0000000012340000-0x0000000012350000"));
  }
  assert(!atomic_flag_test_and_set(&g_vm_fault_diag_lock));
  atomic_flag_clear(&g_vm_fault_diag_lock);
  assert(used>=2&&!strcmp(text+used-2,"\r\n"));
 }
}
'''
with tempfile.TemporaryDirectory(prefix='fault-memory-snapshot-') as directory:
    path = Path(directory)
    (path / 'test.c').write_text(program)
    subprocess.run(['cc', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
                    str(path / 'test.c'), '-o', str(path / 'test')], check=True)
    subprocess.run([str(path / 'test')], check=True)
print('Fatal fault snapshot: before-output memory sample, available/unavailable stats, original fault fields, null VMA and released diagnostic lock PASS')
