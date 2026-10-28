"""按真实访问次数约束普通布局成本，不依赖宿主负载或计时阈值。"""
from pathlib import Path
import resource
import re
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root/'kernel/graphics/control_tree.c').read_text()
source = source.replace('const os_control_node_t *child = nodes + j;',
                        '++child_visits; const os_control_node_t *child = nodes + j;')
source = source.replace('const os_control_node_t *node = control_popup_node(',
                        '++node_visits; const os_control_node_t *node = control_popup_node(')
source = source.replace('*node=*source;', '++popup_copies; *node=*source;')
source = re.sub(r'query->(?:[a-z_]+_)?box = \*box;',
                lambda match: '++box_copies; ' + match.group(0), source)
prefix = r'''
#include <assert.h>
#include <stdio.h>
#include <kernel/control_tree.h>
static unsigned long child_visits,node_visits,popup_copies,box_copies;
'''
checks = r'''
static os_control_node_t nodes[OS_CONTROL_TREE_MAX_NODES];
static control_tree_box_t boxes[OS_CONTROL_TREE_MAX_NODES];
static unsigned queried;
static void query(void *context,uint32_t index,const os_control_node_t *node,const control_tree_box_t *box){
 assert(context==boxes&&index==queried++&&node==nodes+index);
 assert(box->bounds.x0==boxes[index].bounds.x0&&box->bounds.y0==boxes[index].bounds.y0);
 assert(box->bounds.x1==boxes[index].bounds.x1&&box->bounds.y1==boxes[index].bounds.y1);
 assert(box->clip.x0==boxes[index].clip.x0&&box->clip.y0==boxes[index].clip.y0);
 assert(box->clip.x1==boxes[index].clip.x1&&box->clip.y1==boxes[index].clip.y1);
 assert(box->visible==boxes[index].visible&&box->enabled==boxes[index].enabled);
 assert(box->style.font_size==boxes[index].style.font_size);
}
int main(void){
 const unsigned count=OS_CONTROL_TREE_MAX_NODES,containers=OS_CONTROL_TREE_MAX_DEPTH-1;
 Rect viewport={0,0,2048,2048};
 for(unsigned i=0;i<count;++i)nodes[i]=(os_control_node_t){.identifier=i+1,
  .parent=i?(i<containers?i-1:containers-1):OS_CONTROL_NO_PARENT,
  .kind=i<containers?OS_CONTROL_CONTAINER:OS_CONTROL_BUTTON,
  .width=OS_CONTROL_FILL,.height=i<containers?OS_CONTROL_FILL:1,
  .layout=i<containers?OS_CONTROL_COLUMN:OS_CONTROL_ABSOLUTE};
 assert(control_tree_layout(nodes,count,0,viewport,boxes));
 assert(!child_visits&&!popup_copies);
 node_visits=0;
 assert(control_tree_visit_validated(nodes,count,viewport,query,boxes));
 assert(queried==count&&node_visits==count&&!child_visits);
 assert(!control_tree_visit_validated(nodes,count,viewport,0,boxes));
 assert(!control_tree_visit_validated(nodes,count,(Rect){0},query,boxes));
 control_tree_box_t box;
 child_visits=node_visits=0;
 assert(control_tree_locate(nodes,count,viewport,0,0,&box)==containers);
 assert(!child_visits&&node_visits==count&&!popup_copies);
 child_visits=node_visits=0;
 assert(control_tree_find(nodes,count,viewport,count,&box)==count-1);
 assert(!child_visits&&node_visits==count);
 fprintf(stderr,"FIND_BOX_COPIES=%lu\n",box_copies);
 assert(box_copies==1);
 box_copies=0;
 assert(control_tree_find(nodes,count,viewport,count,0)==count-1&&!box_copies);
 box.bounds=(Rect){1,2,3,4};
 assert(control_tree_find(nodes,count,viewport,UINT32_MAX,&box)==OS_CONTROL_NO_PARENT&&!box_copies);
 assert(box.bounds.x0==1&&box.bounds.y1==4);
 child_visits=node_visits=0;
 uint32_t next;
 box_copies=0;
 assert(control_tree_focus_next_query(nodes,count,viewport,0,false,&next,&box,0,0)&&next==containers);
 assert(!child_visits&&node_visits==count);
 fprintf(stderr,"FORWARD_FOCUS_BOX_COPIES=%lu\n",box_copies);
 assert(box_copies<=2);
 /* 方向只影响候选维护，不能截断后续节点的布局或附加查询。 */
 for(unsigned reverse=0;reverse<2;++reverse){
  box_copies=queried=node_visits=0;
  assert(control_tree_focus_next_query(nodes,count,viewport,containers+11U,reverse,&next,&box,query,boxes));
  assert(next==containers+(reverse?9U:11U)&&queried==count&&node_visits==count);
  assert(box_copies==(reverse?10U:2U));
  box_copies=0;
  assert(control_tree_focus_next_query(nodes,count,viewport,containers+11U,reverse,&next,0,0,0));
  assert(next==containers+(reverse?9U:11U)&&!box_copies);
 }
 /* 导航与附加查询必须共享一遍布局，失败时不得发布输出参数。 */
 next=UINT32_MAX-1U;queried=0;node_visits=0;
 assert(control_tree_focus_next_query(nodes,count,viewport,0,false,&next,&box,query,boxes));
 assert(next==containers&&queried==count&&node_visits==count&&!child_visits);
 next=123U;box.bounds=(Rect){1,2,3,4};
 assert(!control_tree_focus_next_query(nodes,count,(Rect){0},0,false,&next,&box,query,boxes));
 assert(next==123U&&box.bounds.x0==1&&box.bounds.y1==4&&queried==count);
 assert(!control_tree_focus_next_query(nodes,count,viewport,0,false,0,&box,query,boxes));
 assert(queried==count);
 /* 需要空间分配时不能误走快速路径，功能仍由原来的统一布局实现承担。 */
 nodes[containers].flags=OS_CONTROL_FLEX_GROW;
 child_visits=0;assert(control_tree_layout(nodes,count,0,viewport,boxes)&&child_visits<=count-1);
 /* 每一层都需要 Flex，仍然只能按直接父子边统计一次，不能遍历整段后代。 */
 for(unsigned i=1;i<count;++i)nodes[i].flags=OS_CONTROL_FLEX_GROW;
 child_visits=0;assert(control_tree_layout(nodes,count,0,viewport,boxes)&&child_visits==count-1);
 printf("Nested Flex: child_visits=%lu (previous=%u)\n",child_visits,
        containers*(2U*count-containers-1U)/2U);
 for(unsigned i=1;i<count;++i)nodes[i].flags=0;
 nodes[containers].flags=0;nodes[0].flags=OS_CONTROL_JUSTIFY_END;
 child_visits=0;assert(control_tree_layout(nodes,count,0,viewport,boxes)&&child_visits==1);
 printf("Root justify: child_visits=%lu (previous=4095)\n",child_visits);
 puts("Control layout work: 4096 nodes/64 levels, linear locate/find/focus, no unnecessary subtree scans; Flex/justify preserved PASS");
}
'''
condition = '''if (node->layout != OS_CONTROL_ABSOLUTE && distribution && ((subtrees[i]&CONTROL_SUBTREE_FLEX) ||
            (node->flags & (OS_CONTROL_JUSTIFY_CENTER | OS_CONTROL_JUSTIFY_END | OS_CONTROL_JUSTIFY_BETWEEN))))'''
