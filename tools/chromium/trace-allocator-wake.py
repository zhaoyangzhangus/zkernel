"""Capture one allocator unlock stack; always resume the canonical guest."""
import argparse
from pathlib import Path
import subprocess

parser = argparse.ArgumentParser()
parser.add_argument('--pid', type=int, required=True)
parser.add_argument('--lock', type=lambda value: int(value, 0), required=True)
args = parser.parse_args()
if args.pid <= 0 or not 0x10000 <= args.lock < 0x800000000000 or args.lock % 4:
    parser.error('Invalid inspected browser PID or aligned user lock address')
root = Path(__file__).resolve().parents[2]
import sys
sys.path.insert(0, str(root / 'tools'))
from qmp_client import QMPClient
subprocess.run(['cc', '-g3', '-fno-eliminate-unused-debug-types', '-ffreestanding',
                '-DOS_KERNEL_BUILD', '-Iinclude', '-c',
                'tools/chromium/inspection-types.c', '-o',
                'build/current-inspection-types.o'], cwd=root, check=True)

def commands(items):
    with QMPClient() as qmp:
        return [qmp.command(name, arguments) for name, arguments in items]

assert commands([('query-status', {})])[0]['running'], 'Guest already stopped'
commands([('human-monitor-command', {'command-line':
           'gdbserver unix:/tmp/system-allocator.sock,server=on,wait=off'})])
try:
    subprocess.run(['gdb', '-q', '-nx', '-batch', 'build/qemu-running-kernel.elf',
                    '-ex', f'set $browser_pid = {args.pid}',
                    '-ex', f'set $allocator_lock = {args.lock}',
                    '-x', 'tools/chromium/trace-allocator-lock-wake.gdb'],
                   cwd=root, check=True, timeout=40)
finally:
    # 终止超时的调试器不等于远程分离。QEMU 可能仍保留硬件断点，并在 `cont` 后立即再次暂停。
    # 禁用调试服务器前，重新连接并显式执行分离。
    try:
        subprocess.run(['gdb', '-q', '-nx', '-batch',
                        '-ex', 'target remote /tmp/system-allocator.sock',
                        '-ex', 'detach'], cwd=root, check=True, timeout=10)
    finally:
        commands([('human-monitor-command', {'command-line': 'gdbserver none'}),
                  ('cont', {})])
