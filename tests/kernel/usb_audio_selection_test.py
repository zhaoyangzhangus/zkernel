"""用真实设备选择函数验证 USB 音频关闭、在途 DMA 和再次切换。"""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'kernel/drivers/usb/xhci/audio.c').read_text()
code = r'''
#include <assert.h>
#include <stdio.h>
#include "kernel/drivers/usb/xhci/internal.h"
static xhci_state_t state;
static xhci_slot_device_t slots[XHCI_MAX_SLOT_TABLE];
static uint8_t g_xhci_audio_slot;
static bool locked;
xhci_state_t *xhci_controller_state(void) { return &state; }
xhci_slot_device_t *xhci_topology_slot(uint32_t n) {
    assert(locked || n < XHCI_MAX_SLOT_TABLE);
    return n < XHCI_MAX_SLOT_TABLE ? &slots[n] : 0;
}
static void xhci_audio_lock(void) { assert(!locked); locked=true; }
static void xhci_audio_unlock(void) { assert(locked); locked=false; }
''' + source[source.index('uint32_t xhci_audio_device_count(void)'):] + r'''
int main(void) {
 assert(xhci_audio_device_count()==0);
 state.initialized=true;
 assert(xhci_audio_select_device(0)==K_ENOENT && !locked);
 slots[4].used=slots[5].used=true;
 slots[4].context.audio_endpoint=slots[5].context.audio_endpoint=2;
 assert(xhci_audio_device_count()==2);
 assert(xhci_audio_select_device(0)==K_OK && g_xhci_audio_slot==4);
 /* 设备关闭且无在途 DMA 时，可以立即切换。 */
 assert(xhci_audio_select_device(1)==K_OK && g_xhci_audio_slot==5);
 slots[5].context.audio_stream=(void*)1;
 assert(xhci_audio_select_device(0)==K_EBUSY && g_xhci_audio_slot==5 && !locked);
 assert(xhci_audio_select_device(1)==K_OK);
 slots[5].context.audio_stream=0;
 slots[5].context.audio_transfer_pending=true;
 assert(xhci_audio_select_device(0)==K_EBUSY && g_xhci_audio_slot==5 && !locked);
 slots[5].context.audio_transfer_pending=false;
 assert(xhci_audio_select_device(0)==K_OK && g_xhci_audio_slot==4);
 assert(xhci_audio_select_device(2)==K_ENOENT && g_xhci_audio_slot==4 && !locked);
 slots[4].used=false;
 assert(xhci_audio_device_count()==1);
 assert(xhci_audio_select_device(0)==K_OK && g_xhci_audio_slot==5 && !locked);
 puts("USB Audio selection: active stream and DMA exclusion, closed stream switch, missing device and lock release passed");
}
'''
with tempfile.TemporaryDirectory() as d:
    exe = str(Path(d) / 'test')
    subprocess.run(['cc', '-O2', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
                    '-I'+str(root), '-I'+str(root/'include'), '-x', 'c', '-',
                    '-o', exe], input=code, text=True, check=True)
    subprocess.run([exe], check=True)
