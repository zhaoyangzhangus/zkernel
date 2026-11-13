"""Whole production firmware manager with controlled, reentrant providers."""
from pathlib import Path
import subprocess
import tempfile

root=Path(__file__).resolve().parents[2]
source=(root/'kernel/drivers/core/firmware.c').read_text()
source=source.replace('atomic_compare_exchange_strong_explicit(&manager->init_state',
                      'counted_cas(&manager->init_state')
assert 'spinlock_' not in source
source=source.replace('atomic_store_explicit(&manager->init_state, 2U, memory_order_release);',
                     'counted_publish(&manager->init_state);')
code=r'''
#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <string.h>
#include <stdatomic.h>
static atomic_uint attempts;
static _Bool parallel;
static atomic_uint observers_ready;
static _Bool controlled_publish;
static void counted_publish(atomic_uint *state) {
    if(controlled_publish)while(atomic_load(&observers_ready)<4)sched_yield();
    atomic_store_explicit(state,2U,memory_order_release);
}
static _Bool counted_cas(atomic_uint *p,unsigned *expected,unsigned value,
                        memory_order success,memory_order failure) {
    atomic_fetch_add(&attempts,1);
    if(parallel)while(atomic_load(&attempts)<8)sched_yield();
    return atomic_compare_exchange_strong_explicit(p,expected,value,success,failure);
}
''' + source + r'''
typedef struct context { atomic_uint resolves,releases; unsigned magic; } context_t;
static context_t context={.magic=0x1234},other_context;
static firmware_manager_t manager;
static unsigned invalid_mode;
static bool provider_fail,reenter;
static _Thread_local bool inside;
static const unsigned char image[]={1,2,3,4};
static void release_provider(firmware_blob_t *blob,void *p);
static kstatus_t resolve_provider(const char *name,uint64_t minimum,firmware_blob_t *out,void *p) {
    context_t *c=p; assert(c==&context && c->magic==0x1234);
    atomic_fetch_add(&c->resolves,1);
    if(provider_fail)return K_ENOENT;
    assert(firmware_manager_init(&manager,resolve_provider,release_provider,p)==K_OK);
    if(reenter && !inside && !strcmp(name,"outer")) {
        inside=true; firmware_blob_t inner;
        assert(firmware_request(&manager,"inner",1,&inner)==K_OK);
        firmware_release(&manager,&inner); inside=false;
    }
    *out=(firmware_blob_t){.data=image,.size=sizeof(image),.version=7,.generation=3,.package_id={1}};
    switch(invalid_mode) {
        case 1:out->data=0;break;
        case 2:out->size=0;break;
        case 3:out->version=0;break;
        case 4:out->generation=0;break;
        case 5:memset(out->package_id,0,sizeof(out->package_id));break;
        case 6:out->version=minimum-1;break;
    }
    return K_OK;
}
static void release_provider(firmware_blob_t *blob,void *p) {
    context_t *c=p; assert(c==&context); atomic_fetch_add(&c->releases,1);
    if(reenter && !inside) {
        inside=true; firmware_blob_t inner;
        assert(firmware_request(&manager,"inner",1,&inner)==K_OK);
        firmware_release(&manager,&inner); inside=false;
    }
    blob->data=0;
}
static kstatus_t other_resolve(const char *n,uint64_t v,firmware_blob_t *b,void *p) {
    return resolve_provider(n,v,b,p);
}
static void other_release(firmware_blob_t *b,void *p) { release_provider(b,p); }
static void *initializer(void *unused) {
    (void)unused;
    assert(firmware_manager_init(&manager,resolve_provider,release_provider,&context)==K_OK);
    assert(manager.resolve==resolve_provider && manager.release==release_provider && manager.context==&context);
    return 0;
}
static void *requester(void *unused) {
    (void)unused;
    for(unsigned i=0;i<1000;++i) {
        firmware_blob_t blob;
        assert(firmware_request(&manager,"gpu/fw.bin",7,&blob)==K_OK);
        assert(blob.data==image && blob.size==4 && blob.version==7 && blob.generation==3 && blob.package_id[0]==1);
        firmware_release(&manager,&blob); assert(!blob.data);
    }
    return 0;
}
static void *publication_observer(void *unused) {
    (void)unused;
    while(atomic_load(&attempts)<8)sched_yield();
    while(atomic_load_explicit(&manager.init_state,memory_order_acquire)!=1U)sched_yield();
    firmware_blob_t blob,before;
    memset(&blob,0x59,sizeof(blob));memcpy(&before,&blob,sizeof(blob));
    assert(firmware_request(&manager,"gpu/fw.bin",7,&blob)==K_EINVAL);
    assert(!memcmp(&blob,&before,sizeof(blob)));
    firmware_release(&manager,&blob);assert(!memcmp(&blob,&before,sizeof(blob)));
    atomic_fetch_add(&observers_ready,1);
    while(atomic_load_explicit(&manager.init_state,memory_order_acquire)!=2U)sched_yield();
    return requester(0);
}
int main(void) {
    firmware_manager_t empty={0}; firmware_blob_t blob,zero={0}; memset(&blob,0x39,sizeof(blob));
    firmware_blob_t before=blob;
    assert(firmware_request(&empty,"x",0,&blob)==K_EINVAL && !memcmp(&blob,&before,sizeof(blob)));
    assert(firmware_manager_init(0,resolve_provider,release_provider,&context)==K_EINVAL);
    assert(firmware_manager_init(&empty,0,release_provider,&context)==K_EINVAL);
    assert(firmware_manager_init(&empty,resolve_provider,0,&context)==K_EINVAL);
    pthread_t threads[8],observers[4]; parallel=true;controlled_publish=true;
    for(unsigned i=0;i<4;++i)assert(!pthread_create(&observers[i],0,publication_observer,0));
    for(unsigned i=0;i<8;++i)assert(!pthread_create(&threads[i],0,initializer,0));
    for(unsigned i=0;i<8;++i)assert(!pthread_join(threads[i],0));
    for(unsigned i=0;i<4;++i)assert(!pthread_join(observers[i],0));
    parallel=false;controlled_publish=false;
    assert(atomic_load(&observers_ready)==4 && atomic_load(&attempts)==8 && firmware_core_self_test());
    assert(firmware_manager_init(&manager,other_resolve,release_provider,&context)==K_EBUSY);
    assert(firmware_manager_init(&manager,resolve_provider,other_release,&context)==K_EBUSY);
    /* 回调相同时，现有 API 会忽略替换后的上下文。 */
    assert(firmware_manager_init(&manager,resolve_provider,release_provider,&other_context)==K_OK);
    assert(manager.context==&context);
    for(unsigned mode=1;mode<=6;++mode) {
        invalid_mode=mode; unsigned releases=atomic_load(&context.releases);
        assert(firmware_request(&manager,"gpu/fw.bin",3,&blob)==K_EIO);
        assert(atomic_load(&context.releases)==releases+1 && !memcmp(&blob,&zero,sizeof(blob)));
    }
    invalid_mode=0; provider_fail=true; memset(&blob,0x75,sizeof(blob));
    unsigned releases=atomic_load(&context.releases);
    assert(firmware_request(&manager,"gpu/fw.bin",3,&blob)==K_ENOENT);
    assert(!memcmp(&blob,&zero,sizeof(blob)) && atomic_load(&context.releases)==releases);
    provider_fail=false;
    memset(&blob,0x39,sizeof(blob)); before=blob;
    assert(firmware_request(&manager,"../x",0,&blob)==K_EINVAL && !memcmp(&blob,&before,sizeof(blob)));
    assert(firmware_request(&manager,"x",0,0)==K_EINVAL);
    reenter=true;
    assert(firmware_request(&manager,"outer",1,&blob)==K_OK); firmware_release(&manager,&blob);
    reenter=false; assert(!memcmp(&blob,&zero,sizeof(blob)));
    releases=atomic_load(&context.releases);
    firmware_release(&manager,&blob); firmware_release(0,&blob); firmware_release(&manager,0);
    assert(atomic_load(&context.releases)==releases);
    unsigned init_before=atomic_load(&attempts),requests_before=atomic_load(&context.resolves);
    for(unsigned i=0;i<10000;++i)
        assert(firmware_manager_init(&manager,resolve_provider,release_provider,&context)==K_OK);
    for(unsigned i=0;i<4;++i)assert(!pthread_create(&threads[i],0,requester,0));
    for(unsigned i=0;i<4;++i)assert(!pthread_join(threads[i],0));
    assert(atomic_load(&context.resolves)==requests_before+4000 && atomic_load(&context.releases)==releases+4000);
    assert(atomic_load(&attempts)==init_before);
    puts("Firmware manager: eight initializers, four forced publication observers, callback/context compatibility, six invalid-blob releases, provider/release reentry and 8000 balanced concurrent requests without provider locks passed");
}
'''
with tempfile.TemporaryDirectory(prefix='liteos-firmware-lifecycle-') as directory:
    binary=str(Path(directory)/'test')
    subprocess.run(['cc','-std=c11','-O2','-Wall','-Wextra','-Werror','-pthread',
                    '-I'+str(root/'include'),'-fsanitize=address,undefined',
                    '-fno-pie','-no-pie','-x','c','-','-o',binary],input=code,text=True,check=True)
    subprocess.run([binary],check=True,timeout=30)
