"""Whole production handle table: references, generations, rollback and threads."""
from pathlib import Path
import subprocess
import tempfile
import resource

root = Path(__file__).resolve().parents[2]
source = (root/'kernel/object/handle.c').read_text()
pointer_copy = 'chunks[i] = table->chunks[i];'
assert source.count(pointer_copy) == 1
source = source.replace(pointer_copy, '{ ++growth_slot_copies; '+pointer_copy+' }')
tail_clear = 'chunks[i] = 0;'
assert source.count(tail_clear) == 1
source = source.replace(tail_clear, '{ ++growth_tail_clears; '+tail_clear+' }')
default_init = 'chunk->entries[i].generation = 1U;'
assert source.count(default_init) == 1
source = source.replace(default_init, '++fresh_entry_initializations; '+default_init)
release_start = source.index('static void handle_table_release(handle_table_t *table, bool close_objects) {')
release = source[release_start:source.index('void handle_table_destroy(', release_start)]
access = 'handle_chunk_t *chunk = chunks[chunk_index];'
assert release.count(access) == 1
source = source[:release_start] + source[release_start:].replace(access,
    '++release_slot_visits; '+access, 1)
code = r'''
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static unsigned release_slot_visits;
static unsigned fresh_entry_initializations;
static unsigned growth_slot_copies;
static unsigned growth_tail_clears;
static unsigned clone_zeroed_bytes;
''' + source + r'''
_Static_assert(sizeof(spinlock_t)==sizeof(rwlock_t),"chunk lock size unchanged");
_Static_assert(_Alignof(spinlock_t)==_Alignof(rwlock_t),"chunk lock alignment unchanged");
typedef struct {rwlock_t lock;handle_entry_t entries[HANDLE_CHUNK_ENTRIES];} old_chunk_layout_t;
_Static_assert(sizeof(handle_chunk_t)==sizeof(old_chunk_layout_t),"chunk size unchanged");
_Static_assert(offsetof(handle_chunk_t,entries)==offsetof(old_chunk_layout_t,entries),"entry offset unchanged");
typedef struct {object_header_t header;atomic_uint count,closes;} test_object_t;
static test_object_t objects[2];
static atomic_uint allocations,frees,lock_entries;
static int fail_after=-1;
static bool reject_reference;
static atomic_uint try_get_calls,get_calls,put_calls;
static _Thread_local unsigned depth;
static unsigned unlocked_arrays;
static void (*allocation_hook)(void);
static void (*chunk_allocation_hook)(void);
static handle_table_t table;
static bool release_mode,destroy_before_lock;
static unsigned destroy_lock_countdown;
static atomic_uint release_callbacks,release_puts;
void sched_preempt_disable(void) {
 ++depth;atomic_fetch_add(&lock_entries,1);
 if(destroy_before_lock || (destroy_lock_countdown && --destroy_lock_countdown==0)) {destroy_before_lock=false;handle_table_destroy(&table);}
}
void sched_preempt_enable(void) {assert(depth);--depth;}
void *kzalloc(size_t n,uint32_t flags) {
 (void)flags;
 if(!depth) {
  assert(n>=4*sizeof(handle_chunk_t *) && n%sizeof(handle_chunk_t *)==0);
  ++unlocked_arrays;
  if(allocation_hook){void (*hook)(void)=allocation_hook;allocation_hook=0;hook();}
 }
 if(fail_after==0)return 0;if(fail_after>0)--fail_after;
 void *p=calloc(1,n);if(p)atomic_fetch_add(&allocations,1);if(p && n==sizeof(handle_chunk_t))clone_zeroed_bytes+=(unsigned)n;return p;
}
void *kmalloc(size_t n,uint32_t flags) {
 (void)flags;assert(depth || n==sizeof(handle_chunk_t));
 if(n==sizeof(handle_chunk_t)) {
  assert(!depth);
  if(chunk_allocation_hook){void (*hook)(void)=chunk_allocation_hook;chunk_allocation_hook=0;hook();}
 }
 if(fail_after==0)return 0;if(fail_after>0)--fail_after;
 void *p=malloc(n);if(p){memset(p,0xa5,n);atomic_fetch_add(&allocations,1);}return p;
}
void kfree(void *p) {if(p)atomic_fetch_add(&frees,1);free(p);}
void object_get(void *p) {assert(depth);atomic_fetch_add(&get_calls,1);assert(atomic_fetch_add(&((test_object_t*)p)->count,1));}
bool object_try_get(void *p) {assert(depth);atomic_fetch_add(&try_get_calls,1);if(reject_reference)return false;object_get(p);return true;}
void object_put(void *p) {
 atomic_fetch_add(&put_calls,1);
 if(release_mode) {assert(!depth);atomic_fetch_add(&release_puts,1);}
 assert(atomic_fetch_sub(&((test_object_t*)p)->count,1)>1);
}
static void closed(void *p) {
 atomic_fetch_add(&((test_object_t*)p)->closes,1);
 if(release_mode) {
  assert(!depth); // another destroyer may own the lock, but this caller must not
  handle_lock(&table.grow_lock);
  assert(!table.chunks && !table.chunk_count && !table.chunk_capacity);
  handle_unlock(&table.grow_lock);
  atomic_fetch_add(&release_callbacks,1);
  void *result=(void*)1;uint32_t flags=123;handle_t handle=123;
  assert(handle_lookup(&table,make_handle(0,1),0,&result)==K_ENOENT && result==(void*)1);
  assert(handle_get_flags(&table,make_handle(0,1),&flags)==K_ENOENT && flags==123);
  assert(handle_close(&table,make_handle(0,1))==K_ENOENT);
  assert(handle_create(&table,p,7,&handle)==K_EINVAL && handle==123);
  handle_table_destroy(&table); // recursive cleanup sees an already empty table
 }
}
static const object_ops_t ops={.handle_close=closed};
static void *destroyer(void *unused) {(void)unused;handle_table_destroy(&table);assert(!depth);return 0;}
static void assert_absent(handle_t h) {
 unsigned nested=depth,refs=atomic_load(&objects[0].count),closes=atomic_load(&objects[0].closes);
 uint32_t flags=123;void *p=(void*)1;
 assert(handle_lookup(&table,h,0,&p)==K_ENOENT && p==(void*)1);
 assert(handle_get_flags(&table,h,&flags)==K_ENOENT && flags==123);
 assert(handle_set_flags(&table,h,HANDLE_FLAG_CLOEXEC)==K_ENOENT);
 assert(handle_set_flags(&table,h,~HANDLE_FLAG_MASK)==K_EINVAL);
 assert(handle_close(&table,h)==K_ENOENT);
 assert(depth==nested && !handle_debug_waiting_lock());
 assert(atomic_load(&objects[0].count)==refs && atomic_load(&objects[0].closes)==closes);
}
static void *worker(void *unused) {
 (void)unused;
 for(unsigned i=0;i<2000;++i) {
  handle_t h;void *p=0;
  assert(handle_create(&table,&objects[0],7,&h)==K_OK);
  uint32_t flags=99;
  assert(handle_set_flags(&table,h,HANDLE_FLAG_CLOEXEC)==K_OK);
  assert(handle_get_flags(&table,h,&flags)==K_OK && flags==HANDLE_FLAG_CLOEXEC);
  assert(handle_set_flags(&table,h,0)==K_OK);
  assert(handle_lookup(&table,h,7,&p)==K_OK && p==&objects[0]);object_put(p);
  assert(handle_close(&table,h)==K_OK && !depth);
 }
 return 0;
}
static void grow_during_clone(void) {
 assert(!depth);handle_lock(&table.grow_lock);
 assert(grow_table_locked(&table)==K_OK);handle_unlock(&table.grow_lock);
}
static void destroy_during_clone(void) {assert(!depth);handle_table_destroy(&table);}
static void add_chunk_during_clone(void) {
 assert(!depth);handle_lock(&table.grow_lock);
 assert(table.chunk_count<table.chunk_capacity);
 handle_chunk_t *chunk=chunk_allocate();assert(chunk);
 table.chunks[table.chunk_count++]=chunk;handle_unlock(&table.grow_lock);
}
static void drop_chunk_during_clone(void) {
 assert(!depth);handle_lock(&table.grow_lock);assert(table.chunk_count);
 handle_chunk_t *chunk=table.chunks[--table.chunk_count];table.chunks[table.chunk_count]=0;
 handle_unlock(&table.grow_lock);kfree(chunk);
}
static void exhaust_after_growth(void) {add_chunk_during_clone();fail_after=1;}
int main(void) {
 for(unsigned i=0;i<2;++i) {objects[i].header.type=100;objects[i].header.flags=0x8000U;objects[i].header.ops=&ops;atomic_init(&objects[i].count,1);}
 assert(handle_table_init(&table)==K_OK && !depth);
 /* 结构上不可能有效的句柄绝不能获取表/块锁，即使此时可能发生存储扩展竞争。
  * 所有操作都必须保留输出和引用。 */
 unsigned rejected_start=atomic_load(&lock_entries);
 for(uint32_t index=1;index<=65536;++index) {
  assert_absent(make_handle(index,0));
  assert_absent(make_handle((HANDLE_MAX_CHUNKS << HANDLE_CHUNK_SHIFT)+index,1));
  handle_t duplicate=123;
  assert(handle_duplicate(&table,make_handle(index,0),0,&duplicate)==K_ENOENT && duplicate==123);
 }
 assert(atomic_load(&lock_entries)==rejected_start);
 /* 已分配指针数组不代表块本身已分配。释放扫描必须恰好遍历已发布的高水位范围，
  * 包括其中的空槽。 */
 for(unsigned count=0;count<=8;++count){
  handle_table_destroy(&table);assert(handle_table_init(&table)==K_OK);
  handle_lock(&table.grow_lock);
  while(table.chunk_capacity<8)assert(grow_table_locked(&table)==K_OK);
  for(unsigned i=0;i<count;++i){
   if(i&1)table.chunks[i]=chunk_allocate();
   assert(!(i&1)||table.chunks[i]);
  }
  table.chunk_count=count;
  handle_chunk_t **original=table.chunks;
  unsigned original_capacity=table.chunk_capacity;
  growth_slot_copies=growth_tail_clears=0;fail_after=0;
  assert(grow_table_locked(&table)==K_ENOMEM && table.chunks==original);
  assert(table.chunk_capacity==original_capacity && table.chunk_count==count && !growth_slot_copies);
  assert(!growth_tail_clears);
  fail_after=-1;
  assert(grow_table_locked(&table)==K_OK && table.chunk_capacity==original_capacity*2);
  assert(growth_slot_copies==count && table.chunk_count==count);
  assert(growth_tail_clears==table.chunk_capacity-count);
  for(unsigned i=0;i<table.chunk_capacity;++i)
   assert((table.chunks[i]!=0)==(i<count && (i&1)));
  handle_unlock(&table.grow_lock);
  release_slot_visits=0;handle_table_destroy(&table);
  assert(release_slot_visits==count && !depth);
  assert(handle_table_init(&table)==K_OK);
 }
 enum {N=1100};handle_t handles[N];
 for(unsigned i=0;i<N;++i) {
  assert(handle_create_with_flags(&table,&objects[i&1],7,i&1,&handles[i])==K_OK);
  assert((uint32_t)handles[i]==i && (handles[i]>>32)==1 && !depth);
 }
 assert(table.chunk_capacity==8 && table.chunk_count==5);
 /* 重用低位空槽后，下一个空槽之前仍会有一段较长的占用后缀。 */
 assert(handle_close(&table,handles[0])==K_OK);
 assert(handle_create(&table,&objects[0],7,&handles[0])==K_OK && (handles[0]>>32)==2);
 unsigned lock_start=atomic_load(&lock_entries);handle_t extra;
 assert(handle_create(&table,&objects[0],7,&extra)==K_OK && (uint32_t)extra==N);
 assert(atomic_load(&lock_entries)-lock_start==1+table.chunk_count);
 assert(handle_close(&table,extra)==K_OK);
 /* 查找和复制必须共用引用获取逻辑，但拒绝检查要先于 try-get；任一失败都不得覆盖输出。 */
 unsigned try_start=atomic_load(&try_get_calls),ref_start=atomic_load(&objects[0].count);
 void *unchanged=(void*)1;handle_t unchanged_handle=123;
 reject_reference=true;
 assert(handle_lookup(&table,handles[0],8,&unchanged)==K_EACCES && unchanged==(void*)1);
 assert(atomic_load(&try_get_calls)==try_start);
 assert(handle_lookup(&table,handles[0],7,&unchanged)==K_ENOENT && unchanged==(void*)1);
 assert(handle_duplicate(&table,handles[0],0,&unchanged_handle)==K_ENOENT && unchanged_handle==123);
 assert(atomic_load(&try_get_calls)==try_start+2 && atomic_load(&objects[0].count)==ref_start && !depth);
 reject_reference=false;
 assert(handle_lookup(0,handles[0],0,&unchanged)==K_EINVAL);
 assert(handle_lookup(&table,0,0,&unchanged)==K_EINVAL);
 assert(handle_lookup(&table,handles[0],0,0)==K_EINVAL);
 assert(handle_lookup(&table,make_handle(UINT32_MAX,1),0,&unchanged)==K_ENOENT);
 assert(handle_lookup(&table,make_handle(0,1),0,&unchanged)==K_ENOENT && unchanged==(void*)1);
 assert(handle_duplicate(&table,make_handle(0,1),0,&unchanged_handle)==K_ENOENT && unchanged_handle==123);
 assert(atomic_load(&try_get_calls)==try_start+2 && !depth);
 sched_preempt_disable();sched_preempt_disable();
 assert_absent(make_handle(0,1)); /* Stale generation in a populated chunk. */
 assert_absent(extra); /* Empty entry in a populated chunk. */
 assert_absent(make_handle(6*HANDLE_CHUNK_ENTRIES,1)); /* Unallocated chunk. */
 assert_absent(make_handle(UINT32_MAX,1)); /* Outside the pointer array. */
 uint32_t invalid_flags=123;
 assert(handle_get_flags(0,handles[0],&invalid_flags)==K_EINVAL && invalid_flags==123);
 assert(handle_get_flags(&table,0,&invalid_flags)==K_EINVAL);
 assert(handle_get_flags(&table,handles[0],0)==K_EINVAL);
 assert(handle_set_flags(0,handles[0],0)==K_EINVAL && handle_set_flags(&table,0,0)==K_EINVAL);
 assert(handle_close(0,handles[0])==K_EINVAL && handle_close(&table,0)==K_EINVAL);
 assert(depth==2);sched_preempt_enable();sched_preempt_enable();
 for(unsigned i=0;i<N;++i) {
  void *p=0;uint32_t flags=99;
  assert(handle_lookup(&table,handles[i],7,&p)==K_OK && p==&objects[i&1]);object_put(p);
  p=(void*)1;assert(handle_lookup(&table,handles[i],8,&p)==K_EACCES && p==(void*)1);
  assert(handle_get_flags(&table,handles[i],&flags)==K_OK && flags==(i&1));
 }
 unsigned refs[2]={atomic_load(&objects[0].count),atomic_load(&objects[1].count)};
 for(unsigned exclude=0;exclude<2;++exclude) for(int failure=0;failure<6;++failure) {
  handle_table_t clone;fail_after=failure;
  unsigned closes[2]={atomic_load(&objects[0].closes),atomic_load(&objects[1].closes)};
  unsigned initialized=fresh_entry_initializations;
  unsigned cleared=clone_zeroed_bytes;
  objects[1].header.flags=0x8000U | (exclude?OBJECT_FLAG_NO_FORK:0U);
  kstatus_t status=handle_table_clone(&table,&clone);
  assert(fresh_entry_initializations==initialized);
  assert(clone_zeroed_bytes==cleared);
  assert(status==K_ENOMEM);fail_after=-1;
  assert(atomic_load(&objects[0].count)==refs[0] && atomic_load(&objects[1].count)==refs[1] && !depth);
  assert(atomic_load(&objects[0].closes)==closes[0] && atomic_load(&objects[1].closes)==closes[1]);
  assert(!clone.chunks && !clone.chunk_count && !clone.chunk_capacity);
  handle_table_destroy(&clone);
  for(unsigned i=0;i<N;++i) {
   void *p=0;assert(handle_lookup(&table,handles[i],7,&p)==K_OK && p==&objects[i&1]);object_put(p);
  }
 }
 /* 同类型对象都禁止继承时，克隆保留表结构但不得获取引用或关闭源对象。 */
 objects[0].header.flags=objects[1].header.flags=OBJECT_FLAG_NO_FORK;
 handle_table_t empty_clone;
 unsigned inherited_gets=atomic_load(&get_calls);
 assert(handle_table_clone(&table,&empty_clone)==K_OK);
 assert(atomic_load(&get_calls)==inherited_gets);
 for(unsigned i=0;i<N;++i) {
  void *p=(void*)1;
  assert(handle_lookup(&empty_clone,handles[i],0,&p)==K_ENOENT && p==(void*)1);
 }
 handle_table_destroy(&empty_clone);
 assert(atomic_load(&objects[0].count)==refs[0] && atomic_load(&objects[1].count)==refs[1]);
 objects[0].header.flags=objects[1].header.flags=0x8000U;
 /* 覆盖保留和排除的克隆项中的代数回绕。 */
 table.chunks[1]->entries[1].generation=UINT32_MAX;
 handles[257]=make_handle(257,UINT32_MAX);
 handle_table_t clone,excluded;
 unsigned initialized=fresh_entry_initializations;
 unsigned cleared=clone_zeroed_bytes;
 objects[1].header.flags=0x8000U;
 assert(handle_table_clone(&table,&clone)==K_OK);
 objects[1].header.flags|=OBJECT_FLAG_NO_FORK;
 assert(handle_table_clone(&table,&excluded)==K_OK);
 objects[1].header.flags=0x8000U;
 assert(fresh_entry_initializations==initialized);
 assert(clone_zeroed_bytes==cleared);
 for(unsigned chunk=0;chunk<table.chunk_count;++chunk){
  assert((clone.chunks[chunk]!=0)==(table.chunks[chunk]!=0));
  if(table.chunks[chunk])assert(!memcmp(clone.chunks[chunk]->entries,
      table.chunks[chunk]->entries,sizeof(table.chunks[chunk]->entries)));
 }
 for(unsigned i=0;i<N;++i) {
  void *p=0;
  assert(handle_lookup(&clone,handles[i],7,&p)==K_OK && p==&objects[i&1]);object_put(p);
  kstatus_t status=handle_lookup(&excluded,handles[i],7,&p);
  assert(status==((i&1)?K_ENOENT:K_OK));if(!(i&1))object_put(p);
 }
 handle_t inherited_holes[N/2];
 unsigned before_allocations=atomic_load(&allocations);
 for(unsigned i=0;i<N/2;++i) {
  assert(handle_create(&excluded,&objects[0],7,&inherited_holes[i])==K_OK);
  unsigned index=2*i+1;
  assert((uint32_t)inherited_holes[i]==index);
  assert((inherited_holes[i]>>32)==(index==257?1:2));
  unchanged=(void*)1;
  assert(handle_lookup(&excluded,handles[index],0,&unchanged)==K_ENOENT && unchanged==(void*)1);
  assert(handle_lookup(&table,handles[index],7,&unchanged)==K_OK && unchanged==&objects[1]);object_put(unchanged);
 }
 assert(atomic_load(&allocations)==before_allocations && excluded.chunk_capacity==table.chunk_capacity);
 for(unsigned i=0;i<N/2;++i)assert(handle_close(&excluded,inherited_holes[i])==K_OK);
 handle_table_destroy(&clone);handle_table_destroy(&excluded);
 assert(atomic_load(&objects[0].closes)==N+2+N/2 && atomic_load(&objects[1].closes)==N/2);
 assert(atomic_load(&objects[0].count)==refs[0] && atomic_load(&objects[1].count)==refs[1]);
 assert(handle_close(&table,handles[0])==K_OK);
 void *p=0;assert(handle_lookup(&table,handles[0],0,&p)==K_ENOENT);
 handle_t replacement,duplicate;
 assert(handle_create(&table,&objects[0],7,&replacement)==K_OK && (uint32_t)replacement==0 && (replacement>>32)==3);
 objects[1].header.flags|=OBJECT_FLAG_NO_FORK;
 assert(handle_duplicate(&table,handles[1],0,&duplicate)==K_OK);
 objects[1].header.flags=0x8000U;
 assert(handle_set_flags(&table,replacement,HANDLE_FLAG_CLOEXEC)==K_OK);
 assert(handle_table_close_on_exec(&table)==K_OK);
 assert(handle_lookup(&table,replacement,0,&p)==K_ENOENT);
 for(unsigned i=1;i<N;++i) {
  kstatus_t status=handle_lookup(&table,handles[i],0,&p);
  assert(status==((i&1)?K_ENOENT:K_OK));if(!(i&1))object_put(p);
 }
 assert(handle_lookup(&table,duplicate,7,&p)==K_OK && p==&objects[1]);object_put(p);
 handle_table_destroy(&table);
 assert(atomic_load(&objects[0].count)==1 && atomic_load(&objects[1].count)==1 && !depth);
 assert_absent(make_handle(0,1)); /* Destroyed table has no backing array. */
 assert(handle_table_init(&table)==K_OK);
 for(unsigned i=0;i<N;++i)assert(handle_create(&table,&objects[0],7,&handles[i])==K_OK);
 for(unsigned hole=0;hole<N;++hole) {
  assert(handle_close(&table,handles[hole])==K_OK);
  assert(handle_create(&table,&objects[0],7,&handles[hole])==K_OK);
  assert((uint32_t)handles[hole]==hole && (handles[hole]>>32)==2);
  lock_start=atomic_load(&lock_entries);
  assert(handle_create(&table,&objects[0],7,&extra)==K_OK && (uint32_t)extra==N);
  assert(atomic_load(&lock_entries)-lock_start==2+(N>>HANDLE_CHUNK_SHIFT)-((hole+1)>>HANDLE_CHUNK_SHIFT));
  assert(handle_close(&table,extra)==K_OK);
 }
 handle_table_destroy(&table);
 assert(atomic_load(&objects[0].count)==1 && !depth);
 assert(handle_table_init(&table)==K_OK);
 for(unsigned granted=0;granted<16;++granted) {
  handle_t original,copied;void *p=0;
  assert(handle_create(&table,&objects[0],granted,&original)==K_OK);
  unsigned gets=atomic_load(&get_calls),puts=atomic_load(&put_calls);
  assert(handle_duplicate(&table,original,HANDLE_FLAG_CLOEXEC,&copied)==K_OK);
  assert(atomic_load(&get_calls)==gets+1 && atomic_load(&put_calls)==puts);
  uint32_t flags=0;assert(handle_get_flags(&table,copied,&flags)==K_OK && flags==HANDLE_FLAG_CLOEXEC);
  for(unsigned requested=0;requested<16;++requested) for(unsigned which=0;which<2;++which) {
   unsigned before=atomic_load(&objects[0].count);p=(void*)1;
   kstatus_t status=handle_lookup(&table,which?copied:original,requested,&p);
   if((granted&requested)==requested) {
    assert(status==K_OK && p==&objects[0] && atomic_load(&objects[0].count)==before+1);object_put(p);
   } else assert(status==K_EACCES && p==(void*)1 && atomic_load(&objects[0].count)==before);
   assert(!depth);
  }
  assert(handle_close(&table,original)==K_OK && handle_close(&table,copied)==K_OK);
 }
 handle_table_destroy(&table);
 assert(atomic_load(&objects[0].count)==1 && !depth);
 /* 分配失败时必须归还临时引用、保持输出不变，并保留所有现有句柄。
  * 分别测试块和数组扩展失败，再使用同一源句柄重试。 */
 for(unsigned limit=HANDLE_CHUNK_ENTRIES;limit<=4*HANDLE_CHUNK_ENTRIES;limit*=4) {
  assert(handle_table_init(&table)==K_OK);
  for(unsigned i=0;i<limit;++i)assert(handle_create(&table,&objects[0],7,&handles[i])==K_OK);
  unsigned refs=atomic_load(&objects[0].count);
  unsigned gets=atomic_load(&get_calls),puts=atomic_load(&put_calls);
  handle_t copied=123;fail_after=0;
  assert(handle_duplicate(&table,handles[0],0,&copied)==K_ENOMEM && copied==123);
  fail_after=-1;
  assert(atomic_load(&objects[0].count)==refs && !depth);
  assert(atomic_load(&get_calls)==gets+1 && atomic_load(&put_calls)==puts+1);
  for(unsigned i=0;i<limit;++i) {void *p=0;assert(handle_lookup(&table,handles[i],7,&p)==K_OK && p==&objects[0]);object_put(p);}
  assert(handle_duplicate(&table,handles[0],0,&copied)==K_OK);
  assert(atomic_load(&objects[0].count)==refs+1);
  handle_table_destroy(&table);
  assert(atomic_load(&objects[0].count)==1 && !depth);
 }
 assert(handle_table_init(&table)==K_OK);
 pthread_t threads[4];for(unsigned i=0;i<4;++i)assert(!pthread_create(&threads[i],0,worker,0));
 for(unsigned i=0;i<4;++i)assert(!pthread_join(threads[i],0));
 handle_table_destroy(&table);
 assert(atomic_load(&objects[0].count)==1 && atomic_load(&objects[1].count)==1);
 assert(handle_table_init(&table)==K_OK);
 for(unsigned i=0;i<N;++i)assert(handle_create(&table,&objects[i&1],7,&handles[i])==K_OK);
 release_mode=true;
 for(unsigned i=0;i<4;++i)assert(!pthread_create(&threads[i],0,destroyer,0));
 for(unsigned i=0;i<4;++i)assert(!pthread_join(threads[i],0));
 release_mode=false;
 assert(atomic_load(&release_callbacks)==N && atomic_load(&release_puts)==N);
 assert(atomic_load(&objects[0].count)==1 && atomic_load(&objects[1].count)==1);
 assert(handle_table_init(&table)==K_OK);
 assert(handle_create(&table,&objects[0],7,&extra)==K_OK);
 unsigned alloc_before=atomic_load(&allocations);handle_t not_created=123;
 destroy_before_lock=true;
 assert(handle_create(&table,&objects[0],7,&not_created)==K_EINVAL && not_created==123);
 assert(!table.chunks && !table.chunk_capacity && atomic_load(&allocations)==alloc_before);
 assert(atomic_load(&objects[0].count)==1);
 assert(handle_table_init(&table)==K_OK);
 assert(handle_create(&table,&objects[0],7,&extra)==K_OK);
 /* 获取两个源锁，然后在发布目标之前执行销毁。 */
 destroy_lock_countdown=3;not_created=123;
 assert(handle_duplicate(&table,extra,0,&not_created)==K_EINVAL && not_created==123);
 assert(!table.chunks && !destroy_lock_countdown && !depth);
 assert(atomic_load(&objects[0].count)==1);
 /* 分配窗口内扩容或销毁：重新加锁后必须更新快照或安全失败。 */
 assert(handle_table_init(&table)==K_OK);
 handle_table_t raced;
 unsigned unlocked_before=unlocked_arrays;
 allocation_hook=grow_during_clone;
 assert(handle_table_clone(&table,&raced)==K_OK && !allocation_hook);
 assert(unlocked_arrays==unlocked_before+2 && raced.chunk_capacity==table.chunk_capacity);
 handle_table_destroy(&raced);
 allocation_hook=destroy_during_clone;unlocked_before=unlocked_arrays;
 assert(handle_table_clone(&table,&raced)==K_EINVAL && !allocation_hook);
 assert(unlocked_arrays==unlocked_before+1 && !raced.chunks && !raced.chunk_capacity && !depth);
 handle_table_destroy(&raced);handle_table_destroy(&table);
 /* 容量不变但实际分块增加：预留不足时必须重试，不能在复制期间分配。 */
 assert(handle_table_init(&table)==K_OK);add_chunk_during_clone();
 chunk_allocation_hook=add_chunk_during_clone;unlocked_before=unlocked_arrays;
 assert(handle_table_clone(&table,&raced)==K_OK && !chunk_allocation_hook);
 assert(unlocked_arrays==unlocked_before+2 && raced.chunk_count==2);
 handle_table_destroy(&raced);
 /* 实际分块减少时回收多余预留块，无需再次分配。 */
 chunk_allocation_hook=drop_chunk_during_clone;unlocked_before=unlocked_arrays;
 assert(handle_table_clone(&table,&raced)==K_OK && !chunk_allocation_hook);
 assert(unlocked_arrays==unlocked_before+1 && raced.chunk_count==1);
 handle_table_destroy(&raced);
 chunk_allocation_hook=exhaust_after_growth;
 assert(handle_table_clone(&table,&raced)==K_ENOMEM && !chunk_allocation_hook);
 fail_after=-1;assert(!raced.chunks && !raced.chunk_capacity && !depth);
 handle_table_destroy(&raced);handle_table_destroy(&table);
 assert(atomic_load(&allocations)==atomic_load(&frees) && !depth && !handle_debug_waiting_lock());
 puts("Handles: growth/reuse/permissions/generations, twelve OOM rollbacks, exec, 8000 concurrent operations, 1100 unlocked reentrant callbacks/puts with four destroyers, no resurrection after destroy and balanced references passed");
}
'''
with tempfile.TemporaryDirectory(prefix='liteos-handles-') as directory:
    binary = str(Path(directory)/'test')
    subprocess.run(['cc','-std=c11','-O2','-Wall','-Wextra','-Werror',
                    '-pthread','-I'+str(root/'include'),'-fsanitize=address,undefined',
                    '-fno-pie','-no-pie','-x','c','-','-o',binary],
                   input=code,text=True,check=True)
    subprocess.run([binary],check=True,timeout=30)
    policy = 'OBJECT_FLAG_NO_FORK) != 0U'
    assert policy in code
    negative = code.replace(policy, 'OBJECT_FLAG_NO_FORK) == 0U')
    subprocess.run(['cc','-std=c11','-O2','-Wall','-Wextra','-Werror',
                    '-pthread','-I'+str(root/'include'),'-x','c','-','-o',binary],
                   input=negative,text=True,check=True)
    result=subprocess.run([binary],capture_output=True,text=True,timeout=30,
                          preexec_fn=lambda:resource.setrlimit(resource.RLIMIT_CORE,(0,0)))
    assert result.returncode != 0 and 'Assertion' in result.stderr,result.stderr
    print('Negative control rejected: inverted fork inheritance; same-type objects, unrelated flags, generations and references passed')
    negative = code.replace('source->chunk_capacity == capacity &&', '')
    assert negative != code
    subprocess.run(['cc','-std=c11','-O2','-Wall','-Wextra','-Werror',
                    '-pthread','-I'+str(root/'include'),'-x','c','-','-o',binary],
                   input=negative,text=True,check=True)
    result=subprocess.run([binary],capture_output=True,text=True,timeout=30)
    assert result.returncode != 0 and 'raced.chunk_capacity==table.chunk_capacity' in result.stderr,result.stderr
    print('Negative control rejected: stale clone array capacity after concurrent growth')
    bound = 'uint32_t chunk_count = table->chunk_count;'
    assert bound in code
    negative = code.replace(bound, 'uint32_t chunk_count = table->chunk_capacity;')
    subprocess.run(['cc','-std=c11','-O2','-Wall','-Wextra','-Werror',
                    '-pthread','-I'+str(root/'include'),'-x','c','-','-o',binary],
                   input=negative,text=True,check=True)
    result=subprocess.run([binary],capture_output=True,text=True,timeout=30,
                          preexec_fn=lambda:resource.setrlimit(resource.RLIMIT_CORE,(0,0)))
    assert result.returncode != 0 and 'Assertion' in result.stderr,result.stderr
    print('Negative control rejected: scanning unused chunk capacity during release')
    direct = 'handle_chunk_t *destination_chunk = (handle_chunk_t *)kmalloc(sizeof(*destination_chunk), 0);'
    assert direct in code
    negative=code.replace(direct,'handle_chunk_t *destination_chunk = chunk_allocate();')
    subprocess.run(['cc','-std=c11','-O2','-Wall','-Wextra','-Werror',
                    '-pthread','-I'+str(root/'include'),'-x','c','-','-o',binary],
                   input=negative,text=True,check=True)
    result=subprocess.run([binary],capture_output=True,text=True,timeout=30,
                          preexec_fn=lambda:resource.setrlimit(resource.RLIMIT_CORE,(0,0)))
    assert result.returncode != 0 and 'Assertion' in result.stderr,result.stderr
    print('Negative control rejected: redundant clone entry defaults')
    negative=code.replace(direct,direct.replace('kmalloc(', 'kzalloc('))
    subprocess.run(['cc','-std=c11','-O2','-Wall','-Wextra','-Werror',
                    '-pthread','-I'+str(root/'include'),'-x','c','-','-o',binary],
                   input=negative,text=True,check=True)
    result=subprocess.run([binary],capture_output=True,text=True,timeout=30,
                          preexec_fn=lambda:resource.setrlimit(resource.RLIMIT_CORE,(0,0)))
    assert result.returncode != 0 and 'clone_zeroed_bytes==cleared' in result.stderr,result.stderr
    print('Negative control rejected: redundant zeroing of cloned handle entries; poisoned raw allocations preserve full entries and failure cleanup')
    growth = 'for (uint32_t i = 0; i < table->chunk_count; ++i)'
    assert growth in code
    negative=code.replace(growth,'for (uint32_t i = 0; i < table->chunk_capacity; ++i)')
    subprocess.run(['cc','-std=c11','-O2','-Wall','-Wextra','-Werror',
                    '-pthread','-I'+str(root/'include'),'-x','c','-','-o',binary],
                   input=negative,text=True,check=True)
    result=subprocess.run([binary],capture_output=True,text=True,timeout=30,
                          preexec_fn=lambda:resource.setrlimit(resource.RLIMIT_CORE,(0,0)))
    assert result.returncode != 0 and 'Assertion' in result.stderr,result.stderr
    print('Negative control rejected: copying unused capacity during growth')
    tail_loop = 'for (uint32_t i = table->chunk_count; i < new_capacity; ++i)'
    assert tail_loop in code
    negative=code.replace(tail_loop,'for (uint32_t i = new_capacity; i < new_capacity; ++i)')
    subprocess.run(['cc','-std=c11','-O2','-Wall','-Wextra','-Werror',
                    '-pthread','-I'+str(root/'include'),'-x','c','-','-o',binary],
                   input=negative,text=True,check=True)
    result=subprocess.run([binary],capture_output=True,text=True,timeout=30,
                          preexec_fn=lambda:resource.setrlimit(resource.RLIMIT_CORE,(0,0)))
    assert result.returncode != 0,result.stderr
    print('Negative control rejected: uninitialized expanded pointer suffix; exact prefix copy plus suffix clearing passed')
    transfer = 'handle_create_internal(table, object, rights, flags, out, true)'
    assert transfer in code
    negative=code.replace(transfer,'handle_create_internal(table, object, rights, flags, out, false)')
    subprocess.run(['cc','-std=c11','-O2','-Wall','-Wextra','-Werror',
                    '-pthread','-I'+str(root/'include'),'-x','c','-','-o',binary],
                   input=negative,text=True,check=True)
    result=subprocess.run([binary],capture_output=True,text=True,timeout=30,
                          preexec_fn=lambda:resource.setrlimit(resource.RLIMIT_CORE,(0,0)))
    assert result.returncode != 0 and 'Assertion' in result.stderr,result.stderr
    print('Negative control rejected: duplicate publication takes an extra reference')
    shape = 'if (handle_generation(handle) == 0U || chunk_index >= HANDLE_MAX_CHUNKS)\n        return 0;'
    assert shape in code
    negative=code.replace(shape,'')
    subprocess.run(['cc','-std=c11','-O2','-Wall','-Wextra','-Werror',
                    '-pthread','-I'+str(root/'include'),'-x','c','-','-o',binary],
                   input=negative,text=True,check=True)
    result=subprocess.run([binary],capture_output=True,text=True,timeout=30,
                          preexec_fn=lambda:resource.setrlimit(resource.RLIMIT_CORE,(0,0)))
    assert result.returncode != 0 and 'rejected_start' in result.stderr,result.stderr
    print('Negative control rejected: impossible handle requests acquire locks; 589824 operations now use zero locks')
