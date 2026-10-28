"""执行真实控件树：布局、裁剪、层叠命中、绘制与事件路径共享同一几何结果。"""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
program = r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <kernel/control_tree.h>
static os_control_node_t node(uint32_t id,uint32_t parent,uint32_t kind,int32_t w,int32_t h){
 return (os_control_node_t){.identifier=id,.parent=parent,.kind=kind,.width=w,.height=h};
}
static unsigned pixels[100*80],calls,surfaces;
static void paint(void *context,const os_control_node_t *n,const control_tree_box_t *b,Rect damage){
 assert(context==pixels&&b->visible);++calls;
 if(n->kind==OS_CONTROL_SURFACE)++surfaces;
 for(int y=damage.y0;y<damage.y1;++y)for(int x=damage.x0;x<damage.x1;++x){
  assert(x>=0&&x<100&&y>=0&&y<80);pixels[y*100+x]=n->identifier;
 }
}
static bool equal(Rect a,Rect b){return !memcmp(&a,&b,sizeof(a));}
typedef struct {const os_control_node_t *nodes;const control_tree_box_t *boxes;uint32_t count,seen;} focus_visit_t;
static void verify_focus_visit(void *raw,uint32_t index,const os_control_node_t *n,const control_tree_box_t *b){
 focus_visit_t *v=raw;assert(index<v->count&&index==v->seen++);
 assert(n->identifier==v->nodes[index].identifier);
 assert(equal(b->bounds,v->boxes[index].bounds)&&equal(b->clip,v->boxes[index].clip));
 assert(b->visible==v->boxes[index].visible&&b->enabled==v->boxes[index].enabled);
}
static void verify_focus(const os_control_node_t *n,uint32_t count,const control_tree_box_t *boxes,Rect viewport){
 for(uint32_t current=0;current<=count;++current){
  uint32_t id=current<count?n[current].identifier:UINT32_MAX;
  control_tree_box_t box={0};
  assert(control_tree_find(n,count,viewport,id,&box)==(current<count?current:OS_CONTROL_NO_PARENT));
  if(current<count)assert(equal(box.bounds,boxes[current].bounds)&&equal(box.clip,boxes[current].clip));
  for(unsigned reverse=0;reverse<2;++reverse){
   uint32_t expected=OS_CONTROL_NO_PARENT;
   for(uint32_t step=1;step<=count;++step){
    uint32_t i=current<count?(reverse?(current+count-step)%count:(current+step)%count):
     (reverse?count-step:step-1);
    const control_tree_box_t *b=boxes+i;
    int32_t left=b->bounds.x0>b->clip.x0?b->bounds.x0:b->clip.x0;
    int32_t top=b->bounds.y0>b->clip.y0?b->bounds.y0:b->clip.y0;
    int32_t right=b->bounds.x1<b->clip.x1?b->bounds.x1:b->clip.x1;
    int32_t bottom=b->bounds.y1<b->clip.y1?b->bounds.y1:b->clip.y1;
    if(n[i].kind==OS_CONTROL_BUTTON&&!(n[i].flags&OS_CONTROL_SKIP_TAB)&&
       b->visible&&b->enabled&&left<right&&top<bottom){expected=i;break;}
   }
   uint32_t next;
   assert(control_tree_focus_next_query(n,count,viewport,id,reverse,&next,&box,0,0)&&next==expected);
   if(expected!=OS_CONTROL_NO_PARENT)assert(equal(box.bounds,boxes[expected].bounds)&&equal(box.clip,boxes[expected].clip));
   focus_visit_t visit={.nodes=n,.boxes=boxes,.count=count};next=UINT32_MAX-1U;
   assert(control_tree_focus_next_query(n,count,viewport,id,reverse,&next,&box,verify_focus_visit,&visit));
   assert(next==expected&&visit.seen==count);
   if(expected!=OS_CONTROL_NO_PARENT)assert(equal(box.bounds,boxes[expected].bounds)&&equal(box.clip,boxes[expected].clip));
  }
 }
}
static void verify_locate(const os_control_node_t *n,uint32_t count,const control_tree_box_t *boxes,Rect viewport){
 verify_focus(n,count,boxes,viewport);
 for(int y=-1;y<=80;++y)for(int x=-1;x<=100;++x){
  uint32_t expected=control_tree_hit_test(n,count,boxes,x,y);control_tree_box_t box;
  assert(control_tree_locate(n,count,viewport,x,y,&box)==expected);
  focus_visit_t visit={.nodes=n,.boxes=boxes,.count=count};uint32_t combined=123U;
  assert(control_tree_locate_query(n,count,viewport,x,y,&combined,&box,verify_focus_visit,&visit));
  assert(combined==expected&&visit.seen==count);
  if(expected!=OS_CONTROL_NO_PARENT)assert(equal(box.bounds,boxes[expected].bounds)&&equal(box.clip,boxes[expected].clip));
 }
}
int main(void){
 os_control_node_t n[70]={0},saved[70];control_tree_box_t boxes[70];
 Rect viewport={0,0,100,80};
 n[0]=node(1,OS_CONTROL_NO_PARENT,OS_CONTROL_CONTAINER,0,0);
 n[0].layout=OS_CONTROL_ROW;n[0].padding=5;n[0].gap=3;
 n[1]=node(2,0,OS_CONTROL_CONTAINER,20,OS_CONTROL_FILL);n[1].flags=OS_CONTROL_CLIP_CHILDREN;
 n[1].layout=OS_CONTROL_COLUMN;n[1].padding=2;n[1].gap=1;
 n[2]=node(3,1,OS_CONTROL_BUTTON,OS_CONTROL_FILL,10);
 n[3]=node(4,1,OS_CONTROL_TEXT,OS_CONTROL_FILL,10);
 n[4]=node(5,0,OS_CONTROL_SURFACE,OS_CONTROL_FILL,OS_CONTROL_FILL);n[4].surface=7;
 memcpy(saved,n,sizeof(n));
 assert(control_tree_layout(n,5,0,viewport,boxes));
 assert(!memcmp(saved,n,sizeof(n)));
 assert(equal(boxes[0].bounds,viewport));
 assert(equal(boxes[1].bounds,(Rect){5,5,25,75}));
 assert(equal(boxes[2].bounds,(Rect){7,7,23,17}));
 assert(equal(boxes[3].bounds,(Rect){7,18,23,28}));
 assert(equal(boxes[4].bounds,(Rect){28,5,95,75}));
 assert(boxes[2].depth==2&&boxes[4].depth==1);
 verify_locate(n,5,boxes,viewport);
 uint32_t path[64]={99,99,99};
 assert(control_tree_event_path(n,5,3,path,64)==3&&path[0]==0&&path[1]==1&&path[2]==3);
 path[0]=99;assert(!control_tree_event_path(n,5,3,path,2)&&path[0]==99);
 assert(control_tree_hit_test(n,5,boxes,7,7)==2);
 assert(control_tree_hit_test(n,5,boxes,30,10)==4);
 assert(control_tree_hit_test(n,5,boxes,100,10)==OS_CONTROL_NO_PARENT);
 memset(pixels,0,sizeof(pixels));control_tree_paint(n,5,boxes,viewport,paint,pixels);
 assert(calls==5&&surfaces==1&&pixels[7*100+7]==3&&pixels[10*100+30]==5);
 /* Surface 与普通按钮共享裁剪、层叠及命中，不能作为永远置顶的特殊窗口。 */
 n[0].layout=OS_CONTROL_ABSOLUTE;n[0].padding=0;
 n[1].width=40;n[1].height=40;n[1].x=n[1].y=10;
 n[4].x=15;n[4].y=15;n[4].width=n[4].height=20;
 assert(control_tree_layout(n,5,0,viewport,boxes));
 assert(control_tree_hit_test(n,5,boxes,16,16)==4);
 n[4].flags=OS_CONTROL_POINTER_PASSTHROUGH;
 assert(control_tree_layout(n,5,0,viewport,boxes));
 assert(control_tree_hit_test(n,5,boxes,16,16)==2);
 n[1].scroll_y=8;assert(control_tree_layout(n,5,0,viewport,boxes));
 assert(boxes[2].bounds.y0==4&&boxes[2].clip.y0==10);
 verify_locate(n,5,boxes,viewport);
 n[1].flags|=OS_CONTROL_DISABLED;
 assert(control_tree_layout(n,5,0,viewport,boxes));
 assert(control_tree_hit_test(n,5,boxes,16,16)==0);
 verify_locate(n,5,boxes,viewport);
 n[1].flags|=OS_CONTROL_HIDDEN;
 assert(control_tree_layout(n,5,0,viewport,boxes)&&!boxes[2].visible&&!boxes[3].visible);
 verify_locate(n,5,boxes,viewport);
 /* 隐藏节点不消耗流式布局的空隙与尺寸。 */
 n[0].layout=OS_CONTROL_ROW;n[0].gap=3;n[4].x=n[4].y=0;
 assert(control_tree_layout(n,5,0,viewport,boxes)&&boxes[4].bounds.x0==0);
 for(unsigned bad=0;bad<18;++bad){
  memcpy(n,saved,sizeof(n));
  switch(bad){
   case 0:n[0].parent=0;break;
   case 1:n[1].parent=4;break;
   case 2:n[3].parent=2;break;
   case 3:n[4].identifier=1;break;
   case 4:n[4].kind=99;break;
   case 5:n[4].width=-2;break;
   case 6:n[4].layout=99;break;
   case 7:n[4].flags=0x80000000;break;
   case 8:n[4].text_length=1;break;
   case 9:n[4].text_offset=UINT32_MAX;break;
   case 10:n[4].padding=UINT32_MAX;break;
   case 11:n[4].identifier=0;break;
   case 12:n[4].flags=OS_CONTROL_MANAGED_FOCUS;break;
   case 13:n[4].flags=OS_CONTROL_DEFAULT_FOCUS;break;
   case 14:n[4].flags=OS_CONTROL_SKIP_TAB;break;
   case 15:n[0].flags=OS_CONTROL_FOCUS_ARROWS;break;
   case 16:n[2].flags=OS_CONTROL_DEFAULT_FOCUS|OS_CONTROL_SKIP_TAB;break;
   case 17:n[4].kind=OS_CONTROL_BUTTON;n[4].flags=n[2].flags=OS_CONTROL_DEFAULT_FOCUS;break;
  }
  assert(!control_tree_validate(n,5,0));
 }
 /* 不允许已关闭的兄弟子树再次接收子节点。 */
 n[0]=saved[0];n[1]=node(2,0,OS_CONTROL_CONTAINER,10,10);
 n[2]=node(3,0,OS_CONTROL_CONTAINER,10,10);n[3]=node(4,1,OS_CONTROL_TEXT,10,10);
 assert(!control_tree_validate(n,4,0));
 for(unsigned i=0;i<65;++i)n[i]=node(i+1,i?i-1:OS_CONTROL_NO_PARENT,OS_CONTROL_CONTAINER,10,10);
 assert(control_tree_layout(n,64,0,viewport,boxes));
 assert(!control_tree_validate(n,65,0));
 /* 32 位坐标边界以宽整数计算，不能在布局时溢出。 */
 n[0]=saved[0];n[0].padding=0;n[0].layout=OS_CONTROL_ABSOLUTE;
 n[1]=node(2,0,OS_CONTROL_BUTTON,10,10);n[1].x=INT32_MAX;
 assert(!control_tree_layout(n,2,0,viewport,boxes));
 /* 根节点已命中，但后续节点布局失败，查找不能提前发布根的几何结果。 */
 control_tree_box_t retained={.bounds={1,2,3,4},.clip={5,6,7,8}};
 assert(control_tree_find(n,2,viewport,1,&retained)==OS_CONTROL_NO_PARENT);
 assert(equal(retained.bounds,(Rect){1,2,3,4})&&equal(retained.clip,(Rect){5,6,7,8}));
 uint32_t retained_next=123U;
 assert(!control_tree_focus_next_query(n,2,viewport,0,false,&retained_next,&retained,0,0));
 assert(retained_next==123U&&equal(retained.bounds,(Rect){1,2,3,4}));
 /* 附加回调可以先收到根节点，但失败查询不能发布半份点击目标。 */
 uint32_t retained_hit=123U;
 assert(!control_tree_locate_query(n,2,viewport,1,1,&retained_hit,&retained,0,0));
 assert(retained_hit==123U&&equal(retained.bounds,(Rect){1,2,3,4})&&equal(retained.clip,(Rect){5,6,7,8}));
 assert(!control_tree_locate_query(n,2,viewport,1,1,0,&retained,0,0));
 assert(!control_tree_validate(0,1,0)&&!control_tree_validate(n,0,0));
 assert(!control_tree_validate(n,OS_CONTROL_TREE_MAX_NODES+1,0));
 /* 独立绝对布局模型对照全屏绘制后的逐像素顶层命中。 */
 unsigned cases=0;
 for(unsigned sample=0;sample<300;++sample){
  n[0]=node(1,OS_CONTROL_NO_PARENT,OS_CONTROL_CONTAINER,0,0);
  for(unsigned i=1;i<20;++i){
   n[i]=node(i+1,0,i%4,10+(sample+i)%25,5+(sample*i)%20);
   n[i].x=(int)((sample*17+i*23)%130)-15;n[i].y=(int)((sample*13+i*7)%100)-10;
   if((sample+i)%7==0)n[i].flags=OS_CONTROL_HIDDEN;
   if(n[i].kind==OS_CONTROL_BUTTON&&(sample+i)%3==0)n[i].flags|=OS_CONTROL_SKIP_TAB;
  }
  assert(control_tree_layout(n,20,0,viewport,boxes));
  verify_focus(n,20,boxes,viewport);
  memset(pixels,0,sizeof(pixels));control_tree_paint(n,20,boxes,viewport,paint,pixels);
  for(int y=0;y<80;++y)for(int x=0;x<100;++x){
   unsigned expected=0;
   for(unsigned i=1;i<20;++i)if(!(n[i].flags&OS_CONTROL_HIDDEN)&&
      x>=n[i].x&&x<(int64_t)n[i].x+n[i].width&&y>=n[i].y&&y<(int64_t)n[i].y+n[i].height)expected=i;
   assert(control_tree_hit_test(n,20,boxes,x,y)==expected);
   control_tree_box_t located;
   assert(control_tree_locate(n,20,viewport,x,y,&located)==expected);
   assert(equal(located.bounds,boxes[expected].bounds)&&equal(located.clip,boxes[expected].clip));
   assert(pixels[y*100+x]==n[expected].identifier);++cases;
  }
  /* 局部重绘只能改变损坏范围，结果应与完整绘制在该范围内一致。 */
  unsigned full[100*80];memcpy(full,pixels,sizeof(full));
  for(unsigned i=0;i<8000;++i)pixels[i]=0xdeadbeef;
  Rect damage={(int)(sample%50),(int)(sample%40),50+(int)(sample%50),40+(int)(sample%40)};
  control_tree_paint(n,20,boxes,damage,paint,pixels);
  for(int y=0;y<80;++y)for(int x=0;x<100;++x){
   bool inside=x>=damage.x0&&x<damage.x1&&y>=damage.y0&&y<damage.y1;
   assert(pixels[y*100+x]==(inside?full[y*100+x]:0xdeadbeef));
  }
 }
 printf("Control tree: %u paint/hit oracle cases; find/focus forward/reverse oracle, nesting, clipping, scrolling, event paths and malformed input PASS\n",cases);
}
'''
with tempfile.TemporaryDirectory(prefix='control-tree-') as directory:
    path = Path(directory)
    (path/'test.c').write_text(program)
    subprocess.run(['cc','-std=c11','-O2','-Wall','-Wextra','-Werror',
                    '-fsanitize=address,undefined','-fno-pie','-no-pie',
                    '-I'+str(root/'include'),str(path/'test.c'),
                    str(root/'kernel/graphics/control_tree.c'),'-o',str(path/'test')],check=True)
    subprocess.run([str(path/'test')],check=True)
