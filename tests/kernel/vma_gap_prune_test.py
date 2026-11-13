"""Production augmented address-gap search versus prior in-order first fit."""
from pathlib import Path
import subprocess
import tempfile

root=Path(__file__).resolve().parents[2]
source=(root/'kernel/mm/vma.c').read_text()
code=source[source.index('vm_area_t *area_from_list('):source.index('vm_area_t *area_allocate(')]
code=code.replace('    if (node == 0) return false;\n    vm_area_t *area = area_from_rb(node);',
                  '    if (node == 0) return false;\n    ++visits;\n    vm_area_t *area = area_from_rb(node);')
program=r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <kernel/vm.h>
#define VM_USER_BASE 0x10000ULL
#define VM_USER_END 0x800000000000ULL
static unsigned visits;
typedef struct { bool present; uint64_t min_start,max_end,max_gap; } augment_result_t;
''' + code + r'''
static uint64_t old_gap(const vm_space_t *space,uint64_t hint,uint64_t size) {
    uint64_t cursor=hint<VM_USER_BASE?VM_USER_BASE:hint;
    cursor=(cursor+PAGE_SIZE-1ULL)&~(PAGE_SIZE-1ULL);
    if(cursor>=VM_USER_END || size>VM_USER_END-cursor)cursor=VM_USER_BASE;
    for(rb_node_t *node=rb_tree_first(&space->areas);node;node=rb_tree_next(node)) {
        vm_area_t *a=area_from_rb(node);
        if(a->end<=cursor)continue;
        if(a->start>=cursor && size<=a->start-cursor)return cursor;
        if(cursor<a->end)cursor=a->end;
        if(cursor>=VM_USER_END || size>VM_USER_END-cursor)return 0;
    }
    return size<=VM_USER_END-cursor?cursor:0;
}
static augment_result_t verify(rb_node_t *node) {
    if(!node)return (augment_result_t){false,0,0,0};
    vm_area_t *a=area_from_rb(node);
    augment_result_t left=verify(node->left),right=verify(node->right);
    augment_result_t result={true,left.present?left.min_start:a->start,
                            right.present?right.max_end:a->end,0};
    if(left.present) {
        assert(left.max_end<=a->start);result.max_gap=left.max_gap;
        if(a->start-left.max_end>result.max_gap)result.max_gap=a->start-left.max_end;
    }
    if(right.present) {
        assert(a->end<=right.min_start);
        if(right.max_gap>result.max_gap)result.max_gap=right.max_gap;
        if(right.min_start-a->end>result.max_gap)result.max_gap=right.min_start-a->end;
    }
    assert(a->subtree_min_start==result.min_start && a->subtree_max_end==result.max_end &&
           a->subtree_max_gap==result.max_gap);return result;
}
static vm_area_t areas[4096];static vm_space_t space;
static uint64_t rng=0xdaf719;
static uint64_t random_value(void) {rng^=rng<<13;rng^=rng>>7;rng^=rng<<17;return rng;}
static void prepare(unsigned bits) {
    memset(&space,0,sizeof(space));list_init(&space.area_list);
    for(unsigned i=0;i<12;++i)if(bits&(1U<<i)) {
        memset(&areas[i],0,sizeof(areas[i]));areas[i].start=VM_USER_BASE+i*PAGE_SIZE;
        areas[i].end=areas[i].start+PAGE_SIZE;list_add_tail(&space.area_list,&areas[i].ordered_node);
    }
    rebuild_area_tree(&space);(void)verify(space.areas.root);
}
static void check(uint64_t hint,uint64_t size) {
    uint64_t expected=old_gap(&space,hint,size);
    visits=0;assert(find_gap(&space,hint,size)==expected);
}
int main(void) {
    unsigned cases=0;
    const uint64_t offsets[]={0,1,PAGE_SIZE-1};
    const uint64_t hints[]={0,VM_USER_BASE-1,VM_USER_END-PAGE_SIZE,
                            VM_USER_END-1,VM_USER_END,VM_USER_END+1,UINT64_MAX};
    const uint64_t sizes[]={0,1,PAGE_SIZE-1,PAGE_SIZE,VM_USER_END-VM_USER_BASE,
                            VM_USER_END,UINT64_MAX};
    for(unsigned bits=0;bits<4096;++bits) {
        prepare(bits);
        for(unsigned page=0;page<15;++page)for(unsigned offset=0;offset<3;++offset)
          for(unsigned count=0;count<=16;++count) {
            check(VM_USER_BASE+page*PAGE_SIZE+offsets[offset],count*PAGE_SIZE);++cases;
          }
        for(unsigned i=0;i<sizeof(hints)/sizeof(*hints);++i)
          for(unsigned j=0;j<sizeof(sizes)/sizeof(*sizes);++j) {check(hints[i],sizes[j]);++cases;}
    }
    for(unsigned stride=1;stride<=2;++stride) {
        memset(&space,0,sizeof(space));list_init(&space.area_list);
        for(unsigned i=0;i<4096;++i) {
            memset(&areas[i],0,sizeof(areas[i]));areas[i].start=VM_USER_BASE+i*stride*PAGE_SIZE;
            areas[i].end=areas[i].start+PAGE_SIZE;list_add_tail(&space.area_list,&areas[i].ordered_node);
        }
        rebuild_area_tree(&space);(void)verify(space.areas.root);
        check(VM_USER_BASE,2*PAGE_SIZE);assert(visits==1);
        check(VM_USER_BASE+2048*PAGE_SIZE,2*PAGE_SIZE);assert(visits==1);
        check(VM_USER_BASE,PAGE_SIZE);assert(visits<=24);
        check(areas[4095].end,PAGE_SIZE);assert(visits==1);
        /* 删除前缀/后缀以及调整大小后都需要重新计算增强信息。 */
        list_del(&areas[0].ordered_node);list_del(&areas[4095].ordered_node);
        areas[4094].end-=1;rebuild_area_tree(&space);(void)verify(space.areas.root);
        check(VM_USER_BASE,2*PAGE_SIZE);check(areas[4094].end,1);
    }
    for(unsigned layout=0;layout<1024;++layout) {
        memset(&space,0,sizeof(space));list_init(&space.area_list);uint64_t cursor=VM_USER_BASE;
        for(unsigned i=0;i<64;++i) {
            memset(&areas[i],0,sizeof(areas[i]));cursor+=(random_value()%8)*PAGE_SIZE;
            areas[i].start=cursor;cursor+=(1+random_value()%8)*PAGE_SIZE;areas[i].end=cursor;
            list_add_tail(&space.area_list,&areas[i].ordered_node);
        }
        rebuild_area_tree(&space);(void)verify(space.areas.root);
        for(unsigned i=0;i<128;++i) {
            check(VM_USER_BASE+random_value()%(cursor-VM_USER_BASE+PAGE_SIZE),
                  (1+random_value()%80)*PAGE_SIZE);++cases;
        }
    }
    memset(&space,0,sizeof(space));list_init(&space.area_list);
    memset(areas,0,2*sizeof(*areas));areas[0].start=VM_USER_BASE;areas[0].end=VM_USER_BASE+PAGE_SIZE;
    areas[1].start=VM_USER_END-PAGE_SIZE;areas[1].end=VM_USER_END;
    list_add_tail(&space.area_list,&areas[0].ordered_node);list_add_tail(&space.area_list,&areas[1].ordered_node);
    rebuild_area_tree(&space);(void)verify(space.areas.root);
    for(unsigned i=0;i<sizeof(hints)/sizeof(*hints);++i)
      for(unsigned j=0;j<sizeof(sizes)/sizeof(*sizes);++j) {check(hints[i],sizes[j]);++cases;}
    printf("VMA gap pruning: %u first-fit oracle cases across all 4096 occupancy patterns, hints/alignment/zero/overflow/user limits; real augmented trees, rebuild mutations and one-node skip of 4096 too-small-gap mappings passed\n",cases);
}
'''
with tempfile.TemporaryDirectory(prefix='liteos-vma-gap-prune-') as directory:
    binary=str(Path(directory)/'test')
    subprocess.run(['cc','-std=c11','-O2','-Wall','-Wextra','-Werror',
                    '-I'+str(root/'include'),'-fsanitize=address,undefined',
                    '-fno-pie','-no-pie','-x','c','-','-o',binary],
                   input=program,text=True,check=True)
    subprocess.run([binary],check=True,timeout=60)
