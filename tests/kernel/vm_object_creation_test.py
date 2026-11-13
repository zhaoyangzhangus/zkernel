"""Actual VM constructors: bounded OOM allocation and ownership rollback."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
code = r'''
#define _GNU_SOURCE
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include "internal.h"
static unsigned attempts, fail_at, live, releases;
static unsigned preempt,disable_calls,direct_calls,fail_direct;
static vm_object_t **publication_probe;
void sched_preempt_disable(void){assert(!preempt);preempt=1;++disable_calls;}
void sched_preempt_enable(void){assert(preempt);preempt=0;}
void *kmalloc(size_t size, uint32_t flags) {
 (void)flags;
 assert(!preempt);
 if(publication_probe)assert(*publication_probe==(void*)1);
 if (++attempts == fail_at) return 0;
 void *p = malloc(size); assert(p); memset(p, 0xa5, size); ++live; return p;
}
void *kzalloc(size_t size, uint32_t flags) {
 void *p = kmalloc(size, flags); if (p) memset(p, 0, size); return p;
}
void kfree(void *p) { if (p) { assert(live); --live; free(p); } }
static unsigned pages_freed;
void page_free(page_t *page) { assert(page && atomic_load(&page->refs)<=1); ++pages_freed; kfree(page); }
page_t *page_alloc(uint8_t order,page_alloc_flags_t flags){
 assert(!order&&flags==PAGE_ALLOC_ZERO&&!preempt);
 page_t *page=kmalloc(sizeof(*page)+PAGE_SIZE,0);
 if(page){atomic_init(&page->refs,1);memset(page+1,0,PAGE_SIZE);}
 return page;
}
paddr_t page_to_phys(const page_t *page){return (paddr_t){(uintptr_t)page};}
void *phys_to_direct(paddr_t physical){
 if(++direct_calls==fail_direct)return 0;
 return (page_t *)(uintptr_t)physical.value+1;
}
static void release_private(void *data) { assert(data == &releases); ++releases; }
static unsigned mapping_refs, mapping_retains, mapping_releases;
static void retain_mapping(void *mapping) {
 assert(mapping == &mapping_refs); ++mapping_refs; ++mapping_retains;
}
static void release_mapping(void *mapping) {
 assert(mapping == &mapping_refs && mapping_refs); --mapping_refs; ++mapping_releases;
}
static kstatus_t get_file_page(void *mapping, uint64_t index, page_t **out) {
 (void)mapping; (void)index; (void)out; assert(!"creation must not fetch pages"); return K_EIO;
}
static void begin(unsigned failure) {
 assert(!live&&!preempt);attempts=0;fail_at=failure;disable_calls=direct_calls=fail_direct=0;
}
static void stats(vm_object_t *object, uint64_t size, uint32_t type) {
 assert(object->type == type && object->size == size);
 assert(object->flags == VM_OBJECT_INTERNAL && atomic_load(&object->refs.value) == 1);
 assert(!vm_object_fault_count(object) && !vm_object_populated_pages(object));
 assert(!vm_object_prefaulted_pages(object) && !vm_object_fault_around_pages(object));
}
int main(void) {
 const size_t sizes[] = {1, PAGE_SIZE, SIZE_MAX};
 for (unsigned i=0; i<3; ++i) for (unsigned failure=0; failure<2; ++failure) {
  begin(failure); vm_object_t *out=(void*)1;
  kstatus_t status=vm_object_create_anon(sizes[i], &out);
  assert(attempts == 1);
  if (failure) { assert(status==K_ENOMEM && out==(void*)1 && !live); continue; }
  assert(status==K_OK && live==1); stats(out,sizes[i],VM_OBJECT_ANON);
  anon_store_t *store=out->u.anon.anon_root;
  assert(store && !store->pages && !store->page_index.root && !atomic_load(&store->lock.state));
  vm_object_put(out); assert(!live);
 }
 begin(0); vm_object_t *out=(void*)1;
 assert(vm_object_create_anon(0,&out)==K_EINVAL && out==(void*)1);
 assert(vm_object_create_anon(PAGE_SIZE,0)==K_EINVAL && !attempts);
 const size_t shared_sizes[]={PAGE_SIZE,17*PAGE_SIZE,SIZE_MAX & ~(PAGE_SIZE-1ULL)};
 for(unsigned i=0;i<3;++i) for(unsigned failure=0;failure<2;++failure) {
  begin(failure);out=(void*)1;
  assert(vm_object_create_shared(shared_sizes[i],&out)==(failure?K_ENOMEM:K_OK));
  assert(attempts==1);
  if(failure) { assert(out==(void*)1 && !live);continue; }
  stats(out,shared_sizes[i],VM_OBJECT_SHARED);
  anon_store_t *store=out->u.shared.shared_root;
  assert(store && !store->pages && !store->page_index.root && !atomic_load(&store->lock.state));
  vm_object_get(out);vm_object_put(out);assert(live==1);
  vm_object_put(out);assert(!live);
 }
 begin(0);out=(void*)1;
 assert(vm_object_create_shared(0,&out)==K_EINVAL);
 assert(vm_object_create_shared(1,&out)==K_EINVAL);
 assert(vm_object_create_shared(PAGE_SIZE,0)==K_EINVAL);
 assert(vm_object_create_anon_store(PAGE_SIZE,VM_OBJECT_FILE,&out)==K_EINVAL);
 assert(!attempts && !live && out==(void*)1);
 /* 构造期间不可发布输出，也不可关抢占；全部页面清零、可检索并精确计数。 */
 const unsigned populated_counts[]={1,17,4096};
 for(unsigned c=0;c<3;++c){
  begin(0);out=(void*)1;publication_probe=&out;
  unsigned count=populated_counts[c];
  assert(vm_object_create_shared_populated(count*PAGE_SIZE,&out)==K_OK);
  publication_probe=0;
  assert(!disable_calls&&attempts==1+2*count&&live==1+2*count);
  assert(atomic_load(&out->populated_pages)==count&&atomic_load(&out->refs.value)==1);
  for(unsigned i=0;i<count;++i){
   page_t *page=0;
   assert(anon_page_get(out,i,false,false,&page)==K_OK);
   assert(page&&page->owner==PAGE_OWNER_ANON&&atomic_load(&page->refs)==1);
   const unsigned char *bytes=(const void *)(page+1);
   for(unsigned j=0;j<PAGE_SIZE;++j)assert(!bytes[j]);
  }
  vm_object_put(out);assert(!live&&!preempt);
 }
 /* 对象头、每个物理页和节点的失败，都必须回收所有已完成部分。 */
 for(unsigned failure=1;failure<=35;++failure){
  begin(failure);out=(void*)1;publication_probe=&out;
  assert(vm_object_create_shared_populated(17*PAGE_SIZE,&out)==K_ENOMEM);
  publication_probe=0;assert(out==(void*)1&&!live&&!disable_calls);
 }
 for(unsigned failure=1;failure<=17;++failure){
  begin(0);out=(void*)1;publication_probe=&out;fail_direct=failure;
  assert(vm_object_create_shared_populated(17*PAGE_SIZE,&out)==K_EIO);
  publication_probe=0;assert(out==(void*)1&&!live&&!disable_calls);
 }
 begin(0);out=(void*)1;
 assert(vm_object_create_shared_populated(0,&out)==K_EINVAL);
 assert(vm_object_create_shared_populated(1,&out)==K_EINVAL);
 assert(vm_object_create_shared_populated(PAGE_SIZE,0)==K_EINVAL);
 assert(!attempts&&out==(void*)1);
 puts("共享对象完整构造：4096 页清零与索引、构造阶段零存储锁、35 处分配失败和 17 处直接映射失败回收通过");
 /* 非空存储也只在最后一个对象引用消失时释放，外部页面引用必须继续有效。 */
 for(unsigned kind=0;kind<2;++kind){
  begin(0);unsigned before=releases,freed_before=pages_freed;
  assert(vm_object_create_anon_store(3*PAGE_SIZE,kind?VM_OBJECT_SHARED:VM_OBJECT_ANON,&out)==K_OK);
  assert(attempts==1&&live==1);
  anon_store_t *store=kind?out->u.shared.shared_root:out->u.anon.anon_root;
  page_t *retained_page=0;
  for(unsigned i=0;i<3;++i){
   anon_page_node_t *node=kzalloc(sizeof(*node),0);
   page_t *page=kzalloc(sizeof(*page),0);
   atomic_init(&page->refs,i==2?2U:1U);
   node->page=page;node->next=store->pages;store->pages=node;
   if(i==2)retained_page=page;
  }
  out->private_data=&releases;out->private_release=release_private;
  vm_object_get(out);vm_object_put(out);
  assert(live==7&&releases==before&&pages_freed==freed_before);
  vm_object_put(out);
  assert(live==1&&releases==before+1&&pages_freed==freed_before+2);
  assert(atomic_fetch_sub(&retained_page->refs,1)==1);
  page_free(retained_page);assert(!live&&pages_freed==freed_before+3);
 }
 uint64_t pages[4096];
 for(unsigned i=0;i<4096;++i) pages[i]=(uint64_t)(i+1)*PAGE_SIZE;
 const unsigned counts[]={1,2,17,4096};
 for(unsigned c=0;c<4;++c) for(unsigned failure=0;failure<3;++failure) {
  begin(failure); out=(void*)1; unsigned before=releases;
  unsigned count=counts[c];
  kstatus_t status=vm_object_create_page_array(pages,count,3,&releases,release_private,&out);
  assert(attempts==(failure==1?1:2));
  if(failure) { assert(status==K_ENOMEM && out==(void*)1 && !live && releases==before); continue; }
  assert(status==K_OK && live==2); stats(out,(uint64_t)count*PAGE_SIZE,VM_OBJECT_PAGE_ARRAY);
  assert(out->u.page_array.page_count==count && out->u.page_array.cache_mode==3);
  assert(out->u.page_array.pages!=pages && !memcmp(out->u.page_array.pages,pages,count*sizeof(*pages)));
  uint64_t saved=pages[count-1];pages[count-1]=0;
  assert(out->u.page_array.pages[count-1]==saved);pages[count-1]=saved;
  vm_object_put(out);assert(!live && releases==before+1);
 }
 /* 保持验证和错误优先级不变，包括在发现物理页面未对齐之前发生分配失败。 */
 for(unsigned bad=0;bad<17;++bad) for(unsigned failure=0;failure<3;++failure) {
  begin(failure);out=(void*)1;unsigned before=releases;pages[bad]++;
  assert(vm_object_create_page_array(pages,17,0,&releases,release_private,&out)==
         (failure?K_ENOMEM:K_EINVAL));
  assert(attempts==(failure==1?1:2) && !live && releases==before && out==(void*)1);
  pages[bad]--;
 }
 begin(0);out=(void*)1;
 assert(vm_object_create_page_array(0,1,0,0,0,&out)==K_EINVAL);
 assert(vm_object_create_page_array(pages,0,0,0,0,&out)==K_EINVAL);
 assert(vm_object_create_page_array(pages,UINT64_MAX/PAGE_SIZE+1,0,0,0,&out)==K_EINVAL);
 assert(vm_object_create_page_array(pages,1,0,0,0,0)==K_EINVAL);
 assert(!attempts && !live && out==(void*)1);
 /* 构造函数失败应先于读取物理地址输入。使用真实的不可访问页面，避免提前验证/复制
  * 未被察觉地通过测试。 */
 assert(sysconf(_SC_PAGESIZE)==PAGE_SIZE);
 unsigned char *guard=mmap(0,2*PAGE_SIZE,PROT_READ|PROT_WRITE,
                           MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
 assert(guard!=MAP_FAILED && !mprotect(guard+PAGE_SIZE,PAGE_SIZE,PROT_NONE));
 for(unsigned c=0;c<4;++c)for(unsigned failure=1;failure<=2;++failure){
  begin(failure);out=(void*)1;unsigned before=releases;
  assert(vm_object_create_page_array((const uint64_t*)(guard+PAGE_SIZE),counts[c],
          0,&releases,release_private,&out)==K_ENOMEM);
  assert(attempts==failure && !live && out==(void*)1 && releases==before);
 }
 /* 源数据恰好在保护页处结束：不得推测性读取额外输入项，不得产生复制存储别名，也不得
  * 转移格式错误尾部的所有权。 */
 const unsigned edge_counts[]={1,17,PAGE_SIZE/sizeof(uint64_t)};
 for(unsigned c=0;c<3;++c){
  unsigned count=edge_counts[c];
  uint64_t *edge=(uint64_t*)(guard+PAGE_SIZE)-count;
  for(unsigned i=0;i<count;++i)edge[i]=(uint64_t)(i+1)*PAGE_SIZE;
  begin(0);out=(void*)1;unsigned before=releases;
  assert(vm_object_create_page_array(edge,count,7,&releases,release_private,&out)==K_OK);
  assert(attempts==2 && live==2 && out->u.page_array.pages!=edge);
  assert(!memcmp(out->u.page_array.pages,edge,count*sizeof(*edge)));
  vm_object_put(out);assert(!live && releases==before+1);
  ++edge[count-1];begin(0);out=(void*)1;before=releases;
  assert(vm_object_create_page_array(edge,count,7,&releases,release_private,&out)==K_EINVAL);
  assert(attempts==2 && !live && out==(void*)1 && releases==before);
 }
 assert(!munmap(guard,2*PAGE_SIZE));
 puts("VM page arrays: eight OOM/protected-input cases and six exact-boundary/invalid-tail cases preserve output and ownership");
 const vm_file_ops_t ops={.retain=retain_mapping,.release=release_mapping,.page_get=get_file_page};
 for(unsigned failure=0;failure<2;++failure) {
  begin(failure);out=(void*)1;unsigned before=mapping_retains;
  assert(vm_object_create_file(&mapping_refs,&ops,3*PAGE_SIZE,PAGE_SIZE,2*PAGE_SIZE,&out)==
         (failure?K_ENOMEM:K_OK));
  assert(attempts==1);
  if(failure) { assert(out==(void*)1 && !live && mapping_retains==before); continue; }
  stats(out,2*PAGE_SIZE,VM_OBJECT_FILE);
  assert(out->u.file.mapping==&mapping_refs && out->u.file.ops==&ops);
  assert(out->u.file.file_offset==PAGE_SIZE && mapping_refs==1 && mapping_retains==before+1);
  assert(!out->private_data && !out->private_release);
  vm_object_get(out);assert(atomic_load(&out->refs.value)==2);
  vm_object_put(out);assert(live==1 && mapping_refs==1);
  vm_object_put(out);assert(!live && !mapping_refs && mapping_retains==mapping_releases);
 }
 const uint64_t lengths[]={PAGE_SIZE,17*PAGE_SIZE,UINT64_MAX & ~(PAGE_SIZE-1ULL)};
 for(unsigned i=0;i<3;++i) for(unsigned failure=0;failure<2;++failure) {
  begin(failure);out=(void*)1;unsigned before=releases;
  assert(vm_object_create_device((paddr_t){0},lengths[i],7,&releases,release_private,&out)==
         (failure?K_ENOMEM:K_OK));
  assert(attempts==1);
  if(failure) { assert(out==(void*)1 && !live && releases==before); continue; }
  stats(out,lengths[i],VM_OBJECT_DEVICE);
  assert(!out->u.device.phys.value && out->u.device.length==lengths[i] && out->u.device.cache_mode==7);
  vm_object_get(out);vm_object_put(out);assert(live==1 && releases==before);
  vm_object_put(out);assert(!live && releases==before+1);
 }
 begin(0);out=(void*)1;
 vm_file_ops_t invalid=ops;invalid.page_get=0;
 assert(vm_object_create_file(&mapping_refs,&invalid,PAGE_SIZE,0,PAGE_SIZE,&out)==K_EINVAL);
 assert(vm_object_create_file(&mapping_refs,&ops,PAGE_SIZE,1,PAGE_SIZE,&out)==K_EINVAL);
 assert(vm_object_create_file(&mapping_refs,&ops,PAGE_SIZE,2*PAGE_SIZE,PAGE_SIZE,&out)==K_EINVAL);
 assert(vm_object_create_file(&mapping_refs,&ops,PAGE_SIZE,0,1,&out)==K_EINVAL);
 assert(vm_object_create_device((paddr_t){1},PAGE_SIZE,0,0,0,&out)==K_EINVAL);
 assert(vm_object_create_device((paddr_t){PAGE_SIZE},lengths[2],0,0,0,&out)==K_EINVAL);
 assert(vm_object_create_device((paddr_t){0},1,0,0,0,&out)==K_EINVAL);
 assert(!attempts && !live && out==(void*)1 && !mapping_refs);
 puts("VM object creation: single-allocation anonymous/shared backing and nonempty final-reference cleanup, all five object types initialized, first-allocation OOM skips dependent allocation; rollback/output/ownership preserved; shared alignment, independent copies, retained file mapping, final-reference release and validation/error precedence passed");
}
'''
with tempfile.TemporaryDirectory(prefix="system-object-create-") as directory:
    binary = str(Path(directory) / "test")
    subprocess.run([
        "cc", "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
        "-ffunction-sections", "-fdata-sections", "-fsanitize=address,undefined",
        "-fno-pie", "-no-pie", "-Wl,--gc-sections",
        "-I"+str(root/"include"), "-I"+str(root/"kernel/mm"),
        "-x", "c", "-", str(root/"kernel/mm/object.c"),
        str(root/"kernel/mm/shared.c"), "-o", binary,
    ], input=code, text=True, check=True)
    subprocess.run([binary], check=True, timeout=30)
