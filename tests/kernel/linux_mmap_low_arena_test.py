"""Real Linux hint policy + actual VMA tree/map retain the MAP_32BIT arena."""
from pathlib import Path
import subprocess
import tempfile
import resource

root = Path(__file__).resolve().parents[2]
source = (root/'kernel/syscall/linux.c').read_text()
begin = source.index('static int64_t linux_mmap(')
end = source.index('    if ((flags & LINUX_MAP_ANONYMOUS) == 0U)', begin)
adapter = source[begin:end] + r'''
    (void)object;(void)entry;
    status=vm_map_object(process,0,&address,0,map_size,vm_prot,vm_flags);
    return status==K_OK?(int64_t)address:status;
}
'''
program = r'''
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include "internal.h"
#include <uapi/linux_abi.h>
#define LINUX_MAP_32BIT 0x40U
#define LINUX_MAP_NORESERVE 0x4000U
typedef vm_space_t process_t;
typedef struct {int dummy;} linux_fd_entry_t;
static bool held;
bool x86_is_canonical(uint64_t a){return a<=X86_64_USER_TOP;}
void *kzalloc(size_t size,uint32_t flags){(void)flags;return calloc(1,size);}
void kfree(void *p){free(p);}
void map_lock(vm_space_t *s){assert(s&&!held);held=true;}
void map_unlock(vm_space_t *s){assert(s&&held);held=false;}
void vm_object_get(vm_object_t *o){if(o)atomic_fetch_add(&o->refs.value,1);}
void vm_object_put(vm_object_t *o){if(o&&atomic_fetch_sub(&o->refs.value,1)==1)free(o);}
kstatus_t vm_object_create_anon(size_t size,vm_object_t **out){
 *out=calloc(1,sizeof(**out));assert(*out);refcount_init(&(*out)->refs,1);
 (*out)->size=size;(*out)->type=VM_OBJECT_ANON;return K_OK;
}
void area_release(vm_area_t *a){vm_object_put(a->object);vm_object_put(a->private_object);free(a);}
kstatus_t vm_tlb_unmap_range(vm_space_t *s,uint64_t first,uint64_t end){assert(s&&held&&first<end);return K_OK;}
void vm_tlb_bump_generation(vm_space_t *s){atomic_fetch_add(&s->tlb_generation,1);}
kstatus_t anon_page_retire_range(vm_object_t *o,uint64_t first,uint64_t end,anon_page_node_t **out){
 assert(held&&o&&first<end&&out);return K_OK;
}
void anon_page_release_retired(anon_page_node_t *p){assert(!held&&!p);}
''' + adapter + r'''
int main(void){
 vm_space_t space={0};list_init(&space.area_list);
 uint64_t anon=LINUX_MAP_PRIVATE|LINUX_MAP_ANONYMOUS;
 // Sparse reservations must not hide low addresses needed for thread stacks.
 for(unsigned i=0;i<16;++i){
  int64_t result=linux_mmap(&space,0,1ULL<<36,0,anon|LINUX_MAP_NORESERVE,-1,0);
  assert(result>=0x100000000LL);
 }
 for(unsigned i=0;i<64;++i){
  int64_t result=linux_mmap(&space,0,0xabf40,3,anon|LINUX_MAP_32BIT,-1,0);
  assert(result>=0x10000 && (uint64_t)result+0xac000<=0x80000000ULL);
 }
 assert(linux_mmap(&space,0x20000000,4096,3,anon,-1,0)==0x20000000);
 assert(linux_mmap(&space,0x30000000,4096,3,anon|LINUX_MAP_FIXED,-1,0)==0x30000000);
 assert(vm_unmap(&space,VM_USER_BASE,VM_USER_END-VM_USER_BASE)==K_OK);
 assert(list_empty(&space.area_list)&&!space.areas.root&&!held);
 puts("Linux mmap: 16 x 64GiB sparse reservations plus 64 real low-32-bit stack maps, explicit hints/fixed addresses and complete teardown PASS");
}
'''
with tempfile.TemporaryDirectory(prefix='linux-mmap-low-arena-') as directory:
    path = Path(directory)
    command = ['cc','-O2','-std=gnu11','-Wall','-Wextra','-Werror','-Wno-unused-parameter',
               '-fsanitize=address,undefined','-fno-pie','-no-pie',
               '-I'+str(root/'include'),'-I'+str(root/'kernel/mm'),
               str(path/'test.c'),str(root/'kernel/mm/map.c'),str(root/'kernel/mm/vma.c'),
               '-o',str(path/'test')]
    (path/'test.c').write_text(program)
    subprocess.run(command,check=True)
    subprocess.run([str(path/'test')],check=True)
    previous=program.replace('address = 0x100000000ULL;', 'address = 0U;')
    assert previous!=program
    (path/'test.c').write_text(previous)
    subprocess.run(command,check=True)
    def no_core():resource.setrlimit(resource.RLIMIT_CORE,(0,0))
    result=subprocess.run([str(path/'test')],capture_output=True,preexec_fn=no_core)
    assert result.returncode!=0 and b'Assertion' in result.stderr
    print('Negative control: normal reservations starting in the low arena fail PASS')
