"""对象等待快速路径：就绪/零超时不分配，阻塞与失败保持引用平衡。"""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
code = r'''
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <kernel/mm.h>
#include <kernel/object.h>
static unsigned allocations, releases, subscriptions, predicates;
static bool fail_allocation, ready, ready_on_subscribe, consume;
void *kmalloc(size_t size, uint32_t flags) {
 (void)flags; ++allocations; return fail_allocation ? 0 : malloc(size);
}
void kfree(void *p) { assert(p); ++releases; free(p); }
void wait_queue_init(wait_queue_t *q) { *q = (wait_queue_t){0}; }
uint32_t wake_all(wait_queue_t *q) { (void)q; return 0; }
static bool is_ready(const void *p) {
 const object_header_t *h=p; assert(atomic_load(&h->refs.value)>=2);return ready;
}
static int64_t value(const void *p) { (void)p;return 42; }
static bool predicate(void *p) {
 ++predicates;assert(atomic_load(&((object_header_t *)p)->refs.value)>=2);
 bool result=ready;if(consume)ready=false;return result;
}
kstatus_t wait_on_queues_in_place(wait_queue_t **queues,size_t count,
 bool (*check)(void *),void *context,uint64_t timeout) {
 assert(timeout==123 && count);++subscriptions;
 for(size_t i=0;i<count;++i)assert(queues[i]);
 if(ready_on_subscribe)ready=true;
 return check(context)?K_OK:K_ETIMEDOUT;
}
int main(void) {
 object_header_t object;const object_ops_t ops={.is_signaled=is_ready,.wait_value=value};
 object_header_init(&object,1,&ops);
 void *objects[1024];for(unsigned i=0;i<1024;++i)objects[i]=&object;
 const size_t counts[]={1,WAIT_INLINE_QUEUE_COUNT,WAIT_INLINE_QUEUE_COUNT+1,1024};
 for(unsigned n=0;n<sizeof(counts)/sizeof(*counts);++n) {
  size_t count=counts[n];uint32_t index;int64_t result;
  for(unsigned all=0;all<2;++all)for(unsigned finite=0;finite<2;++finite) {
   ready=true;fail_allocation=true;unsigned before=allocations,calls=subscriptions;
   index=77;result=-7;
   assert(object_wait_many(objects,count,all,finite?123:0,&index,&result)==K_OK);
   assert(index==(all?UINT32_MAX:0) && result==(all?0:42));
   assert(allocations==before && subscriptions==calls);
   assert(atomic_load(&object.refs.value)==1);
   ready=false;index=77;result=-7;
   assert(object_wait_many(objects,count,all,0,&index,&result)==K_ETIMEDOUT);
   assert(index==77 && result==-7 && allocations==before && subscriptions==calls);
   assert(atomic_load(&object.refs.value)==1);
  }
  unsigned before=allocations,calls=subscriptions;
  ready=true;consume=true;predicates=0;
  assert(object_wait_on_signals(objects,count,predicate,&object,123)==K_OK);
  assert(predicates==1 && !ready && allocations==before && subscriptions==calls);
  consume=false;ready=false;fail_allocation=false;ready_on_subscribe=true;
  assert(object_wait_on_signals(objects,count,predicate,&object,123)==K_OK);
  assert(subscriptions==calls+1 && atomic_load(&object.refs.value)==1);
  ready_on_subscribe=false;ready=false;fail_allocation=true;
  kstatus_t expected=count>WAIT_INLINE_QUEUE_COUNT?K_ENOMEM:K_ETIMEDOUT;
  assert(object_wait_on_signals(objects,count,predicate,&object,123)==expected);
  assert(atomic_load(&object.refs.value)==1);
 }
 void *invalid[]={&object,0};predicates=0;ready=true;
 assert(object_wait_on_signals(invalid,2,predicate,&object,0)==K_EINVAL);
 assert(predicates==0 && atomic_load(&object.refs.value)==1);
 assert(releases==2);
 puts("PASS: object ready/zero-timeout avoids allocations and subscriptions at inline boundary and 1024 entries; consuming predicate, blocking recheck, OOM and reference rollback");
}
'''
with tempfile.TemporaryDirectory(prefix='wait-object-fast-') as directory:
    path = Path(directory)
    (path/'test.c').write_text(code)
    subprocess.run(['cc','-std=c11','-O2','-Wall','-Wextra','-Werror',
                    '-fsanitize=address,undefined','-fno-pie','-no-pie',
                    '-I'+str(root/'include'),str(path/'test.c'),
                    str(root/'kernel/object/object.c'),'-o',str(path/'test')],check=True)
    subprocess.run([str(path/'test')],check=True,timeout=10)
