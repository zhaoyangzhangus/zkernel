"""Actual exec cleanup transfers exactly one reference, never onto input work."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root/'kernel/process/exec.c').read_text()
start = source.index('static void exec_release_old_space_work(')
actual = source[start:source.index('\nenum {', start)]
program = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
typedef struct {unsigned puts;} vm_space_t;
static bool accept;
static unsigned queued,executed;
static void (*work)(void *);
static void *argument;
static bool deferred_background_schedule(void (*fn)(void *),void *arg){
 if(!accept)return false;
 assert(!work);work=fn;argument=arg;++queued;return true;
}
static void vm_space_put(vm_space_t *space){assert(space&&!space->puts);++space->puts;++executed;}
''' + actual + r'''
int main(void){
 exec_release_old_space(0);assert(!queued&&!executed);
 for(unsigned i=0;i<2000;++i){
  vm_space_t space={0};accept=i&1;
  unsigned before=executed;
  exec_release_old_space(&space);
  if(accept){
   assert(!space.puts&&executed==before&&argument==&space&&work);
   work(argument);work=0;assert(space.puts==1&&executed==before+1);
  }else assert(space.puts==1&&!work&&executed==before+1);
 }
 assert(queued==1000&&executed==2000);
}
'''
with tempfile.TemporaryDirectory(prefix='exec-background-release-') as directory:
    binary=str(Path(directory)/'test')
    subprocess.run(['cc','-std=c11','-O2','-Wall','-Wextra','-Werror',
                    '-fsanitize=address,undefined','-fno-pie','-no-pie',
                    '-x','c','-','-o',binary],input=program,text=True,check=True)
    subprocess.run([binary],check=True,timeout=15)
print('Exec cleanup: 2000 accepted/rejected ownership transfers, delayed release, null input and synchronous fallback passed')
