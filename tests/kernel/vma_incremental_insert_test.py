"""Actual VMA insertion: augmented rotations, mixed tree shapes and bounded work."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'kernel/mm/vma.c').read_text()
code = source[source.index('vm_area_t *area_from_list('):source.index('vm_area_t *area_allocate(')]
code = code.replace('area->subtree_min_start = area->start;',
                    '++updates; area->subtree_min_start = area->start;')
code = code.replace('parent = *link;', '++comparisons; parent = *link;')
program = r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <kernel/vm.h>
#define VM_USER_BASE 0x10000ULL
#define VM_USER_END 0x800000000000ULL
static unsigned updates,comparisons;
''' + code + r'''
enum { N=4096 };
static vm_area_t areas[N];static vm_space_t space;
static unsigned seen;
typedef struct {bool present;uint64_t first,last,gap;} summary_t;
static unsigned verify(rb_node_t *node,rb_node_t *parent,summary_t *sum) {
    if(!node){*sum=(summary_t){0};return 1;}
    vm_area_t *a=area_from_rb(node);assert(rb_tree_parent(node)==parent);
    summary_t l,r;unsigned lb=verify(node->left,node,&l),rb=verify(node->right,node,&r);
    assert(lb==rb);
    if(rb_tree_color(node)==RB_TREE_RED)
        assert(rb_tree_color(node->left)==RB_TREE_BLACK && rb_tree_color(node->right)==RB_TREE_BLACK);
    assert(!l.present || l.last<=a->start);assert(!r.present || a->end<=r.first);
    *sum=(summary_t){true,l.present?l.first:a->start,r.present?r.last:a->end,0};
    if(l.present){sum->gap=l.gap;if(a->start-l.last>sum->gap)sum->gap=a->start-l.last;}
    if(r.present){if(r.gap>sum->gap)sum->gap=r.gap;if(r.first-a->end>sum->gap)sum->gap=r.first-a->end;}
    assert(a->subtree_min_start==sum->first && a->subtree_max_end==sum->last && a->subtree_max_gap==sum->gap);
    ++seen;return lb+(rb_tree_color(node)==RB_TREE_BLACK);
}
static uint64_t gap_oracle(uint64_t hint,uint64_t size) {
    uint64_t cursor=hint<VM_USER_BASE?VM_USER_BASE:hint;
    cursor=(cursor+PAGE_SIZE-1)&~(PAGE_SIZE-1);
    if(cursor>=VM_USER_END || size>VM_USER_END-cursor)cursor=VM_USER_BASE;
    for(list_head_t *item=space.area_list.next;item!=&space.area_list;item=item->next){
        vm_area_t *a=area_from_list(item);
        if(a->start>=cursor && size<=a->start-cursor)return cursor;
        if(cursor<a->end)cursor=a->end;
    }
    return size<=VM_USER_END-cursor?cursor:0;
}
static void check(unsigned count) {
    summary_t summary;seen=0;verify(space.areas.root,0,&summary);assert(seen==count);
    assert(rb_tree_color(space.areas.root)==RB_TREE_BLACK);
    rb_node_t *node=rb_tree_first(&space.areas);
    for(list_head_t *item=space.area_list.next;item!=&space.area_list;item=item->next){
        vm_area_t *a=area_from_list(item);assert(node==&a->rb);node=rb_tree_next(node);
        assert(find_area(&space,a->start)==a && find_area(&space,a->end-1)==a);
        assert(a->object_offset==(unsigned)(a-areas)+7);
    }
    assert(!node);
    if((count&63)==0 || count<16)
        for(unsigned i=0;i<32;++i)for(unsigned pages=1;pages<=8;++pages){
            uint64_t hint=VM_USER_BASE+(uint64_t)((i*137)%N)*4*PAGE_SIZE;
            assert(find_gap(&space,hint,pages*PAGE_SIZE)==gap_oracle(hint,pages*PAGE_SIZE));
        }
}
static void prepare(void) {
    memset(&space,0,sizeof(space));list_init(&space.area_list);memset(areas,0,sizeof(areas));
    for(unsigned i=0;i<N;++i){areas[i].start=VM_USER_BASE+(uint64_t)i*4*PAGE_SIZE;
        areas[i].end=areas[i].start+(1+i%3)*PAGE_SIZE;areas[i].object_offset=i+7;
        list_init(&areas[i].ordered_node);}
}
static void link_area(unsigned id) {
    list_head_t *item=space.area_list.next;
    while(item!=&space.area_list && area_from_list(item)->start<areas[id].start)item=item->next;
    list_add_before(item,&areas[id].ordered_node);
}
static void insert(unsigned id,unsigned count) {
    link_area(id);updates=comparisons=0;insert_area_tree(&space,&areas[id]);
    unsigned log=0;for(unsigned n=count;n;n>>=1)++log;
    assert(comparisons<=2*log && updates<=2*log+5);check(count);
}
int main(void) {
    /* 反复裁短内部区域：独立遍历统计原路径长度，验证摘要稳定后确实省去祖先更新。 */
    prepare();
    for(unsigned i=0;i<N;++i) {
        areas[i].end=areas[i].start+2*PAGE_SIZE;link_area(i);
    }
    rebuild_area_tree(&space);check(N);
    unsigned full_path=0,actual=0;
    for(unsigned i=1;i<N-1;++i) {
        for(rb_node_t *node=&areas[i].rb;node;node=rb_tree_parent(node))++full_path;
        areas[i].end-=PAGE_SIZE;updates=0;update_area_tree(&areas[i]);actual+=updates;
        if((i&63)==0)check(N);
    }
    check(N);
    printf("VMA boundary summaries: %u updates versus %u full-path updates\n",actual,full_path);
    assert(actual<full_path*3/4);
    for(unsigned order=0;order<4;++order){prepare();
        for(unsigned i=0;i<N;++i){unsigned id=i;
            if(order==1)id=N-1-i;
            if(order==2){id=0;for(unsigned bit=0;bit<12;++bit)id=(id<<1)|((i>>bit)&1);}
            if(order==3)id=(i*2053+19)&(N-1);
            insert(id,i+1);
        }
        for(unsigned i=0;i<N;++i)if(i%3!=0) {
            if(i%3==1)areas[i].start+=PAGE_SIZE;
            else areas[i].end-=PAGE_SIZE;
            updates=0;update_area_tree(&areas[i]);assert(updates<=24);
            if((i&63)==0 || i==N-2)check(N);
        }
        check(N);
        for(unsigned i=0;i<N;++i) {
            unsigned id=order==0?i:order==1?N-1-i:order==2?(i*2053+19)&(N-1):0;
            if(order==3)id=(unsigned)(area_from_rb(space.areas.root)-areas);
            updates=0;erase_area_tree(&space,&areas[id]);list_del(&areas[id].ordered_node);
            unsigned log=0;for(unsigned n=N-i;n;n>>=1)++log;
            assert(updates<=2*log+7);
            assert(!areas[id].rb.left && !areas[id].rb.right && !areas[id].rb.parent_color);
            check(N-i-1);
        }
    }
    prepare();for(unsigned i=0;i<N;i+=2)link_area(i);
    rebuild_area_tree(&space);check(N/2);
    for(unsigned i=1;i<N;i+=2)insert(i,N/2+(i+1)/2);
    for(unsigned i=0;i<256;++i) {
        unsigned id=(i*2053+19)&(N-1);updates=0;
        erase_area_tree(&space,&areas[id]);list_del(&areas[id].ordered_node);
        assert(updates<=33);check(N-1);insert(id,N);
    }
    for(unsigned i=0;i<N;++i) {
        vm_area_t *area=area_from_rb(space.areas.root);updates=0;
        erase_area_tree(&space,area);list_del(&area->ordered_node);
        unsigned log=0;for(unsigned n=N-i;n;n>>=1)++log;
        assert(updates<=2*log+7);check(N-i-1);
    }
    puts("Incremental VMA index: 18688 insertions, 20736 deletions, 10920 bound clips; ascending/descending/permuted/root removals, rebuilt trees and mixed erase/reinsert; RB/order/parent/augmentation/list/payload/find/gap oracle checks and logarithmically bounded summary work passed");
}
'''
with tempfile.TemporaryDirectory(prefix='liteos-vma-insert-') as directory:
    binary = str(Path(directory) / 'test')
    subprocess.run(['cc', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                    '-I'+str(root/'include'), '-fsanitize=address,undefined',
                    '-fno-pie', '-no-pie', '-x', 'c', '-', '-o', binary],
                   input=program, text=True, check=True)
    subprocess.run([binary], check=True, timeout=60)
    # 同一工作负载应拒绝始终传播到根的旧策略，防止优化检查失效。
    baseline = program.replace('update_area_path(&area->rb, true);',
                               'update_area_path(&area->rb, false);')
    assert baseline != program
    subprocess.run(['cc', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                    '-I'+str(root/'include'), '-fno-pie', '-no-pie',
                    '-x', 'c', '-', '-o', binary],
                   input=baseline, text=True, check=True)
    result = subprocess.run([binary], capture_output=True, timeout=10)
    assert result.returncode != 0 and b'actual<full_path*3/4' in result.stderr
    print('始终传播到根的负面对照已被更新预算检查拒绝')
