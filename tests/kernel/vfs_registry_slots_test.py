"""Production VFS initialization and free-slot publication/growth policy."""
from pathlib import Path
import re
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'kernel/fs/vfs/core.c').read_text()
def function(name):
    start = source.index(name)
    return source[start:source.index('\n}', start) + 2]
initialize = function('static void vfs_initialize(').replace(
    'atomic_compare_exchange_strong_explicit(', 'counted_cas(')
slots = function('static uint32_t vfs_free_node_slot_locked(').replace(
    'if (g_vfs_nodes[index] == 0)', 'if ((++visits, g_vfs_nodes[index]) == 0)')
slots = slots.replace('memcpy(', 'tracked_copy(')
strings = (root / 'kernel/string.c').read_text()
copy = strings[strings.index('static void copy_forward('):strings.index('void *memmove(')]
copy = copy.replace('void *memcpy(', 'static void *actual_memcpy(')
publication = function('static void vfs_set_node_slot_locked(')
# Audit every production registry mutation, not just an isolated hint fixture.
writes = re.findall(r'g_vfs_nodes\[([^\]]+)\]\s*=(?!=)', source)
assert writes == ['i', 'index'], writes
# 定义、文件发布及各条删除路径，包括卸载时淘汰旧卷缓存。
assert source.count('vfs_set_node_slot_locked(') == 9
program = r'''
#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#define VFS_INITIAL_NODE_CAPACITY 256U
typedef struct { unsigned id; } vnode_t;
typedef struct { atomic_uint state; } spinlock_t;
static vnode_t *g_vfs_initial_nodes[VFS_INITIAL_NODE_CAPACITY];
static vnode_t **g_vfs_nodes=g_vfs_initial_nodes;
static vnode_t *g_vfs_initial_buckets[VFS_INITIAL_NODE_CAPACITY];
static vnode_t **g_vfs_buckets=g_vfs_initial_buckets;
static uint32_t g_vfs_initial_fat_buckets[VFS_INITIAL_NODE_CAPACITY];
static uint32_t g_vfs_initial_fat_next[VFS_INITIAL_NODE_CAPACITY];
static uint32_t *g_vfs_fat_buckets=g_vfs_initial_fat_buckets;
static uint32_t *g_vfs_fat_next=g_vfs_initial_fat_next;
static uint32_t g_vfs_node_capacity=VFS_INITIAL_NODE_CAPACITY;
static uint32_t g_vfs_next_free_node;
static spinlock_t g_vfs_lock;
static atomic_uint g_vfs_init_state;
static atomic_uint attempts;
static bool parallel;
static unsigned visits,allocations,frees;
static bool fail_alloc;
static size_t allocation_length,copied_bytes;
static unsigned copies;
static void vfs_namespace_insert_locked(vnode_t *v,uint32_t slot){(void)v;(void)slot;}
static void vfs_namespace_remove_locked(vnode_t *v){(void)v;}
static bool counted_cas(atomic_uint *p,unsigned *expected,unsigned value,
                       memory_order success,memory_order failure) {
 atomic_fetch_add(&attempts,1);
 if(parallel)while(atomic_load(&attempts)<8)sched_yield();
 return atomic_compare_exchange_strong_explicit(p,expected,value,success,failure);
}
static void *kzalloc(size_t size,int flags) {
 assert(flags==0);++allocations;
 if(fail_alloc)return 0;
 allocation_length=size;return calloc(1,size);
}
static void kfree(void *p) { assert(p);++frees;free(p); }
''' + copy + r'''
static void *tracked_copy(void *out,const void *in,size_t length){
 assert(in==g_vfs_nodes && out!=in);
 assert(length==(size_t)g_vfs_node_capacity*sizeof(*g_vfs_nodes));
 assert(allocation_length==2U*(size_t)g_vfs_node_capacity*
        (2U*sizeof(vnode_t*)+2U*sizeof(uint32_t)));
 ++copies;copied_bytes+=length;return actual_memcpy(out,in,length);
}
''' + initialize + slots + publication + r'''
static void *init_thread(void *unused) {
 (void)unused;vfs_initialize();
 assert(atomic_load(&g_vfs_init_state)==2 && !atomic_load(&g_vfs_lock.state));
 assert(!g_vfs_next_free_node);
 for(unsigned i=0;i<g_vfs_node_capacity;++i)assert(!g_vfs_nodes[i]);
 return 0;
}
static uint32_t oracle(void) {
 for(uint32_t i=0;i<g_vfs_node_capacity;++i)if(!g_vfs_nodes[i])return i;
 return g_vfs_node_capacity;
}
static uint32_t rng=12345;
static uint32_t random_value(void) { rng=rng*1664525U+1013904223U;return rng; }
int main(void) {
 vnode_t token={1};pthread_t threads[8];parallel=true;
 atomic_store(&g_vfs_lock.state,1);g_vfs_next_free_node=123;
 for(unsigned i=0;i<256;++i)g_vfs_nodes[i]=&token;
 for(unsigned i=0;i<8;++i)assert(!pthread_create(&threads[i],0,init_thread,0));
 for(unsigned i=0;i<8;++i)assert(!pthread_join(threads[i],0));
 parallel=false;assert(atomic_load(&attempts)==8);
 // A query does not reserve: abandoning it must not leak its slot.
 for(unsigned i=0;i<100;++i)assert(vfs_free_node_slot_locked()==0);
 visits=0;
 for(unsigned i=0;i<4096;++i) {
  assert(vfs_free_node_slot_locked()==i);
  vfs_set_node_slot_locked(i,&token);
 }
 assert(g_vfs_node_capacity==4096 && g_vfs_next_free_node==4096);
 assert(allocations==4 && frees==3 && visits==4092);
 assert(copies==4 && copied_bytes==(256U+512U+1024U+2048U)*sizeof(void*));
 for(unsigned i=0;i<4096;++i)assert(g_vfs_nodes[i]==&token);
 for(unsigned i=0;i<100000;++i)vfs_initialize();
 assert(atomic_load(&attempts)==8 && g_vfs_next_free_node==4096 && g_vfs_node_capacity==4096);
 // Failed growth leaves the full old registry intact and retries without rescanning.
 vnode_t **old=g_vfs_nodes;fail_alloc=true;visits=0;
 for(unsigned i=0;i<3;++i)assert(vfs_free_node_slot_locked()==UINT32_MAX);
 assert(g_vfs_nodes==old && g_vfs_node_capacity==4096 && visits==0);
 assert(copies==4);
 for(unsigned i=0;i<4096;++i)assert(g_vfs_nodes[i]==&token);
 vfs_set_node_slot_locked(700,0);vfs_set_node_slot_locked(123,0);
 assert(g_vfs_next_free_node==123 && vfs_free_node_slot_locked()==123);
 vfs_set_node_slot_locked(123,&token);
 assert(vfs_free_node_slot_locked()==700);vfs_set_node_slot_locked(700,&token);
 assert(vfs_free_node_slot_locked()==UINT32_MAX);
 fail_alloc=false;assert(vfs_free_node_slot_locked()==4096);
 assert(g_vfs_node_capacity==8192 && g_vfs_next_free_node==4096);
 assert(copies==5 && copied_bytes==(256U+512U+1024U+2048U+4096U)*sizeof(void*));
 for(unsigned i=4096;i<8192;++i)assert(!g_vfs_nodes[i]);
 assert(vfs_free_node_slot_locked()==4096);vfs_set_node_slot_locked(4096,&token);
 for(unsigned iteration=0;iteration<50000;++iteration) {
  unsigned index=random_value()%4096;
  if(g_vfs_nodes[index])vfs_set_node_slot_locked(index,0);
  else {
   uint32_t expected=oracle();assert(expected<g_vfs_node_capacity);
   assert(vfs_free_node_slot_locked()==expected);
   assert(vfs_free_node_slot_locked()==expected);
   vfs_set_node_slot_locked(expected,&token);
  }
  uint32_t first=oracle();assert(g_vfs_next_free_node<=first);
  for(uint32_t i=0;i<g_vfs_next_free_node;++i)assert(g_vfs_nodes[i]);
 }
 // Full capacity overflow returns failure without touching an array or allocating.
 uint32_t saved_capacity=g_vfs_node_capacity,saved_hint=g_vfs_next_free_node;
 unsigned before=allocations;
 g_vfs_node_capacity=UINT32_MAX;g_vfs_next_free_node=UINT32_MAX;
 assert(vfs_free_node_slot_locked()==UINT32_MAX && allocations==before);
 g_vfs_node_capacity=saved_capacity;g_vfs_next_free_node=saved_hint;
 kfree(g_vfs_nodes);
 assert(allocations==frees+4); // Four deliberately failed allocations.
 puts("PASS: VFS eight initializers, 100000 read-only fast calls, 4096 fills/4092 slot probes, 50000 lowest-hole model operations, abandoned queries, growth/OOM/overflow");
 puts("Registry growth: actual kernel bulk copy, exact old-prefix/new-zero-tail contents, five copies; failed allocations never copy or replace the old registry");
}
'''
with tempfile.TemporaryDirectory(prefix='liteos-vfs-registry-') as directory:
    path = Path(directory)
    (path / 'test.c').write_text(program)
    subprocess.run(['cc', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                    '-pthread', '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
                    str(path / 'test.c'), '-o', str(path / 'test')], check=True)
    subprocess.run([str(path / 'test')], check=True)
