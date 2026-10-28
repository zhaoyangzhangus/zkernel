"""文件界面实际应用回路：旧事件/伪造关联值不能触发行为，有效事件无需遍历控件。"""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root/'user/runtime/ui_file_demo.c').read_text()
main = source[source.index('int main('):].replace('int main(', 'static int demo_main(', 1)
program = r'''
#include <assert.h>
#include <stdio.h>
#include <uapi/window.h>
#include <uapi/window_theme.h>
#include <uapi/syscall.h>
#include "user/runtime/control_file.h"
static os_window_control_tree_t *tree;
static unsigned reads,submits,closes,mode,interrupted;
static os_control_node_t *named(const char *name){
 os_control_node_t *nodes=(void*)(uintptr_t)tree->nodes;
 for(unsigned i=0;i<tree->count;++i)if(nodes[i].identifier==ui_file_identifier(name))return nodes+i;
 assert(0);return 0;
}
static int64_t ui_call(uint64_t number,void *argument){
 if(number==OS_SYS_WINDOW_CREATE){
  os_window_create_t *w=argument;assert(w->hdr.size==sizeof(*w));w->identifier=77;w->window=777;return 0;
 }
 if(number==OS_SYS_WINDOW_CONTROL_TREE){
  tree=argument;assert(tree->identifier==77);++submits;
  if(mode==1)return -22;
  tree->revision=submits;
  assert(os_control_event_node(tree,(void*)(uintptr_t)tree->nodes,0)==0);
  if(submits==2){assert(named("apply")->background==OS_THEME_DANGER);
   assert(*(unsigned *)(uintptr_t)named("apply")->user_data==1);}
  if(submits==3)assert(named("check")->flags&OS_CONTROL_CHECKED);
  if(submits==4)assert(named("level")->value==7500&&named("progress")->value==7500);
  return 0;
 }
 if(number==OS_SYS_WINDOW_EVENT_READ){
  if(mode==2&&!interrupted++){return -4;}
  os_window_event_read_t *r=argument;assert(r->identifier==77&&r->hdr.size==sizeof(*r));
  assert(reads<6);os_control_node_t *nodes=(void*)(uintptr_t)tree->nodes;
  os_control_node_t *n=named(reads<3?"apply":reads==3?"check":reads==4?"level":"close");
  r->event.type=OS_WINDOW_EVENT_CONTROL;
  r->event.control=(os_control_event_t){.target=n->identifier,.node_index=(uint32_t)(n-nodes),
   .node_kind=n->kind,.tree_revision=tree->revision,.user_data=n->user_data,
   .action=reads==3||reads==4?OS_CONTROL_CHANGE:OS_CONTROL_CLICK,.value=reads==4?7500:1};
  if(reads==0)--r->event.control.tree_revision;
  if(reads==1)r->event.control.user_data=1;
  ++reads;return 0;
 }
 if(number==OS_SYS_HANDLE_CLOSE){assert((uintptr_t)argument==777);++closes;return 0;}
 assert(0);return -1;
}
''' + main + r'''
int main(int argc,char **argv){
 assert(argc==2);
 for(mode=0;mode<3;++mode){
  reads=submits=closes=interrupted=0;
  assert(demo_main(argc,argv)==(mode==1?3:0));
  assert(closes==1&&submits==(mode==1?1U:4U)&&reads==(mode==1?0U:6U));
 }
 puts("UI file application: actual file-to-window submission, direct event binding, stale/forged rejection, choice/range actions, interrupted wait and cleanup PASS");
}
'''
with tempfile.TemporaryDirectory(prefix='ui-file-demo-') as directory:
    path=Path(directory)
    (path/'test.c').write_text(program)
    subprocess.run(['cc','-std=c11','-D_POSIX_C_SOURCE=200809L','-O2','-Wall','-Wextra','-Werror',
                    '-fsanitize=address,undefined','-fno-pie','-no-pie',
                    '-I'+str(root),'-I'+str(root/'include'),str(path/'test.c'),
                    str(root/'user/runtime/control_file.c'),'-o',str(path/'test')],check=True)
    subprocess.run([str(path/'test'),str(root/'user/ui/controls.ui')],check=True)
