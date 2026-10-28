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
void *kmalloc(size_t size, uint32_t flags) {
 (void)flags;
 if (++attempts == fail_at) return 0;
 void *p = malloc(size); assert(p); memset(p, 0xa5, size); ++live; return p;
}
void *kzalloc(size_t size, uint32_t flags) {
 void *p = kmalloc(size, flags); if (p) memset(p, 0, size); return p;
}
void kfree(void *p) { if (p) { assert(live); --live; free(p); } }
void page_free(page_t *page) { (void)page; assert(!"empty objects own no pages"); }
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
static void begin(unsigned failure) { assert(!live); attempts=0; fail_at=failure; }
static void stats(vm_object_t *object, uint64_t size, uint32_t type) {
 assert(object->type == type && object->size == size);
 assert(object->flags == VM_OBJECT_INTERNAL && atomic_load(&object->refs.value) == 1);
 assert(!vm_object_fault_count(object) && !vm_object_populated_pages(object));
 assert(!vm_object_prefaulted_pages(object) && !vm_object_fault_around_pages(object));
}
int main(void) {
 const size_t sizes[] = {1, PAGE_SIZE, SIZE_MAX};
 for (unsigned i=0; i<3; ++i) for (unsigned failure=0; failure<3; ++failure) {
  begin(failure); vm_object_t *out=(void*)1;
  kstatus_t status=vm_object_create_anon(sizes[i], &out);
  assert(attempts == (failure==1 ? 1 : 2));
  if (failure) { assert(status==K_ENOMEM && out==(void*)1 && !live); continue; }
  assert(status==K_OK && live==2); stats(out,sizes[i],VM_OBJECT_ANON);
  anon_store_t *store=out->u.anon.anon_root;
  assert(store && !store->pages && !store->page_index.root && !atomic_load(&store->lock.state));
  vm_object_put(out); assert(!live);
 }
 begin(0); vm_object_t *out=(void*)1;
 assert(vm_object_create_anon(0,&out)==K_EINVAL && out==(void*)1);
 assert(vm_object_create_anon(PAGE_SIZE,0)==K_EINVAL && !attempts);
 const size_t shared_sizes[]={PAGE_SIZE,17*PAGE_SIZE,SIZE_MAX & ~(PAGE_SIZE-1ULL)};
 for(unsigned i=0;i<3;++i) for(unsigned failure=0;failure<3;++failure) {
  begin(failure);out=(void*)1;
  assert(vm_object_create_shared(shared_sizes[i],&out)==(failure?K_ENOMEM:K_OK));
  assert(attempts==(failure==1?1:2));
  if(failure) { assert(out==(void*)1 && !live);continue; }
  stats(out,shared_sizes[i],VM_OBJECT_SHARED);
  anon_store_t *store=out->u.shared.shared_root;
  assert(store && !store->pages && !store->page_index.root && !atomic_load(&store->lock.state));
  vm_object_get(out);vm_object_put(out);assert(live==2);
  vm_object_put(out);assert(!live);
 }
 begin(0);out=(void*)1;
 assert(vm_object_create_shared(0,&out)==K_EINVAL);
 assert(vm_object_create_shared(1,&out)==K_EINVAL);
 assert(vm_object_create_shared(PAGE_SIZE,0)==K_EINVAL);
 assert(vm_object_create_anon_store(PAGE_SIZE,VM_OBJECT_FILE,&out)==K_EINVAL);
 assert(!attempts && !live && out==(void*)1);
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
 /* Keep validation and error precedence unchanged, including allocation
  * failure before discovering an unaligned physical page. */
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
 /* Constructor failures precede reading the physical-address input. Use a
  * real inaccessible page so an eager validation/copy cannot pass unnoticed. */
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
 /* End the source exactly at a protected page: no speculative extra input
  * element, copied-storage alias or malformed-tail ownership transfer. */
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
 puts("VM object creation: common anonymous/shared backing, all five object types initialized, first-allocation OOM skips dependent allocation; rollback/output/ownership preserved; shared alignment, independent copies, retained file mapping, final-reference release and validation/error precedence passed");
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
