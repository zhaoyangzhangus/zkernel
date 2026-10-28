"""执行真实锁外缺页段，验证取页固定后的所有退出路径只释放一次。"""
from pathlib import Path
import resource
import subprocess
import tempfile

root=Path(__file__).resolve().parents[2]
source=(root/'kernel/mm/fault.c').read_text()
body=source[source.index('    vm_area_t area_snapshot = *area;'):
            source.index('\nkstatus_t vm_handle_fault(vm_space_t *space,')]
# 函数尾的返回由夹具补上 retry 标签；生产分支及清理顺序完整保留。
assert body.endswith('}\n')
body=body[:-2]
program=r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <kernel/vm.h>
#include <arch/x86_64/paging.h>
typedef struct anon_page_node { unsigned unused; } anon_page_node_t;
static vm_space_t vm;
static vm_object_t backing,shadow;
static vm_area_t region;
static page_t result_page;
static anon_page_node_t retired_node;
static unsigned held,kind,mode,get_calls,pin_calls,unpins,retired_count,object_puts,maps;
static bool retried;
static void map_lock(vm_space_t *s) {
    assert(!held && s==&vm);held=1;
    if(mode==3)atomic_fetch_add(&s->advice_generation,1);
    if(mode==5)region.prot=VM_PROT_READ;
}
static void map_unlock(vm_space_t *s) {assert(held && s==&vm);held=0;}
void vm_object_get(vm_object_t *o) {assert(held);if(o)atomic_fetch_add(&o->refs.value,1);}
void vm_object_put(vm_object_t *o) {
    assert(!held);if(o){assert(atomic_fetch_sub(&o->refs.value,1)==2);++object_puts;}
}
static kstatus_t anon_page_get(vm_object_t *o,uint64_t index,bool create,bool pin,page_t **out) {
    assert(!held && o==&backing && index==0 && create && pin);++get_calls;
    if(mode==6)return K_ENOMEM;
    atomic_fetch_add(&result_page.refs,1);*out=&result_page;return K_OK;
}
static bool anon_page_pin(vm_object_t *o,uint64_t index,page_t *p) {
    assert(!held && index==0 && p==&result_page && o==&shadow);++pin_calls;
    assert(atomic_fetch_add(&p->refs,1)==1);return true;
}
static void anon_page_unpin(page_t *p) {
    assert(!held && p==&result_page);assert(atomic_fetch_sub(&p->refs,1)>0);++unpins;
}
static page_t *anon_page_lookup(vm_object_t *o,uint64_t index) {
    assert(!held && o==&shadow && index==0);return kind==3 || kind==4?&result_page:0;
}
static kstatus_t file_get(void *mapping,uint64_t index,page_t **out) {
    assert(!held && mapping==&backing && index==0);
    if(mode==6)return K_EIO;
    *out=&result_page;return K_OK;
}
static kstatus_t private_file_shadow_page(vm_area_t *a,uint64_t index,paddr_t src,page_t **out) {
    assert(!held && a->private_object==&shadow && index==0 && src.value==0x20000);
    *out=&result_page;return K_OK;
}
static vm_area_t *find_area(vm_space_t *s,uint64_t address) {
    assert(held && s==&vm && address==region.start);return mode==2?0:&region;
}
static kstatus_t anon_page_retire_range(vm_object_t *o,uint64_t start,uint64_t end,anon_page_node_t **out) {
    assert(held && (o==&backing || o==&shadow) && !start && end==1 && !*out);
    *out=&retired_node;++retired_count;return K_OK;
}
static void anon_page_release_retired(anon_page_node_t *p) {
    assert(!held);if(p){assert(p==&retired_node);assert(atomic_fetch_sub(&result_page.refs,1)==2);}
}
kstatus_t x86_translate_page(paddr_t root,vaddr_t va,paddr_t *physical,uint64_t *flags) {
    (void)root;(void)physical;(void)flags;assert(held && va==region.start);
    return mode==4?K_OK:K_ENOENT;
}
static uint32_t vm_area_page_flags(const vm_area_t *a,uint64_t va,uint32_t prot) {
    assert(held && a==&region && va==a->start);return prot;
}
paddr_t page_to_phys(const page_t *p) {assert(p==&result_page);return paddr_make(0x20000);}
kstatus_t x86_map_page(paddr_t root,vaddr_t va,paddr_t physical,uint32_t flags,enum x86_cache_mode cache) {
    (void)root;(void)flags;(void)cache;assert(held && va==region.start && physical.value==0x20000);
    ++maps;return mode==1?K_ENOMEM:K_OK;
}
static uint64_t telemetry_timestamp(void) {return 0;}
static void vm_write_repair_diag(const char *s,const vm_fault_info_t *f,const vm_area_t *a,
                                 uint64_t va,uint64_t idx,uint64_t flags,kstatus_t status) {
    (void)s;(void)f;(void)a;(void)va;(void)idx;(void)flags;assert(status==K_ENOMEM);
}
static kstatus_t fault_transaction(vm_space_t *space,const vm_fault_info_t *fault) {
    vm_area_t *area=&region;
    struct {uint64_t backing_ready_tsc,pte_ready_tsc;} *trace=0;
    uint32_t access=fault->access;
    uint64_t object_index=0,page_address=region.start,existing_flags=0;
    paddr_t existing={0};page_t *page=0;
    bool private_file=kind>=3;
''' + body + r'''
retry_fault:
    assert(!held);retried=true;return K_EAGAIN;
}
int main(void) {
    static const vm_file_ops_t file_ops={.page_get=file_get};
    for(kind=0;kind<7;++kind)for(mode=0;mode<7;++mode) {
        memset(&vm,0,sizeof(vm));memset(&region,0,sizeof(region));
        memset(&backing,0,sizeof(backing));memset(&shadow,0,sizeof(shadow));
        memset(&result_page,0,sizeof(result_page));atomic_init(&result_page.refs,1);
        refcount_init(&backing.refs,1);refcount_init(&shadow.refs,1);
        backing.type=kind==0?VM_OBJECT_ANON:kind==1?VM_OBJECT_SHARED:VM_OBJECT_FILE;
        backing.size=PAGE_SIZE;shadow.type=VM_OBJECT_ANON;
        if(kind==0)backing.flags=VM_OBJECT_FLAG_MAP_OWNED;
        shadow.flags=VM_OBJECT_FLAG_MAP_OWNED;
        if(kind>=2){backing.u.file.ops=&file_ops;backing.u.file.mapping=&backing;}
        region.start=0x10000;region.end=region.start+PAGE_SIZE;
        region.object=&backing;region.private_object=kind>=3?&shadow:0;
        region.prot=VM_PROT_READ|VM_PROT_WRITE;region.flags=kind>=3?VM_MAP_PRIVATE:VM_MAP_SHARED;
        vm_fault_info_t fault={.address=region.start,.access=kind==4 || kind==5?VM_PROT_WRITE:VM_PROT_READ};
        held=1;get_calls=pin_calls=unpins=retired_count=object_puts=maps=0;retried=false;
        kstatus_t status=fault_transaction(&vm,&fault);
        bool failed=mode==6 && kind!=3 && kind!=4;
        bool pinned=kind<2 || kind==4 || kind==5;
        bool retry=!failed && mode>=2 && mode<=5;
        assert(status==(failed?(kind<2?K_ENOMEM:K_EIO):retry?K_EAGAIN:mode==1?K_ENOMEM:K_OK));
        assert(!held && retried==retry);
        assert(get_calls==(kind<2) && pin_calls==(!failed && (kind==4 || kind==5)));
        assert(unpins==(!failed && pinned));
        assert(maps==(!failed && !retry));
        assert(object_puts==(kind>=3?2:1));
        assert(atomic_load(&result_page.refs)==1-retired_count);
        assert(atomic_load(&backing.refs.value)==1 && atomic_load(&shadow.refs.value)==1);
    }
    puts("锁外缺页：49 条匿名/共享/文件路径，成功、MMU 失败、取消映射、权限/建议变化、并发映射及取页失败均无重复固定或引用泄漏");
}
'''
with tempfile.TemporaryDirectory(prefix='fault-backing-pin-') as directory:
    source=Path(directory)/'test.c';binary=Path(directory)/'test'
    command=['cc','-std=c11','-O1','-Wall','-Wextra','-Werror',
             '-fsanitize=address,undefined','-fno-pie','-no-pie',
             '-I'+str(root/'include'),str(source),'-o',str(binary)]
    source.write_text(program);subprocess.run(command,check=True)
    subprocess.run([str(binary)],check=True)
    for anchor,replacement in [
        ('page_owner != 0 && !page_pinned','page_owner != 0'),
        ('page_pinned = status == K_OK;', 'page_pinned = false;'),
    ]:
        assert anchor in program
        source.write_text(program.replace(anchor,replacement));subprocess.run(command,check=True)
        result=subprocess.run([str(binary)],capture_output=True,
            preexec_fn=lambda:resource.setrlimit(resource.RLIMIT_CORE,(0,0)))
        assert result.returncode!=0 and b'Assertion' in result.stderr
    print('负面对照通过：重复固定和遗漏引用所有权均被拒绝')
