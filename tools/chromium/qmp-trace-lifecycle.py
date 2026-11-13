"""Run existing tab/header regression under a failure-only GDB breakpoint."""
import argparse
import json
from pathlib import Path
import signal
import subprocess
import time

parser = argparse.ArgumentParser()
parser.add_argument('--cycles', type=int, default=20)
parser.add_argument('--batches', type=int, default=1)
args = parser.parse_args()
if not 1 <= args.cycles <= 20:
    parser.error('cycles must be 1..20')
if not 1 <= args.batches <= 5:
    parser.error('batches must be 1..5')
root = Path(__file__).resolve().parents[2]
import sys
sys.path.insert(0, str(root / 'tools'))
from qmp_client import QMPClient
if not (root/'build/qemu-running-kernel.elf').is_file():
    parser.error('Missing symbols for the running guest kernel')


def command(name, arguments=None):
    # 在工作负载创建自己的控制连接之前先关闭连接。
    with QMPClient() as qmp:
        return qmp.command(name, arguments)


if not command('query-status')['running']:
    raise RuntimeError('Guest already stopped; leave existing debugger alone')
debugger = workload = None
reports = []
try:
    command('human-monitor-command', {'command-line':
            'gdbserver unix:/tmp/system-keys.sock,server=on,wait=off'})
    with (root / 'build/chrome-tab-lifecycle-destroy.log').open('w') as trace:
        debugger = subprocess.Popen([
            'gdb', '-q', '-batch', 'build/qemu-running-kernel.elf',
            '-x', 'tools/chromium/trace-destroying-window.gdb'],
            cwd=root, stdout=trace, stderr=subprocess.STDOUT)
        time.sleep(1)
        if debugger.poll() is not None:
            raise RuntimeError('Debugger exited before workload; inspect trace')
        for batch in range(1, args.batches + 1):
            print('Lifecycle batch %d/%d' % (batch, args.batches), flush=True)
            workload = subprocess.Popen([
                'python3', 'tools/chromium/qmp-tab-close-test.py',
                '--cycles', str(args.cycles)], cwd=root)
            try:
                result = workload.wait(timeout=60)
            except subprocess.TimeoutExpired:
                workload.terminate()
                workload.wait(timeout=3)
                raise RuntimeError('Bounded workload expired; no automatic relaunch')
            if result != 0:
                raise RuntimeError('Tab regression failed; inspect screenshot and trace')
            report = json.loads((root / 'build/chrome-tab-close-test.json').read_text())
            reports.append({'batch': batch, 'cycles': report['cycles']})
            (root / 'build/chrome-tab-lifecycle-report.json').write_text(json.dumps({
                'completed_batches': len(reports),
                'completed_cycles': sum(len(r['cycles']) for r in reports),
                'requested_batches': args.batches,
                'batches': reports,
                'verification': 'Actual guest new-tab creation, X click and header restoration',
                'limitation': 'Failure-only GDB breakpoint armed; timings include hold/capture/polling'
            }, indent=2) + '\n')
finally:
    if workload is not None and workload.poll() is None:
        workload.terminate()
        workload.wait(timeout=3)
    if debugger is not None and debugger.poll() is None:
        debugger.send_signal(signal.SIGINT)
        try:
            debugger.wait(timeout=3)
        except subprocess.TimeoutExpired:
            debugger.terminate()
            debugger.wait(timeout=3)
    command('human-monitor-command', {'command-line': 'gdbserver none'})
    command('cont')
    print(command('query-status'), flush=True)
print((root / 'build/chrome-tab-lifecycle-destroy.log').read_text())
