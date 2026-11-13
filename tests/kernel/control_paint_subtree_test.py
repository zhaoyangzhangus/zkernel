"""真实布局和绘制：子树跳过与线性像素结果一致，覆盖不裁剪父节点的溢出。"""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'kernel/graphics/control_tree.c').read_text()
start = source.index('void control_tree_paint(')
end = source.index('typedef struct control_paint_query', start)
paint = source[start:end]
instrumented = paint.replace('for (uint32_t i = 0U; i < count; ++i) {',
                            'for (uint32_t i = 0U; i < count; ++i) { ++visits;')
instrumented = instrumented.replace('control_intersection(', 'counted_intersection(')
source = source[:start] + instrumented + source[end:]
source = source.replace('uint32_t index = i - 1U;', 'uint32_t index = i - 1U; ++hit_visits;',1)
helper = '''static Rect counted_intersection(Rect a, Rect b){
 ++intersections;return control_intersection(a,b);
}
'''
source = source.replace('void control_tree_paint(', helper+'void control_tree_paint(',1)
program = r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <kernel/control_tree.h>
_Static_assert(sizeof(control_tree_box_t)==sizeof(struct {
 Rect bounds,clip;bool visible,enabled;uint32_t depth;control_tree_style_t style;
}), "layout cache footprint");
static unsigned visits,intersections,hit_visits,hit_oracles;
''' + source + r'''
static os_control_node_t nodes[4096];
static control_tree_box_t boxes[4096];
static unsigned actual[128*80],expected[128*80];
static void pixel(void *raw,const os_control_node_t *node,const control_tree_box_t *box,Rect damage){
 (void)box;unsigned *pixels=raw;
 assert(damage.x0>=0&&damage.y0>=0&&damage.x1<=128&&damage.y1<=80);
 for(int y=damage.y0;y<damage.y1;++y)for(int x=damage.x0;x<damage.x1;++x)
  pixels[y*128+x]=node->identifier;
}
static Rect intersection(Rect a,Rect b){
 return (Rect){a.x0>b.x0?a.x0:b.x0,a.y0>b.y0?a.y0:b.y0,
  a.x1<b.x1?a.x1:b.x1,a.y1<b.y1?a.y1:b.y1};
}
static void compare(Rect damage){
 memset(actual,0xa5,sizeof(actual));memcpy(expected,actual,sizeof(actual));
 for(unsigned i=0;i<4096;++i){
  if(!boxes[i].visible)continue;
  Rect clip=intersection(intersection(damage,boxes[i].clip),boxes[i].bounds);
  if(!rect_is_empty(clip))pixel(expected,nodes+i,boxes+i,clip);
 }
 visits=intersections=0;control_tree_paint(nodes,4096,boxes,damage,pixel,actual);
 assert(!memcmp(expected,actual,sizeof(actual)));
 /* 独立反向线性基准，覆盖被隐藏的父节点及其子树和越界控件。 */
 for(int y=0;y<80;y+=13)for(int x=0;x<128;x+=17){
  unsigned expected_hit=OS_CONTROL_NO_PARENT;
  for(unsigned i=4096;i;--i){
   unsigned j=i-1;
   if(!boxes[j].visible||!boxes[j].enabled||
      (nodes[j].flags&OS_CONTROL_POINTER_PASSTHROUGH))continue;
   Rect r=intersection(boxes[j].bounds,boxes[j].clip);
   if(x>=r.x0&&x<r.x1&&y>=r.y0&&y<r.y1){expected_hit=j;break;}
  }
  assert(control_tree_hit_test(nodes,4096,boxes,x,y)==expected_hit);++hit_oracles;
 }
}
int main(void){
 nodes[0]=(os_control_node_t){.identifier=1,.parent=OS_CONTROL_NO_PARENT,.kind=OS_CONTROL_CONTAINER};
 for(unsigned group=0;group<63;++group){
  unsigned parent=1+group*65;
  nodes[parent]=(os_control_node_t){.identifier=parent+1,.parent=0,.kind=OS_CONTROL_CONTAINER,
   .x=0,.y=(int)group*30,.width=64,.height=20,.flags=OS_CONTROL_CLIP_CHILDREN};
  for(unsigned item=0;item<64;++item){
   unsigned index=parent+1+item;
   nodes[index]=(os_control_node_t){.identifier=index+1,.parent=parent,.kind=OS_CONTROL_CONTAINER,
    .x=(int)(item%16)*4,.y=(int)(item/16)*4,.width=4,.height=4};
  }
 }
 Rect viewport={0,0,128,80};
 assert(control_tree_layout(nodes,4096,0,viewport,boxes));
 assert(boxes[0].subtree_end==4096);
 for(unsigned group=0;group<63;++group){
  unsigned parent=1+group*65;
  assert(boxes[parent].subtree_end==parent+65);
  for(unsigned child=parent+1;child<parent+65;++child)assert(boxes[child].subtree_end==child+1);
 }
 compare((Rect){4,4,6,6});assert(visits==128);
 unsigned pruned=visits;
 compare((Rect){200,200,201,201});assert(visits==1 && intersections==0);
 compare((Rect){0,0,0,0});assert(visits==0);
 /* 空继承裁剪位于损坏范围内部时，也必须直接跳过整树。 */
 Rect saved_clip=boxes[0].clip;boxes[0].clip=(Rect){0};
 visits=intersections=0;
 control_tree_paint(nodes,4096,boxes,(Rect){-1,-1,1,1},pixel,actual);
 assert(visits==1&&!intersections);boxes[0].clip=saved_clip;
 /* 取消第二分支的父裁剪，让子节点越过父边界回到重绘区域，不能错误跳过。 */
 nodes[66].flags=0;nodes[67].y=-30;
 assert(control_tree_layout(nodes,4096,0,viewport,boxes));
 compare((Rect){0,0,4,4});assert(actual[0]==68);
 nodes[66].flags=OS_CONTROL_HIDDEN;
 assert(control_tree_layout(nodes,4096,0,viewport,boxes));
 compare((Rect){0,0,4,4});assert(actual[0]!=68);
 /* 不同滚动、隐藏、裁剪组合与原始逐节点绘制比较，保留完整绘制顺序。 */
 for(unsigned sample=0;sample<1000;++sample){
  for(unsigned group=0;group<63;++group){
   unsigned parent=1+group*65;
   nodes[parent].flags=((sample+group)%3?OS_CONTROL_CLIP_CHILDREN:0)|
    ((sample+group)%11==0?OS_CONTROL_HIDDEN:0);
   nodes[parent].y=(int)((sample*7+group*17)%160)-40;
   nodes[parent+1].y=-(int)(sample%40);
  }
  assert(control_tree_layout(nodes,4096,0,viewport,boxes));
  compare((Rect){(int)(sample%64),(int)(sample%40),64+(int)(sample%64),40+(int)(sample%40)});
 }
 /* 最大深度链下挂满叶子，验证退栈结束位置和整条隐藏分支的跳过。 */
 for(unsigned i=1;i<4096;++i)
  nodes[i]=(os_control_node_t){.identifier=i+1,.parent=i<63?i-1:62,
   .kind=OS_CONTROL_CONTAINER,.width=64,.height=20,.flags=OS_CONTROL_CLIP_CHILDREN};
 assert(control_tree_layout(nodes,4096,0,viewport,boxes));
 for(unsigned i=0;i<63;++i)assert(boxes[i].subtree_end==4096);
 for(unsigned i=63;i<4096;++i)assert(boxes[i].subtree_end==i+1);
 compare((Rect){0,0,16,16});
 nodes[1].flags|=OS_CONTROL_HIDDEN;
 assert(control_tree_layout(nodes,4096,0,viewport,boxes));
 compare((Rect){0,0,16,16});assert(visits==2 && intersections==2);
 hit_visits=0;
 assert(control_tree_hit_test(nodes,4096,boxes,0,0)==0);
 assert(hit_visits==2);
 /* 整棵树隐藏时只访问最后一个节点，沿父链直接跳过根。 */
 nodes[0].flags|=OS_CONTROL_HIDDEN;
 assert(control_tree_layout(nodes,4096,0,viewport,boxes));
 hit_visits=0;
 assert(control_tree_hit_test(nodes,4096,boxes,0,0)==OS_CONTROL_NO_PARENT);
 assert(hit_visits==1);
 printf("Hidden subtree hit test: %u linear oracle comparisons, 4096 nodes -> 2 visits, hidden root -> 1 visit PASS\n",hit_oracles);
 printf("Control paint subtree: 1000 4096-node pixel oracles, depth 64, overflow/hidden/clip/order; small damage visits 4096 -> %u PASS\n",pruned);
}
'''
with tempfile.TemporaryDirectory(prefix='control-paint-subtree-') as directory:
    binary = str(Path(directory) / 'test')
    command = ['cc', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
               '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
               '-I' + str(root / 'include'), '-x', 'c', '-', '-o', binary]
    subprocess.run(command, input=program, text=True, check=True)
    subprocess.run([binary], check=True)
    broken = program.replace('if (end > i && end <= count)', 'if (false)')
    # 删除跳过逻辑后保留变量使用，确保对照失败来自工作量而不是编译错误。
    broken = broken.replace('uint32_t end = boxes[i].subtree_end;',
                            'uint32_t end = boxes[i].subtree_end; (void)end;')
    subprocess.run(command, input=broken, text=True, check=True)
    assert subprocess.run([binary], stderr=subprocess.DEVNULL).returncode != 0
    print('负面对照通过：恢复全树扫描时，访问次数断言拒绝该实现')
    broken = program.replace('(nodes[i].flags & OS_CONTROL_CLIP_CHILDREN)', '(true)')
    subprocess.run(command, input=broken, text=True, check=True)
    assert subprocess.run([binary], stderr=subprocess.DEVNULL).returncode != 0
    print('负面对照通过：错误裁掉允许越界的子控件时，像素断言拒绝该实现')

    guard = 'while (parent < index && !boxes[parent].visible)'
    assert program.count(guard)==1
    broken = program.replace(guard, 'while (false && parent < index && !boxes[parent].visible)')
    subprocess.run(command, input=broken, text=True, check=True)
    assert subprocess.run([binary], stderr=subprocess.DEVNULL).returncode != 0
    print('负面对照通过：隐藏命中恢复逐节点扫描时访问次数断言失败')
