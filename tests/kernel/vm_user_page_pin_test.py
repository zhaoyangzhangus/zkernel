"""Execute the actual VM pin adapter with guarded backing-store mocks."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'kernel/mm/shared.c').read_text()
begin = source.index('kstatus_t vm_pin_user_page(')
end = source.index('/* REFACTOR_', begin)
object_source = (root / 'kernel/mm/object.c').read_text()
put_begin = object_source.index('static void page_reference_put(')
put_end = object_source.index('void vm_object_get(', put_begin)
unpin_begin = object_source.index('void anon_page_unpin(')
unpin_end = object_source.index('}', unpin_begin) + 1
harness = r'''
#include <stdint.h>
#include <stdbool.h>
#include <assert.h>
#include <stdatomic.h>
#define PAGE_SIZE 4096ULL
#define PAGE_SHIFT 12
#define VM_USER_BASE 0x10000ULL
#define VM_USER_END 0x800000000000ULL
#define VM_PROT_READ 1
#define VM_PROT_WRITE 2
#define VM_OBJECT_ANON 1
#define VM_OBJECT_SHARED 2
#define K_OK 0
#define K_EINVAL -1
#define K_EACCES -2
#define K_ENOSYS -3
#define K_EIO -4
#define K_ENOENT -5
typedef int kstatus_t;
typedef uint64_t vaddr_t, paddr_t;
typedef struct {atomic_uint refs;} page_t;
typedef struct {int type;} vm_object_t;
typedef struct {uint64_t start,object_offset; unsigned prot; vm_object_t *object;} vm_area_t;
typedef struct {int root_table;} vm_space_t;
typedef struct {vaddr_t address;unsigned access,cpu_error;} vm_fault_info_t;
static bool locked, present=true, writable_pte=true, owner=true, found=true;
static unsigned pins,freed,faults;
static int fault_status,translate_status;
static bool repair_fault,two_step_cow;
static unsigned fault_action,locks;
static page_t page={1};
static vm_object_t object={VM_OBJECT_SHARED};
static vm_area_t area={0x10000,4096,3,&object};
static kstatus_t vm_handle_fault(vm_space_t *s,const vm_fault_info_t *f) {
 assert(s && !locked && f->address==0x12000 && f->cpu_error==0);
 assert(f->access==VM_PROT_READ || f->access==VM_PROT_WRITE);++faults;
 if(!fault_status && repair_fault) {
  if(two_step_cow && translate_status==K_ENOENT){
   translate_status=0;writable_pte=false;return 0;
  }
  translate_status=0;writable_pte=true;
  if(fault_action==1)found=false;
  if(fault_action==2)area.prot=1;
  if(fault_action==3)object.type=99;
  if(fault_action==4)owner=false;
 }
 return fault_status;
}
static void map_lock(vm_space_t *s) {assert(s && !locked);locked=true;++locks;}
static void map_unlock(vm_space_t *s) {assert(s && locked);locked=false;}
static vm_area_t *find_area(vm_space_t *s,uint64_t address) {assert(s && locked && address==0x12000);return found?&area:0;}
static kstatus_t x86_translate_page(int root,uint64_t address,paddr_t *p,uint64_t *flags) {
 (void)root;assert(locked && address==0x12000);*p=42;*flags=writable_pte;return translate_status;
}
static bool x86_page_entry_writable(uint64_t f) {return f!=0;}
static page_t *phys_to_page(paddr_t p) {assert(locked && p==42);return present?&page:0;}
static bool anon_page_pin(vm_object_t *o,uint64_t index,page_t *p) {
 assert(locked && o==&object && index==3 && p==&page);++pins;
 if(owner)++p->refs;
 return owner;
}
static void page_free(page_t *p) {assert(p==&page && p->refs==0);++freed;}
''' + object_source[put_begin:put_end] + object_source[unpin_begin:unpin_end] + source[begin:end] + r'''
int main(void) {
 vm_space_t s={0};page_t *out=(void*)1;
 assert(vm_pin_user_page(&s,0x12000,true,&out)==0 && out==&page && page.refs==2);
 assert(!faults && locks==1); /* Already resident: no PF or second map guard. */
 page.refs--; /* VMA/object teardown after pin; only pin remains. */
 vm_unpin_user_page(out);assert(page.refs==0 && freed==1);
 page.refs=1;
 for(unsigned mode=0;mode<8;++mode) {
  found=mode!=0;area.prot=mode==1?1:3;object.type=mode==2?99:VM_OBJECT_SHARED;
  translate_status=mode==3?K_EIO:mode==7?K_ENOENT:0;writable_pte=mode!=4;
  present=mode!=5;owner=mode!=6;fault_status=mode==7?K_EIO:0;
  unsigned before=pins;out=(void*)1;
  assert(vm_pin_user_page(&s,0x12000,true,&out)!=0 && !out && !locked && page.refs==1);
  assert(pins==before+(mode==6));
 }
 fault_status=translate_status=0;found=present=owner=true;object.type=VM_OBJECT_ANON;
 area.prot=1;writable_pte=false;
 assert(vm_pin_user_page(&s,0x12000,false,&out)==0 && out==&page);
 vm_unpin_user_page(out);assert(page.refs==1);
 /* 只有缺失页/COW 页会触发故障，且必须重新验证处理后的 VMA。 */
 repair_fault=true;area.prot=3;
 for(unsigned cow=0;cow<2;++cow) {
  translate_status=cow?0:K_ENOENT;writable_pte=false;
  unsigned old_faults=faults,old_locks=locks;
  assert(vm_pin_user_page(&s,0x12000,true,&out)==0 && out==&page && page.refs==2);
  assert(faults==old_faults+1 && locks==old_locks+2);
  vm_unpin_user_page(out);
 }
 for(fault_action=1;fault_action<=4;++fault_action) {
  found=owner=true;area.prot=3;object.type=VM_OBJECT_ANON;
  translate_status=K_ENOENT;unsigned old_faults=faults;
  assert(vm_pin_user_page(&s,0x12000,true,&out)!=0 && !out && page.refs==1 && !locked);
  assert(faults==old_faults+1);
 }
 fault_action=0;found=owner=true;area.prot=3;object.type=VM_OBJECT_ANON;
 translate_status=K_ENOENT;writable_pte=false;two_step_cow=true;
 unsigned cow_faults=faults,cow_locks=locks;
 assert(vm_pin_user_page(&s,0x12000,true,&out)==0&&out==&page);
 assert(faults==cow_faults+2&&locks==cow_locks+3&&page.refs==2);
 vm_unpin_user_page(out);two_step_cow=false;
 /* 故障处理错误或重复时，不能造成无界重试循环。 */
 repair_fault=false;translate_status=K_ENOENT;unsigned repeated_faults=faults;
 assert(vm_pin_user_page(&s,0x12000,true,&out)!=0&&!out);
 assert(faults==repeated_faults+2&&!locked&&page.refs==1);
 repair_fault=true;translate_status=0;writable_pte=true;
 fault_action=0;found=owner=true;area.prot=3;object.type=VM_OBJECT_SHARED;
 unsigned old_faults=faults;
 for(unsigned i=0;i<64;++i)assert(vm_pin_user_page(&s,0x12000,true,&out)==0);
 assert(faults==old_faults && page.refs==65);
 for(unsigned i=0;i<64;++i)vm_unpin_user_page(&page);
 assert(page.refs==1);
 unsigned before=faults;
 assert(vm_pin_user_page(&s,0x12001,true,&out)==K_EINVAL && !out);
 assert(vm_pin_user_page(0,0x12000,true,&out)==K_EINVAL && !out);
 assert(vm_pin_user_page(&s,0x12000,true,0)==K_EINVAL);
 assert(faults==before);
 vm_unpin_user_page(0);
 assert(freed==1); /* Retained owner reference must never be freed by unpin. */
}
'''
with tempfile.TemporaryDirectory(prefix='vm-user-pin-') as directory:
    base = Path(directory)
    (base / 'test.c').write_text(harness)
    subprocess.run(['cc', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                    str(base / 'test.c'), '-o', str(base / 'test')], check=True)
    subprocess.run([str(base / 'test')], check=True)
print('VM user pin: resident fast path with zero faults, one guard per page, missing/COW resolution, bounded retry, raced unmap/protect/owner rejection and balanced lifetimes passed')
