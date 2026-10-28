"""Shared read-only GDB setup for the sole canonical guest.

Compile types before stopping the guest, reject mismatched executable bytes,
and restore the original run state even when a diagnostic raises.
"""
from pathlib import Path
import json
import os
import shlex
import struct
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]


def executable_segments(path):
    data = Path(path).read_bytes()
    if len(data) < 64 or data[:6] != b'\x7fELF\x02\x01':
        raise RuntimeError('Expected a little-endian ELF64 kernel')
    phoff = struct.unpack_from('<Q', data, 32)[0]
    phsize, count = struct.unpack_from('<HH', data, 54)
    if phsize != 56 or not count or phoff + phsize * count > len(data):
        raise RuntimeError('Invalid kernel program headers')
    segments = []
    total = 0
    for index in range(count):
        kind, flags, offset, address, _, size, memory_size, _ = struct.unpack_from(
            '<IIQQQQQQ', data, phoff + index * phsize)
        if kind != 1 or not flags & 1:
            continue
        total += size
        if (size == 0 or size > memory_size or total > 64 * 1024 * 1024 or
                offset + size > len(data) or address + size > 1 << 64):
            raise RuntimeError('Invalid kernel executable segment')
        segments.append((address, data[offset:offset + size]))
    if not segments:
        raise RuntimeError('Kernel has no executable LOAD segment')
    return segments


def verify_executable(inferior, segments):
    for address, data in segments:
        for offset in range(0, len(data), 65536):
            expected = data[offset:offset + 65536]
            actual = bytes(inferior.read_memory(address + offset, len(expected)))
            if actual != expected:
                raise RuntimeError('Refusing stale kernel symbols: executable '
                                   f'bytes differ at {address + offset:#x}')


def restore_run_state(qmp, was_running):
    state = qmp.command('query-status')
    if state['status'] not in ('running', 'paused'):
        raise RuntimeError('Guest changed to a terminal state during inspection')
    if state['running'] != was_running:
        qmp.command('cont' if was_running else 'stop')
        if qmp.command('query-status')['running'] != was_running:
            raise RuntimeError('Could not restore the guest run state')


def run(snapshot):
    import gdb
    sys.path.insert(0, str(ROOT / 'tools'))
    from qmp_client import QMPClient

    if gdb.selected_inferior().threads():
        raise RuntimeError('Use a separate batch GDB; refusing an attached session')
    kernel = ROOT / 'build/esp/EFI/OS/kernel.elf'
    segments = executable_segments(kernel)
    endpoint = ROOT / 'build/chrome-debug-gdb.sock'
    # Do not reuse a shared .o: another inspector may replace it underneath
    # GDB, and four independently cached objects can disagree on one type.
    with tempfile.TemporaryDirectory(prefix='system-inspection-') as directory:
        types = Path(directory) / 'types.o'
        subprocess.run(shlex.split(os.environ.get('CC', 'cc')) + [
            '-g3', '-fno-eliminate-unused-debug-types', '-ffreestanding',
            '-DOS_KERNEL_BUILD', '-Iinclude', '-c',
            'tools/chromium/inspection-types.c', '-o', str(types)],
            cwd=ROOT, check=True, timeout=30)
        gdb.execute('file ' + json.dumps(str(kernel)))
        gdb.execute('add-symbol-file ' + json.dumps(str(types)) + ' 0')
        with QMPClient() as qmp:
            state = qmp.command('query-status')
            if state['status'] not in ('running', 'paused'):
                raise RuntimeError('Guest is not available for inspection')
            attached = False
            try:
                gdb.execute('target remote ' + str(endpoint))
                attached = True
                verify_executable(gdb.selected_inferior(), segments)
                print('GUEST_KERNEL_TEXT_MATCH bytes=' +
                      str(sum(len(data) for _, data in segments)))
                snapshot()
            finally:
                try:
                    if attached or gdb.selected_inferior().threads():
                        gdb.execute('detach')
                finally:
                    restore_run_state(qmp, state['running'])
    print('GUEST_INSPECTION_OK')
