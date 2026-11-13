"""真实 msync：合并文件页范围、分配上限、失败清理及解映射后的对象寿命。"""
from pathlib import Path
import resource
import signal
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root/'kernel/mm/protection.c').read_text()
body = source[source.index('kstatus_t vm_sync('):source.index('kstatus_t vm_advise(')]
ranges = source[source.index('static bool range_fully_mapped('):
                source.index('static bool vm_advice_supported(')]
vma = (root/'kernel/mm/vma.c').read_text()
limits = '\n'.join(line for line in (root/'kernel/mm/internal.h').read_text().splitlines()
                   if line.startswith(('#define VM_USER_BASE ', '#define VM_USER_END ')))
validation = vma[vma.index('bool range_size_valid('):vma.index('static bool find_gap_subtree(')]
prefix = r'''
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <kernel/vm.h>
#include <uapi/mm.h>
#include <arch/x86_64/paging.h>
#define LIMIT 1024U
#define FILE_PAGES 8192U
#define BASE 0x10000U
static vm_space_t space;
static vm_area_t areas[LIMIT];
static vm_object_t objects[LIMIT];
static vm_object_t *owners[LIMIT];
static struct mapping {unsigned id,refs;} mappings[2];
static unsigned char expected[2][2][FILE_PAGES],observed[2][2][FILE_PAGES];
static unsigned locked,allocations,frees,calls,gets,puts_count,count;
static int fail_alloc,fail_callback,mutate_alloc;
static unsigned mutation;
static bool unmap_during_sync;
static uint64_t virtual_end;
static void map_lock(vm_space_t *s){assert(s==&space&&!locked);locked=1;}
static void map_unlock(vm_space_t *s){assert(s==&space&&locked);locked=0;}
static vm_area_t *find_area(const vm_space_t *s,uint64_t address){
 assert(s==&space&&locked);
 for(unsigned i=0;i<count;++i)if(areas[i].start<=address&&address<areas[i].end)return areas+i;
 return 0;
}
static vm_area_t *area_from_list(list_head_t *item){
 return list_entry(item,vm_area_t,ordered_node);
}
void *kmalloc(size_t size,uint32_t flags){
 assert(!locked&&!flags);
 /* 模拟锁外预留时并发修改映射；后续快照必须重新验证地址和文件页。 */
 if((int)allocations==mutate_alloc){
  assert(!calls);
  if(mutation==0){
   areas[0].object_offset=100*PAGE_SIZE;
   expected[0][0][0]=0;expected[0][0][100]=1;
  }else if(mutation==1)areas[0].flags=VM_MAP_GUARD;
  else areas[0].start+=PAGE_SIZE;
 }
 if((int)allocations++==fail_alloc)return 0;
 return malloc(size);
}
void kfree(void *p){assert(!locked&&p);++frees;free(p);}
void vm_object_get(vm_object_t *o){
 assert(locked&&o->type==VM_OBJECT_FILE&&atomic_fetch_add(&o->refs.value,1U)>0);++gets;
}
void vm_object_put(vm_object_t *o){
 assert(!locked);++puts_count;unsigned old=atomic_fetch_sub(&o->refs.value,1U);assert(old>0);
 if(old==1){
  struct mapping *m=o->u.file.mapping;assert(m->refs);--m->refs;
  o->u.file.mapping=0;o->u.file.ops=0;o->type=0;
 }
}
static void unmap_all(void){
 map_lock(&space);list_init(&space.area_list);memset(areas,0xa5,sizeof(areas));map_unlock(&space);
 for(unsigned i=0;i<count;++i)vm_object_put(owners[i]);
}
static kstatus_t sync_file(void *raw,uint64_t first,uint64_t pages,unsigned kind){
 struct mapping *m=raw;
 assert(!locked&&m&&m->id<2&&m->refs&&pages&&first<FILE_PAGES&&pages<=FILE_PAGES-first);
 ++calls;
 if(unmap_during_sync&&calls==1)unmap_all();
 assert(m->refs);
 for(uint64_t i=first;i<first+pages;++i){
  assert(expected[kind][m->id][i]);observed[kind][m->id][i]=1;
 }
 return (int)calls==fail_callback?K_EIO:K_OK;
}
static kstatus_t sync0(void *m,uint64_t first,uint64_t n){return sync_file(m,first,n,0);}
static kstatus_t sync1(void *m,uint64_t first,uint64_t n){return sync_file(m,first,n,1);}
static const vm_file_ops_t ops[2]={{.sync=sync0},{.sync=sync1}};
''' + limits + '\n' + validation + ranges + body
suffix = r'''
static void reset(unsigned n){
 memset(&space,0,sizeof(space));list_init(&space.area_list);
 memset(areas,0,sizeof(areas));memset(objects,0,sizeof(objects));
 memset(expected,0,sizeof(expected));memset(observed,0,sizeof(observed));
 mappings[0]=(struct mapping){.id=0};mappings[1]=(struct mapping){.id=1};
 locked=allocations=frees=calls=gets=puts_count=0;count=n;virtual_end=BASE;
 fail_alloc=fail_callback=mutate_alloc=-1;mutation=0;unmap_during_sync=false;
}
static void add(unsigned i,unsigned object,unsigned mapping,unsigned kind,
                uint64_t first,unsigned pages,bool skip){
 assert(i<count&&object<count&&mapping<2&&kind<2&&pages&&first+pages<=FILE_PAGES);
 vm_object_t *o=objects+object;
 if(!atomic_load(&o->refs.value)){
  o->type=VM_OBJECT_FILE;o->u.file.mapping=mappings+mapping;o->u.file.ops=ops+kind;
  ++mappings[mapping].refs;
 }
 assert(o->u.file.mapping==mappings+mapping&&o->u.file.ops==ops+kind);
 atomic_fetch_add(&o->refs.value,1U);owners[i]=o;
 areas[i]=(vm_area_t){.start=virtual_end,.end=virtual_end+pages*PAGE_SIZE,
  .object=o,.object_offset=first*PAGE_SIZE,.flags=skip?0:VM_MAP_SHARED};
 virtual_end=areas[i].end;list_add_tail(&space.area_list,&areas[i].ordered_node);
 if(!skip)for(uint64_t p=first;p<first+pages;++p)expected[kind][mapping][p]=1;
}
static void run_range(kstatus_t wanted,unsigned groups,uint64_t start,uint64_t end){
 kstatus_t status=vm_sync(&space,start,end-start,OS_VM_SYNC_SYNC);
 assert(status==wanted&&!locked);
 if(wanted==K_OK){
  assert(!memcmp(expected,observed,sizeof(expected)));
  if(groups)assert(calls==groups);
 }
 assert(allocations==frees+(status==K_ENOMEM));
 assert(puts_count==gets+(unmap_during_sync&&calls?count:0U));
 if(!unmap_during_sync||!calls)unmap_all();
 assert(puts_count==gets+count);
 for(unsigned i=0;i<count;++i)assert(!atomic_load(&objects[i].refs.value));
 assert(!mappings[0].refs&&!mappings[1].refs);
}
static void run(kstatus_t wanted,unsigned groups){run_range(wanted,groups,BASE,virtual_end);}
static uint32_t random_state=0x912efU;
static unsigned next(void){random_state=random_state*1664525U+1013904223U;return random_state;}
int main(void){
 /* 同一对象的正序映射、不同对象的倒序映射、重复和相交范围都只需一个请求。 */
 for(unsigned mode=0;mode<4;++mode){
  reset(LIMIT);
  for(unsigned i=0;i<count;++i){
   unsigned first=mode==0?i:mode==1?LIMIT-1U-i:mode==2?17U:17U+i%4;
   add(i,mode==0?0:i,0,0,first,mode==3?4:1,false);
  }
  fail_alloc=1;unmap_during_sync=true;
  run(K_OK,1);assert(allocations==1&&gets==1);
 }
 puts("1024 split/overlapping file mappings: requests, allocations and callbacks 1024 -> 1; unmap during callback preserves the retained mapping PASS");
 /* 截取首尾 VMA 的中间部分，并叠加文件对象偏移，只同步实际请求的页。 */
 reset(3);
 for(unsigned i=0;i<count;++i){
  add(i,i,0,0,i*4,4,false);objects[i].u.file.file_offset=40*PAGE_SIZE;
 }
 memset(expected,0,sizeof(expected));
 for(unsigned p=42;p<50;++p)expected[0][0][p]=1;
 unmap_during_sync=true;fail_alloc=1;
 run_range(K_OK,1,BASE+2*PAGE_SIZE,virtual_end-2*PAGE_SIZE);
 /* 虚拟地址相邻不代表文件连续；不同映射或操作表必须保持独立。 */
 for(unsigned mode=0;mode<3;++mode){
  reset(32);
  for(unsigned i=0;i<count;++i)add(i,i,mode==1?i%2:0,mode==2?i%2:0,
                                mode==0?i*2:i,1,false);
  run(K_OK,32);assert(allocations==32);
 }
 /* 忽略私有映射，但不能因此把未请求的文件页间隙也回写。 */
 reset(5);add(0,0,0,0,0,1,false);add(1,1,0,0,100,1,true);
 add(2,2,0,0,1,1,false);add(3,3,0,0,200,1,true);add(4,4,0,0,3,1,false);
 run(K_OK,2);
 /* 全部请求准备成功前不执行任何 I/O；失败也要释放已合并的唯一所有者。 */
 for(int failure=0;failure<2;++failure){
  reset(4);add(0,0,0,0,0,1,false);add(1,1,0,0,1,1,false);
  add(2,2,1,0,0,1,false);add(3,3,1,0,1,1,false);
  fail_alloc=failure;run(K_ENOMEM,0);assert(!calls&&gets==(unsigned)failure);
 }
 for(int failure=1;failure<=2;++failure){
  reset(4);add(0,0,0,0,0,1,false);add(1,1,0,0,1,1,false);
  add(2,2,1,0,0,1,false);add(3,3,1,0,1,1,false);
  fail_callback=failure;unmap_during_sync=true;run(K_EIO,0);
  assert(calls==(unsigned)failure&&gets==3);
 }
 reset(3);for(unsigned i=0;i<count;++i)add(i,i,0,0,i,1,false);
 areas[2].flags=VM_MAP_GUARD;run(K_EINVAL,0);assert(!calls&&gets==1);
 reset(2);add(0,0,0,0,0,1,false);add(1,1,0,0,1,1,false);
 objects[1].u.file.file_offset=UINT64_MAX-PAGE_SIZE+1U;
 run(K_EOVERFLOW,0);assert(!calls&&gets==1);
 /* 预留不足时释放旧快照引用；地址出现空洞或保护页时不得写回旧快照。 */
 for(unsigned mode=0;mode<3;++mode){
  reset(3);for(unsigned i=0;i<count;++i)add(i,i,0,0,i*2,1,false);
  mutate_alloc=1;mutation=mode;
  run(mode==0?K_OK:mode==1?K_EINVAL:K_ENOENT,mode==0?3:0);
  if(mode==0)assert(allocations==4&&gets==6);
  else assert(!calls&&gets==1);
 }
 /* 覆盖倍增预留的每个失败点，以及非二次幂请求留下的空闲节点。 */
 for(int failure=0;failure<8;++failure){
  reset(7);for(unsigned i=0;i<count;++i)add(i,i,0,0,i*2,1,false);
  fail_alloc=failure;run(K_ENOMEM,0);assert(!calls);
 }
 for(unsigned trial=0;trial<1000;++trial){
  unsigned n=1+next()%64;reset(n);
  for(unsigned i=0;i<n;++i){
   unsigned mapping=(next()>>16)%2,kind=(next()>>16)%2;
   unsigned first=(next()>>16)%256,pages=1+(next()>>16)%8;
   add(i,i,mapping,kind,first,pages,(next()>>16)%5==0);
  }
  unmap_during_sync=trial%2;run(K_OK,0);
 }
 puts("msync coalescing: gaps, identities, private ranges, allocation/callback/guard/overflow failures and 1000 randomized exact page-set oracles PASS");
}
'''

