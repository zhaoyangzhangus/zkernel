"""Production device/driver registry with controlled object and hardware hooks."""
from pathlib import Path
import subprocess
import tempfile

root=Path(__file__).resolve().parents[2]
source=(root/'kernel/drivers/core/device.c').read_text()
source=source.replace('for (uint32_t i = 0; i < DEVICE_REGISTRY_LIMIT; ++i) {',
    'for (uint32_t i = 0; i < DEVICE_REGISTRY_LIMIT; ++i) { atomic_fetch_add(&device_visits,1);')
source=source.replace('for (uint32_t i = 0; i < DRIVER_REGISTRY_LIMIT; ++i) {',
    'for (uint32_t i = 0; i < DRIVER_REGISTRY_LIMIT; ++i) { atomic_fetch_add(&driver_visits,1);')
code=r'''
#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <string.h>
#include <stdatomic.h>
static atomic_uint attempts;
static _Thread_local unsigned preempt_depth;
void sched_preempt_disable(void) {++preempt_depth;}
void sched_preempt_enable(void) {assert(preempt_depth);--preempt_depth;}
static atomic_uint device_visits,driver_visits;
static _Bool parallel;
static _Bool counted_cas(atomic_uint *p,unsigned *expected,unsigned value,
                        memory_order success,memory_order failure) {
    atomic_fetch_add(&attempts,1);
    if(parallel)while(atomic_load(&attempts)<8)sched_yield();
    return atomic_compare_exchange_strong_explicit(p,expected,value,success,failure);
}
''' + source.replace('atomic_compare_exchange_strong_explicit(&g_device_init_state',
                     'counted_cas(&g_device_init_state') + r'''
static atomic_uint detached,probes,removes;
static bool accept_probe,reject_reference;
void object_get(void *p) { object_header_t *h=p; assert(atomic_fetch_add(&h->refs.value,1)>0); }
bool object_try_get(void *p) { if(reject_reference)return false; object_get(p); return true; }
void object_put(void *p) { object_header_t *h=p; assert(atomic_fetch_sub(&h->refs.value,1)>1); }
kstatus_t iommu_detach_device(device_t *d) {
    /* 另一个 CPU 上的其他注册线程可能持有全局锁。 */
    assert(d && atomic_load(&d->state)==DEVICE_REMOVED && !preempt_depth);
    atomic_fetch_add(&detached,1); return K_OK;
}
kstatus_t power_register_device_object(device_t *d,power_device_t **out) { (void)d;(void)out;assert(0);return K_EIO; }
kstatus_t power_unregister_device(power_device_t *p) { (void)p;assert(0);return K_EIO; }
bool io_request_claim_device_cancel(io_request_t *r) { (void)r;assert(0);return false; }
void io_request_finish_device_cancel(io_request_t *r,kstatus_t s) { (void)r;(void)s;assert(0); }
kstatus_t io_cancel_with_status(io_request_t *r,kstatus_t s) { (void)r;(void)s;assert(0);return K_EIO; }
static kstatus_t probe(device_t *d) {
    assert(d && !atomic_load(&g_device_lock.state) && !preempt_depth); atomic_fetch_add(&probes,1);
    return accept_probe?K_OK:K_ENOENT;
}
static void remove_driver(device_t *d) {
    assert(d && !atomic_load(&g_device_lock.state) && !preempt_depth); atomic_fetch_add(&removes,1);
}
static device_t devices[DEVICE_REGISTRY_LIMIT+1],parallel_devices[4];
static driver_t drivers[DRIVER_REGISTRY_LIMIT+1];
static unsigned callback_depth;
static kstatus_t callback_status;
static uint64_t test_clock;
uint64_t x86_read_tsc(void) {return ++test_clock;}
uint64_t x86_timeout_ns_to_tsc(uint64_t ns) {return ns;}
kstatus_t telemetry_record_latency(uint32_t category,uint64_t id,uint64_t start) {
    (void)category;(void)id;(void)start;assert(preempt_depth==callback_depth);return K_OK;
}
static kstatus_t power_callback(device_t *d,uint32_t state) {
    assert(d && preempt_depth==callback_depth && !atomic_load(&g_device_lock.state));
    assert(!atomic_load(&d->state_lock.state) && atomic_load(&d->state)==DEVICE_RECOVERING);
    assert(state==DEVICE_POWER_ACTIVE || state==DEVICE_POWER_SUSPENDED);
    return callback_status;
}
static kstatus_t reset_callback(device_t *d,uint32_t level) {
    (void)level;return power_callback(d,DEVICE_POWER_ACTIVE);
}
static const device_ops_t power_ops={.set_power=power_callback,.reset=reset_callback};
static void *initializer(void *unused) {
    (void)unused; device_initialize(); assert(!atomic_load(&g_device_lock.state));
    for(unsigned i=0;i<DEVICE_REGISTRY_LIMIT;++i)assert(!g_devices[i]);
    for(unsigned i=0;i<DRIVER_REGISTRY_LIMIT;++i)assert(!g_drivers[i]); return 0;
}
static void check_state_preemption(void) {
    device_t nested;device_object_init(&nested,999,0,0,0);
    assert(device_register(&nested)==K_OK);nested.ops=&power_ops;
    for(unsigned nesting=0;nesting<32;++nesting) {
        for(unsigned i=0;i<nesting;++i)sched_preempt_disable();
        callback_depth=nesting;callback_status=K_OK;
        device_lock();assert(preempt_depth==nesting+1);
        device_state_lock(&nested);assert(preempt_depth==nesting+2);
        device_state_unlock(&nested);assert(preempt_depth==nesting+1);
        device_unlock();assert(preempt_depth==nesting);
        atomic_store(&nested.state,DEVICE_ACTIVE);
        assert(device_suspend_timeout(&nested,100)==K_OK && preempt_depth==nesting);
        assert(device_suspend_timeout(&nested,100)==K_EBUSY && preempt_depth==nesting);
        assert(device_resume_timeout(&nested,100)==K_OK && preempt_depth==nesting);
        assert(device_reset(&nested,1)==K_OK && preempt_depth==nesting);
        callback_status=K_EIO;
        assert(device_suspend_timeout(&nested,100)==K_EIO && preempt_depth==nesting);
        assert(atomic_load(&nested.state)==DEVICE_FAILED);
        atomic_store(&nested.state,DEVICE_SUSPENDED);
        assert(device_resume_timeout(&nested,100)==K_EIO && preempt_depth==nesting);
        assert(atomic_load(&nested.state)==DEVICE_FAILED);
        assert(device_reset(&nested,1)==K_EIO && preempt_depth==nesting);
        assert(atomic_load(&nested.state)==DEVICE_FAILED);
        atomic_store(&nested.state,DEVICE_ACTIVE);atomic_store(&nested.io_inflight,1);
        assert(device_suspend_timeout(&nested,1)==K_ETIMEDOUT && preempt_depth==nesting);
        assert(atomic_load(&nested.state)==DEVICE_FAILED);
        atomic_store(&nested.state,DEVICE_SUSPENDED);
        assert(device_resume_timeout(&nested,1)==K_ETIMEDOUT && preempt_depth==nesting);
        assert(atomic_load(&nested.state)==DEVICE_FAILED);
        assert(device_reset(&nested,1)==K_EBUSY && preempt_depth==nesting);
        atomic_store(&nested.io_inflight,0);
        for(unsigned i=0;i<nesting;++i)sched_preempt_enable();
    }
    callback_depth=0;nested.ops=0;device_unregister(&nested);assert(!preempt_depth);
}
static void *registrar(void *arg) {
    unsigned index=(unsigned)(uintptr_t)arg; device_t *d=&parallel_devices[index];
    for(unsigned i=0;i<1000;++i) {
        device_object_init(d,10000+index,0,0,0);
        assert(device_register(d)==K_OK && atomic_load(&d->object.refs.value)==2);
        device_t *found=0; assert(device_get_by_id(d->device_id,&found)==K_OK && found==d); object_put(found);
        device_unregister(d); assert(atomic_load(&d->object.refs.value)==1 && !preempt_depth);
    }
    return 0;
}
int main(void) {
    pthread_t threads[8]; parallel=true;
    for(unsigned i=0;i<DEVICE_REGISTRY_LIMIT;++i)g_devices[i]=(void *)(uintptr_t)1;
    for(unsigned i=0;i<DRIVER_REGISTRY_LIMIT;++i)g_drivers[i]=(void *)(uintptr_t)1;
    for(unsigned i=0;i<8;++i)assert(!pthread_create(&threads[i],0,initializer,0));
    for(unsigned i=0;i<8;++i)assert(!pthread_join(threads[i],0));
    parallel=false; assert(atomic_load(&attempts)==8);
    assert(device_register(0)==K_EINVAL && driver_register(0)==K_EINVAL);
    assert(device_get_by_id(0,0)==K_EINVAL && device_get_by_index(0,0)==K_EINVAL);
    for(unsigned i=0;i<DEVICE_REGISTRY_LIMIT+1;++i)device_object_init(&devices[i],i+1,0,0,0);
    for(unsigned i=0;i<DEVICE_REGISTRY_LIMIT;++i) {
        atomic_store(&device_visits,0);
        assert(device_register(&devices[i])==K_OK && atomic_load(&device_visits)==DEVICE_REGISTRY_LIMIT);
        assert(device_register(&devices[i])==K_EBUSY);
        assert(atomic_load(&devices[i].object.refs.value)==2);
        device_t *found=0;
        assert(device_get_by_id(i+1,&found)==K_OK && found==&devices[i]); object_put(found);
        assert(device_get_by_index(i,&found)==K_OK && found==&devices[i]); object_put(found);
    }
    assert(device_register(&devices[DEVICE_REGISTRY_LIMIT])==K_ENOMEM);
    assert(atomic_load(&devices[DEVICE_REGISTRY_LIMIT].object.refs.value)==1);
    for(unsigned i=0;i<DRIVER_REGISTRY_LIMIT+1;++i)driver_object_init(&drivers[i],"driver",probe,remove_driver);
    for(unsigned i=0;i<DRIVER_REGISTRY_LIMIT;++i) {
        atomic_store(&driver_visits,0);
        assert(driver_register(&drivers[i])==K_OK && atomic_load(&driver_visits)==DRIVER_REGISTRY_LIMIT);
        assert(driver_register(&drivers[i])==K_EBUSY);
        assert(atomic_load(&drivers[i].object.refs.value)==2);
    }
    assert(driver_register(&drivers[DRIVER_REGISTRY_LIMIT])==K_ENOMEM);
    assert(atomic_load(&probes)==DEVICE_REGISTRY_LIMIT*DRIVER_REGISTRY_LIMIT);
    driver_unregister(&drivers[0]);
    assert(driver_register(&drivers[DRIVER_REGISTRY_LIMIT-1])==K_EBUSY);
    assert(!g_drivers[0] && atomic_load(&drivers[DRIVER_REGISTRY_LIMIT-1].object.refs.value)==2);
    assert(driver_register(&drivers[0])==K_OK && g_drivers[0]==&drivers[0]);
    for(unsigned i=0;i<DRIVER_REGISTRY_LIMIT;++i) {
        driver_unregister(&drivers[i]); driver_unregister(&drivers[i]);
        assert(atomic_load(&drivers[i].object.refs.value)==1);
    }
    device_t *found=(void *)(uintptr_t)1;
    reject_reference=true;
    assert(device_get_by_id(1,&found)==K_ENOENT && !found);
    found=(void *)(uintptr_t)1; assert(device_get_by_index(0,&found)==K_ENOENT && !found);
    reject_reference=false;
    device_unregister(&devices[0]);
    assert(device_register(&devices[DEVICE_REGISTRY_LIMIT-1])==K_EBUSY);
    assert(!g_devices[0] && atomic_load(&devices[DEVICE_REGISTRY_LIMIT-1].object.refs.value)==2);
    assert(device_register(&devices[0])==K_OK && g_devices[0]==&devices[0]);
    atomic_store(&devices[0].state,DEVICE_REMOVING);
    assert(device_get_by_id(1,&found)==K_ENOENT && !found);
    assert(device_get_by_index(0,&found)==K_OK && found==&devices[1]); object_put(found);
    atomic_store(&devices[0].state,DEVICE_ENUMERATED);
    for(unsigned i=0;i<DEVICE_REGISTRY_LIMIT;i+=3)device_unregister(&devices[i]);
    unsigned visible=0;
    for(unsigned i=0;i<DEVICE_REGISTRY_LIMIT;++i)if(i%3) {
        assert(device_get_by_index(visible++,&found)==K_OK && found==&devices[i]); object_put(found);
    }
    assert(device_get_by_index(visible,&found)==K_ENOENT && !found);
    for(unsigned i=0;i<DEVICE_REGISTRY_LIMIT;++i) {
        device_unregister(&devices[i]); assert(atomic_load(&devices[i].object.refs.value)==1);
    }
    device_object_init(&devices[0],1,0,0,0); device_object_init(&devices[1],2,0,0,0);
    devices[1].parent=&devices[0];
    assert(device_register(&devices[0])==K_OK && device_register(&devices[1])==K_OK);
    assert(!list_empty(&devices[0].children));
    device_unregister(&devices[1]); assert(list_empty(&devices[0].children)); device_unregister(&devices[0]);
    accept_probe=true; driver_object_init(&drivers[0],"bind",probe,remove_driver);
    assert(driver_register(&drivers[0])==K_OK);
    device_object_init(&devices[0],1,0,0,0); assert(device_register(&devices[0])==K_OK);
    assert(devices[0].driver==&drivers[0] && atomic_load(&devices[0].state)==DEVICE_ACTIVE);
    driver_unregister(&drivers[0]); assert(!devices[0].driver && atomic_load(&devices[0].state)==DEVICE_ENUMERATED);
    assert(atomic_load(&removes)==1); device_unregister(&devices[0]); accept_probe=false;
    unsigned before=atomic_load(&attempts);
    for(unsigned i=0;i<10000;++i)device_initialize();
    for(unsigned i=0;i<4;++i)assert(!pthread_create(&threads[i],0,registrar,(void *)(uintptr_t)i));
    for(unsigned i=0;i<4;++i)assert(!pthread_join(threads[i],0));
    assert(atomic_load(&attempts)==before && !atomic_load(&g_device_lock.state));
    for(unsigned i=0;i<DEVICE_REGISTRY_LIMIT;++i)assert(!g_devices[i]);
    check_state_preemption();
    puts("Device registry: eight initializers, exact single 256/128-slot insertion scans, full/duplicate-after-hole rejection, lookup references, parent unlink, binding callbacks, 4000 concurrent lifecycles and 32 nested suspend/resume/reset success/failure/timeout checks passed");
}
'''
with tempfile.TemporaryDirectory(prefix='liteos-device-registry-') as directory:
    binary=str(Path(directory)/'test')
    subprocess.run(['cc','-std=c11','-O2','-Wall','-Wextra','-Werror','-pthread',
                    '-I'+str(root/'include'),'-fsanitize=address,undefined',
                    '-fno-pie','-no-pie','-ffunction-sections','-fdata-sections','-Wl,--gc-sections',
                    '-x','c','-','-o',binary],input=code,text=True,check=True)
    subprocess.run([binary],check=True,timeout=30)
