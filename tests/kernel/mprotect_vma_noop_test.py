"""使用实际 VMA/页表事务验证重复权限不切分、不解除 COW，也不遗漏文件影子准备。"""
from pathlib import Path
import resource
import runpy
import subprocess
import tempfile

root=Path(__file__).resolve().parents[2]
fixture=runpy.run_path(str(root/'tests/kernel/mprotect_index_test.py'))['program']
fixture=fixture[:fixture.index('int main(void)')]
main=r'''
int main(void) {
    unsigned cases=0;
    for(unsigned layout=0;layout<2;++layout)
      for(unsigned mode=0;mode<3;++mode)
       for(unsigned kind=0;kind<4;++kind)
        for(unsigned first=0;first<32;++first)
         for(unsigned last=first+1;last<=32;++last) {
            unsigned count=layout?4:1;
            prepare(count,mode,kind>=2);
            uint32_t prot=VM_PROT_READ|VM_PROT_USER;
            if(kind!=3)prot|=VM_PROT_WRITE;
            for(unsigned i=0;i<count;++i) {
                areas[i].prot=prot;
                if(kind==1)backing[i].type=VM_OBJECT_SHARED;
                if(kind==2) {
                    areas[i].private_object=&shadows[i];
                    shadows[i].type=VM_OBJECT_ANON;refcount_init(&shadows[i].refs,1);
                }
                /* COW 可以比 VMA 权限更严格；重复权限不得强行放开 PTE。 */
                areas[i].flags|=VM_AREA_COW;
            }
            for(unsigned i=0;i<32;++i)ptes[i]=X86_PAGE_USER;
            vm_area_t snapshot[128];memcpy(snapshot,areas,sizeof(areas));
            unsigned before=allocated;
            fail_alloc=fail_shadow=fail_protect=fail_flush=1;
            assert(vm_protect(&space,VM_USER_BASE+first*PAGE_SIZE,
                              (last-first)*PAGE_SIZE,prot)==K_OK);
            assert(!locked && allocated==before && !alloc_calls && !shadow_calls);
            assert(!protect_calls && !flushes && !bumps && !rebuilds);
            assert(!memcmp(snapshot,areas,sizeof(areas)));
            for(unsigned i=0;i<32;++i)assert(ptes[i]==X86_PAGE_USER);
            for(unsigned i=0;i<count;++i) {
                assert(atomic_load(&backing[i].refs.value)==1);
                if(kind==2)assert(atomic_load(&shadows[i].refs.value)==1);
            }
            ++cases;
         }
    /* 相同的可写文件权限但缺少影子对象时，必须继续原有准备。 */
    prepare(1,1,true);
    assert(vm_protect(&space,VM_USER_BASE+PAGE_SIZE,30*PAGE_SIZE,
                      VM_PROT_READ|VM_PROT_WRITE|VM_PROT_USER)==K_OK);
    assert(alloc_calls==2 && shadow_calls==1 && bumps==1);
    assert(find_area(&space,VM_USER_BASE+PAGE_SIZE)->private_object);
    /* 只有第一个 VMA 相同不能提前成功，后续不同权限仍须生效。 */
    prepare(4,1,false);areas[1].prot=VM_PROT_READ|VM_PROT_USER;
    for(unsigned i=8;i<16;++i)ptes[i]=X86_PAGE_USER;
    assert(vm_protect(&space,VM_USER_BASE,32*PAGE_SIZE,
                      VM_PROT_READ|VM_PROT_WRITE|VM_PROT_USER)==K_OK);
    assert(bumps==1);
    for(unsigned i=0;i<32;++i)assert(ptes[i]==initial_flags);
    /* 无变化检查必须在完整映射验证之后，不能把空洞当成成功。 */
    prepare(4,1,false);areas[1].end-=PAGE_SIZE;update_area_tree(&areas[1]);
    assert(vm_protect(&space,VM_USER_BASE,32*PAGE_SIZE,
                      VM_PROT_READ|VM_PROT_WRITE|VM_PROT_USER)==K_ENOENT);
    assert(!bumps && !alloc_calls && !protect_calls);
    printf("重复权限：%u 个实际事务零切分/分配/PTE 修改；COW、引用、混合权限、空洞及文件影子准备通过\n",cases);
}
'''
with tempfile.TemporaryDirectory(prefix='mprotect-vma-noop-') as directory:
    base=Path(directory);source=base/'test.c';binary=base/'test'
    command=['cc','-std=gnu11','-O1','-Wall','-Wextra','-Werror',
             '-fsanitize=address,undefined','-fno-pie','-no-pie',
             '-I'+str(root/'include'),str(source),'-o',str(binary)]
    source.write_text(fixture+main)
    subprocess.run(command,check=True)
    subprocess.run([str(binary)],check=True,timeout=30)
    anchor='    if (unchanged) {\n        map_unlock(space);\n        return K_OK;\n    }'
    assert anchor in fixture
    source.write_text(fixture.replace(anchor,'    (void)unchanged;')+main)
    subprocess.run(command,check=True)
    result=subprocess.run([str(binary)],capture_output=True,timeout=30,
        preexec_fn=lambda:resource.setrlimit(resource.RLIMIT_CORE,(0,0)))
    assert result.returncode!=0 and b'Assertion' in result.stderr
    print('负面对照通过：恢复重复权限的切分路径被回归测试拒绝')