with tempfile.TemporaryDirectory(prefix='vm-sync-coalesce-') as directory:
    binary = str(Path(directory)/'test')
    command = ['cc','-std=gnu11','-O2','-Wall','-Wextra','-Werror',
               '-fsanitize=address,undefined','-fno-pie','-no-pie',
               '-I'+str(root/'include'),'-x','c','-','-o',binary]
    program = prefix + suffix
    subprocess.run(command,input=program,text=True,check=True)
    subprocess.run([binary],check=True,timeout=30)
    guard = 'if (last_request != 0 && last_request->object->u.file.ops == ops &&'
    assert program.count(guard)==1
    negative = program.replace(guard,'if (false && last_request != 0 && last_request->object->u.file.ops == ops &&')
    subprocess.run(command,input=negative,text=True,check=True)
    result = subprocess.run([binary],capture_output=True,text=True,timeout=30,
        preexec_fn=lambda:resource.setrlimit(resource.RLIMIT_CORE,(0,0)))
    assert result.returncode == -signal.SIGABRT and 'status==wanted' in result.stderr
    print('负面对照通过：恢复逐 VMA 分配会在已合并范围触发额外分配失败')
    guard = 'if (!retry && status == K_OK)'
    assert program.count(guard)==1
    negative = program.replace(guard, 'if (status == K_OK)')
    subprocess.run(command,input=negative,text=True,check=True)
    result = subprocess.run([binary],capture_output=True,text=True,timeout=30,
        preexec_fn=lambda:resource.setrlimit(resource.RLIMIT_CORE,(0,0)))
    assert result.returncode == -signal.SIGABRT and 'calls==groups' in result.stderr
    print('负面对照通过：未完成快照就写回会导致重复回调')
