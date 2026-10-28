"""Exercise production anonymous-page publication with concurrent allocators."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'kernel/mm/object.c').read_text()
store_helper = source[source.index('static anon_store_t *anon_object_store('):
                      source.index('void vm_object_stats_init(')]
header = (root / 'kernel/mm/internal.h').read_text()
types = header[header.index('typedef struct anon_page_node'):]
types = types[:types.index('} anon_store_t;') + len('} anon_store_t;')]
index = source[source.index('static anon_page_node_t *anon_page_index_find('):
               source.index('kstatus_t anon_page_get(')]
index = index.replace('rb_node_t *tree_node = store->page_index.root;',
                      '++find_calls; rb_node_t *tree_node = store->page_index.root;', 1)
get = source[source.index('kstatus_t anon_page_get('):]
get = get[:get.index('/* 在不创建页面')]
program = r'''
#include <kernel/rbtree.h>
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <stdio.h>
enum { VM_OBJECT_ANON=1, VM_OBJECT_SHARED=2, PAGE_ALLOC_ZERO=1, PAGE_OWNER_ANON=7 };
typedef struct { unsigned owner; atomic_uint refs; } page_t;
typedef pthread_mutex_t spinlock_t;
''' + types + r'''
typedef struct {
    unsigned type;
    union { struct { void *anon_root; } anon; struct { void *shared_root; } shared; } u;
    atomic_uint_fast64_t populated_pages;
} vm_object_t;
static _Thread_local unsigned held, fail_page, fail_node;
static atomic_uint allocated, released, nodes, freed, node_attempts;
static void (*allocation_hook)(void);
static atomic_uint find_calls;
static unsigned race;
static pthread_barrier_t barrier;
static void vm_store_lock(spinlock_t *lock) {
    assert(!held); assert(!pthread_mutex_lock(lock)); held=1;
}
static void vm_store_unlock(spinlock_t *lock) {
    assert(held); held=0; assert(!pthread_mutex_unlock(lock));
}
static page_t *page_alloc(unsigned order, unsigned flags) {
    assert(!held && order==0 && flags==PAGE_ALLOC_ZERO);
    if (race) { int rc=pthread_barrier_wait(&barrier); assert(rc==0 || rc==PTHREAD_BARRIER_SERIAL_THREAD); }
    if(allocation_hook) {void (*hook)(void)=allocation_hook;allocation_hook=0;hook();}
    if (fail_page) return 0;
    page_t *page=calloc(1,sizeof(*page)); assert(page); atomic_init(&page->refs,1); ++allocated; return page;
}
static void page_free(page_t *page) { assert(!held && atomic_load(&page->refs)==1); ++released; free(page); }
static void *kmalloc(size_t size, unsigned flags) {
    assert(!held && !flags); ++node_attempts; if (fail_node) return 0;
    void *p=calloc(1,size); assert(p); ++nodes; return p;
}
static void kfree(void *p) { assert(!held); if(p) { ++freed; free(p); } }
''' + store_helper + index + get + r'''
static anon_store_t store;
static vm_object_t object={.type=VM_OBJECT_ANON};
static page_t *winner;
static void publish_during_failed_allocation(void) {
    fail_page=0;
    assert(anon_page_get(&object,77,true,false,&winner)==K_OK);
    fail_page=1;
}
static page_t *outputs[8];
static void *worker(void *arg) {
    unsigned id=(unsigned)(uintptr_t)arg;
    assert(anon_page_get(&object,42,true,false,&outputs[id])==K_OK);
    assert(!held); return 0;
}
static void clear(void) {
    while (store.pages) {
        anon_page_node_t *node=store.pages;
        store.pages=node->next; page_free(node->page); kfree(node);
    }
    store.page_index.root=0; object.populated_pages=0;
    assert(allocated==released && nodes==freed);
}
int main(void) {
    assert(!pthread_mutex_init(&store.lock,0)); object.u.anon.anon_root=&store;
    page_t *out=0;
    assert(anon_page_get(0,0,true,false,&out)==K_EINVAL);
    assert(anon_page_get(&object,0,true,false,0)==K_EINVAL);
    assert(anon_page_get(&object,0,false,false,&out)==K_ENOENT && !allocated && !nodes);
    fail_page=1; assert(anon_page_get(&object,0,true,false,&out)==K_ENOMEM && !node_attempts); fail_page=0;
    assert(!store.pages && !object.populated_pages && nodes==freed);
    fail_node=1; assert(anon_page_get(&object,0,true,false,&out)==K_ENOMEM); fail_node=0;
    assert(!store.pages && allocated==released);
    unsigned before_attempts=node_attempts;
    allocation_hook=publish_during_failed_allocation;fail_page=1;
    assert(anon_page_get(&object,77,true,false,&out)==K_OK && out==winner);
    assert(node_attempts==before_attempts+1 && object.populated_pages==1);
    fail_page=0;clear();
    for (unsigned round=0;round<100;++round) {
        unsigned resident=round%2?16:0;
        for(unsigned i=0;i<resident;++i)
            assert(anon_page_get(&object,1000+i,true,false,&out)==K_OK);
        unsigned before_race_find=find_calls;
        pthread_t threads[8]; race=1; assert(!pthread_barrier_init(&barrier,0,8));
        for(unsigned i=0;i<8;++i) assert(!pthread_create(&threads[i],0,worker,(void *)(uintptr_t)i));
        for(unsigned i=0;i<8;++i) assert(!pthread_join(threads[i],0));
        race=0; assert(!pthread_barrier_destroy(&barrier));
        assert(find_calls==before_race_find+8);
        for(unsigned i=0;i<8;++i) assert(outputs[i]==outputs[0]);
        assert(object.populated_pages==resident+1 && store.pages);
        unsigned count=0;
        anon_page_node_t **link=&store.pages;
        for(anon_page_node_t *item=store.pages;item;item=item->next) {
            assert(item->list_link==link);link=&item->next;++count;
        }
        assert(count==resident+1);
        assert(store.pages->list_link==&store.pages && outputs[0]->owner==PAGE_OWNER_ANON);
        assert(allocated==released+resident+1 && nodes==freed+resident+1);
        unsigned before=allocated; fail_page=fail_node=1;
        assert(anon_page_get(&object,42,true,false,&out)==K_OK && out==outputs[0]);
        assert(anon_page_get(&object,42,false,false,&out)==K_OK && allocated==before);
        fail_page=fail_node=0; clear();
    }
    object.type=VM_OBJECT_SHARED; object.u.shared.shared_root=&store;
    unsigned before_find=find_calls;
    assert(anon_page_get(&object,100,true,false,&out)==K_OK);
    assert(find_calls==before_find+1);
    /* A nonempty tree must also publish a fresh index without a second find. */
    before_find=find_calls;
    assert(anon_page_get(&object,101,true,false,&out)==K_OK);
    assert(find_calls==before_find+1 && object.populated_pages==2); clear();
    assert(!held); assert(!pthread_mutex_destroy(&store.lock));
    puts("Anonymous publication: off-lock allocation/free, 100 eight-way empty/populated-tree races with exactly one find per caller, hit/miss/OOM/shared and winner ownership/backlinks passed");
}
'''
with tempfile.TemporaryDirectory(prefix='liteos-anon-publish-') as directory:
    path = Path(directory)
    (path / 'test.c').write_text(program)
    subprocess.run(['cc', '-std=gnu11', '-Wall', '-Wextra', '-Werror',
                    '-Wno-unused-function', '-pthread', '-fsanitize=address,undefined',
                    '-fno-pie', '-no-pie', '-I' + str(root / 'include'),
                    str(path / 'test.c'), '-o', str(path / 'test')], check=True)
    subprocess.run([str(path / 'test')], check=True, timeout=30)
