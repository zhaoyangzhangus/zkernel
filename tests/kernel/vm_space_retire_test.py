"""Production final VM release: ACK before retirement, destructors outside lock."""
from pathlib import Path
import subprocess
import tempfile

root=Path(__file__).resolve().parents[2]
source=(root/'kernel/mm/vm_space.c').read_text()
start=source.index('void vm_space_put(')
end=source.index('\n}',start)+2
program=r'''
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <kernel/list.h>
typedef struct { int unused; } page_t;
typedef struct {
 list_head_t ordered_node; unsigned long start,end;
 unsigned id; bool acknowledged;
} vm_area_t;
typedef struct {
 struct { atomic_uint value; } refs;
 list_head_t area_list; struct { void *root; } areas; paddr_t root_table;
} vm_space_t;
static unsigned locks,calls,releases,rebuilds,root_frees,space_frees,fail_at;
static page_t root_page;
static vm_space_t *current;
static vm_area_t *area_from_list(list_head_t *node) {
 return list_entry(node,vm_area_t,ordered_node);
}
static void map_lock(vm_space_t *space) { assert(space==current && !locks);++locks; }
static void map_unlock(vm_space_t *space) { assert(space==current && locks==1);--locks; }
static kstatus_t vm_tlb_unmap_range(vm_space_t *space,unsigned long start,unsigned long end) {
 assert(space==current && locks==1 && !releases && end>start);
 ++calls;
 vm_area_t *area=area_from_list(space->area_list.next);
 assert(area->start==start && !area->acknowledged);
 if(calls==fail_at) return K_EIO;
 area->acknowledged=true;return K_OK;
}
static void rebuild_area_tree(vm_space_t *space) {
 assert(locks==1 && space==current);++rebuilds;
 space->areas.root=list_empty(&space->area_list)?0:area_from_list(space->area_list.next);
}
static void area_release(vm_area_t *area) {
 assert(!locks && area->acknowledged && list_empty(&area->ordered_node));
 // 保留的失败索引不能继续指向已释放的 VMA。
 assert(current->areas.root!=area);
 ++releases;free(area);
}
static page_t *phys_to_page(paddr_t physical) { assert(physical.value==4096);return &root_page; }
static void page_free(page_t *page) { assert(!locks && page==&root_page);++root_frees; }
static void kfree(void *space) { assert(!locks && space==current);++space_frees;free(space); }
''' + source[start:end] + r'''
static vm_space_t *setup(unsigned count,unsigned refs,unsigned failure) {
 locks=calls=releases=rebuilds=root_frees=space_frees=0;fail_at=failure;
 vm_space_t *space=calloc(1,sizeof(*space));assert(space);
 atomic_init(&space->refs.value,refs);list_init(&space->area_list);space->root_table.value=4096;
 for(unsigned i=0;i<count;++i) {
  vm_area_t *area=calloc(1,sizeof(*area));assert(area);
  area->start=(i+1)*4096;area->end=area->start+4096;area->id=i;
  list_add_tail(&space->area_list,&area->ordered_node);
 }
 if(count) space->areas.root=area_from_list(space->area_list.next);
 current=space;return space;
}
int main(void) {
 vm_space_put(0);
 vm_space_t *space=setup(3,2,0);
 vm_space_put(space);assert(atomic_load(&space->refs.value)==1 && !calls && !releases);
 vm_space_put(space);assert(calls==3 && releases==3 && root_frees==1 && space_frees==1 && !rebuilds);
 space=setup(0,1,0);vm_space_put(space);assert(!calls && root_frees==1 && space_frees==1);
 for(unsigned fail=1;fail<=256;++fail) {
  space=setup(256,1,fail);
  void *unchanged_root=space->areas.root;
  vm_space_put(space);
  assert(calls==fail && releases==fail-1 && !root_frees && !space_frees &&
         rebuilds==(fail>1));
  if(fail==1) assert(space->areas.root==unchanged_root);
  assert(atomic_load(&space->refs.value)==1 && !locks);
  unsigned remaining=0;
  for(list_head_t *node=space->area_list.next;node!=&space->area_list;node=node->next) {
   vm_area_t *area=area_from_list(node);assert(!area->acknowledged);++remaining;
  }
  assert(remaining==257-fail && space->areas.root==area_from_list(space->area_list.next));
  // 在首个保留 VMA 之前反复失败，不得改变索引或引用计数。
  unchanged_root=space->areas.root;
  calls=releases=rebuilds=0;fail_at=1;vm_space_put(space);
  assert(calls==1 && !releases && !rebuilds && !root_frees && !space_frees);
  assert(atomic_load(&space->refs.value)==1 && !locks &&
         space->areas.root==unchanged_root);
  // 重试保留状态：只释放一次，且不能提前释放根节点。
  calls=releases=0;fail_at=0;vm_space_put(space);
  assert(calls==remaining && releases==remaining && root_frees==1 && space_frees==1);
 }
 space=setup(4096,1,0);vm_space_put(space);
 assert(calls==4096 && releases==4096 && root_frees==1 && space_frees==1);
}
'''
with tempfile.TemporaryDirectory(prefix='liteos-vm-retire-') as directory:
    path=Path(directory)
    (path/'test.c').write_text(program)
    subprocess.run(['cc','-std=gnu11','-Wall','-Wextra','-Werror',
                    '-fsanitize=address,undefined','-fno-pie','-no-pie',
                    '-I'+str(root/'include'),str(path/'test.c'),'-o',str(path/'test')],check=True)
    subprocess.run([str(path/'test')],check=True)
print('Final VM release: 256 failure boundaries, untouched-index and repeated-failure fast paths, refcounts, ACK-before-retire, unlocked destructors, retry and 4096 areas passed')
