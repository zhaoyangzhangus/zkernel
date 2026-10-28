"""Run production anonymous-index clone against the real red-black tree code."""
from pathlib import Path
import argparse
import resource
import subprocess
import tempfile

parser = argparse.ArgumentParser()
parser.add_argument('--sanitize', action='store_true')
args = parser.parse_args()
root = Path(__file__).resolve().parents[2]
source = (root / 'kernel/mm/object.c').read_text()
clone = source[source.index('static kstatus_t anon_page_index_clone('):
               source.index('static anon_page_node_t *anon_page_index_find(')]
clone = clone.replace('atomic_fetch_add_explicit(&target_object->populated_pages,',
                      'count_add(&target_object->populated_pages,')
clone = clone.replace('atomic_store_explicit(&(*out)->populated_pages,',
                      'count_store(&(*out)->populated_pages,')
insert = source[source.index('static anon_page_node_t *anon_page_index_insert(anon_store_t *store,',
                              source.index('static anon_page_node_t *anon_page_index_find(')):
                source.index('kstatus_t anon_page_get(')]
# Count real search comparisons in the old reinsertion path for an operation
# count comparison. This instrumentation is confined to the host fixture.
insert = insert.replace('parent = *link;', '++comparisons; parent = *link;')
program = r'''
#include <kernel/rbtree.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
typedef struct { atomic_uint refs; } page_t;
typedef struct anon_page_node {
    uint64_t index; page_t *page; struct anon_page_node *next;
    struct anon_page_node **list_link; rb_node_t index_node;
} anon_page_node_t;
typedef struct {
    unsigned lock; anon_page_node_t *pages; rb_root_t page_index;
} anon_store_t;
typedef struct {
    unsigned flags;
    uint64_t size; struct { struct { void *anon_root; } anon; } u;
    atomic_uint_fast64_t populated_pages;
} vm_object_t;
#define VM_OBJECT_FLAG_MAP_OWNED 2U
static long fail_at=-1;
static bool persistent_failure=true;
static unsigned attempts, live_nodes, held, comparisons;
static unsigned locked_attempts;
static unsigned count_adds,count_stores;
static inline uint64_t count_add(atomic_uint_fast64_t *value,uint64_t count,memory_order order) {
    ++count_adds;return atomic_fetch_add_explicit(value,count,order);
}
static inline void count_store(atomic_uint_fast64_t *value,uint64_t count,memory_order order) {
    assert(!held);++count_stores;atomic_store_explicit(value,count,order);
}
static void *kmalloc(size_t n, unsigned flags) {
    (void)flags;
    locked_attempts += held != 0;
    long attempt=(long)attempts++;
    if (fail_at>=0 && (persistent_failure?attempt>=fail_at:attempt==fail_at)) return 0;
    void *p=malloc(n); assert(p); ++live_nodes; return p;
}
static void kfree(void *p) { assert(p && live_nodes && !held); --live_nodes; free(p); }
static void vm_store_lock(unsigned *lock) { assert(!*lock && !held); *lock=1; ++held; }
static void vm_store_unlock(unsigned *lock) { assert(*lock && held==1); *lock=0; --held; }
static kstatus_t vm_object_create_anon(size_t size, vm_object_t **out) {
    *out=calloc(1,sizeof(**out)); assert(*out);
    (*out)->size=size; (*out)->u.anon.anon_root=calloc(1,sizeof(anon_store_t));
    assert((*out)->u.anon.anon_root); atomic_init(&(*out)->populated_pages,0);
    return K_OK;
}
static void vm_object_put(vm_object_t *object) {
    anon_store_t *store=object->u.anon.anon_root;
    anon_page_node_t *node=store->pages;
    unsigned owned=0;
    for(anon_page_node_t *n=node;n;n=n->next)++owned;
    assert(owned==atomic_load(&object->populated_pages));
    while(node) {
        anon_page_node_t *next=node->next;
        assert(atomic_fetch_sub(&node->page->refs,1)>0);
        kfree(node); node=next;
    }
    free(store); free(object);
}
''' + clone + insert + r'''
static anon_page_node_t *owner(rb_node_t *n) {
    return (anon_page_node_t *)((char *)n - offsetof(anon_page_node_t,index_node));
}
static unsigned verify(rb_node_t *s,rb_node_t *c,rb_node_t *parent,unsigned *count) {
    if(!s) { assert(!c); return 1; }
    assert(c && c!=s && rb_tree_parent(c)==parent);
    assert(rb_tree_color(s)==rb_tree_color(c));
    anon_page_node_t *a=owner(s),*b=owner(c);
    assert(b->list_link && *b->list_link==b);
    if(b->next) assert(b->next->list_link==&b->next);
    assert(a->index==b->index && a->page==b->page); ++*count;
    if(c->left) assert(owner(c->left)->index < b->index);
    if(c->right) assert(owner(c->right)->index > b->index);
    if(rb_tree_color(c)==RB_TREE_RED) {
        assert(rb_tree_color(c->left)==RB_TREE_BLACK);
        assert(rb_tree_color(c->right)==RB_TREE_BLACK);
    }
    unsigned left=verify(s->left,c->left,c,count);
    unsigned right=verify(s->right,c->right,c,count);
    assert(left==right); return left+(rb_tree_color(c)==RB_TREE_BLACK);
}
static void refs(page_t *pages,unsigned n,unsigned expected) {
    for(unsigned i=0;i<n;++i) assert(atomic_load(&pages[i].refs)==expected);
}
static unsigned verify_mutated(rb_node_t *node,rb_node_t *parent,
                               uint64_t *previous,bool *first,unsigned *count) {
    if(!node) return 1;
    assert(rb_tree_parent(node)==parent);
    if(rb_tree_color(node)==RB_TREE_RED) {
        assert(rb_tree_color(node->left)==RB_TREE_BLACK);
        assert(rb_tree_color(node->right)==RB_TREE_BLACK);
    }
    unsigned left=verify_mutated(node->left,node,previous,first,count);
    uint64_t index=owner(node)->index;
    assert(*first || index>*previous); *first=false; *previous=index; ++*count;
    unsigned right=verify_mutated(node->right,node,previous,first,count);
    assert(left==right); return left+(rb_tree_color(node)==RB_TREE_BLACK);
}
int main(void) {
    const unsigned n=100000;
    vm_object_t *source=0,*copy=0;
    assert(vm_object_create_anon(1ULL<<46,&source)==K_OK);
    assert(vm_object_clone_anon(source,&copy)==K_OK && !held && !live_nodes);
    assert(!(copy->flags & VM_OBJECT_FLAG_MAP_OWNED));
    assert(count_stores==1 && !count_adds);
    vm_object_put(copy);
    /* An emptied store can retain an older nonzero sizing sample. */
    atomic_store(&source->populated_pages,17);fail_at=0;attempts=0;
    assert(vm_object_clone_anon(source,&copy)==K_OK && copy && !held && !live_nodes);
    assert(!atomic_load(&copy->populated_pages) && attempts==1);
    vm_object_put(copy);atomic_store(&source->populated_pages,0);fail_at=-1;
    page_t *pages=calloc(n,sizeof(*pages)); assert(pages);
    anon_store_t *store=source->u.anon.anon_root;
    for(unsigned i=0;i<n;++i) {
        anon_page_node_t *node=kmalloc(sizeof(*node),0);
        node->index=((uint64_t)i*7919U%n)*65537U;
        node->page=&pages[i]; atomic_init(&pages[i].refs,1);
        assert(anon_page_index_insert(store,node)==node);
    }
    atomic_store(&source->populated_pages,n);
    /* Baseline: the formerly used full-list reinsertion algorithm. */
    vm_object_t *baseline=0; assert(vm_object_create_anon(source->size,&baseline)==K_OK);
    anon_store_t *old=baseline->u.anon.anon_root;
    comparisons=attempts=0; clock_t started=clock();
    for(anon_page_node_t *node=store->pages;node;node=node->next) {
        anon_page_node_t *item=kmalloc(sizeof(*item),0);
        item->index=node->index; item->page=node->page;
        assert(anon_page_index_insert(old,item)==item); atomic_fetch_add(&item->page->refs,1);
    }
    double before=(double)(clock()-started)*1000/CLOCKS_PER_SEC;
    assert(attempts==n && comparisons>n*10);
    unsigned searches=comparisons;atomic_store(&baseline->populated_pages,n);
    vm_object_put(baseline);refs(pages,n,1);
    source->flags |= VM_OBJECT_FLAG_MAP_OWNED;
    attempts=comparisons=count_adds=count_stores=locked_attempts=0;started=clock();
    assert(vm_object_clone_anon(source,&copy)==K_OK && !held);
    assert(copy->flags & VM_OBJECT_FLAG_MAP_OWNED);
    double after=(double)(clock()-started)*1000/CLOCKS_PER_SEC;
    assert(attempts==n && comparisons==0 && live_nodes==2*n);
    assert(!locked_attempts);
    assert(count_stores==1 && !count_adds);
    assert(atomic_load(&copy->populated_pages)==n); refs(pages,n,2);
    unsigned count=0; anon_store_t *target=copy->u.anon.anon_root;
    verify(store->page_index.root,target->page_index.root,0,&count); assert(count==n);
    /* Copied tree remains usable for later insert/erase independently. */
    anon_page_node_t *extra=kmalloc(sizeof(*extra),0); page_t extra_page;
    atomic_init(&extra_page.refs,1); extra->index=UINT64_MAX; extra->page=&extra_page;
    assert(anon_page_index_insert(target,extra)==extra);
    rb_tree_erase(&target->page_index,&extra->index_node);
    assert(target->pages==extra); target->pages=extra->next;
    if(target->pages) target->pages->list_link=&target->pages;
    kfree(extra);
    /* Rebalancing need not restore the old topology after erase. */
    count=0; uint64_t previous=0; bool first=true;
    assert(rb_tree_color(target->page_index.root)==RB_TREE_BLACK);
    verify_mutated(target->page_index.root,0,&previous,&first,&count);
    assert(count==n); vm_object_put(copy); refs(pages,n,1);
    const long failures[]={0,1,2,50,999,n-1};
    for(unsigned i=0;i<sizeof(failures)/sizeof(failures[0]);++i) {
        fail_at=failures[i];attempts=count_adds=count_stores=0;copy=0;
        assert(vm_object_clone_anon(source,&copy)==K_ENOMEM && !copy && !held);
        assert(live_nodes==n); refs(pages,n,1);
        assert(count_stores==1 && !count_adds);
    }
    fail_at=-1;
    /* Simulate faults/discards between the relaxed sizing sample and lock
     * acquisition. The locked tree, never the hint, defines clone contents. */
    const unsigned hints[]={0,1,n/2,n-1,n,n+17};
    for(unsigned h=0;h<sizeof(hints)/sizeof(*hints);++h){
        unsigned hint=hints[h];atomic_store(&source->populated_pages,hint);
        attempts=locked_attempts=count_stores=0;
        assert(vm_object_clone_anon(source,&copy)==K_OK && copy && !held);
        assert(atomic_load(&copy->populated_pages)==n && count_stores==1);
        assert(attempts==(hint>n?hint:n));
        assert(locked_attempts==(hint<n?n-hint:0));
        count=0;target=copy->u.anon.anon_root;
        verify(store->page_index.root,target->page_index.root,0,&count);assert(count==n);
        refs(pages,n,2);vm_object_put(copy);refs(pages,n,1);assert(live_nodes==n);
    }
    /* Deficit allocation can fail after some page refs were copied. */
    atomic_store(&source->populated_pages,2);
    for(unsigned f=2;f<6;++f){
        fail_at=f;attempts=locked_attempts=count_stores=0;copy=0;
        assert(vm_object_clone_anon(source,&copy)==K_ENOMEM && !copy && !held);
        assert(live_nodes==n && locked_attempts==f-1 && count_stores==1);refs(pages,n,1);
    }
    fail_at=-1;atomic_store(&source->populated_pages,n);
    /* Optional reservation must not turn one transient allocation failure
     * into fork failure. Existing reserved nodes remain usable. */
    persistent_failure=false;
    for(unsigned i=0;i<sizeof(failures)/sizeof(failures[0]);++i){
        fail_at=failures[i];attempts=locked_attempts=count_stores=0;
        assert(vm_object_clone_anon(source,&copy)==K_OK && copy && !held);
        assert(attempts==n+1 && locked_attempts==n-(unsigned)fail_at);
        assert(atomic_load(&copy->populated_pages)==n && count_stores==1);
        count=0;target=copy->u.anon.anon_root;
        verify(store->page_index.root,target->page_index.root,0,&count);assert(count==n);
        refs(pages,n,2);vm_object_put(copy);refs(pages,n,1);assert(live_nodes==n);
    }
    /* Concurrent discards can leave the sizing hint larger than the tree.
     * Permanent OOM after all needed nodes are reserved must still succeed. */
    persistent_failure=true;atomic_store(&source->populated_pages,n+17);
    fail_at=n;attempts=locked_attempts=count_stores=0;
    assert(vm_object_clone_anon(source,&copy)==K_OK && copy && !held);
    assert(attempts==n+1 && !locked_attempts && count_stores==1);
    count=0;target=copy->u.anon.anon_root;
    verify(store->page_index.root,target->page_index.root,0,&count);assert(count==n);
    refs(pages,n,2);vm_object_put(copy);refs(pages,n,1);assert(live_nodes==n);
    fail_at=-1;atomic_store(&source->populated_pages,n);
    printf("100000 pages: reinsertion %u comparisons %.2fms; topology clone 0 comparisons %.2fms\n",
           searches,before,after);
    vm_object_put(source); refs(pages,n,0); assert(!live_nodes); free(pages);
    puts("anon index clone: 100000 off-lock node reservations, six sizing hints/transient failures, persistent OOM rollback, surplus/empty-tree recovery and exact topology/ref ownership passed");
}
'''
with tempfile.TemporaryDirectory(prefix='liteos-anon-index-clone-') as directory:
    path = Path(directory)
    flags = ['-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-fno-pie', '-no-pie'] if args.sanitize else []
    subprocess.run(['cc', '-O2', '-std=c11', '-Wall', '-Wextra', '-Werror', *flags,
                    '-I', str(root / 'include'), '-x', 'c', '-', '-o',
                    str(path / 'test')], input=program, text=True, check=True)
    subprocess.run([str(path / 'test')], check=True)
    negative = program.replace('if (copy != 0) *spare = copy->next;\n    else copy = kmalloc(sizeof(*copy), 0);',
                               'copy = kmalloc(sizeof(*copy), 0);')
    assert negative != program
    subprocess.run(['cc', '-O2', '-std=c11', '-Wall', '-Wextra', '-Werror', *flags,
                    '-I', str(root / 'include'), '-x', 'c', '-', '-o',
                    str(path / 'test')], input=negative, text=True, check=True)
    result = subprocess.run([str(path / 'test')], capture_output=True, text=True,
                            preexec_fn=lambda: resource.setrlimit(resource.RLIMIT_CORE, (0, 0)))
    assert result.returncode != 0 and 'Assertion' in result.stderr
    print('Negative control rejected: clone allocates every index node under source lock')
    negative = program.replace('if (node == 0) break;',
                               'if (node == 0) { status=K_ENOMEM; goto cleanup; }')
    negative = negative.replace('    while (spare != 0) {', 'cleanup:\n    while (spare != 0) {')
    assert negative != program
    subprocess.run(['cc', '-O2', '-std=c11', '-Wall', '-Wextra', '-Werror', *flags,
                    '-I', str(root / 'include'), '-x', 'c', '-', '-o',
                    str(path / 'test')], input=negative, text=True, check=True)
    result = subprocess.run([str(path / 'test')], capture_output=True, text=True,
                            preexec_fn=lambda: resource.setrlimit(resource.RLIMIT_CORE, (0, 0)))
    assert result.returncode != 0 and 'Assertion' in result.stderr
    print('Negative control rejected: optional reserve failure aborts a recoverable clone')
