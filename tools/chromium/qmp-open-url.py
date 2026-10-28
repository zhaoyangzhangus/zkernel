"""Navigate the existing guest Chrome using guest keys; save visible evidence."""
import argparse
import json
from pathlib import Path
import subprocess
import time

parser = argparse.ArgumentParser()
parser.add_argument('url')
parser.add_argument('--label', default='online-probe')
parser.add_argument('--observe-seconds', type=int, default=30)
parser.add_argument('--save-samples', action='store_true',
                    help='Retain timestamped screenshots, not only the final frame')
parser.add_argument('--network-snapshot-after', type=int, default=0,
                    help='Capture a bounded guest network snapshot during loading; 0 disables')
args = parser.parse_args()
if not args.label.replace('-', '').isalnum() or not 1 <= args.observe_seconds <= 120:
    parser.error('invalid label or observation interval')
if not 0 <= args.network_snapshot_after < args.observe_seconds:
    parser.error('network-snapshot-after must be 0 or less than observe-seconds')
punctuation = {':': ('shift', 'semicolon'), '/': ('slash',), '.': ('dot',),
               '-': ('minus',), '_': ('shift', 'minus'), '?': ('shift', 'slash'),
               '=': ('equal',), '&': ('shift', '7'), '%': ('shift', '5'),
               ',': ('comma',), '#': ('shift', '3'), '!': ('shift', '1')}
keys = []
for char in args.url:
    if char in punctuation:
        keys.append(punctuation[char])
    elif char.isascii() and char.isalnum():
        keys.append(('shift', char.lower()) if char.isupper() else (char,))
    else:
        parser.error(f'unsupported URL character: {char!r}')
root = Path(__file__).resolve().parents[2]
import sys
sys.path.insert(0, str(root / 'tools'))
from qmp_client import QMPClient
sys.path.insert(0, str(root / 'tools/chromium'))
from guest_inspection import restore_run_state
qmp = QMPClient()

def command(name, arguments=None):
    return qmp.command(name, arguments)

def capture_network_snapshot():
    """Yield QMP to the verified inspector; preserve state even on timeout."""
    global qmp
    state = command('query-status')
    if state['status'] not in ('running', 'paused'):
        raise RuntimeError('Guest is not available for inspection')
    endpoint = root / 'build/chrome-debug-gdb.sock'
    if not endpoint.is_socket():
        command('human-monitor-command', {'command-line':
            'gdbserver unix:' + str(endpoint) + ',server=on,wait=off'})
    # QEMU accepts only one QMP client. The inspector owns that connection
    # while it verifies every executable byte and restores the guest state.
    qmp.close()
    try:
        path = root / f'build/chrome-{args.label}-network.log'
        with path.open('w') as log:
            result = subprocess.run(['gdb', '-q', '-nx', '-batch', '-x',
                str(root / 'tools/chromium/inspect-current-tcp.gdb')],
                cwd=root, stdout=log, stderr=subprocess.STDOUT, timeout=60)
        output = path.read_text()
        if (result.returncode != 0 or 'GUEST_KERNEL_TEXT_MATCH bytes=' not in output or
                'GUEST_INSPECTION_OK' not in output):
            raise RuntimeError('Network inspection failed; see ' + str(path))
    finally:
        qmp = QMPClient()
        # Cover GDB being killed by timeout before its own finally can run.
        # Terminal guest states are never resumed or reset.
        restore_run_state(qmp, state['running'])


def key(*codes, hold=40):
    # Complete the whole chord before typing the next character. send-key
    # schedules members separately and can leave Ctrl/Shift held here.
    try:
        command('input-send-event', {'events': [
            {'type': 'key', 'data': {'down': True,
             'key': {'type': 'qcode', 'data': code}}} for code in codes]})
        time.sleep(hold / 1000)
    finally:
        command('input-send-event', {'events': [
            {'type': 'key', 'data': {'down': False,
             'key': {'type': 'qcode', 'data': code}}} for code in reversed(codes)]})
    time.sleep(.05)


def observe_navigation():
    start = time.monotonic()
    deadline = start + args.observe_seconds
    snapshot_done = False
    snapshot_pause_seconds = 0.0
    samples = []
    sample_directory = root/f'build/chrome-{args.label}-samples'
    if args.save_samples:
        sample_directory.mkdir(parents=True, exist_ok=True)
    next_sample = start
    next_progress = start
    while time.monotonic() < deadline:
        now = time.monotonic()
        if (args.network_snapshot_after and not snapshot_done and
                now - start >= args.network_snapshot_after):
            assert command('query-status')['running'], 'Guest stopped before snapshot'
            snapshot_done = True
            snapshot_start = time.monotonic()
            try:
                capture_network_snapshot()
            finally:
                snapshot_pause_seconds += time.monotonic() - snapshot_start
            now = time.monotonic()
            if now >= deadline:
                break
        if args.save_samples and now >= next_sample:
            filename = sample_directory/f'{len(samples):03d}.png'
            before = time.monotonic() - start
            command('screendump', {'filename': str(filename), 'format': 'png'})
            samples.append({'file': str(filename.relative_to(root)),
                            'capture_start_seconds': before,
                            'capture_end_seconds': time.monotonic() - start})
            # 慢截图或诊断跨过的采样点直接跳过，避免补拍造成连续阻塞。
            next_sample = start + int(time.monotonic() - start) + 1
        now = time.monotonic()
        if now >= next_progress:
            print(f'observing navigation: {now - start:.1f}s', flush=True)
            next_progress = start + (int((now - start) / 5) + 1) * 5
        wake = min(deadline, next_progress)
        if args.save_samples:
            wake = min(wake, next_sample)
        if args.network_snapshot_after and not snapshot_done:
            wake = min(wake, start + args.network_snapshot_after)
        delay = wake - time.monotonic()
        if delay > 0:
            time.sleep(delay)
    # 默认只保留观察结束时的画面，不反复编码、覆盖同一张图片。
    observed = time.monotonic() - start
    command('screendump', {'filename': str(root / f'build/chrome-{args.label}.png'),
                          'format': 'png'})
    report = {'url': args.url, 'observation_seconds': observed,
              'diagnostic_pause_seconds': snapshot_pause_seconds,
              'verification': 'Inspect typed URL and screenshots; command delivery alone is not a pass'}
    if args.save_samples:
        report['samples'] = samples
        (sample_directory/'timing.json').write_text(json.dumps(report, indent=2)+'\n')
    return report


assert command('query-status')['running'], 'Guest already stopped; leave debugger state alone'
key('ctrl', 'l', hold=100)
for codes in keys:
    key(*codes)
# Omnibox inline completion selects an untyped suffix (e.g. the previous
# /search?q=... when navigating to the bare origin). Forward Delete removes
# that selection and suppresses inline completion; at an unselected end it is
# a no-op. Enter must navigate the requested URL, not the history completion.
key('delete')
command('screendump', {'filename': str(root / f'build/chrome-{args.label}-typed.png'),
                      'format': 'png'})
key('ret')
try:
    report = observe_navigation()
finally:
    qmp.close()
print(json.dumps({key:value for key,value in report.items() if key != 'samples'}))
