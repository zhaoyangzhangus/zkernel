"""缩放标题栏不得重叠；绘制和点击共用生产控件几何。"""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
program = r'''
#include <assert.h>
#include <stdio.h>
#include <kernel/control_tree.h>
#include <uapi/window.h>
#include <uapi/chrome_caption.h>
#include "user/desktop/fileman/caption_layout.h"
int main(void) {
 unsigned cases=0;
 for(unsigned caption=40;caption<=128;++caption)for(unsigned width=100;width<=2200;width+=11) {
  os_control_node_t n[8]={{.identifier=1,.parent=OS_CONTROL_NO_PARENT,.kind=OS_CONTROL_CONTAINER}};
  for(unsigned i=1;i<8;++i)n[i]=(os_control_node_t){.identifier=i+1,.parent=0,.kind=OS_CONTROL_BUTTON};
  fm4_caption_layout(n+1,width,caption,198U*(caption-24U)/16U);
  control_tree_box_t b[8];Rect view={0,0,(int)width,(int)caption};
  assert(control_tree_layout(n,8,0,view,b));
  int font=caption-24;if(font>96)font=96;
  int right=(int)width-3*(int)os_window_caption_button_width(OS_WINDOW_CAPTION_HEIGHT(caption));
  int previous=198*font/16;
  for(unsigned i=1;i<8;++i) {
   if(n[i].flags&OS_CONTROL_HIDDEN)continue;
   assert(n[i].x>=previous && n[i].x+n[i].width<=right && n[i].width>0);
   previous=n[i].x+n[i].width;
   int x=n[i].x+n[i].width/2,y=n[i].y+n[i].height/2;
   assert(control_tree_hit_test(n,8,b,x,y)==i);
   assert(control_tree_locate(n,8,view,x,y,0)==i);
  }
  ++cases;
 }
 {
  os_control_node_t n[7]={0};
  fm4_caption_layout(n,1280U,76U,198U*52U/16U);
  assert(!(n[0].flags&OS_CONTROL_HIDDEN) && !(n[1].flags&OS_CONTROL_HIDDEN));
  assert(!(n[2].flags&OS_CONTROL_HIDDEN) && !(n[6].flags&OS_CONTROL_HIDDEN));
  assert(n[3].flags&OS_CONTROL_HIDDEN);
 }
 /* 归一化圆形覆盖率对称；四角透明，圆心实心，不允许缩放后矩形填充。 */
 for(unsigned d=20;d<=100;++d) {
  assert(!os_window_caption_circle_alpha(d,0,0));
  assert(os_window_caption_circle_alpha(d,d/2,d/2)==255);
  for(unsigned y=0;y<d;++y)for(unsigned x=0;x<d;++x) {
   unsigned a=os_window_caption_circle_alpha(d,x,y);
   assert(a==os_window_caption_circle_alpha(d,d-1-x,y));
   assert(a==os_window_caption_circle_alpha(d,y,x));
  }
 }
 printf("Caption: %u font/window sizes, non-overlap, exact hits and circular hover PASS\n",cases);
}
'''
with tempfile.TemporaryDirectory(prefix='fileman-caption-') as directory:
    path = Path(directory)
    (path/'test.c').write_text(program)
    subprocess.run(['cc','-std=c11','-O2','-Wall','-Wextra','-Werror',
                    '-fsanitize=address,undefined','-fno-pie','-no-pie',
                    '-I'+str(root/'include'),'-I'+str(root),str(path/'test.c'),
                    str(root/'kernel/graphics/control_tree.c'),'-o',str(path/'test')],check=True)
    subprocess.run([str(path/'test')],check=True)
