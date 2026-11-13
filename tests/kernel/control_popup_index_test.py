"""弹层统计复用子树索引，与逐后代扫描对照布局、绘制及命中。"""
from pathlib import Path
import re
import resource
import signal
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'kernel/graphics/control_tree.c').read_text()
loop = ('for (uint32_t j=index+1U;j<count && nodes[j].parent==index;'
        'j=subtrees[j]&CONTROL_SUBTREE_END) {')
assert source.count(loop) == 2
instrumented = source.replace(loop, loop + '\n            ++popup_scans;')
# 参考实现保留原统计规则，只恢复逐后代遍历。
reference = instrumented.replace(loop, 'for (uint32_t j=index+1U;'
    'j<count && nodes[j].parent>=index;++j) {')
reference = reference.replace('++popup_scans;',
    '++popup_scans; if (nodes[j].parent!=index) continue;')
reference = reference.replace('const os_control_node_t *source=nodes+index;',
    '(void)subtrees; const os_control_node_t *source=nodes+index;')
names = re.findall(r'^(?:bool|uint32_t|void) (control_tree_\w+)\(', source, re.M)
legacy = reference
reference = ''.join(f'#define {name} reference_{name}\n' for name in names) + reference

program = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <kernel/control_tree.h>
bool reference_control_tree_layout(const os_control_node_t *,uint32_t,uint32_t,Rect,control_tree_box_t *);
uint32_t reference_control_tree_locate(const os_control_node_t *,uint32_t,Rect,int32_t,int32_t,control_tree_box_t *);
uint64_t optimized_scans(void),reference_scans(void);
static os_control_node_t nodes[OS_CONTROL_TREE_MAX_NODES],saved[OS_CONTROL_TREE_MAX_NODES];
static control_tree_box_t boxes[OS_CONTROL_TREE_MAX_NODES],expected[OS_CONTROL_TREE_MAX_NODES];
static uint32_t state=0x91d35U;
static uint32_t next(void){state=state*1664525U+1013904223U;return state;}
static void equal(const control_tree_box_t *a,const control_tree_box_t *b){
 assert(!memcmp(&a->bounds,&b->bounds,sizeof(Rect)));
 assert(!memcmp(&a->clip,&b->clip,sizeof(Rect)));
 assert(!memcmp(&a->style,&b->style,sizeof(a->style)));
 assert(a->depth==b->depth&&a->subtree_end==b->subtree_end);
 assert(a->visible==b->visible&&a->enabled==b->enabled&&a->inert==b->inert);
}
typedef struct {uint64_t digest;unsigned calls;} painted_t;
static void paint(void *raw,const os_control_node_t *n,const control_tree_box_t *b,Rect r){
 painted_t *p=raw;++p->calls;
 p->digest=p->digest*1099511628211ULL+n->identifier;
 p->digest^=(uint32_t)r.x0+(uint64_t)(uint32_t)r.y0*65537U;
 p->digest^=(uint32_t)r.x1+(uint64_t)(uint32_t)r.y1*65537U;
 p->digest^=b->style.background;
}
static void check(unsigned count,Rect viewport,bool linear){
 memcpy(saved,nodes,count*sizeof(*nodes));
 uint64_t before=optimized_scans(),old=reference_scans();
 assert(control_tree_layout(nodes,count,0,viewport,boxes));
 assert(reference_control_tree_layout(nodes,count,0,viewport,expected));
 uint64_t work=optimized_scans()-before,prior=reference_scans()-old;
 assert(work<=count-1U&&work<=prior);
 if(linear){
  assert(work==count-1U&&prior>work*30U);
  printf("Nested popup child visits: %llu -> %llu\n",
         (unsigned long long)prior,(unsigned long long)work);
 }
 for(unsigned i=0;i<count;++i)equal(boxes+i,expected+i);
 for(unsigned p=0;p<8;++p){
  int x=(int)(next()%2100)-30,y=(int)(next()%1200)-30;
  control_tree_box_t a,b;
  unsigned found=control_tree_locate(nodes,count,viewport,x,y,&a);
  unsigned wanted=reference_control_tree_locate(nodes,count,viewport,x,y,&b);
  assert(found==wanted&&found==control_tree_hit_test(nodes,count,boxes,x,y));
  if(found!=OS_CONTROL_NO_PARENT){
   /* 即时命中不生成整树缓存，因此不比较缓存专用的子树末尾。 */
   a.subtree_end=b.subtree_end;equal(&a,&b);
  }
 }
 painted_t a={0},b={0};
 control_tree_paint(nodes,count,boxes,viewport,paint,&a);
 control_tree_paint(nodes,count,expected,viewport,paint,&b);
 assert(a.calls==b.calls&&a.digest==b.digest);
 assert(!memcmp(saved,nodes,count*sizeof(*nodes)));
}
int main(void){
 const unsigned count=OS_CONTROL_TREE_MAX_NODES,containers=OS_CONTROL_TREE_MAX_DEPTH-1U;
 for(unsigned mode=0;mode<2;++mode){
  for(unsigned i=0;i<count;++i)nodes[i]=(os_control_node_t){
   .identifier=i+1U,.parent=i?(i<containers?i-1U:containers-1U):OS_CONTROL_NO_PARENT,
   .kind=i<containers?OS_CONTROL_CONTAINER:(mode?OS_CONTROL_TEXT:OS_CONTROL_LIST_ITEM),
   .flags=OS_CONTROL_INHERIT_FONT|((i<containers&&(!mode||i))?
          (mode?OS_CONTROL_DIALOG_PANEL:OS_CONTROL_MENU_PANEL):0),
   .font_size=16,.width=mode&&i?0:OS_CONTROL_FILL,
   .height=mode&&i?0:OS_CONTROL_FILL};
  check(count,(Rect){0,0,1920,1080},!mode);
 }
 for(unsigned trial=0;trial<1000;++trial){
  unsigned n=2+next()%255,depth=1,ancestors[OS_CONTROL_TREE_MAX_DEPTH]={0};
  for(unsigned i=0;i<n;++i){
   unsigned d=i?next()%(depth<64?depth:63):0;
   nodes[i]=(os_control_node_t){.identifier=i+1U,
    .parent=i?ancestors[d]:OS_CONTROL_NO_PARENT,.kind=OS_CONTROL_CONTAINER,
    .flags=OS_CONTROL_INHERIT_STYLE|(next()%7==0?OS_CONTROL_HIDDEN:0)|
           (next()%7==0?OS_CONTROL_DISABLED:0),
    .width=next()%2?OS_CONTROL_FILL:(int)(next()%500),
    .height=next()%2?OS_CONTROL_FILL:(int)(next()%400),
    .layout=next()%3,.font_size=12+next()%37,
    .x=(int)(next()%40)-20,.y=(int)(next()%40)-20,
    .scroll_x=(int)(next()%20)-10,.scroll_y=(int)(next()%20)-10};
   unsigned popup=next()%5;
   if(popup==0)nodes[i].flags|=OS_CONTROL_MENU_PANEL;
   else if(i&&popup==1){
    nodes[i].flags|=OS_CONTROL_DIALOG_PANEL;
    if(next()%2)nodes[i].width=nodes[i].height=0;
   }
   depth=i?d+1U:0;ancestors[depth++]=i;
  }
  check(n,(Rect){0,0,(int)(40+next()%2000),(int)(40+next()%1100)},false);
 }
 puts("Popup index: deep menus/dialogs and 1000 mixed trees preserve style, geometry, clipping, painting, hit order and immutable nodes PASS");
}
'''

with tempfile.TemporaryDirectory(prefix='control-popup-index-') as directory:
    path = Path(directory)
    prefix = '#include <stdint.h>\nstatic uint64_t popup_scans;\n'
    for name, body in [('optimized', instrumented), ('reference', reference)]:
        (path / f'{name}.c').write_text(prefix + body +
            f'\nuint64_t {name}_scans(void){{return popup_scans;}}\n')
    (path / 'test.c').write_text(program)
    command = ['cc', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
               '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
               '-I'+str(root/'include'), str(path/'test.c'),
               str(path/'optimized.c'), str(path/'reference.c'),
               '-o', str(path/'test')]
    subprocess.run(command, check=True)
    subprocess.run([str(path/'test')], check=True, timeout=30)
    # 几何正确不足以证明消除了重复扫描，恢复旧遍历必须被工作量断言拒绝。
    (path/'optimized.c').write_text(prefix + legacy +
        '\nuint64_t optimized_scans(void){return popup_scans;}\n')
    subprocess.run(command, check=True)
    result = subprocess.run([str(path/'test')], capture_output=True, text=True,
        timeout=30, preexec_fn=lambda:resource.setrlimit(resource.RLIMIT_CORE,(0,0)))
    assert result.returncode == -signal.SIGABRT and 'work<=count-1U' in result.stderr
    print('负面对照通过：恢复弹层逐后代扫描会触发工作量回归')
