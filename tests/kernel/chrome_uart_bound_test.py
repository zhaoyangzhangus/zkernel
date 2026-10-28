#!/usr/bin/env python3
"""Fault-inject the actual diagnostic byte writer without touching host UARTs."""
from pathlib import Path
import subprocess
import tempfile

source = (Path(__file__).resolve().parents[2] / 'kernel/syscall/linux.c').read_text()
function = source[source.index('static void linux_chrome_diag_char('):
                  source.index('static void linux_chrome_diag_text(')]
limit = next(line for line in source.splitlines()
             if line.startswith('#define LINUX_CHROME_UART_POLL_LIMIT '))
disabled = next(line for line in source.splitlines()
                if line.startswith('static bool g_linux_chrome_uart_disabled'))
fixture = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
''' + disabled + r'''
static unsigned reads, writes, ready_after;
static uint8_t status, written;
static uint8_t linux_chrome_uart_status(void) {
    ++reads;
    return reads > ready_after ? status : 0;
}
static void linux_chrome_uart_write(uint8_t value) { ++writes; written=value; }
static void reset(uint8_t value, unsigned delay) {
    reads=writes=0; status=value; ready_after=delay;
    g_linux_chrome_uart_disabled=false;
}
''' + limit + '\n' + function + r'''
int main(void) {
    assert(g_linux_chrome_uart_disabled == !OS_DEBUG_SERIAL);
    linux_chrome_diag_char('x');
    if (!OS_DEBUG_SERIAL) {
        assert(!reads && !writes);
        for (unsigned i=0;i<16384;++i) linux_chrome_diag_char('x');
        assert(!reads && !writes);
    }
    reset(0x20,0); linux_chrome_diag_char('a');
    assert(reads==1 && writes==1 && written=='a');
    reset(0x20,127); linux_chrome_diag_char('b');
    assert(reads==128 && writes==1 && written=='b');
    reset(0xff,0); linux_chrome_diag_char('c');
    assert(reads==1 && !writes && g_linux_chrome_uart_disabled);
    for(unsigned i=0;i<16384;++i) linux_chrome_diag_char('x');
    assert(reads==1 && !writes);
    reset(0,0); linux_chrome_diag_char('d');
    assert(reads==128 && !writes && g_linux_chrome_uart_disabled);
    for(unsigned i=0;i<16384;++i) linux_chrome_diag_char('x');
    assert(reads==128 && !writes);
    reset(0x20,128); linux_chrome_diag_char('e');
    assert(!writes && g_linux_chrome_uart_disabled);
}
'''
with tempfile.TemporaryDirectory(prefix='chrome-uart-bound-') as temporary:
    binary = str(Path(temporary) / 'test')
    for serial in (0, 1):
        subprocess.run(['cc','-std=gnu11','-Wall','-Wextra','-Werror',
                        f'-DOS_DEBUG_SERIAL={serial}', '-x','c','-','-o',binary],
                       input=fixture, text=True, check=True)
        subprocess.run([binary],check=True)
print('chrome UART: quiet default has no hardware I/O; explicit ready/delayed/absent/stuck paths bounded')
