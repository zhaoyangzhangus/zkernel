"""Bounded single-wheel visible response probe in the canonical guest.

Screen samples are not renderer FPS. Report capture cost and sampling gaps;
use a loaded, static long page and inspect the saved baseline/down/final.
"""
import argparse
import ast
import json
from pathlib import Path
import statistics
import tempfile
import time

root = Path(__file__).resolve().parents[2]
import sys
sys.path.insert(0, str(root / 'tools'))
from qmp_client import QMPClient
parser = argparse.ArgumentParser()
parser.add_argument('--cycles', type=int, default=5)
args = parser.parse_args()
if not 1 <= args.cycles <= 20:
    parser.error('cycles must be 1..20')
tree = ast.parse((root/'tools/chromium/qmp-startup-latency.py').read_text())
extent_node = next(n for n in tree.body if isinstance(n, ast.FunctionDef)
                   and n.name == 'browser_extent')
namespace = {}
exec(compile(ast.Module(body=[extent_node], type_ignores=[]), '<extent>', 'exec'), namespace)
qmp = QMPClient()

def command(name, arguments=None):
    return qmp.command(name, arguments)

def relative(dx, dy):
    while dx or dy:
        x, y = max(-100, min(100, dx)), max(-100, min(100, dy))
        command('input-send-event', {'events': [
            {'type': 'rel', 'data': {'axis': 'x', 'value': x}},
            {'type': 'rel', 'data': {'axis': 'y', 'value': y}}]})
        dx, dy = dx-x, dy-y
        time.sleep(.03)

def difference(a, b):
    assert len(a) == len(b) and a
    return sum(abs(x-y) > 20 for x, y in zip(a, b))/len(a)

def screenshot(label):
    command('screendump', {'filename': str(root/f'build/chrome-scroll-cadence-{label}.png'),
                         'format': 'png'})


assert command('query-status')['running'], 'Do not change stopped guest state'
relative(-2000, -1200)
relative(920, 400)
time.sleep(.2)
results = []
with tempfile.TemporaryDirectory(prefix='system-scroll-cadence-') as directory:
    filename = str(Path(directory)/'frame.ppm')

    def frame(check_geometry=False):
        started = time.monotonic()
        command('screendump', {'filename': filename})
        with open(filename, 'rb') as image:
            assert image.readline().strip() == b'P6'
            width, height = map(int, image.readline().split())
            assert (width, height) == (1920, 1080)
            assert image.readline().strip() == b'255'
            pixels = image.read()
            assert len(pixels) == width*height*3
        if check_geometry:
            extent = namespace['browser_extent'](width, pixels)
            if extent is None or not (8 <= extent[0] <= 14 and 950 <= extent[1] <= 958):
                raise RuntimeError('Expected normal inspected Chrome; do not inject input blindly')
        roi = b''.join(pixels[(y*width+x)*3:(y*width+x)*3+3]
                       for y in range(200, 650, 8) for x in range(100, 850, 8))
        return roi, (time.monotonic()-started)*1000

    baseline, _ = frame(True)
    screenshot('baseline')
    for cycle in range(1, args.cycles+1):
        for button in ('wheel-down', 'wheel-up'):
            previous, _ = frame()
            started = time.monotonic()
            for down in (True, False):
                command('input-send-event', {'events': [
                    {'type': 'btn', 'data': {'button': button, 'down': down}}]})
            changes, captures, gaps = [], [], []
            last_sample = started
            # 此项测量不保持按键，也不设置三个时钟节拍的间隔。
            while time.monotonic()-started < .7:
                current, capture_ms = frame()
                observed = time.monotonic()
                captures.append(capture_ms)
                gaps.append((observed-last_sample)*1000)
                last_sample = observed
                if difference(previous, current) > .005:
                    changes.append((observed-started)*1000)
                    previous = current
                time.sleep(.005)
            if not changes:
                screenshot('failure')
                raise RuntimeError(f'{button}: no significant visible response')
            restored = difference(baseline, current)
            if button == 'wheel-up' and restored > .02:
                screenshot('failure')
                raise RuntimeError(f'Single-wheel round trip did not restore: {restored}')
            record = {'cycle': cycle, 'button': button,
                      'first_visible_change_ms': changes[0],
                      'visible_change_times_ms': changes,
                      'capture_median_ms': statistics.median(captures),
                      'max_sample_gap_ms': max(gaps),
                      'baseline_difference': restored}
            results.append(record)
            print(json.dumps(record), flush=True)
            if cycle == 1 and button == 'wheel-down':
                screenshot('down')
        time.sleep(.1)
    screenshot('final')
summary = {'results': results,
           'first_change_median_ms': statistics.median(r['first_visible_change_ms'] for r in results),
           'first_change_max_ms': max(r['first_visible_change_ms'] for r in results),
           'scope': 'Single wheel tick to significant sampled page-pixel change; not renderer FPS',
           'includes': 'QMP delivery, screenshot capture, ROI comparison and sample spacing'}
(root/'build/chrome-scroll-cadence.json').write_text(json.dumps(summary, indent=2)+'\n')
print(json.dumps({k: v for k, v in summary.items() if k != 'results'}), flush=True)
