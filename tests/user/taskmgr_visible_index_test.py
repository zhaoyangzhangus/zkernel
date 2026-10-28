"""Compare the actual row index against uncached process/thread grouping."""
from pathlib import Path
import re
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root/'user/desktop/taskmgr/main.c').read_text()
begin = source.index('static void visible_index_rebuild(')
actual = source[begin:source.index('static const char *process_state_text(', begin)]
actual = actual.replace('g_visible_count = 0U;', '++builds; g_visible_count = 0U;')
actual = re.sub(r'g_entries\[((?:[^\[\]]|\[[^\[\]]*\])*)\]',r'(*entry_at(\1))',actual)
program = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define TASKMGR_MAX_ENTRIES 768U
typedef struct {bool thread,expanded;uint64_t id,parent_id;} entry_t;
static entry_t g_entries[TASKMGR_MAX_ENTRIES];
static uint32_t g_entry_count,g_visible_entries[TASKMGR_MAX_ENTRIES];
static uint32_t g_entry_rows[TASKMGR_MAX_ENTRIES],g_visible_count;
static bool g_visible_dirty=true;
static unsigned builds,visits;
static entry_t *entry_at(uint32_t i){assert(i<g_entry_count);++visits;return g_entries+i;}
''' + actual + r'''
static void check(void){
 uint32_t expected[768],rows[768],count=0;
 for(unsigned i=0;i<g_entry_count;++i)rows[i]=UINT32_MAX;
 for(unsigned i=0;i<g_entry_count;++i){
  if(g_entries[i].thread)continue;
  expected[count]=i;rows[i]=count++;
  if(!g_entries[i].expanded)continue;
  for(unsigned j=0;j<g_entry_count;++j)
   if(g_entries[j].thread&&g_entries[j].parent_id==g_entries[i].id&&rows[j]==UINT32_MAX){
    expected[count]=j;rows[j]=count++;
   }
 }
 g_visible_dirty=true;visits=0;unsigned previous=builds;
 assert(visible_count()==count && builds==previous+1);
 unsigned scans=visits;
 assert(scans<=12U*g_entry_count);
 for(unsigned round=0;round<10;++round){
  for(unsigned row=0;row<count;++row)assert(visible_entry(row)==expected[row]);
  assert(visible_entry(count)==g_entry_count && visible_entry(UINT32_MAX)==g_entry_count);
  for(unsigned i=0;i<g_entry_count;++i){
   assert(entry_visible(i)==(rows[i]!=UINT32_MAX));
   assert(entry_row(i)==(rows[i]!=UINT32_MAX?rows[i]:0));
  }
  assert(!entry_visible(g_entry_count)&&!entry_visible(UINT32_MAX));
  assert(entry_row(UINT32_MAX)==0 && visible_count()==count);
 }
 assert(visits==scans && builds==previous+1);
}
int main(void){
 unsigned rng=19,cases=0;
 for(unsigned mode=0;mode<5;++mode)for(unsigned layout=0;layout<200;++layout){
  unsigned parents=layout%65,threads=(layout*7)%513;
  g_entry_count=parents+threads;
  for(unsigned i=0;i<parents;++i)
   g_entries[i]=(entry_t){.id=100+i,.expanded=mode==0?false:mode==1?true:(i&1)};
  for(unsigned i=0;i<threads;++i)
   g_entries[parents+i]=(entry_t){.thread=true,.id=1000+i,
    .parent_id=parents?100+(i%(parents+1)):999}; // Include orphan threads.
  if(mode>=3)for(unsigned i=g_entry_count;i>1;--i){
   rng=rng*1664525U+1013904223U;unsigned j=rng%i;
   entry_t swap=g_entries[i-1];g_entries[i-1]=g_entries[j];g_entries[j]=swap;
  }
  check();++cases;
  for(unsigned i=0;i<g_entry_count;++i)if(!g_entries[i].thread)g_entries[i].expanded=!g_entries[i].expanded;
  check();++cases;
 }
 // Maximum production capacity: every process expanded, every thread visible.
 g_entry_count=768;
 for(unsigned i=0;i<256;++i)g_entries[i]=(entry_t){.id=i+1,.expanded=true};
 for(unsigned i=256;i<768;++i)g_entries[i]=(entry_t){.thread=true,.id=i+1,.parent_id=1+i%256};
 check();++cases;
 printf("Full 768-entry grouping: %u entry reads instead of 196608 nested candidates\n",visits);
 /* 重复父标识只归入首个展开节点；支持 64 位标识和线程早于父节点的乱序。 */
 g_entry_count=5;
 g_entries[0]=(entry_t){.thread=true,.id=99,.parent_id=UINT64_MAX};
 g_entries[1]=(entry_t){.id=UINT64_MAX,.expanded=false};
 g_entries[2]=(entry_t){.id=UINT64_MAX,.expanded=true};
 g_entries[3]=(entry_t){.id=UINT64_MAX,.expanded=true};
 g_entries[4]=(entry_t){.thread=true,.id=100,.parent_id=UINT64_MAX};
 check();++cases;
 g_entry_count=0;check();++cases;
 printf("Task row index: %u grouping/expansion/shuffle/orphan/full/empty oracles; bounded rebuild reads; ten repeated query passes scan no entries\n",cases);
}
'''
with tempfile.TemporaryDirectory(prefix='taskmgr-visible-index-') as directory:
    binary=str(Path(directory)/'test')
    subprocess.run(['cc','-std=c11','-O2','-Wall','-Wextra','-Werror',
                    '-fsanitize=address,undefined','-fno-pie','-no-pie',
                    '-x','c','-','-o',binary],input=program,text=True,check=True)
    subprocess.run([binary],check=True,timeout=30)
