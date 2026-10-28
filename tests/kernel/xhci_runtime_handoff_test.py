"""真实运行时和解锁代码：忙锁休眠交接，不反复排队，不丢登记竞态。"""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
runtime = (ROOT / 'kernel/drivers/usb/xhci/runtime.c').read_text()
runtime = runtime[runtime.index('static bool g_xhci_runtime_ready;'):]
core = (ROOT / 'kernel/drivers/usb/xhci/core.c').read_text()
unlock = core[core.index('void xhci_event_unlock('):core.index('bool xhci_core_initialize(')]
code = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#define XHCI_TRB_TYPE_SHIFT 10U
#define XHCI_TRANSFER_EVENT_TYPE 32U
typedef struct {bool held;} spinlock_t;
typedef struct {
    bool initialized;
    spinlock_t event_lock;
    uint64_t event_lock_saved_flags;
    uint32_t event_lock_owner;
    atomic_bool event_work_waiting;
    unsigned pending,handled,tries,checks;
} xhci_state_t;
typedef struct {unsigned control;} xhci_trb_t;
static xhci_state_t primary,aux;
static bool auxiliary,irq,critical_fail;
static unsigned queued,scheduled,callbacks,race,race_attempt;
static const int g_xhci_event_dispatch_ops=0;
void xhci_runtime_resume_events(void);
void xhci_deferred_work(void *);
bool xhci_schedule_deferred_work(void);
static void spinlock_unlock(spinlock_t *s){assert(s->held);s->held=false;}
static void xhci_event_restore_flags(uint64_t flags){(void)flags;}
static void sched_preempt_enable(void){}
''' + unlock + r'''
static xhci_state_t *xhci_controller_state(void){return &primary;}
static xhci_state_t *xhci_hid_controller_state(void){return &aux;}
static bool xhci_hid_controller_active(void){return auxiliary;}
static bool xhci_event_try_lock(xhci_state_t *s){
    ++s->tries;
    if(s->event_lock.held){
        if(s==&primary && race && ++race_attempt==race){
            /* 解锁分别插在登记等待意图之前、之后。当前 try 仍报告失败。 */
            race=0;xhci_event_unlock(s);
        }
        return false;
    }
    s->event_lock.held=true;return true;
}
static bool xhci_event_pending(xhci_state_t *s){
    assert(s && s->event_lock.held);++s->checks;return s->pending!=0;
}
static bool xhci_process_event_ring(xhci_state_t *s,unsigned budget,const int *ops){
    (void)ops;assert(s->event_lock.held && budget);
    unsigned n=s->pending<budget?s->pending:budget;s->pending-=n;s->handled+=n;return n!=0;
}
static bool xhci_next_event(xhci_state_t *s,xhci_trb_t *e){
    assert(s->event_lock.held);
    if(!s->pending)return false;
    --s->pending;e->control=32U<<10;return true;
}
static bool xhci_handle_hid_transfer_event(xhci_state_t *s,const xhci_trb_t *e){
    (void)e;assert(s->event_lock.held);++s->handled;return true;
}
static void xhci_event_handler_complete(xhci_state_t *s){assert(s->event_lock.held);}
static bool xhci_interrupt_take_pending(void){bool result=irq;irq=false;return result;}
static void xhci_interrupt_reset_state(void){irq=false;}
static void kernel_serial_write_serial_only(const char *s){(void)s;}
static void xhci_report_hid_completion_milestones(void){}
static bool enqueue(void (*fn)(void *),void *arg){
    assert(fn==xhci_deferred_work && !arg && !queued);++queued;++scheduled;return true;
}
static bool deferred_schedule_critical(void (*fn)(void *),void *arg){
    return critical_fail?false:enqueue(fn,arg);
}
static bool deferred_try_schedule(void (*fn)(void *),void *arg){return enqueue(fn,arg);}
''' + runtime + r'''
static void reset(void){
    primary=(xhci_state_t){.initialized=true};aux=(xhci_state_t){.initialized=true};
    queued=scheduled=callbacks=race=race_attempt=0;auxiliary=critical_fail=false;
    xhci_runtime_reset_state();xhci_runtime_set_ready(true);
}
static void drain(void){
    while(queued){assert(++callbacks<10);--queued;xhci_deferred_work(0);}
}
int main(void){
    for(unsigned mode=0;mode<3;++mode)for(unsigned count=1;count<=512;count+=7){
        reset();primary.event_lock.held=true;primary.pending=count;race=mode;
        assert(xhci_schedule_deferred_work());drain();
        if(!mode){
            assert(primary.handled==0 && primary.tries==4 && !primary.checks);
            assert(scheduled==1 && atomic_load(&primary.event_work_waiting));
            xhci_event_unlock(&primary);assert(queued==1);drain();
        }
        assert(primary.handled==count && !primary.pending && !queued);
        assert(!atomic_load(&primary.event_work_waiting) && !primary.event_lock.held);
        assert(callbacks<=2);
    }
    /* 存储控制器被占用时，独立 HID 控制器仍被消费。 */
    reset();auxiliary=true;primary.event_lock.held=true;primary.pending=9;aux.pending=77;
    assert(xhci_schedule_deferred_work());drain();
    assert(aux.handled==77 && primary.handled==0 && callbacks==1 && scheduled==1);
    assert(!primary.checks && primary.tries<=8);
    xhci_event_unlock(&primary);drain();assert(primary.handled==9);
    /* 队列合并、备用队列及超过一个批次的积压保持原语义。 */
    reset();critical_fail=true;primary.pending=1100;
    assert(xhci_schedule_deferred_work() && !xhci_schedule_deferred_work());drain();
    assert(primary.handled==1100 && callbacks==3 && !queued);
    reset();irq=true;assert(xhci_schedule_deferred_work());drain();
    assert(callbacks==1 && scheduled==1 && !queued);
    xhci_runtime_stop();xhci_runtime_resume_events();assert(!queued);
    puts("xHCI runtime: busy-lock handoff, 222 registration races, independent HID, locked pending reads, coalescing and bounded backlog PASS");
}
'''
with tempfile.TemporaryDirectory(prefix='xhci-runtime-handoff-') as directory:
    binary = str(Path(directory) / 'test')
    flags = ['cc', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
             '-fsanitize=address,undefined', '-fno-pie', '-no-pie', '-x', 'c', '-', '-o', binary]
    subprocess.run(flags, input=code, text=True, check=True)
    subprocess.run([binary], check=True)
    for broken in (
        code.replace('if (!xhci_event_try_lock(state)) return false;', 'return false;'),
        code.replace('pending = xhci_runtime_events_pending(xhci_controller_state());',
                     'pending = xhci_event_pending(xhci_controller_state());'),
    ):
        subprocess.run(flags, input=broken, text=True, check=True)
        assert subprocess.run([binary], stdout=subprocess.DEVNULL,
                              stderr=subprocess.DEVNULL).returncode != 0
