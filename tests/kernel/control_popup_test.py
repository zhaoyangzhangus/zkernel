"""公共弹层使用同一布局进行绘制、命中和焦点查询，应用快照不可被修改。"""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
program = r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <kernel/control_tree.h>
#include <uapi/window_theme.h>
static const os_control_node_t *source;
static unsigned painted;
static void paint(void *p,const os_control_node_t *n,const control_tree_box_t *b,Rect damage) {
 (void)p;(void)b;(void)damage;assert(n>=source&&n<source+5);++painted;
}
int main(void) {
 for(unsigned font=12;font<=48;font+=4)for(unsigned w=40;w<=700;w+=33) {
  os_control_node_t n[5]={
   {.identifier=1,.parent=OS_CONTROL_NO_PARENT,.kind=OS_CONTROL_CONTAINER,.font_size=font},
   {.identifier=2,.parent=0,.kind=OS_CONTROL_CONTAINER,.flags=OS_CONTROL_MENU_PANEL|OS_CONTROL_INHERIT_FONT,
    .x=690,.y=490,.width=240,.background=0xff0000,.radius=99},
   {.identifier=3,.parent=1,.kind=OS_CONTROL_LIST_ITEM,.flags=OS_CONTROL_INHERIT_FONT},
   {.identifier=4,.parent=1,.kind=OS_CONTROL_LIST_ITEM,.flags=OS_CONTROL_DISABLED|OS_CONTROL_INHERIT_FONT},
   {.identifier=5,.parent=1,.kind=OS_CONTROL_LIST_ITEM,.flags=OS_CONTROL_INHERIT_FONT}};
  os_control_node_t saved[5];memcpy(saved,n,sizeof(n));
  control_tree_box_t b[5];Rect viewport={0,0,(int)w,500};
  assert(control_tree_layout(n,5,0,viewport,b));
  assert(!memcmp(saved,n,sizeof(n)));
  assert(b[1].bounds.x0>=0&&b[1].bounds.x1<=(int)w&&b[1].bounds.y1<=500);
  assert(b[1].style.background==OS_THEME_SURFACE&&b[1].style.border_width==1);
  assert(b[2].bounds.y1-b[2].bounds.y0==(int)(font*2));
  assert(b[3].bounds.y0==b[2].bounds.y1&&b[4].bounds.y0==b[3].bounds.y1);
  assert(b[2].style.font_size==font&&b[2].style.background==OS_THEME_SURFACE);
  for(int y=0;y<500;y+=3)for(int x=0;x<(int)w;x+=3)
   assert(control_tree_hit_test(n,5,b,x,y)==control_tree_locate(n,5,viewport,x,y,0));
  source=n;assert(control_tree_paint_view(n,5,viewport,viewport,paint,0));
  n[1].flags=OS_CONTROL_DIALOG_PANEL|OS_CONTROL_INHERIT_FONT;
  n[1].width=320;n[1].height=180;
  assert(control_tree_layout(n,5,0,viewport,b));
  assert(b[1].bounds.x0==((int)w-((w<320)?(int)w:320))/2);
  assert(b[1].bounds.y0==160);
  n[1].flags|=OS_CONTROL_MENU_PANEL;assert(!control_tree_validate(n,5,0));
  n[1].flags=0;n[2].flags=OS_CONTROL_DIALOG_PANEL;assert(!control_tree_validate(n,5,0));
  n[2].flags=0;n[0].flags=OS_CONTROL_MENU_PANEL;assert(control_tree_validate(n,5,0));
 }
 assert(painted);puts("Common popup: theme ownership, scaled menu rows, edge clamp, centered dialogs, immutable descriptors and paint/hit agreement PASS");
}
'''
with tempfile.TemporaryDirectory(prefix='control-popup-') as directory:
    path = Path(directory)
    (path/'test.c').write_text(program)
    subprocess.run(['cc','-std=c11','-O2','-Wall','-Wextra','-Werror',
                    '-fsanitize=address,undefined','-fno-pie','-no-pie',
                    '-I'+str(root/'include'),str(path/'test.c'),
                    str(root/'kernel/graphics/control_tree.c'),'-o',str(path/'test')],check=True)
    subprocess.run([str(path/'test')],check=True)
