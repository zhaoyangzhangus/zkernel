"""Production sparse discard: indexed work, ownership links and page refs."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root/'kernel/mm/object.c').read_text()
store_helper = source[source.index('static anon_store_t *anon_object_store('):
                      source.index('void vm_object_stats_init(')]
header = (root/'kernel/mm/internal.h').read_text()
types = header[header.index('typedef struct anon_page_node'):]
types = types[:types.index('} anon_store_t;')+len('} anon_store_t;')]
index = source[source.index('static anon_page_node_t *anon_page_index_find('):
               source.index('kstatus_t anon_page_get(')]
discard = source[source.index('kstatus_t anon_page_retire_range('):
                 source.index('/* 将文件页复制', source.index('kstatus_t anon_page_discard_range('))]
# 统计实际遍历步数，包括旧版完整扫描所有权列表的操作。
discard = discard.replace('while (*link != 0) {', 'while (*link != 0) { ++visits;')
discard = discard.replace('while (tree_node != 0) {', 'while (tree_node != 0) { ++visits;')
discard = discard.replace('while (last != 0 && last->right != 0) last = last->right;',
                          'while (last != 0 && last->right != 0) { ++maximum_visits; last = last->right; }')
discard = discard.replace('rb_tree_erase(&store->page_index, tree_node);',
                          '++erasures; rb_tree_erase(&store->page_index, tree_node);')
discard = discard.replace('atomic_fetch_sub_explicit(&object->populated_pages,',
                          'count_sub(&object->populated_pages,')
index = index.replace('while (tree_node != 0) {', 'while (tree_node != 0) { ++visits;')
program = r'''
#include <kernel/rbtree.h>
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdatomic.h>
enum { VM_OBJECT_ANON=1, VM_OBJECT_SHARED=2 };
typedef struct { unsigned refs; } page_t;
typedef struct { atomic_uint state; } spinlock_t;
''' + types + r'''
typedef struct {
    unsigned type;
    union { struct { void *anon_root; } anon; struct { void *shared_root; } shared; } u;
    atomic_uint_fast64_t populated_pages;
} vm_object_t;
static unsigned held, visits, released, freed,subtractions,erasures,maximum_visits;
static bool inject_unlock;
static void inject_after_unlock(void);
static uint64_t count_sub(atomic_uint_fast64_t *value,uint64_t count,memory_order order) {
    assert(held && count);++subtractions;return atomic_fetch_sub_explicit(value,count,order);
}
static void vm_store_lock(spinlock_t *lock) { (void)lock; assert(!held); held=1; }
static void vm_store_unlock(spinlock_t *lock) {
    (void)lock;assert(held);held=0;if(inject_unlock)inject_after_unlock();
}
static void page_reference_put(page_t *page) { assert(!held && page->refs); --page->refs; ++released; }
static void kfree(void *p) { assert(!held);++freed;free(p); }
''' + store_helper + index + discard + r'''
static unsigned validate(vm_object_t *object) {
    anon_store_t *store=object->u.anon.anon_root;
    unsigned count=0;
    anon_page_node_t **link=&store->pages;
    while (*link) {
        anon_page_node_t *node=*link;
        assert(anon_page_index_find(store,node->index)==node);
        assert(node->page->refs);
        LINK_ASSERT
        ++count; link=&node->next;
    }
    unsigned tree_count=0;
    for (rb_node_t *n=rb_tree_first(&store->page_index);n;n=rb_tree_next(n)) ++tree_count;
    assert(count==tree_count && count==atomic_load(&object->populated_pages));
    return count;
}
static vm_object_t *race_object;static page_t old_page,new_page;
static void inject_after_unlock(void) {
    inject_unlock=false;
    anon_store_t *store=race_object->u.anon.anon_root;
    assert(!held && !atomic_load(&race_object->populated_pages));
    assert(!store->pages && !store->page_index.root && old_page.refs==2);
    anon_page_node_t *node=calloc(1,sizeof(*node));assert(node);
    node->index=7;node->page=&new_page;new_page.refs=2;
    assert(anon_page_index_insert(store,node)==node);++race_object->populated_pages;
}
int main(void) {
    enum { N=10000 };
    page_t *pages=calloc(N,sizeof(*pages)); assert(pages);
    anon_store_t store={0}; vm_object_t object={.type=VM_OBJECT_ANON};
    object.u.anon.anon_root=&store;
    for (unsigned i=0;i<N;++i) {
        unsigned k=(i*7919U)%N;
        anon_page_node_t *node=calloc(1,sizeof(*node)); assert(node);
        node->index=k*2U; node->page=&pages[k]; pages[k].refs=2;
        assert(anon_page_index_insert(&store,node)==node); ++object.populated_pages;
    }
    assert(validate(&object)==N);
    visits=subtractions=0;
    assert(anon_page_discard_range(&object,10000,10004)==K_OK);
    assert(visits<40); /* Must not inspect all 10,000 unrelated pages. */
    printf("10000 resident pages, discard 2: %u traversal steps\n",visits);
    assert(released==2 && freed==2 && pages[5000].refs==1 && pages[5001].refs==1);
    assert(subtractions==1);
    assert(validate(&object)==N-2);
    visits=subtractions=0; assert(anon_page_discard_range(&object,10000,10004)==K_OK);
    assert(visits<40 && released==2 && !subtractions);
    assert(anon_page_discard_range(&object,0,0)==K_OK && !held);
    assert(anon_page_discard_range(&object,5,4)==K_EINVAL && !held);
    /* 移除实际的所有权列表头节点，再重新插入相同索引。 */
    uint64_t head=store.pages->index;
    assert(anon_page_discard_range(&object,head,head+1)==K_OK);
    assert(validate(&object)==N-3);
    anon_page_node_t *replacement=calloc(1,sizeof(*replacement)); assert(replacement);
    replacement->index=head; replacement->page=&pages[head/2]; ++replacement->page->refs;
    assert(anon_page_index_insert(&store,replacement)==replacement); ++object.populated_pages;
    assert(validate(&object)==N-2);
    /* 共享后备存储具有相同的所有权语义；稀疏尾部为空。 */
    object.type=VM_OBJECT_SHARED; object.u.shared.shared_root=&store;
    visits=0; assert(anon_page_discard_range(&object,30000,40000)==K_OK);
    assert(visits<40 && validate(&object)==N-2);
    /* 反复混合删除和重新插入，以覆盖旋转和反向链接。 */
    for (unsigned round=0;round<100;++round) {
        uint64_t first=(round*7919U)%N;
        unsigned before=validate(&object);
        visits=0;
        assert(anon_page_discard_range(&object,first*2,first*2+8)==K_OK);
        assert(visits<40 && validate(&object)<=before);
        if (!anon_page_index_find(&store,first*2)) {
            anon_page_node_t *node=calloc(1,sizeof(*node)); assert(node);
            node->index=first*2; node->page=&pages[first]; ++node->page->refs;
            assert(anon_page_index_insert(&store,node)==node); ++object.populated_pages;
        }
        validate(&object);
    }
    visits=erasures=subtractions=maximum_visits=0;
    assert(anon_page_discard_range(&object,0,UINT64_MAX)==K_OK);
    assert(subtractions==1 && !visits && !erasures && maximum_visits<32);
    assert(!store.pages && !store.page_index.root && !object.populated_pages);
    for (unsigned i=0;i<N;++i) assert(pages[i].refs==1);
    assert(!held);free(pages);
    /* 模拟在解锁后、释放分离页面前，另一次缺页处理立即重新填充相同索引。新的所有权应继续有效。 */
    anon_page_node_t *old=calloc(1,sizeof(*old));assert(old);
    old->index=7;old->page=&old_page;old_page.refs=2;
    assert(anon_page_index_insert(&store,old)==old);++object.populated_pages;
    race_object=&object;inject_unlock=true;subtractions=0;
    erasures=0;
    assert(anon_page_discard_range(&object,0,8)==K_OK && !inject_unlock && !erasures);
    assert(subtractions==1 && old_page.refs==1 && new_page.refs==2);
    assert(validate(&object)==1 && anon_page_index_find(&store,7)->page==&new_page);
    assert(anon_page_discard_range(&object,7,8)==K_OK);
    assert(new_page.refs==1 && validate(&object)==0);
    /* 即使索引达到可表示的最大值，排他上界也不能将部分请求变成整个存储区回收。 */
    page_t boundary_pages[2]={{.refs=2},{.refs=2}};
    for(unsigned i=0;i<2;++i) {
        anon_page_node_t *node=calloc(1,sizeof(*node));assert(node);
        node->index=i?UINT64_MAX:7;node->page=&boundary_pages[i];
        assert(anon_page_index_insert(&store,node)==node);++object.populated_pages;
    }
    assert(anon_page_discard_range(&object,0,7)==K_OK && validate(&object)==2);
    erasures=0;
    assert(anon_page_discard_range(&object,0,UINT64_MAX)==K_OK);
    assert(erasures==1 && validate(&object)==1 && store.pages->index==UINT64_MAX);
    assert(boundary_pages[0].refs==1 && boundary_pages[1].refs==2);
    /* 清理没有可表示排他终点的索引测试夹具。 */
    anon_page_node_t *boundary=store.pages;
    store.pages=0;store.page_index.root=0;object.populated_pages=0;
    page_reference_put(boundary->page);kfree(boundary);
    /* 多个分离范围在旧存储区销毁后仍保持有效。之后释放物理页面时，不得影响已重新填充的相同索引。 */
    page_t deferred_pages[8];
    for(unsigned i=0;i<8;++i) {
        deferred_pages[i].refs=2;
        anon_page_node_t *node=calloc(1,sizeof(*node));assert(node);
        node->index=i;node->page=&deferred_pages[i];
        assert(anon_page_index_insert(&store,node)==node);++object.populated_pages;
    }
    anon_page_node_t *retired=0;
    unsigned before_released=released,before_freed=freed;
    assert(anon_page_retire_range(&object,2,4,&retired)==K_OK);
    anon_page_node_t *saved=retired;
    assert(anon_page_retire_range(&object,9,8,&retired)==K_EINVAL && retired==saved);
    assert(anon_page_retire_range(&object,0,2,&retired)==K_OK);
    assert(anon_page_retire_range(&object,0,8,&retired)==K_OK);
    assert(validate(&object)==0 && released==before_released && freed==before_freed);
    for(unsigned i=0;i<8;++i)assert(deferred_pages[i].refs==2);
    page_t fresh={.refs=2};
    anon_page_node_t *fresh_node=calloc(1,sizeof(*fresh_node));assert(fresh_node);
    fresh_node->index=3;fresh_node->page=&fresh;
    assert(anon_page_index_insert(&store,fresh_node)==fresh_node);++object.populated_pages;
    anon_page_release_retired(retired);
    assert(released==before_released+8 && freed==before_freed+8);
    for(unsigned i=0;i<8;++i)assert(deferred_pages[i].refs==1);
    assert(validate(&object)==1 && fresh.refs==2);
    assert(anon_page_discard_range(&object,3,4)==K_OK && fresh.refs==1);
}
'''
# 旧版生产节点尚无反向链接；操作次数断言仍可针对实际回归问题让修复前的实现失败。
program = program.replace('LINK_ASSERT', 'assert(node->list_link==link);' if 'list_link' in types else '')
with tempfile.TemporaryDirectory(prefix='liteos-anon-discard-') as directory:
    path=Path(directory); (path/'test.c').write_text(program)
    subprocess.run(['cc','-std=gnu11','-Wall','-Wextra','-Werror',
                    '-Wno-unused-function','-fsanitize=address,undefined','-fno-pie','-no-pie',
                    '-I'+str(root/'include'),str(path/'test.c'),'-o',str(path/'test')],check=True)
    subprocess.run([str(path/'test')],check=True)
print('Anonymous discard: unlocked page/node reclamation, one counter update per nonempty batch, sparse indexed work, ownership backlinks and same-index insertion between detach/free passed')
