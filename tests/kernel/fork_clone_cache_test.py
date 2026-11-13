"""Actual fork cache: aligned-address collision oracle, RB bounds and lifetime."""
from pathlib import Path
import subprocess
import resource
import signal
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / "kernel/mm/vm_space.c").read_text()
cache = source[source.index("typedef struct vm_clone_entry"):
               source.index("kstatus_t vm_space_clone_cow(")]
cache = cache.replace("while (*link != 0) {", "while (*link != 0) { ++visits;")
program = r'''
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <kernel/vm.h>
enum { COUNT=4096 };
typedef struct { _Alignas(4096) vm_object_t object; unsigned char padding[4096-sizeof(vm_object_t)]; } slot_t;
static slot_t sources[COUNT];
static vm_object_t copies[COUNT];
static unsigned visits,attempts,allocations,frees,clones,gets,fail_entry,fail_clone;
static bool locked;
void *kmalloc(size_t size,uint32_t flags) {
 assert(!locked && !flags);
 if(++attempts==fail_entry)return 0;
 void *p=malloc(size);assert(p);++allocations;return p;
}
void kfree(void *p) {assert(!locked && p);++frees;free(p);}
void vm_object_get(vm_object_t *object) {
 assert(locked && atomic_load(&object->refs.value));++gets;
 atomic_fetch_add(&object->refs.value,1);
}
static kstatus_t vm_object_clone_anon(vm_object_t *source,vm_object_t **out) {
 assert(locked);
 if(++clones==fail_clone)return K_ENOMEM;
 unsigned id=(unsigned)(((uintptr_t)source-(uintptr_t)sources)/sizeof(slot_t));
 assert(id<COUNT && source==&sources[id].object && !atomic_load(&copies[id].refs.value));
 refcount_init(&copies[id].refs,1);*out=&copies[id];return K_OK;
}
''' + cache + r'''
static unsigned check_tree(rb_node_t *node,rb_node_t *parent,uintptr_t low,uintptr_t high,unsigned depth,unsigned *count) {
 if(!node)return 1;
 assert(depth<=24 && rb_tree_parent(node)==parent);
 vm_clone_entry_t *entry=(void*)((unsigned char*)node-__builtin_offsetof(vm_clone_entry_t,index_node));
 uintptr_t key=(uintptr_t)entry->source;assert(key>low && key<high);++*count;
 if(rb_tree_color(node)==RB_TREE_RED) {
  assert(!node->left || rb_tree_color(node->left)==RB_TREE_BLACK);
  assert(!node->right || rb_tree_color(node->right)==RB_TREE_BLACK);
 }
 unsigned left=check_tree(node->left,node,low,key,depth+1,count);
 unsigned right=check_tree(node->right,node,key,high,depth+1,count);
 assert(left==right);return left+(rb_tree_color(node)==RB_TREE_BLACK);
}
static void validate(rb_root_t *tree,unsigned expected) {
 unsigned count=0;assert(!tree->root || rb_tree_color(tree->root)==RB_TREE_BLACK);
 check_tree(tree->root,0,0,UINTPTR_MAX,0,&count);assert(count==expected);
}
static void reset(void) {
 assert(allocations==frees);attempts=allocations=frees=clones=gets=visits=fail_entry=fail_clone=0;
 memset(copies,0,sizeof(copies));
}
int main(void) {
 assert(sizeof(slot_t)==4096);
 uint64_t total_new=0,total_old=0;
 for(unsigned order=0;order<4;++order) {
  reset();vm_clone_cache_t tree={0};int heads[64],next[COUNT];
  for(unsigned i=0;i<64;++i)heads[i]=-1;
  unsigned permutation[COUNT];for(unsigned i=0;i<COUNT;++i)permutation[i]=i;
  if(order==1)for(unsigned i=0;i<COUNT;++i)permutation[i]=COUNT-1-i;
  if(order==2)for(unsigned i=0;i<COUNT;++i)permutation[i]=(i*2053U)%COUNT;
  if(order==3)for(unsigned i=0;i<COUNT;++i)permutation[i]=(i%2)?COUNT-1-i/2:i/2;
  assert(clone_cache_reserve(&tree,COUNT)==K_OK);
  locked=true;
  for(unsigned i=0;i<COUNT;++i) {
   unsigned id=permutation[i],before=visits;vm_object_t *out=(void*)1;
   assert(clone_anon_cached(&tree,&sources[id].object,&out)==K_OK && out==&copies[id]);
   assert(visits-before<=24);
   unsigned bucket=((uintptr_t)&sources[id].object>>4)%64;
   for(int item=heads[bucket];item!=-1;item=next[item])++total_old;
   next[id]=heads[bucket];heads[bucket]=(int)id;
   if((i%64)==63)validate(&tree.index,i+1);
  }
  fail_entry=attempts+1;fail_clone=clones+1;
  for(unsigned i=0;i<2*COUNT;++i) {
   unsigned id=(i*2053U)%COUNT,before=visits;vm_object_t *out=0;
   assert(clone_anon_cached(&tree,&sources[id].object,&out)==K_OK && out==&copies[id]);
   assert(visits-before<=24);
   unsigned bucket=((uintptr_t)&sources[id].object>>4)%64;
   bool found=false;
   for(int item=heads[bucket];item!=-1;item=next[item]) {
    ++total_old;if((unsigned)item==id){found=true;break;}
   }
   assert(found);
  }
  assert(clones==COUNT && allocations==1 && gets==2*COUNT && attempts==1);
  validate(&tree.index,COUNT);total_new+=visits;locked=false;
  clone_cache_release(&tree);assert(!tree.index.root && frees==allocations);
  /* 缓存项借用引用：释放记账数据时不得消耗合成子进程别名持有的三个引用。 */
  for(unsigned i=0;i<COUNT;++i)assert(atomic_load(&copies[i].refs.value)==3);
 }
 assert(total_new*50<total_old);
 reset();vm_clone_cache_t pool={0};
 fail_entry=1;assert(clone_cache_reserve(&pool,4)==K_ENOMEM&&!pool.entries&&!pool.capacity);
 fail_entry=0;assert(clone_cache_reserve(&pool,4)==K_OK);
 vm_clone_entry_t *saved=pool.entries;size_t capacity=pool.capacity;
 fail_entry=attempts+1;
 assert(clone_cache_reserve(&pool,8)==K_ENOMEM&&pool.entries==saved&&pool.capacity==capacity);
 assert(clone_cache_reserve(&pool,SIZE_MAX)==K_ENOMEM&&pool.entries==saved);
 fail_entry=0;assert(clone_cache_reserve(&pool,8)==K_OK&&pool.capacity==8);
 assert(clone_cache_reserve(&pool,3)==K_OK&&pool.capacity==8);
 clone_cache_release(&pool);assert(!pool.entries&&!pool.used&&!pool.capacity&&!pool.index.root);

 for(unsigned kind=0;kind<2;++kind)for(unsigned boundary=0;boundary<64;++boundary) {
  reset();vm_clone_cache_t tree={0};
  assert(clone_cache_reserve(&tree,kind==0?boundary:64)==K_OK);
  locked=true;
  if(kind==1)fail_clone=boundary+1;
  for(unsigned i=0;i<=boundary;++i) {
   vm_object_t *out=(void*)1;
   kstatus_t status=clone_anon_cached(&tree,&sources[i].object,&out);
   if(i==boundary)assert(status==K_ENOMEM && out==(void*)1);
   else assert(status==K_OK && out==&copies[i]);
  }
  validate(&tree.index,boundary);
  for(unsigned i=0;i<boundary;++i) {
   vm_object_t *out=0;assert(clone_anon_cached(&tree,&sources[i].object,&out)==K_OK && out==&copies[i]);
  }
  locked=false;clone_cache_release(&tree);assert(!tree.index.root && allocations==frees);
  for(unsigned i=0;i<boundary;++i)assert(atomic_load(&copies[i].refs.value)==2);
 }
 printf("Fork clone cache: 4096 aligned sources, four orders, 32768 aliases: %llu old-chain visits -> %llu RB visits; <=24 per request, RB invariants, 128 capacity/clone failure boundaries; one unlocked cache allocation per fork and borrowed lifetime passed\n",
        (unsigned long long)total_old,(unsigned long long)total_new);
}
'''
with tempfile.TemporaryDirectory(prefix="system-clone-cache-") as directory:
    binary = str(Path(directory) / "test")
    subprocess.run([
        "cc", "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
        "-fsanitize=address,undefined", "-fno-pie", "-no-pie",
        "-I"+str(root/"include"), "-x", "c", "-", "-o", binary,
    ], input=program, text=True, check=True)
    subprocess.run([binary], check=True, timeout=30)
    allocation='vm_clone_entry_t *entries = kmalloc(target * sizeof(*entries), 0U);'
    assert program.count(allocation)==1
    negative=program.replace(allocation,'locked=true; '+allocation+' locked=false;')
    subprocess.run(['cc','-std=c11','-O2','-Wall','-Wextra','-Werror',
                    '-I'+str(root/'include'),'-x','c','-','-o',binary],
                    input=negative,text=True,check=True)
    result=subprocess.run([binary],capture_output=True,text=True,timeout=30,
        preexec_fn=lambda:resource.setrlimit(resource.RLIMIT_CORE,(0,0)))
    assert result.returncode==-signal.SIGABRT and '!locked' in result.stderr
    print('负面对照通过：克隆缓存预留重新进入映射锁时触发断言')
