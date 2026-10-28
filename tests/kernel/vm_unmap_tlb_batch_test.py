"""使用实际 VMA 事务和稀疏页表批处理验证跨 VMA 合并、确认失败及重试。"""
from pathlib import Path
import resource
import runpy
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
fixture = runpy.run_path(str(root/'tests/kernel/vm_unmap_seek_test.py'))['program']
fixture = fixture[:fixture.index('int main(void)')]
fixture = fixture.replace('initial[40],removed[40]', 'initial[128],removed[128]')
start = fixture.index('static kstatus_t vm_tlb_unmap_range(')
end = fixture.index('static void vm_tlb_bump_generation(', start)
tlb = (root/'kernel/mm/tlb.c').read_text()
tlb = tlb[tlb.index('static void rss_decrement('):tlb.index('void vm_tlb_bump_generation(')]
mocks = r'''
#include <arch/x86_64/paging.h>
static unsigned batches, fail_batch, extent;
kstatus_t x86_next_present_page(paddr_t table,vaddr_t first,vaddr_t end,vaddr_t *out) {
    assert(table.value==1 && locked);
    for(uint64_t address=first;address<end;address+=PAGE_SIZE) {
        unsigned i=(address-VM_USER_BASE)/PAGE_SIZE;assert(i<extent);
        if(initial[i] && !removed[i]) { *out=address;return K_OK; }
    }
    return K_ENOENT;
}
kstatus_t x86_unmap_user_page_batch(paddr_t table,const vaddr_t *addresses,
                                   size_t count,size_t *unmapped) {
    assert(table.value==1 && locked && count && count<=X86_USER_UNMAP_BATCH_MAX);
    ++batches;*unmapped=0;
    if(batches==fail_batch)return K_EIO;
    for(size_t n=0;n<count;++n) {
        unsigned i=(addresses[n]-VM_USER_BASE)/PAGE_SIZE;
        assert(i<extent && initial[i] && !removed[i]);
        if(n)assert(addresses[n]>addresses[n-1]);
        removed[i]=true;++*unmapped;
    }
    return K_OK;
}
'''
# 统计实际范围调用，不替代其中的驻留扫描、批次边界和 RSS 记账。
tlb = tlb.replace('    if (space == 0 || start >= end)',
                  '    ++tlb_calls;\n    if (space == 0 || start >= end)')
fixture = fixture[:start]+mocks+tlb+fixture[end:]
main = r'''
static void setup(unsigned count,unsigned stride,bool resident) {
    prepare(0);
    memset(areas,0,sizeof(areas));memset(live,0,sizeof(live));
    memset(initial,0,sizeof(initial));memset(removed,0,sizeof(removed));
    list_init(&space.area_list);space.areas.root=0;
    allocated=count;extent=count*stride;assert(extent<=128);
    atomic_store(&space.commit_pages,count);
    atomic_store(&space.rss_pages,resident?count:0);
    atomic_store(&objects[0].refs.value,count);
    for(unsigned i=0;i<count;++i) {
        vm_area_t *area=&areas[i];live[i]=true;
        area->start=VM_USER_BASE+i*stride*PAGE_SIZE;area->end=area->start+PAGE_SIZE;
        area->object=&objects[0];area->prot=VM_PROT_READ;area->flags=VM_MAP_PRIVATE;
        list_add_tail(&space.area_list,&area->ordered_node);
        initial[i*stride]=resident;
    }
    rebuild_area_tree(&space);space.root_table=paddr_make(1);
    batches=fail_batch=tlb_calls=bumps=rebuilds=0;
}
static kstatus_t unmap_all(void) {
    return vm_unmap(&space,VM_USER_BASE,extent*PAGE_SIZE);
}
int main(void) {
    for(unsigned count=1;count<=128;++count) {
        setup(count,1,true);
        assert(unmap_all()==K_OK && !locked);
        assert(tlb_calls==1 && batches==(count+63)/64);
        assert(!atomic_load(&space.rss_pages) && !atomic_load(&space.commit_pages));
        assert(!space.areas.root && list_empty(&space.area_list));
        assert(!atomic_load(&objects[0].refs.value));
    }
    puts("连续 64 个单页 VMA：一次范围提交、一次 TLB 确认批次通过");
    setup(64,2,true);assert(unmap_all()==K_OK && tlb_calls==64 && batches==64);
    setup(64,1,false);assert(unmap_all()==K_OK && tlb_calls==1 && !batches);
    for(unsigned failure=1;failure<=2;++failure) {
        setup(65,1,true);fail_batch=failure;
        assert(unmap_all()==K_EIO && !locked && bumps==1);
        assert(atomic_load(&space.commit_pages)==65);
        assert(atomic_load(&objects[0].refs.value)==65);
        assert(atomic_load(&space.rss_pages)==(failure==1?65:1));
        for(unsigned i=0;i<65;++i) {
            assert(live[i] && find_area(&space,VM_USER_BASE+i*PAGE_SIZE)==&areas[i]);
            assert(removed[i]==(failure==2 && i<64));
        }
        fail_batch=0;batches=tlb_calls=0;
        assert(unmap_all()==K_OK && !locked && tlb_calls==1);
        assert(batches==(failure==1?2:1));
        assert(!atomic_load(&space.rss_pages) && !atomic_load(&space.commit_pages));
        assert(!atomic_load(&objects[0].refs.value) && !space.areas.root);
    }
    puts("空洞、未驻留页、确认失败前缀 RSS、VMA/后备引用保留及重试回收通过");
}
'''
with tempfile.TemporaryDirectory(prefix='vm-unmap-tlb-') as directory:
    base=Path(directory);source=base/'test.c';binary=base/'test'
    command=['cc','-std=gnu11','-O1','-Wall','-Wextra','-Werror',
             '-Wno-unused-function','-fsanitize=address,undefined','-fno-pie','-no-pie',
             '-I'+str(root/'include'),str(source),'-o',str(binary)]
    source.write_text(fixture+main)
    subprocess.run(command,check=True)
    subprocess.run([str(binary)],check=True,timeout=15)
    old='if (cut_start == area->start && cut_end == area->end) {'
    assert old in fixture
    source.write_text(fixture.replace(old,'if (false) {',1)+main)
    subprocess.run(command,check=True)
    result=subprocess.run([str(binary)],capture_output=True,timeout=15,
        preexec_fn=lambda:resource.setrlimit(resource.RLIMIT_CORE,(0,0)))
    assert result.returncode!=0 and b'tlb_calls==1' in result.stderr
    print('负面对照通过：恢复逐 VMA 提交会违反合并预算')
