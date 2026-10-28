"""与逐项扫描的参考实现对照随机嵌套布局，保证优化不改变几何和命中。"""
from pathlib import Path
import re
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'kernel/graphics/control_tree.c').read_text()
loop = 'for (uint32_t j=i+1U;j<count && nodes[j].parent==i;j=subtrees[j]&CONTROL_SUBTREE_END)'
assert loop in source
reference = source.replace(loop, 'for (uint32_t j=i+1U;j<count && nodes[j].parent>=i;++j)')
reference = reference.replace('if (child->flags & OS_CONTROL_HIDDEN)',
                              'if (child->parent!=i || (child->flags & OS_CONTROL_HIDDEN))')
reference = reference.replace('distribution && ((subtrees[i]&CONTROL_SUBTREE_FLEX) ||',
                              'distribution && (distribution ||')
names = re.findall(r'^(?:bool|uint32_t|void) (control_tree_\w+)\(', source, re.M)
reference = ''.join(f'#define {name} reference_{name}\n' for name in names) + reference
program = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <kernel/control_tree.h>
bool reference_control_tree_layout(const os_control_node_t *,uint32_t,uint32_t,Rect,control_tree_box_t *);
uint32_t reference_control_tree_locate(const os_control_node_t *,uint32_t,Rect,int32_t,int32_t,control_tree_box_t *);
static uint32_t random_state=0x70918623;
static uint32_t next(void){random_state=random_state*1664525U+1013904223U;return random_state;}
static os_control_node_t nodes[256],saved[256];
static control_tree_box_t boxes[256],expected[256];
static void equal(const control_tree_box_t *a,const control_tree_box_t *b) {
 assert(!memcmp(&a->bounds,&b->bounds,sizeof(Rect))&&!memcmp(&a->clip,&b->clip,sizeof(Rect)));
 assert(!memcmp(&a->style,&b->style,sizeof(a->style)));
 assert(a->visible==b->visible&&a->enabled==b->enabled&&a->depth==b->depth);
}
int main(void) {
 for(unsigned trial=0;trial<3000;++trial) {
  unsigned count=1+next()%256,depth=1,ancestors[64]={0};
  for(unsigned i=0;i<count;++i) {
   unsigned parent_depth=i?next()%(depth<64?depth:63):0;
   unsigned parent=i?ancestors[parent_depth]:OS_CONTROL_NO_PARENT;
   nodes[i]=(os_control_node_t){.identifier=i+1,.parent=parent,.kind=OS_CONTROL_CONTAINER,
     .width=next()%3?OS_CONTROL_FILL:(int32_t)(next()%90),
     .height=next()%3?OS_CONTROL_FILL:(int32_t)(next()%90),
     .x=(int32_t)(next()%13)-6,.y=(int32_t)(next()%13)-6,
     .padding=next()%9,.gap=next()%7,.layout=next()%3,
     .scroll_x=(int32_t)(next()%13)-6,.scroll_y=(int32_t)(next()%13)-6,
     .flags=(next()%5==0?OS_CONTROL_HIDDEN:0)|(next()%5==0?OS_CONTROL_DISABLED:0)|
       (next()%3==0?OS_CONTROL_CLIP_CHILDREN:0)|
       (next()%3==0?OS_CONTROL_INHERIT_STYLE:0)};
   if(i&&nodes[parent].layout!=OS_CONTROL_ABSOLUTE&&next()%2)nodes[i].flags|=OS_CONTROL_FLEX_GROW;
   if(nodes[i].layout!=OS_CONTROL_ABSOLUTE) {
    unsigned justify=next()%4,align=next()%4;
    if(justify)nodes[i].flags|=OS_CONTROL_JUSTIFY_CENTER<<(justify-1);
    if(align)nodes[i].flags|=OS_CONTROL_ALIGN_CENTER<<(align-1);
   }
   depth=i?parent_depth+1:0;ancestors[depth++]=i;
  }
  memcpy(saved,nodes,sizeof(nodes));
  Rect viewport={0,0,(int32_t)(1+next()%4096),(int32_t)(1+next()%2160)};
  bool a=control_tree_layout(nodes,count,0,viewport,boxes);
  bool b=reference_control_tree_layout(nodes,count,0,viewport,expected);
  assert(a==b&&a);
  for(unsigned i=0;i<count;++i)equal(boxes+i,expected+i);
  for(unsigned point=0;point<5;++point) {
   int x=(int)(next()%4200)-50,y=(int)(next()%2260)-50;
   control_tree_box_t hit,old;
   unsigned found=control_tree_locate(nodes,count,viewport,x,y,&hit);
   unsigned wanted=reference_control_tree_locate(nodes,count,viewport,x,y,&old);
   assert(found==wanted);
   if(found!=OS_CONTROL_NO_PARENT)equal(&hit,&old);
  }
  assert(!memcmp(saved,nodes,sizeof(nodes)));
 }
 puts("Flex subtree index: 3000 mixed nested layouts, style/clip/hidden/disabled/scroll and 15000 hit comparisons PASS");
}
'''
with tempfile.TemporaryDirectory(prefix='control-flex-index-') as directory:
    path = Path(directory)
    (path / 'reference.c').write_text(reference)
    (path / 'test.c').write_text(program)
    subprocess.run(['cc', '-O2', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
                    '-I' + str(root / 'include'), str(path / 'test.c'),
                    str(path / 'reference.c'), str(root / 'kernel/graphics/control_tree.c'),
                    '-o', str(path / 'test')], check=True)
    subprocess.run([str(path / 'test')], check=True, timeout=30)
