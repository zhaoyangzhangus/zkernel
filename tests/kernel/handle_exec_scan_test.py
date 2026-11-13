"""Whole handle implementation: linear exec scans and callback/concurrent cleanup."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root/'kernel/object/handle.c').read_text()
begin = source.index('kstatus_t handle_table_close_on_exec(')
end = source.index('static void handle_table_release(', begin)
part = source[begin:end]
needle = 'handle_entry_t *entry = &chunk->entries[entry_index];'
assert part.count(needle) == 1
source = source[:begin] + part.replace(needle, 'atomic_fetch_add(&visits,1); '+needle) + source[end:]
code = r'''
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdatomic.h>
static atomic_ulong visits;
''' + source + r'''
typedef struct {object_header_t header;atomic_uint refs,closes;} fixture_t;
static fixture_t object;
static handle_table_t table;
static handle_t original[4096],created[600];
static atomic_uint allocations,frees;
static _Thread_local unsigned depth;
static unsigned mode;
static atomic_bool reentered;
void sched_preempt_disable(void) {++depth;}
void sched_preempt_enable(void) {assert(depth);--depth;}
void *kzalloc(size_t n,uint32_t flags) {
    (void)flags;assert(depth);void *p=calloc(1,n);if(p)atomic_fetch_add(&allocations,1);return p;
}
void kfree(void *p) {if(p)atomic_fetch_add(&frees,1);free(p);}
void *kmalloc(size_t n,uint32_t flags) {
    (void)flags;assert(depth);void *p=malloc(n);if(p)atomic_fetch_add(&allocations,1);return p;
}
void object_get(void *p) {assert(depth);assert(atomic_fetch_add(&((fixture_t*)p)->refs,1));}
bool object_try_get(void *p) {object_get(p);return true;}
void object_put(void *p) {assert(atomic_fetch_sub(&((fixture_t*)p)->refs,1)>1);}
static void closed(void *p) {
    atomic_fetch_add(&((fixture_t*)p)->closes,1);
    if (!mode) return;
    assert(!depth && !atomic_load(&table.grow_lock.state));
    if (atomic_exchange(&reentered,true)) return;
    assert(handle_set_flags(&table,original[0],HANDLE_FLAG_CLOEXEC)==K_OK);
    /* 在扫描到未来槽位之前先将其清空。 */
    if (mode==1) assert(handle_set_flags(&table,original[299],0)==K_OK);
    unsigned count=mode==2 ? 600 : 1;
    for (unsigned i=0;i<count;++i)
        assert(handle_create_with_flags(&table,&object,7,HANDLE_FLAG_CLOEXEC,&created[i])==K_OK);
    /* 新句柄会以不同的代数重用已分离的槽位。 */
    assert((uint32_t)created[0]==100 && created[0]!=original[100]);
}
static const object_ops_t ops={.handle_close=closed};
static void populate(unsigned n,unsigned pattern) {
    assert(handle_table_init(&table)==K_OK);mode=0;atomic_store(&reentered,false);
    atomic_store(&object.closes,0);
    for (unsigned i=0;i<n;++i) {
        bool marked=pattern==1 || (pattern==2 && (i&1));
        assert(handle_create_with_flags(&table,&object,7,marked?HANDLE_FLAG_CLOEXEC:0,&original[i])==K_OK);
    }
}
static void verify(unsigned n,unsigned pattern) {
    unsigned count=pattern==1 ? n : pattern==2 ? n/2 : 0;
    assert(atomic_load(&object.closes)==count && atomic_load(&object.refs)==1+n-count);
    for (unsigned i=0;i<n;++i) {
        bool marked=pattern==1 || (pattern==2 && (i&1));void *p=0;
        assert(handle_lookup(&table,original[i],7,&p)==(marked?K_ENOENT:K_OK));
        if (!marked) {assert(p==&object);object_put(p);}
    }
    mode=0;handle_table_destroy(&table);assert(atomic_load(&object.refs)==1 && !depth);
}
static void *cleaner(void *unused) {
    (void)unused;assert(handle_table_close_on_exec(&table)==K_OK && !depth);return 0;
}
int main(void) {
    object.header.ops=&ops;atomic_init(&object.refs,1);
    assert(handle_table_close_on_exec(0)==K_EINVAL);
    for (unsigned n=0;n<=4096;n=n==0?1:n==1?256:n==256?1100:n==1100?4096:4097)
    for (unsigned pattern=0;pattern<3;++pattern) {
        populate(n,pattern);atomic_store(&visits,0);
        unsigned alloc_before=atomic_load(&allocations);
        assert(handle_table_close_on_exec(&table)==K_OK && !depth);
        unsigned capacity=table.chunk_count*HANDLE_CHUNK_ENTRIES;
        assert(atomic_load(&visits)<=(pattern?2UL:1UL)*capacity);
        assert(atomic_load(&allocations)==alloc_before);
        if(n==4096 && pattern==1) assert(atomic_load(&visits)==8192);
        verify(n,pattern);
    }
    for (unsigned reentry=1;reentry<=2;++reentry) {
        populate(300,0);
        if (reentry==2) {
            table.chunks[0]->entries[100].generation=UINT32_MAX;
            original[100]=make_handle(100,UINT32_MAX);
        }
        assert(handle_set_flags(&table,original[100],HANDLE_FLAG_CLOEXEC)==K_OK);
        assert(handle_set_flags(&table,original[299],HANDLE_FLAG_CLOEXEC)==K_OK);
        mode=reentry;
        assert(handle_table_close_on_exec(&table)==K_OK && atomic_load(&reentered));
        unsigned count=reentry==2?600:1;
        if (reentry==2) assert(handle_generation(created[0])==1);
        assert(atomic_load(&object.closes)==count+(reentry==2?3:2));
        void *p=0;
        assert(handle_lookup(&table,original[0],0,&p)==K_ENOENT);
        assert(handle_lookup(&table,original[100],0,&p)==K_ENOENT);
        for (unsigned i=0;i<count;++i)assert(handle_lookup(&table,created[i],0,&p)==K_ENOENT);
        assert(handle_lookup(&table,original[299],0,&p)==(reentry==1?K_OK:K_ENOENT));
        if(reentry==1)object_put(p);
        mode=0;handle_table_destroy(&table);assert(atomic_load(&object.refs)==1);
    }
    for (unsigned round=0;round<16;++round) {
        populate(4096,round&1?1:2);pthread_t threads[4];
        for(unsigned i=0;i<4;++i)assert(!pthread_create(&threads[i],0,cleaner,0));
        for(unsigned i=0;i<4;++i)assert(!pthread_join(threads[i],0));
        verify(4096,round&1?1:2);
    }
    assert(atomic_load(&allocations)==atomic_load(&frees));
    puts("Exec handles: dense/sparse/empty tables, 4096 removals in 8192 slot visits, no cleanup allocation, reentrant lower-slot insertion/flags/growth, 16 four-closer rounds and exact references passed");
}
'''
with tempfile.TemporaryDirectory(prefix='liteos-handle-exec-') as directory:
    binary = str(Path(directory)/'test')
    subprocess.run(['cc','-std=c11','-O2','-Wall','-Wextra','-Werror','-pthread',
                    '-I'+str(root/'include'),'-fsanitize=address,undefined',
                    '-fno-pie','-no-pie','-x','c','-','-o',binary],
                   input=code,text=True,check=True)
    subprocess.run([binary],check=True,timeout=60)
