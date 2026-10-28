"""读盘等待中立即消费正常 HID，异常恢复不得嵌套吞掉存储完成。"""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
source = (ROOT / 'kernel/drivers/usb/xhci/hid_runtime.c').read_text()
body = source[source.index('static xhci_device_context_t *xhci_find_hid_context('):
              source.index('void xhci_report_hid_completion_milestones(')]
msc = (ROOT / 'kernel/drivers/usb/xhci/msc.c').read_text()
assert msc.count('if (!xhci_handle_hid_io_event(state, &event) &&\n') == 2
code = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdatomic.h>
#include <stdarg.h>
#define XHCI_TRB_TYPE_SHIFT 10U
#define XHCI_TRB_SLOT_SHIFT 24U
#define XHCI_TRB_ENDPOINT_SHIFT 16U
#define XHCI_TRANSFER_EVENT_TYPE 32U
#define XHCI_COMPLETION_SHIFT 24U
#define XHCI_COMPLETION_SUCCESS 1U
#define XHCI_COMPLETION_SHORT_PACKET 13U
typedef unsigned dma_mapping_t;
typedef struct {uint64_t parameter;uint32_t status,control;} xhci_trb_t;
typedef struct {void *cpu;dma_mapping_t mapping;} xhci_dma_page_t;
typedef struct {
    uint8_t device_slot,hid_endpoint;
    uint16_t hid_max_packet;
    xhci_dma_page_t hid_report;
    bool hid_transfer_pending;
    struct {uint8_t endpoint;uint16_t max_packet;xhci_dma_page_t report;
        bool transfer_pending;} hid_secondary;
} xhci_device_context_t;
typedef struct {xhci_device_context_t device;} xhci_state_t;
typedef struct {bool used;xhci_device_context_t context;} xhci_slot_device_t;
static xhci_state_t primary,aux;
static xhci_slot_device_t slots[256];
static bool ready=true,session=true;
static unsigned reports,rearms,restarts,errors,expected_length;
static atomic_bool g_xhci_hid_lookup_failure_seen,g_xhci_hid_secondary_transfer_seen,
    g_xhci_hid_transfer_seen;
static atomic_uint g_xhci_hid_completion_count;
static xhci_state_t *xhci_hid_controller_state(void){return &aux;}
static xhci_slot_device_t *xhci_topology_slot(uint8_t slot){return &slots[slot];}
static bool xhci_runtime_ready(void){return ready;}
static void kernel_serial_printf_serial_only(const char *s,...){(void)s;}
static void kernel_serial_write_serial_only(const char *s){(void)s;}
static void dma_sync_for_cpu(dma_mapping_t *m){(void)m;}
static void xhci_hid_consume_report(xhci_device_context_t *d,uint32_t n){
    assert(session && !d->hid_transfer_pending && n==expected_length);++reports;
}
static void xhci_hid_consume_report_secondary(xhci_device_context_t *d,uint32_t n){
    assert(session && !d->hid_secondary.transfer_pending && n==expected_length);++reports;
}
static bool xhci_queue_hid_report(xhci_state_t *s,xhci_device_context_t *d){
    (void)s;assert(session && !d->hid_transfer_pending);d->hid_transfer_pending=true;++rearms;return true;
}
static bool xhci_queue_hid_report_secondary(xhci_state_t *s,xhci_device_context_t *d){
    (void)s;assert(session && !d->hid_secondary.transfer_pending);
    d->hid_secondary.transfer_pending=true;++rearms;return true;
}
static bool xhci_restart_hid_endpoint(xhci_state_t *s,xhci_device_context_t *d){
    (void)s;(void)d;++restarts;return true;
}
static bool xhci_restart_hid_endpoint_secondary(xhci_state_t *s,xhci_device_context_t *d){
    (void)s;(void)d;++restarts;return true;
}
static void xhci_set_error(unsigned e){(void)e;++errors;}
static void xhci_clear_error(void){}
''' + body + r'''
static xhci_trb_t event(unsigned slot,unsigned endpoint,unsigned cc,unsigned residual){
    return (xhci_trb_t){0,(cc<<24)|residual,(slot<<24)|(endpoint<<16)|(32U<<10)};
}
int main(void){
    slots[2].used=true;
    slots[2].context=(xhci_device_context_t){.device_slot=2,.hid_endpoint=1,
        .hid_max_packet=8,.hid_transfer_pending=true,
        .hid_secondary={.endpoint=2,.max_packet=16,.transfer_pending=true}};
    for(unsigned i=0;i<10000;++i){
        bool secondary=i%2;
        unsigned maximum=secondary?16:8;
        unsigned residual=i%(maximum+1);
        expected_length=maximum-residual;
        xhci_trb_t e=event(2,secondary?5:3,i%3?1:13,residual);
        assert(xhci_handle_hid_io_event(&primary,&e));
        /* 会话仍未结束，但输入已送出，端点已有下一份报告请求。 */
        assert(reports==i+1 && rearms==i+1 && session && !restarts && !errors);
    }
    for(unsigned cc=0;cc<256;++cc){
        if(cc==1 || cc==13)continue;
        xhci_trb_t e=event(2,3,cc,0);
        assert(!xhci_handle_hid_io_event(&primary,&e));
    }
    xhci_trb_t invalid[]={event(2,3,1,9),event(2,5,13,17),event(2,7,1,0),
        event(3,3,1,0),event(0,3,1,0),event(2,2,1,0),event(2,1,1,0)};
    for(unsigned i=0;i<sizeof(invalid)/sizeof(*invalid);++i)
        assert(!xhci_handle_hid_io_event(&primary,&invalid[i]));
    xhci_trb_t e=event(2,3,1,0);
    e.control^=(32U^33U)<<10;
    assert(!xhci_handle_hid_io_event(&primary,&e));
    e=event(2,3,1,0);ready=false;
    assert(!xhci_handle_hid_io_event(&primary,&e));ready=true;
    assert(!xhci_handle_hid_io_event(0,&e) && !xhci_handle_hid_io_event(&primary,0));
    assert(reports==10000 && rearms==10000 && !restarts && !errors);
    aux.device=(xhci_device_context_t){.device_slot=9,.hid_endpoint=2,
        .hid_max_packet=8,.hid_transfer_pending=true};
    e=event(9,3,1,0);
    assert(!xhci_handle_hid_io_event(&aux,&e));
    expected_length=8;e=event(9,5,1,0);
    assert(xhci_handle_hid_io_event(&aux,&e) && reports==10001 && rearms==10001);
    /* 同一异常交给普通消费者后仍执行原有恢复。 */
    e=event(2,3,6,0);
    assert(xhci_handle_hid_transfer_event(&primary,&e) && restarts==1);
}
'''
with tempfile.TemporaryDirectory(prefix='xhci-storage-hid-') as directory:
    binary = str(Path(directory) / 'test')
    flags = ['cc', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
             '-fsanitize=address,undefined', '-fno-pie', '-no-pie', '-x', 'c', '-', '-o', binary]
    subprocess.run(flags, input=code, text=True, check=True)
    subprocess.run([binary], check=True)
    negative = code.replace('return xhci_handle_hid_transfer_event(state, event);', 'return false;')
    subprocess.run(flags, input=negative, text=True, check=True)
    assert subprocess.run([binary], stdout=subprocess.DEVNULL,
                          stderr=subprocess.DEVNULL).returncode != 0
print('xHCI storage/HID: 10000 reports delivered and rearmed before session end; '
      'BOT/UAS routing, malformed events, recovery isolation and negative control PASS')
