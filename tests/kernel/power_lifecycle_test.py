"""Production power state machine with mocked hardware, no real ACPI operations."""
from pathlib import Path
import subprocess
import tempfile

root=Path(__file__).resolve().parents[2]
source=(root/'kernel/power.c').read_text()
code=r'''
#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <string.h>
#include <stdatomic.h>
static atomic_uint attempts;
static _Thread_local unsigned preempt_depth;
static unsigned callback_depth;
void sched_preempt_disable(void) {++preempt_depth;}
void sched_preempt_enable(void) {assert(preempt_depth);--preempt_depth;}
static _Bool parallel;
static _Bool counted_cas(atomic_uint *p,unsigned *expected,unsigned value,
                        memory_order success,memory_order failure) {
    atomic_fetch_add(&attempts,1);
    if(parallel)while(atomic_load(&attempts)<8)sched_yield();
    return atomic_compare_exchange_strong_explicit(p,expected,value,success,failure);
}
''' + source.replace('atomic_compare_exchange_strong_explicit(&g_power.init_state',
                     'counted_cas(&g_power.init_state') + r'''
kstatus_t vfs_sync_all(void) { assert(0); return K_EIO; }
kstatus_t e1000_shutdown(void) { assert(0); return K_EIO; }
void x86_system_reboot(void) { assert(0); __builtin_trap(); }
kstatus_t x86_acpi_poweroff(void) { assert(0); return K_EIO; }
static bool sleep_supported;
static kstatus_t sleep_status;
static unsigned sleeps;
bool x86_acpi_sleep_supported(void) { return sleep_supported; }
kstatus_t x86_acpi_enter_sleep(uint8_t state) {
    assert(preempt_depth==callback_depth && !atomic_load(&g_power.lock.state));
    assert(state==3 || state==4); ++sleeps;
    assert(power_get_system_state()==POWER_SYSTEM_SUSPENDED); return sleep_status;
}
kstatus_t device_suspend(device_t *d) { (void)d; assert(0); return K_EIO; }
kstatus_t device_resume(device_t *d) { (void)d; assert(0); return K_EIO; }
static unsigned events[128],event_count;
typedef struct context { unsigned id; bool fail_suspend,fail_resume; power_device_t *device; } context_t;
static kstatus_t suspend_callback(void *p) {
    context_t *c=p; assert(!atomic_load(&g_power.lock.state) && preempt_depth==callback_depth);
    assert(power_get_system_state()==POWER_SYSTEM_SUSPENDING);
    assert(power_get_device_state(c->device)==POWER_DEVICE_SUSPENDING);
    assert(power_unregister_device(c->device)==K_EBUSY);
    events[event_count++]=c->id; assert(event_count<128);
    return c->fail_suspend?K_EIO:K_OK;
}
static kstatus_t resume_callback(void *p) {
    context_t *c=p; assert(!atomic_load(&g_power.lock.state) && preempt_depth==callback_depth);
    assert(power_get_system_state()==POWER_SYSTEM_RESUMING || power_get_system_state()==POWER_SYSTEM_SUSPENDING);
    assert(power_unregister_device(c->device)==K_EBUSY);
    events[event_count++]=c->id+100; assert(event_count<128);
    return c->fail_resume?K_EIO:K_OK;
}
static void *initializer(void *unused) {
    (void)unused; assert(power_manager_init());
    assert(!atomic_load(&g_power.lock.state) && power_get_system_state()==POWER_SYSTEM_RUNNING);
    for(unsigned i=0;i<POWER_MAX_DEVICES;++i)
        assert(!g_power.devices[i].registered && atomic_load(&g_power.devices[i].state)==POWER_DEVICE_RUNNING);
    return 0;
}
static void *registrar(void *unused) {
    (void)unused;
    for(unsigned i=0;i<1000;++i) {
        power_device_t *d=0;
        assert(power_register_device("parallel",suspend_callback,resume_callback,0,&d)==K_OK);
        assert(power_get_device_state(d)==POWER_DEVICE_RUNNING);
        assert(power_unregister_device(d)==K_OK);
        assert(!preempt_depth);
    }
    return 0;
}
int main(void) {
    pthread_t threads[8]; parallel=true;
    for(unsigned i=0;i<POWER_MAX_DEVICES;++i) {
        g_power.devices[i].registered=true; atomic_store(&g_power.devices[i].state,POWER_DEVICE_FAILED);
    }
    for(unsigned i=0;i<8;++i)assert(!pthread_create(&threads[i],0,initializer,0));
    for(unsigned i=0;i<8;++i)assert(!pthread_join(threads[i],0));
    parallel=false; assert(atomic_load(&attempts)==8 && power_self_test());
    context_t contexts[POWER_MAX_DEVICES]={0};
    for(unsigned i=0;i<POWER_MAX_DEVICES;++i) {
        contexts[i].id=i+1;
        assert(power_register_device("device",suspend_callback,resume_callback,&contexts[i],&contexts[i].device)==K_OK);
    }
    power_device_t *sentinel=(void *)(uintptr_t)1;
    assert(power_register_device("full",suspend_callback,resume_callback,0,&sentinel)==K_ENOMEM && sentinel==(void *)(uintptr_t)1);
    uintptr_t base=(uintptr_t)g_power.devices;
    for(unsigned offset=0;offset<sizeof(g_power.devices);++offset) {
        power_device_t *p=(void *)(base+offset); bool valid=offset%sizeof(*p)==0;
        assert(power_known_locked(p)==valid);
        assert(power_get_device_state(p)==(valid?POWER_DEVICE_RUNNING:POWER_DEVICE_FAILED));
        if(!valid)assert(power_unregister_device(p)==K_ENOENT);
    }
    const uintptr_t bad[]={0,1,base-1,base+sizeof(g_power.devices),UINTPTR_MAX};
    for(unsigned i=0;i<sizeof(bad)/sizeof(bad[0]);++i) {
        assert(!power_known_locked((void *)bad[i]));
        assert(power_unregister_device((void *)bad[i])==K_ENOENT);
    }
    for(unsigned i=3;i<POWER_MAX_DEVICES;++i)assert(power_unregister_device(contexts[i].device)==K_OK);
    for(unsigned cycle=0;cycle<1000;++cycle) {
        event_count=0; assert(power_system_suspend()==K_OK && power_system_suspend()==K_EBUSY);
        assert(power_system_resume()==K_OK && power_system_resume()==K_EBUSY);
        const unsigned expected[]={1,2,3,103,102,101};
        assert(event_count==6 && !memcmp(events,expected,sizeof(expected)));
    }
    contexts[1].fail_suspend=true; event_count=0;
    assert(power_system_suspend()==K_EIO && power_get_system_state()==POWER_SYSTEM_RUNNING);
    assert(power_get_device_state(contexts[0].device)==POWER_DEVICE_RUNNING);
    assert(power_get_device_state(contexts[1].device)==POWER_DEVICE_FAILED);
    assert(power_get_device_state(contexts[2].device)==POWER_DEVICE_RUNNING);
    assert(event_count==3 && events[0]==1 && events[1]==2 && events[2]==101);
    contexts[1].fail_suspend=false;
    contexts[2].fail_suspend=true; event_count=0;
    assert(power_system_suspend()==K_EIO && power_get_system_state()==POWER_SYSTEM_RUNNING);
    assert(power_get_device_state(contexts[2].device)==POWER_DEVICE_FAILED);
    contexts[2].fail_suspend=false; contexts[1].fail_suspend=true; event_count=0;
    assert(power_system_suspend()==K_EIO && power_get_system_state()==POWER_SYSTEM_RUNNING);
    assert(power_get_device_state(contexts[2].device)==POWER_DEVICE_FAILED);
    contexts[1].fail_suspend=false;
    assert(power_system_enter_acpi_sleep(2)==K_EINVAL);
    assert(power_system_enter_acpi_sleep(3)==K_ENOSYS && sleeps==0);
    sleep_supported=true; sleep_status=K_EIO; event_count=0;
    assert(power_system_enter_acpi_sleep(3)==K_EIO && sleeps==1);
    assert(power_get_system_state()==POWER_SYSTEM_RUNNING && event_count==6);
    sleep_status=K_OK; event_count=0;
    assert(power_system_enter_acpi_sleep(4)==K_OK && sleeps==2);
    assert(power_get_system_state()==POWER_SYSTEM_SUSPENDED && power_system_resume()==K_OK);
    for(unsigned i=0;i<3;++i)assert(power_unregister_device(contexts[i].device)==K_OK);
    unsigned before=atomic_load(&attempts);
    for(unsigned i=0;i<10000;++i)assert(power_manager_init() && power_get_system_state()==POWER_SYSTEM_RUNNING);
    for(unsigned i=0;i<4;++i)assert(!pthread_create(&threads[i],0,registrar,0));
    for(unsigned i=0;i<4;++i)assert(!pthread_join(threads[i],0));
    assert(atomic_load(&attempts)==before && !preempt_depth);
    for(unsigned nesting=0;nesting<32;++nesting) {
        for(unsigned i=0;i<nesting;++i)sched_preempt_disable();
        callback_depth=nesting;
        context_t nested={.id=1};
        assert(power_register_device("nested",suspend_callback,resume_callback,
                                     &nested,&nested.device)==K_OK);
        assert(preempt_depth==nesting);
        event_count=0;
        assert(power_system_suspend()==K_OK && preempt_depth==nesting);
        assert(power_system_suspend()==K_EBUSY && preempt_depth==nesting);
        assert(power_get_device_state(nested.device)==POWER_DEVICE_SUSPENDED && preempt_depth==nesting);
        assert(power_system_resume()==K_OK && preempt_depth==nesting);
        nested.fail_suspend=true;event_count=0;
        assert(power_system_suspend()==K_EIO && preempt_depth==nesting);
        assert(power_get_system_state()==POWER_SYSTEM_RUNNING);
        nested.fail_suspend=false;event_count=0;
        assert(power_system_enter_acpi_sleep(3)==K_OK && preempt_depth==nesting);
        assert(power_system_resume()==K_OK && preempt_depth==nesting);
        assert(power_unregister_device(nested.device)==K_OK && preempt_depth==nesting);
        assert(power_unregister_device(nested.device)==K_ENOENT && preempt_depth==nesting);
        for(unsigned i=0;i<nesting;++i)sched_preempt_enable();
    }
    callback_depth=0;assert(!preempt_depth);
    for(unsigned i=0;i<3;++i)
        assert(power_register_device("rollback",suspend_callback,resume_callback,&contexts[i],&contexts[i].device)==K_OK);
    contexts[0].fail_resume=true; contexts[1].fail_suspend=true; event_count=0;
    assert(power_system_suspend()==K_EIO && power_get_system_state()==POWER_SYSTEM_FAILED);
    assert(power_get_device_state(contexts[0].device)==POWER_DEVICE_FAILED);
    assert(power_get_device_state(contexts[2].device)==POWER_DEVICE_RUNNING);
    assert(!atomic_load(&g_power.lock.state));
    /* Independent fixtures for each failure position. Reset only mock state;
     * this is not an implementation of recovery from SYSTEM_FAILED. */
    for(unsigned failed=0;failed<3;++failed) {
        atomic_store(&g_power.system_state,POWER_SYSTEM_RUNNING);
        for(unsigned i=0;i<3;++i) {
            contexts[i].fail_suspend=false;contexts[i].fail_resume=(i==failed);
            atomic_store(&contexts[i].device->state,POWER_DEVICE_RUNNING);
        }
        event_count=0;assert(power_system_suspend()==K_OK);
        assert(power_system_resume()==K_EIO && power_get_system_state()==POWER_SYSTEM_FAILED);
        assert(event_count==6-failed);
        for(unsigned i=0;i<3;++i) {
            power_device_state_t expected=i<failed?POWER_DEVICE_SUSPENDED:
                i==failed?POWER_DEVICE_FAILED:POWER_DEVICE_RUNNING;
            assert(power_get_device_state(contexts[i].device)==expected);
        }
        for(unsigned i=0;i<3;++i)assert(events[i]==contexts[i].id);
        for(unsigned i=0;i<3-failed;++i)assert(events[3+i]==contexts[2-i].id+100);
        assert(power_system_resume()==K_EBUSY && !atomic_load(&g_power.lock.state));
    }
    assert(!preempt_depth);
    puts("Power: eight initializers, all pointer offsets, 1000 ordered suspend/resume cycles, partial/failed rollback, mock ACPI, 4000 concurrent registrations and 32 nested-preemption depths passed");
}
'''
with tempfile.TemporaryDirectory(prefix='liteos-power-') as directory:
    binary=str(Path(directory)/'test')
    command=['cc','-std=c11','-O2','-Wall','-Wextra','-Werror','-pthread',
             '-I'+str(root/'include'),'-fsanitize=address,undefined',
             '-fno-pie','-no-pie','-x','c','-','-o',binary]
    subprocess.run(command,input=code,text=True,check=True)
    subprocess.run([binary],check=True,timeout=30)
    start=code.index('            /* No suspend callback ran for the remaining devices. */')
    end=code.index('            atomic_store_explicit(&g_power.system_state,',start)
    subprocess.run(command,input=code[:start]+'            (void)previous_states;\n'+code[end:],text=True,check=True)
    failure=subprocess.run([binary],text=True,capture_output=True,timeout=30)
    assert failure.returncode!=0 and 'contexts[2].device' in failure.stderr
    print('Regression control: omitted untouched-device restoration reproduces stranded SUSPENDING state')
    start=code.index('            /* Reverse-order resume stopped here:')
    end=code.index('            atomic_store_explicit(&g_power.system_state,',start)
    subprocess.run(command,input=code[:start]+code[end:],text=True,check=True)
    failure=subprocess.run([binary],text=True,capture_output=True,timeout=30)
    assert failure.returncode!=0 and '==expected' in failure.stderr
    print('Regression control: omitted resume restoration reproduces stranded RESUMING state')
