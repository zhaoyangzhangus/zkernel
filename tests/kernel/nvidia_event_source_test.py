"""Exercise the actual event source and native object reference/notify dispatch."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
strip = lambda text: '\n'.join(line for line in text.splitlines()
                               if not line.startswith(('#include', '#pragma')))
header = strip((root / 'kernel/drivers/gpu/nvidia/port/include/nv_port_events.h').read_text())
source = strip((root / 'kernel/drivers/gpu/nvidia/port/nv_port_events.c').read_text())
object_header = strip((root / 'include/kernel/object.h').read_text())
object_source = (root / 'kernel/object/object.c').read_text()
object_source = object_source[object_source.index('void object_header_init('):
                              object_source.index('static bool object_pin_wait_set(')]
code = r'''
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <stddef.h>
typedef int kstatus_t;
enum {K_OK=0,K_EINVAL=-22,K_ENOMEM=-12,K_EBADF=-9,K_ENOENT=-2};
typedef struct {atomic_uint value;} refcount_t;
static void refcount_init(refcount_t *r,unsigned n){atomic_init(&r->value,n);}
typedef struct {unsigned notifications;} wait_queue_t;
static void wait_queue_init(wait_queue_t *q){q->notifications=0;}
typedef pthread_mutex_t nv_port_spinlock_t;
static _Thread_local unsigned held;
static void (*after_unlock)(void);
static unsigned allocations,frees;
static bool allocation_failure;
static void nv_port_spin_init(nv_port_spinlock_t *q){assert(!pthread_mutex_init(q,NULL));}
static uint64_t nv_port_spin_lock_irqsave(nv_port_spinlock_t *q){assert(!pthread_mutex_lock(q));assert(!held++);return 5070;}
static void nv_port_spin_unlock_irqrestore(nv_port_spinlock_t *q,uint64_t f){assert(f==5070&&held--==1);assert(!pthread_mutex_unlock(q));if(after_unlock){void (*callback)(void)=after_unlock;after_unlock=NULL;callback();}}
static void *kmalloc(size_t n,unsigned flags){assert(!held&&!flags);if(allocation_failure)return NULL;void *p=malloc(n);assert(p);++allocations;return p;}
static void *kmalloc_try_cached(size_t n,unsigned flags){assert(held==1&&!flags);if(allocation_failure)return NULL;void *p=malloc(n);assert(p);++allocations;return p;}
static void kfree(void *p){assert(p&&!held);++frees;free(p);}
static int wake_all(wait_queue_t *q){assert(!held);++q->notifications;return 0;}
''' + object_header + '\n' + header + '\n' + object_source + '\n' + source + r'''
static nv_port_event_source_t *race_source;
static void consume_then_post(void){
    assert(nv_port_event_source_poll(race_source,true)==NV_PORT_EVENT_READ);
    assert(!nv_port_event_source_post_irq(race_source,NULL));
}
int main(void) {
    allocation_failure=true;
    assert(nv_port_event_source_create()==NULL);
    allocation_failure=false;
    nv_port_event_source_t *a=nv_port_event_source_create(),*b=nv_port_event_source_create();
    assert(a&&b&&a!=b);
    void *wait_object=nv_port_event_source_wait_object(a);
    assert(wait_object==a&&object_try_get(wait_object));
    assert(a->header.type==0x0124&&a->header.ops->handle_close==NULL);
    assert(!object_is_signaled(a)&&!object_wait_value(a));
    assert(!nv_port_event_source_post(a,NULL));
    assert(a->header.signal_waiters.notifications==1&&!b->header.signal_waiters.notifications);
    unsigned allocated_before=allocations;
    for(unsigned i=0;i<100000;++i){
        assert(!nv_port_event_source_post(a,NULL));
        assert(!nv_port_event_source_post_irq(a,NULL));
    }
    assert(a->header.signal_waiters.notifications==1&&allocations==allocated_before);

    for(unsigned i=0;i<1000;++i) {
        assert(object_is_signaled(a)&&object_wait_value(a)==NV_PORT_EVENT_READ);
    }
    assert(nv_port_event_source_poll(a,true)==NV_PORT_EVENT_READ);
    assert(!object_is_signaled(a));
    nv_port_event_record_t record={.parent=12,.object=34,.index=56,.info32=78,.info16=90},out={0};
    bool more=true;
    allocation_failure=true;
    assert(nv_port_event_source_post(a,&record)==K_ENOMEM);
    assert(a->header.signal_waiters.notifications==1&&!object_is_signaled(a));
    allocation_failure=false;
    assert(!nv_port_event_source_post(a,&record));
    assert(nv_port_event_source_poll(a,true)==NV_PORT_EVENT_READ&&object_is_signaled(a));
    assert(!nv_port_event_source_take(a,&out,&more)&&!more&&out.object==34&&out.info16==90);
    assert(!object_is_signaled(a));
    assert(!nv_port_event_source_post(a,&record));
    unsigned frees_before=frees;
    nv_port_event_source_close(a);
    assert(frees==frees_before+1); /* queued payload flushed */
    assert(object_is_signaled(a)&&object_wait_value(a)==NV_PORT_EVENT_CLOSED);
    unsigned wakes=a->header.signal_waiters.notifications;
    nv_port_event_source_close(a);
    assert(a->header.signal_waiters.notifications==wakes);
    assert(nv_port_event_source_post(a,NULL)==K_EBADF);
    assert(nv_port_event_source_take(a,&out,&more)==K_EBADF&&!more);
    assert(a->header.signal_waiters.notifications==wakes);
    object_put(a); /* owner gone, subscribed waiter still pins the object */
    assert(frees==frees_before+1&&object_is_signaled(wait_object));
    assert(nv_port_event_source_poll(wait_object,true)==NV_PORT_EVENT_CLOSED);
    object_put(wait_object);
    assert(frees==frees_before+2);
    assert(!b->header.signal_waiters.notifications&&!object_is_signaled(b));
    // 首次发布解锁后、通知前被消费并再次发布，两次通知均不可丢失。
    race_source=b;after_unlock=consume_then_post;
    assert(!nv_port_event_source_post(b,NULL));
    assert(b->header.signal_waiters.notifications==2);
    assert(nv_port_event_source_poll(b,true)==NV_PORT_EVENT_READ);
    // 消费后重新发布必须产生新通知；普通和 IRQ 路径交替验证。
    for(unsigned i=0;i<1000;++i){
        unsigned before=b->header.signal_waiters.notifications;
        assert(!nv_port_event_source_post_irq(b,NULL));
        assert(!nv_port_event_source_post(b,NULL));
        assert(b->header.signal_waiters.notifications==before+1);
        assert(nv_port_event_source_poll(b,true)==NV_PORT_EVENT_READ);
        assert(!object_is_signaled(b));
    }

    assert(!nv_port_event_source_post(b,&record));
    assert(!nv_port_event_source_post_irq(b,NULL));
    assert(!nv_port_event_source_post_irq(b,&record));
    unsigned irq_wakes=b->header.signal_waiters.notifications;
    allocation_failure=true;
    assert(nv_port_event_source_post_irq(b,&record)==K_ENOMEM);
    assert(b->header.signal_waiters.notifications==irq_wakes);
    allocation_failure=false;
    object_put(b); /* destructor also flushes when owner forgot logical close */
    assert(allocations==frees);
    assert(nv_port_event_source_wait_object(NULL)==NULL);
    assert(nv_port_event_source_poll(NULL,false)==NV_PORT_EVENT_CLOSED);
    assert(nv_port_event_source_post(NULL,NULL)==K_EINVAL);
    assert(nv_port_event_source_take(NULL,&out,&more)==K_EINVAL);
    nv_port_event_source_close(NULL);
}
'''
with tempfile.TemporaryDirectory(prefix='nvidia-event-source-') as directory:
    base = Path(directory)
    (base / 'test.c').write_text(code)
    subprocess.run(['cc', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                    '-pthread', '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
                    str(base / 'test.c'), '-o', str(base / 'test')], check=True)
    subprocess.run([str(base / 'test')], check=True, timeout=15)
print('NVIDIA event source: native object dispatch, precise notifications, non-consuming readiness, allocation failure, close flush and waiter lifetime passed')
