"""Reject stale guest code and restore QEMU state on every diagnostic path."""
from pathlib import Path
import struct
import sys
import tempfile
from types import SimpleNamespace
from unittest.mock import patch

root = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(root / 'tools/chromium'))
import guest_inspection as inspection


def rejects(function, *args):
    try:
        function(*args)
    except RuntimeError:
        return
    raise AssertionError('invalid diagnostic input accepted')


with tempfile.TemporaryDirectory(prefix='system-inspection-test-') as directory:
    path = Path(directory) / 'kernel.elf'
    payload = b'a' * 65536 + b'z'
    address = 0xffffffff80000000
    header = bytearray(64)
    header[:6] = b'\x7fELF\x02\x01'
    struct.pack_into('<Q', header, 32, 64)
    struct.pack_into('<HH', header, 54, 56, 1)
    program = struct.pack('<IIQQQQQQ', 1, 5, 120, address, address,
                          len(payload), len(payload), 4096)
    valid = bytes(header) + program + payload
    path.write_bytes(valid)
    segments = inspection.executable_segments(path)
    assert segments == [(address, payload)]
    reads = []
    def read_memory(start, length):
        reads.append((start, length))
        return memoryview(payload)[start - address:start - address + length]
    inspection.verify_executable(SimpleNamespace(read_memory=read_memory), segments)
    assert reads == [(address, 65536), (address + 65536, 1)]
    rejects(inspection.verify_executable,
            SimpleNamespace(read_memory=lambda start, size: b'b' * size), segments)
    # Catch a mismatch beyond the first transport chunk as well.
    rejects(inspection.verify_executable,
            SimpleNamespace(read_memory=lambda start, size:
                            read_memory(start, size) if start == address else b'x'), segments)
    for broken in (b'', valid[:100], b'BAD!' + valid[4:],
                   valid[:54] + b'\x01\x00' + valid[56:],
                   valid[:56] + b'\x00\x00' + valid[58:],
                   valid[:68] + struct.pack('<I', 4) + valid[72:],
                   valid[:96] + struct.pack('<Q', len(payload) + 1) + valid[104:]):
        path.write_bytes(broken)
        rejects(inspection.executable_segments, path)


class QMP:
    def __init__(self, running):
        self.running = running
        self.commands = []
    def __enter__(self):
        return self
    def __exit__(self, *args):
        pass
    def command(self, name):
        self.commands.append(name)
        if name == 'query-status':
            return {'running': self.running,
                    'status': 'running' if self.running else 'paused'}
        assert name in ('cont', 'stop')
        self.running = name == 'cont'
        return {}


for initially_running in (False, True):
    for failure in (None, 'snapshot', 'mismatch', 'connect', 'detach', 'compile'):
        qmp = QMP(initially_running)
        commands, builds, snapshots = [], [], []
        attached = [False]
        def execute(command):
            commands.append(command)
            if command.startswith('target remote '):
                attached[0] = True
                qmp.running = False
                if failure == 'connect':
                    raise RuntimeError('partial connection failure')
            if command == 'detach':
                attached[0] = False
                if failure == 'detach':
                    raise RuntimeError('detach failure')
                qmp.running = True
        def compile_types(command, **kwargs):
            builds.append((command, kwargs))
            assert 'inspection-types.c' in command[-3]
            assert '/build/' not in command[-1]
            if failure == 'compile':
                raise RuntimeError('compiler failed before touching QEMU')
        def snapshot():
            snapshots.append(True)
            if failure == 'snapshot':
                raise RuntimeError('diagnostic failed')
        inferior = SimpleNamespace(
            threads=lambda: [1] if attached[0] else [],
            read_memory=lambda address, length: b'x' if failure == 'mismatch' else b'z')
        fake_gdb = SimpleNamespace(execute=execute, selected_inferior=lambda: inferior)
        fake_client = SimpleNamespace(QMPClient=lambda: qmp)
        with patch.dict(sys.modules, {'gdb': fake_gdb, 'qmp_client': fake_client}), \
             patch.object(inspection, 'executable_segments', return_value=[(1, b'z')]), \
             patch.object(inspection.subprocess, 'run', side_effect=compile_types):
            if failure:
                rejects(inspection.run, snapshot)
            else:
                inspection.run(snapshot)
        assert qmp.running == initially_running
        assert not attached[0]
        assert len(builds) == 1
        assert bool(snapshots) == (failure in (None, 'snapshot', 'detach'))
        if failure == 'compile':
            assert not commands and not qmp.commands
        else:
            assert commands.count('detach') == 1

for name in ('inspect-guest-waits.gdb', 'inspect-current-cpus.gdb',
             'inspect-current-tcp.gdb'):
    script = (root / 'tools/chromium' / name).read_text()
    assert 'from guest_inspection import run' in script and 'run(snapshot)' in script
    assert 'target remote' not in script and 'inspection-types.o' not in script
waits = (root / 'tools/chromium/inspect-guest-waits.gdb').read_text()
assert 'range(4)' not in waits and "lookup_type('arch_trap_frame_t').sizeof" in waits
assert 'pid == 125' not in waits and '0x60000000' not in waits
assert 'from guest_memory import read_user' in waits and 'OS_INSPECT_TIDS' in waits
tcp = (root / 'tools/chromium/inspect-current-tcp.gdb').read_text()
assert 'g_registry_capacity' in tcp and 'range(capacity)' in tcp
assert 'system_inspection_rtl_ring_type' in tcp
assert "['mapped_length']" not in tcp  # Page-rounded DMA span is not ring capacity.
assert 'runs[:32]' in tcp and 'Invalid receive ring geometry' in tcp
print('Guest inspection: ELF/code validation, fresh private types, two run states, '
      'compile/connect/snapshot/detach failures and unified consumers passed')
