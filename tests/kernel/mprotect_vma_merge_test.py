"""实际权限事务与增广红黑树：碎片上界、合并屏障、引用及失败重试。"""
from pathlib import Path
import resource
import runpy
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
fixture = runpy.run_path(str(root/'tests/kernel/mprotect_index_test.py'))['program']
fixture = fixture[:fixture.index('int main(void)')]
fixture = fixture.replace('static unsigned allocated,',
                          'static bool reusable[128];\nstatic unsigned released;\nstatic unsigned allocated,')
fixture = fixture.replace('assert(allocated<128);return &areas[allocated++];', r'''
    for(unsigned i=0;i<allocated;++i)if(reusable[i]) {
        reusable[i]=false;memset(&areas[i],0,sizeof(areas[i]));return &areas[i];
    }
    assert(allocated<128);return &areas[allocated++];''')
fixture = fixture.replace('assert(!locked);\n    vm_object_put(a->object);',
    'assert(!locked);\n    assert(!reusable[a-areas]);reusable[a-areas]=true;++released;\n    vm_object_put(a->object);')
main = r'''
static unsigned count_areas(void) {
    unsigned n=0;
    for(list_head_t *p=space.area_list.next;p!=&space.area_list;p=p->next)++n;
    return n;
}
static void setup(unsigned mode,bool cow) {
    prepare(1,mode,false);memset(reusable,0,sizeof(reusable));released=0;
    if(cow) {
        areas[0].flags|=VM_AREA_COW;
        areas[0].private_object=&shadows[0];
        shadows[0].type=VM_OBJECT_ANON;refcount_init(&shadows[0].refs,1);
    }
}
static void check_refs(bool shadow) {
    unsigned n=count_areas();assert(atomic_load(&backing[0].refs.value)==n);
    if(shadow)assert(atomic_load(&shadows[0].refs.value)==n);
    for(list_head_t *p=space.area_list.next;p!=&space.area_list;p=p->next) {
        vm_area_t *a=area_from_list(p);
        assert(a->object==&backing[0] && a->object_offset==a->start-VM_USER_BASE);
        assert((a->flags&VM_AREA_COW)==(shadow?VM_AREA_COW:0));
    }
}
static void merge_all(void) {
    list_head_t retired;list_init(&retired);
    map_lock(&space);boundary_seeks=1;
    vm_area_merge_range(&space,area_from_list(space.area_list.next),&space.area_list,&retired);
    map_unlock(&space);
    while(!list_empty(&retired)) {
        vm_area_t *a=area_from_list(retired.next);list_del(&a->ordered_node);area_release(a);
    }
}
int main(void) {
    const uint32_t ro=VM_PROT_READ|VM_PROT_USER,rw=ro|VM_PROT_WRITE;
    unsigned cases=0;
    /* 任意起止页收缩后恢复，验证两侧合并、树摘要和锁外引用释放。 */
    for(unsigned mode=0;mode<3;++mode)for(unsigned cow=0;cow<2;++cow)
      for(unsigned first=0;first<32;++first)for(unsigned last=first+1;last<=32;++last) {
        setup(mode,cow);uint64_t va=VM_USER_BASE+first*PAGE_SIZE;
        assert(vm_protect(&space,va,(last-first)*PAGE_SIZE,ro)==K_OK);
        unsigned fragments=1+(first!=0)+(last!=32);
        assert(count_areas()==fragments);check_refs(cow);
        assert(vm_protect(&space,va,(last-first)*PAGE_SIZE,rw)==K_OK);
        assert(count_areas()==1 && released==fragments-1);check_refs(cow);
        assert(find_area(&space,VM_USER_BASE)->end==VM_USER_BASE+32*PAGE_SIZE);
        cases+=2;
      }
    /* 文件切片共享同一影子时可合并；分别创建的影子即使权限相同也不能混用。 */
    setup(1,true);backing[0].type=VM_OBJECT_FILE;
    assert(vm_protect(&space,VM_USER_BASE+PAGE_SIZE,30*PAGE_SIZE,ro)==K_OK);
    assert(vm_protect(&space,VM_USER_BASE+PAGE_SIZE,30*PAGE_SIZE,rw)==K_OK);
    assert(count_areas()==1 && !shadow_calls);check_refs(true);
    setup(0,false);backing[0].type=VM_OBJECT_FILE;
    assert(vm_protect(&space,VM_USER_BASE,16*PAGE_SIZE,rw)==K_OK);
    assert(vm_protect(&space,VM_USER_BASE+16*PAGE_SIZE,16*PAGE_SIZE,rw)==K_OK);
    assert(count_areas()==2 && shadow_calls==2 && !released);
    assert(vm_protect(&space,VM_USER_BASE,32*PAGE_SIZE,ro)==K_OK);
    assert(count_areas()==2 && !released);
    for(unsigned i=0;i<2;++i)assert(atomic_load(&shadows[i].refs.value)==1);
    /* 模拟分配器连续提交 16 KiB，反向取消提交；重复操作不能积累 VMA。 */
    setup(0,false);
    for(unsigned cycle=0;cycle<256;++cycle) {
        for(unsigned i=0;i<8;++i) {
            assert(vm_protect(&space,VM_USER_BASE+i*4*PAGE_SIZE,4*PAGE_SIZE,ro)==K_OK);
            assert(count_areas()<=2);check_refs(false);
        }
        assert(count_areas()==1);
        for(unsigned i=8;i>0;--i) {
            assert(vm_protect(&space,VM_USER_BASE+(i-1)*4*PAGE_SIZE,4*PAGE_SIZE,rw)==K_OK);
            assert(count_areas()<=2);check_refs(false);
        }
        assert(count_areas()==1 && allocated<=3);
    }
    /* 现有内部碎片也应一次收拢，不能只处理首尾两对。 */
    setup(1,true);map_lock(&space);
    vm_area_t *a=&areas[0];
    for(unsigned i=1;i<32;++i) {
        vm_area_t *right=vm_area_split(a,VM_USER_BASE+i*PAGE_SIZE);assert(right);
        update_area_tree(a);insert_area_tree(&space,right);a=right;
    }
    boundary_seeks=1;map_unlock(&space);
    assert(count_areas()==32 && atomic_load(&shadows[0].refs.value)==32);
    assert(vm_protect(&space,VM_USER_BASE,32*PAGE_SIZE,ro)==K_OK);
    assert(count_areas()==1 && released==31);check_refs(true);
    /* 任何语义差异均为合并屏障，包括 COW、影子对象和偏移回绕。 */
    for(unsigned barrier=0;barrier<9;++barrier) {
        setup(0,false);map_lock(&space);
        vm_area_t *middle=vm_area_split(&areas[0],VM_USER_BASE+8*PAGE_SIZE);
        update_area_tree(&areas[0]);insert_area_tree(&space,middle);
        vm_area_t *last=vm_area_split(middle,VM_USER_BASE+16*PAGE_SIZE);
        update_area_tree(middle);insert_area_tree(&space,last);
        if(barrier==0)middle->prot=ro;
        if(barrier==1)middle->flags|=VM_AREA_COW;
        if(barrier==2)middle->flags|=VM_MAP_SHARED;
        if(barrier==3) {
            vm_object_put(middle->object);middle->object=&backing[1];
            refcount_init(&backing[1].refs,1);
        }
        if(barrier==4) {
            middle->private_object=&shadows[1];refcount_init(&shadows[1].refs,1);
        }
        if(barrier==5)middle->object_offset+=PAGE_SIZE;
        if(barrier==6)middle->object_offset=UINT64_MAX-4*PAGE_SIZE;
        if(barrier==7) {middle->start+=PAGE_SIZE;update_area_tree(middle);middle->end-=PAGE_SIZE;update_area_tree(middle);}
        if(barrier==8)middle->flags|=VM_AREA_CLONE_OLD_COW;
        boundary_seeks=1;map_unlock(&space);merge_all();
        assert(count_areas()==3 && !released);
    }
    /* 页表修改和刷新失败时不得合并，成功重试才释放两侧节点。 */
    for(unsigned failure=0;failure<2;++failure) {
        setup(1,true);
        assert(vm_protect(&space,VM_USER_BASE+PAGE_SIZE,30*PAGE_SIZE,ro)==K_OK);
        assert(count_areas()==3);
        if(failure)fail_flush=flushes+1;else fail_protect=protect_calls+2;
        assert(vm_protect(&space,VM_USER_BASE+PAGE_SIZE,30*PAGE_SIZE,rw)==
               (failure?K_EIO:K_EINVAL));
        assert(count_areas()==3 && !released);check_refs(true);
        for(unsigned i=1;i<31;++i)assert(ptes[i]==X86_PAGE_USER);
        fail_flush=fail_protect=0;
        assert(vm_protect(&space,VM_USER_BASE+PAGE_SIZE,30*PAGE_SIZE,rw)==K_OK);
        assert(count_areas()==1 && released==2);check_refs(true);
    }
    printf("VMA 合并：%u 个权限往返事务、4096 次连续提交/取消、32 段收拢、9 类屏障及失败重试通过；引用和索引一致\n",cases);
}
'''
with tempfile.TemporaryDirectory(prefix='mprotect-vma-merge-') as directory:
    base=Path(directory);source=base/'test.c';binary=base/'test'
    command=['cc','-std=c11','-O1','-Wall','-Wextra','-Werror',
             '-fsanitize=address,undefined','-fno-pie','-no-pie',
             '-I'+str(root/'include'),str(source),'-o',str(binary)]
    source.write_text(fixture+main)
    subprocess.run(command,check=True)
    subprocess.run([str(binary)],check=True,timeout=30)
    mutations=[
        ('    vm_area_merge_range(space, start_area, range_end, &retired);',
         '    /* 负面对照：保留旧的碎片行为。 */'),
        ('left->private_object == right->private_object &&', ''),
        ('right->object_offset - left->object_offset == length', '(length > 0)'),
    ]
    for anchor,replacement in mutations:
        assert anchor in fixture
        source.write_text(fixture.replace(anchor,replacement)+main)
        subprocess.run(command,check=True)
        result=subprocess.run([str(binary)],capture_output=True,timeout=30,
            preexec_fn=lambda:resource.setrlimit(resource.RLIMIT_CORE,(0,0)))
        assert result.returncode!=0 and b'Assertion' in result.stderr
    print('负面对照通过：禁用合并、忽略影子对象或偏移连续性均被拒绝')
