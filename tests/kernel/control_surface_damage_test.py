"""Surface 损坏投影与逐像素参考结果对照，不允许改写源树。"""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
program = r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <kernel/control_tree.h>
static unsigned char actual[80*60];
static void emit(void *context,Rect r){
 assert(context==actual&&r.x0>=0&&r.y0>=0&&r.x1<=80&&r.y1<=60&&!rect_is_empty(r));
 for(int y=r.y0;y<r.y1;++y)for(int x=r.x0;x<r.x1;++x)actual[y*80+x]=1;
}
int main(void){
 os_control_node_t n[7]={
  {.identifier=1,.parent=OS_CONTROL_NO_PARENT,.kind=OS_CONTROL_CONTAINER},
  {.identifier=2,.parent=0,.kind=OS_CONTROL_CONTAINER,.width=45,.height=40,
   .x=8,.y=5,.flags=OS_CONTROL_CLIP_CHILDREN},
  {.identifier=3,.parent=1,.kind=OS_CONTROL_SURFACE,.width=55,.height=48,.surface=1},
  {.identifier=4,.parent=1,.kind=OS_CONTROL_SURFACE,.width=20,.height=20,.surface=1},
  {.identifier=5,.parent=0,.kind=OS_CONTROL_SURFACE,.width=25,.height=25,.surface=1},
  {.identifier=6,.parent=0,.kind=OS_CONTROL_SURFACE,.width=80,.height=60,.surface=2},
  {.identifier=7,.parent=0,.kind=OS_CONTROL_BUTTON,.width=80,.height=60}};
 Rect viewport={0,0,80,60};unsigned long cases=0;
 for(unsigned sample=0;sample<600;++sample){
  n[0].layout=sample%3;n[0].padding=sample%4;n[0].gap=sample%5;
  n[1].layout=(sample/3)%3;n[1].scroll_x=(int)(sample%21)-10;n[1].scroll_y=(int)(sample%17)-8;
  n[2].x=(int)(sample%19)-9;n[2].y=(int)(sample%13)-6;
  n[3].flags=sample%2?OS_CONTROL_DISABLED:OS_CONTROL_HIDDEN;
  n[4].x=(int)(sample%51)-15;n[4].y=(int)(sample%43)-12;
  n[1].flags=OS_CONTROL_CLIP_CHILDREN|(sample%7==0?OS_CONTROL_HIDDEN:0);
  control_tree_box_t boxes[7];os_control_node_t saved[7];memcpy(saved,n,sizeof(n));
  assert(control_tree_layout(n,7,0,viewport,boxes));
  Rect source={(int)(sample%25)-10,(int)(sample%23)-10,15+(int)(sample%29),12+(int)(sample%27)};
  unsigned sw=sample%41,sh=sample%37;
  memset(actual,0,sizeof(actual));
  assert(control_tree_surface_damage(n,7,viewport,1,sw,sh,source,emit,actual));
  assert(!memcmp(saved,n,sizeof(n)));
  for(int y=0;y<60;++y)for(int x=0;x<80;++x){
   bool expected=false;
   for(unsigned i=0;i<7;++i){
    control_tree_box_t b=boxes[i];int64_t sx=(int64_t)x-b.bounds.x0,sy=(int64_t)y-b.bounds.y0;
    if(n[i].kind==OS_CONTROL_SURFACE&&n[i].surface==1&&b.visible&&
       x>=b.bounds.x0&&x<b.bounds.x1&&y>=b.bounds.y0&&y<b.bounds.y1&&
       x>=b.clip.x0&&x<b.clip.x1&&y>=b.clip.y0&&y<b.clip.y1&&
       sx>=0&&sy>=0&&sx<sw&&sy<sh&&sx>=source.x0&&sx<source.x1&&sy>=source.y0&&sy<source.y1)
     expected=true;
   }
   assert(actual[y*80+x]==expected);++cases;
  }
 }
 /* 即使前面的节点已发出损坏，后面的溢出也必须报告失败，交由调用方完整恢复。 */
 n[0].layout=OS_CONTROL_ABSOLUTE;n[4].x=INT32_MAX;
 assert(!control_tree_surface_damage(n,7,viewport,1,80,60,(Rect){0,0,10,10},emit,actual));
 assert(!control_tree_surface_damage(n,7,viewport,1,80,60,(Rect){0,0,10,10},0,actual));
 printf("Surface damage: %lu pixel oracles, multi-instance/scroll/clip/disabled/hidden/source bounds/overflow PASS\n",cases);
}
'''
with tempfile.TemporaryDirectory(prefix='control-surface-damage-') as directory:
    path = Path(directory)
    (path/'test.c').write_text(program)
    subprocess.run(['cc', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
                    '-I'+str(root/'include'), str(path/'test.c'),
                    str(root/'kernel/graphics/control_tree.c'), '-o', str(path/'test')], check=True)
    subprocess.run([str(path/'test')], check=True)