assert condition in source
resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
with tempfile.TemporaryDirectory(prefix='control-layout-work-') as directory:
    path = Path(directory)
    def compile_test(body):
        (path/'test.c').write_text(prefix + body + checks)
        subprocess.run(['cc','-std=c11','-O2','-Wall','-Wextra','-Werror',
                        '-fsanitize=address,undefined','-fno-pie','-no-pie',
                        '-I'+str(root/'include'),str(path/'test.c'),'-o',str(path/'test')],check=True)
    compile_test(source)
    subprocess.run([str(path/'test')],check=True)
    # 恢复查找时无关的焦点候选维护，几何结果仍正确，但复制成本必须被拒绝。
    negative = source.replace('    bool copy_box;',
        '    bool copy_box;\n    control_focus_query_t focus;')
    negative = negative.replace('    control_find_query_t *query = raw;',
        '    control_find_query_t *query = raw;\n'
        '    if(!index)query->focus=(control_focus_query_t){.reverse=true,.copy_box=true,\n'
        '        .first=OS_CONTROL_NO_PARENT,.selected=OS_CONTROL_NO_PARENT};\n'
        '    control_focus_visit(&query->focus,index,node,box);')
    assert negative != source
    compile_test(negative)
    result = subprocess.run([str(path/'test')],capture_output=True,text=True,timeout=15)
    assert result.returncode != 0 and 'box_copies==1' in result.stderr, result
    print('负面对照通过：标识查找恢复焦点候选维护会触发复制成本回归')
    # 保持正向选择结果正确，只恢复每个候选的多余几何复制。
    negative = source.replace('control_tree_box_t first_box, selected_box;',
        'control_tree_box_t first_box, selected_box, redundant_box;')
    negative = negative.replace('    if (query->reverse) {',
        '    if (!query->reverse) { ++box_copies; query->redundant_box = *box; }\n'
        '    if (query->reverse) {', 1)
    assert negative != source
    compile_test(negative)
    result = subprocess.run([str(path/'test')],capture_output=True,text=True,timeout=15)
    assert result.returncode != 0 and 'box_copies<=2' in result.stderr, result
    print('负面对照通过：正向导航恢复逐候选复制会触发成本回归')
    # 负面对照仅恢复无条件子树扫描，像素正确也必须被成本回归拒绝。
    negative = source.replace(condition, 'if (node->layout != OS_CONTROL_ABSOLUTE)')
    negative = negative.replace('bool distribution = false;', 'bool distribution = false; (void)distribution;')
    negative = negative.replace('if (distribution && !control_layout_index', 'if (!control_layout_index')
    compile_test(negative)
    result = subprocess.run([str(path/'test')],capture_output=True,text=True,timeout=15)
    assert result.returncode != 0 and '!child_visits' in result.stderr, result
    print('负面对照通过：恢复重复子树扫描会触发访问次数回归')
    traversal = 'for (uint32_t j=i+1U;j<count && nodes[j].parent==i;j=subtrees[j]&CONTROL_SUBTREE_END)'
    assert traversal in source
    negative = source.replace(traversal, 'for (uint32_t j=i+1U;j<count && nodes[j].parent>=i;++j)')
    negative = negative.replace('if (child->flags & OS_CONTROL_HIDDEN)',
                                'if (child->parent!=i || (child->flags & OS_CONTROL_HIDDEN))')
    compile_test(negative)
    result = subprocess.run([str(path/'test')],capture_output=True,text=True,timeout=15)
    assert result.returncode != 0 and 'child_visits==count-1' in result.stderr, result
    print('负面对照通过：恢复后代逐项遍历会触发嵌套 Flex 成本回归')
