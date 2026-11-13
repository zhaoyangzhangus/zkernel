#!/usr/bin/env python3
"""Cycle two visually distinct history pages; begin with a forward entry."""
import argparse
import json
import pathlib
import shutil
import statistics
import tempfile
import time

parser = argparse.ArgumentParser()
parser.add_argument('--cycles', type=int, default=20)
parser.add_argument('--dwell-seconds', type=float, default=.3)
parser.add_argument('--hold-ms', type=int, default=100)
args = parser.parse_args()
if args.cycles < 1 or args.dwell_seconds < 0 or not 1 <= args.hold_ms <= 1000:
    parser.error('invalid cycles, dwell-seconds or hold-ms (1..1000)')
root = pathlib.Path(__file__).resolve().parents[2]
import sys
sys.path.insert(0, str(root / 'tools'))
from qmp_client import QMPClient
qmp = QMPClient()
command_timings = []

def command(name, arguments=None):
    return qmp.command(name, arguments)


def navigate(direction):
    command('send-key', {'keys': [
        {'type': 'qcode', 'data': 'alt'},
        {'type': 'qcode', 'data': direction}], 'hold-time': args.hold_ms})

results = []
soak_start = time.monotonic()
with tempfile.TemporaryDirectory(prefix='system-navigation-') as directory:
    filename = str(pathlib.Path(directory) / 'frame.ppm')

    def frame():
        command('screendump', {'filename': filename})
        with open(filename, 'rb') as image:
            assert image.readline().strip() == b'P6'
            width, height = map(int, image.readline().split())
            assert image.readline().strip() == b'255' and height >= 900
            pixels = image.read()
        # 避开焦点/插入光标、菜单和光标区域；对文章正文采样。
        return b''.join(pixels[(y * width + x) * 3:(y * width + x) * 3 + 3]
                        for y in range(200, 550, 4) for x in range(100, 850, 4))

    def difference(a, b):
        return sum(abs(x - y) > 20 for x, y in zip(a, b)) / len(a)

    def stable_frame():
        # 绝不将尚未完整呈现的帧作为预期页面。保留现有的不匹配门禁，改为验证基线稳定性。
        previous = frame()
        stable_since = time.monotonic()
        deadline = stable_since + 5
        while time.monotonic() < deadline:
            time.sleep(.1)
            current = frame()
            if difference(previous, current) >= .001:
                stable_since = time.monotonic()
            elif time.monotonic() - stable_since >= .5:
                return current
            previous = current
        raise RuntimeError('Page baseline did not stabilize; cannot verify history')

    home = stable_frame()
    shutil.copyfile(filename, root / 'build/chrome-soak-home-reference.ppm')
    command('screendump', {'filename': str(root / 'build/chrome-soak-home.png'), 'format': 'png'})
    navigate('right')
    time.sleep(3)
    article = stable_frame()
    shutil.copyfile(filename, root / 'build/chrome-soak-article-reference.ppm')
    assert difference(home, article) > .05, 'History pages not distinct; do not count as passed'
    command('screendump', {'filename': str(root / 'build/chrome-soak-article.png'), 'format': 'png'})
    for cycle in range(args.cycles):
        for direction, expected in [('left', home), ('right', article)]:
            start = time.monotonic()
            cpu_start = time.process_time()
            first_command = len(command_timings)
            navigate(direction)
            while time.monotonic() - start < 15:
                mismatch = difference(frame(), expected)
                if mismatch < .02:
                    elapsed = time.monotonic() - start
                    results.append({'cycle': cycle + 1, 'direction': direction,
                                    'seconds': elapsed, 'mismatch': mismatch,
                                    'observer_cpu_seconds': time.process_time() - cpu_start,
                                    'qmp_seconds': sum(t['seconds'] for t in command_timings[first_command:]),
                                    'slowest_command': max(command_timings[first_command:],
                                                           key=lambda t: t['seconds'])})
                    if elapsed > 1:
                        (root / 'build/chrome-navigation-slow-events.json').write_text(
                            json.dumps([r for r in results if r['seconds'] > 1], indent=2) + '\n')
                    print(f'{cycle + 1} {direction}: {elapsed:.3f}s', flush=True)
                    break
                time.sleep(.1)
            else:
                (root / 'build/chrome-navigation-failure-timing.json').write_text(
                    json.dumps({'cycle': cycle + 1, 'direction': direction,
                                'elapsed_seconds': time.monotonic() - start,
                                'observer_cpu_seconds': time.process_time() - cpu_start,
                                'mismatch': mismatch,
                                'commands': command_timings[first_command:]}, indent=2) + '\n')
                command('screendump', {'filename': str(root / 'build/chrome-soak-failure.png'), 'format': 'png'})
                shutil.copyfile(filename, root / 'build/chrome-soak-failure.ppm')
                raise RuntimeError('History navigation did not restore the expected page')
            # 不要让下一个快捷键与尚未释放的按键重叠。
            time.sleep(max(args.dwell_seconds,
                           args.hold_ms / 1000 - (time.monotonic() - start) + .05))
        (root / 'build/chrome-navigation-soak-progress.json').write_text(
            json.dumps({'completed_cycles': cycle + 1,
                        'requested_cycles': args.cycles,
                        'transitions': len(results),
                        'elapsed_seconds': time.monotonic() - soak_start,
                        'last_transition': results[-1]}, indent=2) + '\n')
    command('screendump', {'filename': str(root / 'build/chrome-soak-final.png'), 'format': 'png'})
report = {'transitions': len(results), 'median_seconds': statistics.median(
    r['seconds'] for r in results), 'max_seconds': max(r['seconds'] for r in results),
    'verification': 'Sampled body pixels; inspect article/final screenshots',
    'includes': 'QMP input, frame capture and polling; not pure browser latency',
    'results': results}
report['elapsed_seconds'] = time.monotonic() - soak_start
report['hold_ms'] = args.hold_ms
(root / 'build/chrome-navigation-soak.json').write_text(json.dumps(report, indent=2) + '\n')
print(json.dumps({key: value for key, value in report.items() if key != 'results'}))
