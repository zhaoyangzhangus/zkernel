"""Production sorted-list tree construction: RB invariants and exact linear work."""
from pathlib import Path
import subprocess
import tempfile

root=Path(__file__).resolve().parents[2]
source=(root/'kernel/mm/vma.c').read_text()
code=source[source.index('vm_area_t *area_from_list('):source.index('vm_area_t *find_area(')]
code=code.replace('vm_area_t *area_from_list(list_head_t *node) {',
                  'vm_area_t *area_from_list(list_head_t *node) { ++list_reads;')
code=code.replace('vm_area_t *area_from_rb(rb_node_t *node) {',
                  'vm_area_t *area_from_rb(rb_node_t *node) { ++index_reads;')
code=code.replace('item = item->next) ++count;',
                  'item = item->next) { ++count; ++count_reads; }')
assert '++count_reads' in code
program=r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <kernel/vm.h>
static unsigned list_reads,index_reads,count_reads;
typedef struct { bool present; uint64_t min_start,max_end,max_gap; } augment_result_t;
''' + code + r'''
static vm_area_t areas[8192];static vm_space_t space;
static unsigned seen,height;
static unsigned verify(rb_node_t *node,rb_node_t *parent,unsigned depth,
                       augment_result_t *summary) {
    if(!node) {*summary=(augment_result_t){false,0,0,0};return 1;}
    vm_area_t *a=area_from_rb(node);assert(rb_tree_parent(node)==parent);
    if(depth>height)height=depth;
    if(rb_tree_color(node)==RB_TREE_RED) {
        assert(rb_tree_color(node->left)==RB_TREE_BLACK && rb_tree_color(node->right)==RB_TREE_BLACK);
    }
    augment_result_t left,right;
    unsigned left_black=verify(node->left,node,depth+1,&left);
    assert(a==&areas[seen]);++seen;
    unsigned right_black=verify(node->right,node,depth+1,&right);
    assert(left_black==right_black);
    summary->present=true;summary->min_start=left.present?left.min_start:a->start;
    summary->max_end=right.present?right.max_end:a->end;summary->max_gap=0;
    if(left.present) {
        assert(left.max_end<=a->start);summary->max_gap=left.max_gap;
        if(a->start-left.max_end>summary->max_gap)summary->max_gap=a->start-left.max_end;
    }
    if(right.present) {
        assert(a->end<=right.min_start);
        if(right.max_gap>summary->max_gap)summary->max_gap=right.max_gap;
        if(right.min_start-a->end>summary->max_gap)summary->max_gap=right.min_start-a->end;
    }
    assert(a->subtree_min_start==summary->min_start && a->subtree_max_end==summary->max_end &&
           a->subtree_max_gap==summary->max_gap);
    return left_black+(rb_tree_color(node)==RB_TREE_BLACK);
}
static void prepare(unsigned count) {
    memset(&space,0,sizeof(space));list_init(&space.area_list);
    memset(areas,0,count*sizeof(*areas));
    for(unsigned i=0;i<count;++i) {
        areas[i].start=0x10000ULL+i*3*PAGE_SIZE;
        areas[i].end=areas[i].start+(1+i%2)*PAGE_SIZE;
        areas[i].object_offset=i+7;areas[i].prot=0x50+i;areas[i].flags=0xa0^i;
        list_add_tail(&space.area_list,&areas[i].ordered_node);
    }
}
static void check(unsigned count) {
    list_reads=index_reads=count_reads=0;rebuild_area_tree(&space);
    assert(list_reads==count && index_reads==0 && count_reads==count);
    assert(rb_tree_color(space.areas.root)==RB_TREE_BLACK);
    seen=height=0;augment_result_t summary;
    (void)verify(space.areas.root,0,0,&summary);assert(seen==count);
    unsigned bound=0;for(unsigned n=count;n;n>>=1)++bound;
    if(count)assert(height<bound);else assert(!space.areas.root);
    rb_node_t *node=rb_tree_first(&space.areas);
    for(unsigned i=0;i<count;++i) {
        assert(node==&areas[i].rb);node=rb_tree_next(node);
        assert(areas[i].object_offset==i+7 && areas[i].prot==0x50+i && areas[i].flags==(0xa0^i));
        assert(areas[i].ordered_node.next->prev==&areas[i].ordered_node);
    }
    assert(!node);
}
static unsigned old_rebuild(void) {
    unsigned comparisons=0;space.areas.root=0;
    for(list_head_t *item=space.area_list.next;item!=&space.area_list;item=item->next) {
        vm_area_t *area=area_from_list(item);area->rb.left=area->rb.right=0;
        rb_node_t *parent=0,**link=&space.areas.root;
        while(*link) {
            ++comparisons;parent=*link;vm_area_t *candidate=area_from_rb(parent);
            link=area->start<candidate->start?&parent->left:&parent->right;
        }
        area->rb.parent_color=(uintptr_t)parent|RB_TREE_RED;*link=&area->rb;
        rb_tree_insert_rebalance(&space.areas,&area->rb);
    }
    return comparisons;
}
int main(void) {
    for(unsigned count=0;count<=8192;++count) {
        prepare(count);check(count);
        if(count<128 || (count && (count&(count-1))==0)) {
            (void)old_rebuild();check(count); // Replace a differently shaped valid old tree.
        }
    }
    prepare(4096);unsigned old=old_rebuild();assert(old>4096*10);check(4096);
    /* 在移除和插入后重用链表节点，而不只是使用刚清零的新节点。 */
    list_del(&areas[0].ordered_node);list_add_before(space.area_list.next,&areas[0].ordered_node);check(4096);
    printf("Fused VMA rebuild: every size 0..8192, parent/order/color/equal-black-height/height/augmentation/list/payload invariants; exactly N count reads + N construction reads, zero separate tree traversal; old 4096-node rebuild used %u search comparisons, new zero passed\n",old);
}
'''
with tempfile.TemporaryDirectory(prefix='liteos-vma-linear-rebuild-') as directory:
    binary=str(Path(directory)/'test')
    subprocess.run(['cc','-std=c11','-O2','-Wall','-Wextra','-Werror',
                    '-I'+str(root/'include'),'-fsanitize=address,undefined',
                    '-fno-pie','-no-pie','-x','c','-','-o',binary],
                   input=program,text=True,check=True)
    subprocess.run([binary],check=True,timeout=60)
