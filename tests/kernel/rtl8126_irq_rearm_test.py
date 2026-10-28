"""Actual RTL interrupt/rearm register order, including coalesced and stale MSI."""
from pathlib import Path
import re
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'kernel/drivers/net/rtl8126.c').read_text()
irq = source[source.index('static void rtl8126_irq_handler('):
             source.index('bool rtl8126_bind_interrupt_state(')]
rearm = source[source.index('void rtl8126_rearm_interrupts('):
               source.index('bool rtl8126_poll_receive_state(')]
poll = source[source.index('bool rtl8126_poll_receive_state('):
              source.index('bool rtl8126_interrupt_ready(')]
assert poll.index('RTL8126_REG_INT_MASK, 0U') < poll.index('dma_sync_for_cpu')
assert 'rtl8126_rearm_interrupts(state)' not in poll
defines = '\n'.join(re.findall(r'^#define RTL8126_INT_\w+[^\n\\]*$', source, re.M))
code = r'''
#include <stdint.h>
#include <stdbool.h>
#include <assert.h>
#include <stdio.h>
#define RTL8126_REG_INT_MASK 0x38U
#define RTL8126_REG_INT_STATUS 0x3cU
#define RTL8126_INT_MASK_VALUE 0x2fU
struct arch_trap_frame;
typedef struct {
    bool initialized, rtl_irq_bound;
    uint32_t rtl_irq_count, rtl_irq_status_count;
} e1000_state_t;
static uint32_t cause, mask, posted_mask;
static bool mask_pending;
static unsigned writes, scheduled, flushes;
static unsigned offsets[8], values[8];
static uint32_t rtl8126_read32(e1000_state_t *s, unsigned offset) {
    assert(s);
    if (offset == RTL8126_REG_INT_MASK) {
        if (mask_pending) { mask = posted_mask; mask_pending = false; }
        ++flushes; return mask;
    }
    assert(offset == RTL8126_REG_INT_STATUS); return cause;
}
static void rtl8126_write32(e1000_state_t *s, unsigned offset, uint32_t value) {
    assert(s && writes < 8);
    offsets[writes] = offset; values[writes++] = value;
    if (offset == RTL8126_REG_INT_MASK) { posted_mask = value; mask_pending = true; }
    else {
        assert(offset == RTL8126_REG_INT_STATUS);
        assert(!mask_pending); cause &= ~value;
    }
}
static bool e1000_schedule_deferred_poll(void) {
    assert(!mask_pending && !mask); ++scheduled; return false;
}
''' + irq + rearm + r'''
int main(void) {
    e1000_state_t s = {.initialized = true, .rtl_irq_bound = true};
    for (unsigned status = 1; status < 256; ++status) {
        cause = status; mask = RTL8126_INT_MASK_VALUE; mask_pending = false;
        writes = scheduled = flushes = 0;
        rtl8126_irq_handler(0, 0, &s);
        bool relevant = (status & RTL8126_INT_MASK_VALUE) != 0;
        assert(!cause && scheduled == relevant);
        if (relevant) {
            assert(writes == 2 && offsets[0] == RTL8126_REG_INT_MASK && !values[0]);
            assert(offsets[1] == RTL8126_REG_INT_STATUS && values[1] == status);
            assert(!mask);
        } else assert(writes == 1 && mask == RTL8126_INT_MASK_VALUE);
        /* Events arriving during a batch belong to the next ISR, not to
         * the batch that just finished. Rearming must not acknowledge them. */
        cause = 0x95; writes = 0;
        rtl8126_rearm_interrupts(&s);
        assert(cause == 0x95 && mask == RTL8126_INT_MASK_VALUE && flushes == 1U + relevant);
        assert(!mask_pending);
        assert(writes == 1 && offsets[0] == RTL8126_REG_INT_MASK && values[0] == RTL8126_INT_MASK_VALUE);
    }
    writes = 0; cause = UINT32_MAX;
    rtl8126_irq_handler(0, 0, &s); rtl8126_rearm_interrupts(&s);
    assert(!writes);
    cause = 0; rtl8126_irq_handler(0, 0, &s); assert(!writes);
    s.rtl_irq_bound = false;
    rtl8126_rearm_interrupts(&s); assert(!writes);
    puts("RTL8126 MSI: ISR-only ACK, rearm preserves new causes, unknown causes and absent device PASS");
}
'''
with tempfile.TemporaryDirectory(prefix='rtl8126-irq-') as temp:
    path = Path(temp)
    (path / 'test.c').write_text(code)
    subprocess.run(['cc', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                    str(path / 'test.c'), '-o', str(path / 'test')], check=True)
    subprocess.run([str(path / 'test')], check=True, timeout=10)
    missing_flush = irq.replace('(void)rtl8126_read32(state, RTL8126_REG_INT_MASK);', '')
    assert missing_flush != irq
    (path / 'test.c').write_text(code.replace(irq, missing_flush))
    subprocess.run(['cc', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                    str(path / 'test.c'), '-o', str(path / 'test')], check=True)
    result = subprocess.run([str(path / 'test')], capture_output=True, timeout=10)
    assert result.returncode != 0
    print('Negative control rejected: deferred work before posted mask reaches NIC')
