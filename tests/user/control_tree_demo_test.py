"""演示程序缩放：按最新客户端尺寸呈现，仅对已确认的尺寸竞态延后重绘。"""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root/'tests/user/control_tree_demo.c').read_text()
body = source[source.index('static int demo_repaint('):source.index('__attribute__((used))')]
program = r'''
#include <assert.h>
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <uapi/linux_abi.h>
#include <uapi/window_state.h>
static unsigned width=12,height=10,queries,presents,cancels;
static bool race,hard,query_fail,small;
static uint32_t pixels[512+2];
static int64_t call(uint64_t n,uint64_t fd,uint64_t request,uint64_t arg){
 assert(n==LINUX_SYS_IOCTL&&fd==9&&request==LINUX_WINDOW_IOCTL_SURFACE);
 os_surface_command_t *c=(void*)(uintptr_t)arg;
 if(c->operation==OS_SURFACE_BEGIN){
  ++queries;if(query_fail)return -1;
  c->frame=(os_surface_frame_t){.token=1,.address=(uintptr_t)(pixels+1),.width=width,
   .height=height,.stride=width*4,.capacity=small?4:512*4};return 0;
 }
 if(c->operation==OS_SURFACE_CANCEL){++cancels;return 0;}
 assert(c->operation==OS_SURFACE_COMMIT);++presents;
 assert(c->frame.width==width&&c->frame.height==height);
 if(race){race=false;width=16;height=12;return -11;}
 return hard?-22:0;
}
''' + body + r'''
int main(void){
 for(unsigned i=0;i<514;++i)pixels[i]=0xdeadbeef;
 linux_window_create_t window={.buffer_size=512*4,.address=0x1000};
 race=true;assert(demo_repaint(9,&window,0)==1&&queries==1&&presents==1);
 assert(demo_repaint(9,&window,0)==0&&queries==2&&presents==2);
 assert(window.width==16&&window.height==12&&pixels[0]==0xdeadbeef&&pixels[513]==0xdeadbeef);
 for(unsigned i=1;i<=16*12;++i)assert(pixels[i]==0x438fc0);
 hard=true;assert(demo_repaint(9,&window,0)==-1&&cancels==1);
 query_fail=true;assert(demo_repaint(9,&window,0)==-1&&cancels==1);
 query_fail=false;width=100;height=100;unsigned before=presents;
 assert(demo_repaint(9,&window,0)==-1&&presents==before&&cancels==2);
 width=0;assert(demo_repaint(9,&window,0)==-1&&cancels==3);
 width=16;height=12;small=true;assert(demo_repaint(9,&window,1)==-1&&cancels==4);
 small=false;hard=false;assert(demo_repaint(9,&window,1)==0);
 assert(pixels[1+8*16+8]==0xe85d3f&&pixels[0]==0xdeadbeef&&pixels[513]==0xdeadbeef);
 puts("Control demo resize: back-slot-only writes, atomic commit, resize retry, error cancellation, patch persistence and buffer guards PASS");
}
'''
with tempfile.TemporaryDirectory(prefix='control-demo-resize-') as directory:
    path = Path(directory)
    (path/'test.c').write_text(program)
    subprocess.run(['cc','-std=c11','-O2','-Wall','-Wextra','-Werror',
                    '-fsanitize=address,undefined','-fno-pie','-no-pie',
                    '-I'+str(root/'include'),str(path/'test.c'),'-o',str(path/'test')],check=True)
    subprocess.run([str(path/'test')],check=True)
    # 编译真实事件循环：移动事件洪流中不得逐条休眠，Apply/Reset 必须及时且仅执行一次。
    loop = source[source.index('__attribute__((used))'):source.index('__attribute__((naked,noreturn))')]
    pump = r'''
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <uapi/linux_abi.h>
#include <uapi/window.h>
#include <uapi/window_state.h>
#include <uapi/control_tree.h>
#include <uapi/window_theme.h>
#include <uapi/syscall.h>
static uint32_t pixels[720*420];
static unsigned reads,waits,submits,patches,closes,mode;
static os_window_control_tree_t *current_tree;
static int64_t call(uint64_t n,uint64_t a,uint64_t b,uint64_t c){
 if(n==LINUX_SYS_WINDOW_CREATE){
  linux_window_create_t *w=(void*)(uintptr_t)a;
  w->address=(uintptr_t)pixels;w->buffer_size=sizeof(pixels);w->identifier=7;return 9;
 }
 if(n==(OS_SYSCALL_NATIVE_FLAG|OS_SYS_WINDOW_CONTROL_TREE)){
  os_window_control_tree_t *r=(void*)(uintptr_t)a;
  os_control_node_t *nodes=(void*)(uintptr_t)r->nodes;
  if(nodes[3].parent==3)return -22;
  current_tree=r;r->revision=submits+1U;
  if(++submits==2){const char *t=(void*)(uintptr_t)r->text;assert(t[37]=='D'&&t[41]=='1');}
  assert(r->count==20);
  if(submits==3){
   if(mode>=7&&mode<=9)assert(nodes[mode].flags&OS_CONTROL_CHECKED);
   if(mode==10||mode==11){assert(nodes[mode+2].value==7500&&nodes[11].value==7500);}
   if(mode==12||mode==13)assert(nodes[mode+3].flags&OS_CONTROL_CHECKED);
  }
  return 0;
 }
 if(n==LINUX_SYS_READ){
  assert(a==9&&c==sizeof(os_window_event_t));
  os_window_event_t *e=(void*)(uintptr_t)b;*e=(os_window_event_t){0};
  unsigned index=reads++;
  if(index==1003)return mode==3?-4:mode==4?-5:-11;
  if(index==1004){e->type=OS_WINDOW_EVENT_CLOSE_REQUEST;return sizeof(*e);}
  e->type=OS_WINDOW_EVENT_CONTROL;
  e->control.action=index<1000?OS_CONTROL_MOVE:OS_CONTROL_CLICK;
  e->control.target=index==1001?7:4;
  /* 第三次是普通悬停，不应再次激活按钮。 */
  if(index==1002)e->control.action=OS_CONTROL_ENTER;
  if(index==1002&&mode>=7){
   static const unsigned targets[]={10,11,12,15,16,18,19};
   e->control.target=targets[mode-7];e->control.value=mode>=10?7500:1;
   e->control.action=mode>=12?OS_CONTROL_CLICK:OS_CONTROL_CHANGE;
  }
  os_control_node_t *nodes=(void*)(uintptr_t)current_tree->nodes;
  for(unsigned i=0;i<current_tree->count;++i)if(nodes[i].identifier==e->control.target){
   e->control.node_index=i;e->control.node_kind=nodes[i].kind;
   e->control.tree_revision=current_tree->revision;e->control.user_data=nodes[i].user_data;break;
  }
  return sizeof(*e);
 }
 if(n==LINUX_SYS_IOCTL){
  assert(a==9&&b==LINUX_WINDOW_IOCTL_SURFACE);
  os_surface_command_t *command=(void*)(uintptr_t)c;
  if(command->operation==OS_SURFACE_BEGIN){
   command->frame=(os_surface_frame_t){.token=1,.address=(uintptr_t)pixels,.capacity=sizeof(pixels),
    .width=720,.height=420,.stride=720*4};return 0;
  }
  assert(command->operation==OS_SURFACE_COMMIT);
  assert(pixels[8*720+8]==(reads<1002?0x438fc0U:0xe85d3fU));++patches;
  return mode==1&&reads>=1002?-11:0;
 }
 if(n==LINUX_SYS_POLL){
  struct {int32_t fd;int16_t events,revents;} *wait=(void*)(uintptr_t)a;
  assert(reads==1004&&b==1&&wait->fd==9&&wait->events==LINUX_POLLIN);
  assert(c==(mode==1?16U:UINT64_MAX));++waits;
  if(mode==2)return -22;
  if(mode==5){wait->revents=LINUX_POLLHUP;return 1;}
  if(mode==6)return -4;
  wait->revents=LINUX_POLLIN;return 1;
 }
 if(n==LINUX_SYS_CLOSE){assert(a==9);++closes;return 0;}
 assert(0);return -1;
}
''' + body + loop + r'''
int main(void){
 for(mode=0;mode<14;++mode){
  reads=waits=submits=patches=closes=0;
  assert(demo_main()==(mode==2||mode==4||mode==5?5:0));
  assert(submits==(mode>=7?3U:2U)&&patches==(mode==3||mode==4?1U:2U));
  assert(waits==(mode==3||mode==4?0U:1U));
  assert(reads==(mode==2||mode==4||mode==5?1004U:1005U));
  assert(closes==(mode==2||mode==4||mode==5?0U:1U));
 }
 puts("Control demo event loop: motion drain, Apply/Reset, choice/range/list/tab updates, blocking poll, resize retry, interruption/errors and close PASS");
}
'''
    (path/'test.c').write_text(pump)
    subprocess.run(['cc','-std=c11','-O2','-Wall','-Wextra','-Werror',
                    '-fsanitize=address,undefined','-fno-pie','-no-pie',
                    '-I'+str(root/'include'),str(path/'test.c'),'-o',str(path/'test')],check=True)
    subprocess.run([str(path/'test')],check=True)
