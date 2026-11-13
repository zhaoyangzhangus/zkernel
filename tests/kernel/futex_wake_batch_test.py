"""futex 唤醒入口：批量接口、数量上限、键选择和失败清理。"""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root/'kernel/sync/futex.c').read_text()
start = source.index('kstatus_t futex_wake_keyed(')
actual = source[start:source.index('\n}',start)+2]
code = r'''
#include <assert.h>
#include <stdio.h>
#include <kernel/wait.h>
#define __user
typedef struct { unsigned refs; } process_t;
typedef struct { wait_queue_t waitq; } futex_entry_t;
static futex_entry_t entry;
static process_t process;
static unsigned initialized, gets, puts_count, batches;
static kstatus_t get_status;
static bool expected_private;
static uint32_t maximum, available;
static void futex_init(void) { ++initialized; }
static kstatus_t futex_get(process_t *p,vaddr_t address,bool create,bool private_key,futex_entry_t **out) {
 assert(p==&process && address==0x10000 && !create && private_key==expected_private);
 ++gets;if(get_status==K_OK)*out=&entry;return get_status;
}
static void futex_put(futex_entry_t *p) { assert(p==&entry);++puts_count; }
uint32_t wake_n(wait_queue_t *q,uint32_t count) {
 assert(q==&entry.waitq && count==maximum);++batches;
 return available<count?available:count;
}
''' + actual + r'''
int main(void) {
 const uint32_t limits[]={0,1,2,15,16,17,1024,UINT32_MAX};
 const kstatus_t statuses[]={K_OK,K_ENOENT,K_EACCES,K_ENOMEM};
 for(unsigned private=0;private<2;++private)
 for(unsigned i=0;i<sizeof(limits)/sizeof(*limits);++i)
 for(unsigned j=0;j<sizeof(statuses)/sizeof(*statuses);++j)
 for(unsigned a=0;a<3;++a) {
  expected_private=private;maximum=limits[i];available=a==0?0:a==1?1:1024;
  initialized=gets=puts_count=batches=0;get_status=statuses[j];uint32_t woken=99;
  kstatus_t status=futex_wake_keyed(&process,(uint32_t *)0x10000,maximum,&woken,private);
  assert(initialized==1 && gets==(maximum?1U:0U));
  bool success=maximum && get_status==K_OK;
  assert(batches==(success?1U:0U) && puts_count==(success?1U:0U));
  assert(woken==(success?(available<maximum?available:maximum):0U));
  assert(status==(!maximum || get_status==K_ENOENT?K_OK:get_status));
 }
 uint32_t woken=77;initialized=gets=0;
 assert(futex_wake_keyed(0,(uint32_t *)0x10000,1,&woken,true)==K_EINVAL);
 assert(futex_wake_keyed(&process,0,1,&woken,true)==K_EINVAL);
 assert(futex_wake_keyed(&process,(uint32_t *)0x10001,1,&woken,true)==K_EINVAL);
 assert(futex_wake_keyed(&process,(uint32_t *)0x10000,1,0,true)==K_EINVAL);
 assert(!initialized && !gets && woken==77);
 puts("PASS: 192 futex wake cases use one bounded batch call; private/shared keys, zero maximum, absent queue, errors and entry release");
}
'''
with tempfile.TemporaryDirectory(prefix='futex-wake-batch-') as directory:
    path = Path(directory)
    (path/'test.c').write_text(code)
    subprocess.run(['cc','-std=c11','-O2','-Wall','-Wextra','-Werror',
                    '-I'+str(root/'include'),'-fsanitize=address,undefined',
                    '-fno-pie','-no-pie',str(path/'test.c'),'-o',str(path/'test')],check=True)
    subprocess.run([str(path/'test')],check=True,timeout=10)
