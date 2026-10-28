"""实际匿名页发布：固定引用和索引查找共用锁，覆盖竞争及锁外丢弃。"""
from pathlib import Path
import resource
import runpy
import subprocess
import tempfile

root=Path(__file__).resolve().parents[2]
fixture=runpy.run_path(str(root/'tests/kernel/anon_page_publish_test.py'))['program']
fixture=fixture[:fixture.index('int main(void)')]
fixture=fixture.replace('static unsigned race;',
    'static unsigned race;\nstatic atomic_uint lock_calls;\nstatic void (*unlock_hook)(void);')
fixture=fixture.replace('assert(!held); assert(!pthread_mutex_lock(lock)); held=1;',
    'assert(!held); assert(!pthread_mutex_lock(lock)); held=1; ++lock_calls;')
fixture=fixture.replace('held=0; assert(!pthread_mutex_unlock(lock));',
    'held=0; assert(!pthread_mutex_unlock(lock));\n'
    '    if(unlock_hook){void (*hook)(void)=unlock_hook;unlock_hook=0;hook();}')
fixture=fixture.replace('atomic_load(&page->refs)==1', 'atomic_load(&page->refs)<=1')
main=r'''
static void put(page_t *p) {
    assert(!held);unsigned before=atomic_fetch_sub(&p->refs,1);assert(before);
    if(before==1)page_free(p);
}
static void retire(void) {
    vm_store_lock(&store.lock);
    anon_page_node_t *n=store.pages;assert(n && !n->next);
    store.pages=0;store.page_index.root=0;object.populated_pages=0;
    vm_store_unlock(&store.lock);put(n->page);kfree(n);
}
static void arm_retire(void) {unlock_hook=retire;}
static void *pin_worker(void *arg) {
    unsigned id=(unsigned)(uintptr_t)arg;
    assert(anon_page_get(&object,42,true,id%2!=0,&outputs[id])==K_OK);
    assert(!held);return 0;
}
int main(void) {
    assert(!pthread_mutex_init(&store.lock,0));object.u.anon.anon_root=&store;
    page_t *out=0;
    for(unsigned shared=0;shared<2;++shared) {
        object.type=shared?VM_OBJECT_SHARED:VM_OBJECT_ANON;
        if(shared)object.u.shared.shared_root=&store;
        for(unsigned create=0;create<2;++create) {
            unsigned before=lock_calls,find_before=find_calls;
            assert(anon_page_get(&object,100,true,true,&out)==K_OK);
            assert(lock_calls==before+2 && find_calls==find_before+1);
            assert(atomic_load(&out->refs)==2);put(out);
            before=lock_calls;find_before=find_calls;
            fail_page=fail_node=1;
            assert(anon_page_get(&object,100,create,true,&out)==K_OK);
            assert(lock_calls==before+1 && find_calls==find_before+1);
            assert(atomic_load(&out->refs)==2);put(out);
            fail_page=fail_node=0;clear();
        }
        /* 返回前另一条执行路径摘除后备页，固定引用仍保证页面有效。 */
        for(unsigned existing=0;existing<2;++existing) {
            if(existing)assert(anon_page_get(&object,100,true,false,&out)==K_OK);
            unsigned before_free=released;
            if(existing)unlock_hook=retire;else allocation_hook=arm_retire;
            assert(anon_page_get(&object,100,true,true,&out)==K_OK);
            assert(!store.pages && !object.populated_pages && !unlock_hook);
            assert(atomic_load(&out->refs)==1 && released==before_free);
            put(out);assert(released==before_free+1 && nodes==freed);
        }
        /* 分配失败但竞争者成功发布：取得胜出页的固定引用，不能漏加。 */
        allocation_hook=publish_during_failed_allocation;fail_page=1;
        assert(anon_page_get(&object,77,true,true,&out)==K_OK && out==winner);
        assert(atomic_load(&out->refs)==2);put(out);fail_page=0;clear();
        for(unsigned round=0;round<50;++round) {
            pthread_t threads[8];race=1;
            assert(!pthread_barrier_init(&barrier,0,8));
            unsigned before=lock_calls,find_before=find_calls;
            for(unsigned i=0;i<8;++i)assert(!pthread_create(&threads[i],0,pin_worker,(void*)(uintptr_t)i));
            for(unsigned i=0;i<8;++i)assert(!pthread_join(threads[i],0));
            race=0;assert(!pthread_barrier_destroy(&barrier));
            assert(lock_calls==before+16 && find_calls==find_before+8);
            assert(atomic_load(&outputs[0]->refs)==5 && object.populated_pages==1);
            for(unsigned i=0;i<8;++i) {
                assert(outputs[i]==outputs[0]);if(i%2)put(outputs[i]);
            }
            assert(atomic_load(&outputs[0]->refs)==1);clear();
        }
        for(unsigned fail=0;fail<2;++fail) {
            fail_page=fail==0;fail_node=fail==1;out=(page_t*)1;
            assert(anon_page_get(&object,100,true,true,&out)==K_ENOMEM);
            assert(out==(page_t*)1 && !store.pages && allocated==released && nodes==freed);
            fail_page=fail_node=0;
        }
    }
    assert(!held && !pthread_mutex_destroy(&store.lock));
    puts("固定取页：命中一次加锁、新页两次加锁；匿名/共享、100 轮混合引用竞争、返回前丢弃及 OOM 胜出页引用通过");
}
'''
with tempfile.TemporaryDirectory(prefix='anon-page-pin-get-') as directory:
    source=Path(directory)/'test.c';binary=Path(directory)/'test'
    command=['cc','-std=gnu11','-O1','-Wall','-Wextra','-Werror',
             '-Wno-unused-function','-pthread','-fsanitize=address,undefined',
             '-fno-pie','-no-pie','-I'+str(root/'include'),str(source),'-o',str(binary)]
    source.write_text(fixture+main);subprocess.run(command,check=True)
    subprocess.run([str(binary)],check=True,timeout=30)
    anchor='if (pin) atomic_fetch_add_explicit(&node->page->refs, 1U, memory_order_relaxed);'
    assert anchor in fixture
    block=anchor+'\n    *out = node->page;\n    vm_store_unlock(&store->lock);'
    late='*out = node->page;\n    vm_store_unlock(&store->lock);\n    '+anchor
    for old,new in [(anchor,'(void)pin;'),(block,late)]:
        assert old in fixture
        source.write_text(fixture.replace(old,new)+main)
        subprocess.run(command,check=True)
        result=subprocess.run([str(binary)],capture_output=True,timeout=30,
            preexec_fn=lambda:resource.setrlimit(resource.RLIMIT_CORE,(0,0)))
        assert result.returncode!=0 and (
            b'Assertion' in result.stderr or b'AddressSanitizer' in result.stderr)
    print('负面对照通过：漏加引用或解锁后才固定均被引用/生命周期检查拒绝')
