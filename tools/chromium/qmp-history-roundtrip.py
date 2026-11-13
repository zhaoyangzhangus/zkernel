"""Actual toolbar history clicks, comparing both loaded page references.

Requires a normal Chrome with an existing forward-history entry. The two
bootstrap screenshots need semantic review; restoration compares page bodies,
not merely an address-bar or button change. Leaves the original page open.
"""
import argparse
import ast
import json
from pathlib import Path
import tempfile
import time

root = Path(__file__).resolve().parents[2]
import sys
sys.path.insert(0, str(root / 'tools'))
from qmp_client import QMPClient
parser = argparse.ArgumentParser()
parser.add_argument('--cycles', type=int, default=3)
args = parser.parse_args()
if not 1 <= args.cycles <= 10:
    parser.error('cycles must be 1..10')
tree = ast.parse((root/'tools/chromium/qmp-startup-latency.py').read_text())
node = next(n for n in tree.body if isinstance(n, ast.FunctionDef) and n.name == 'browser_extent')
namespace = {}
exec(compile(ast.Module(body=[node], type_ignores=[]), '<extent>', 'exec'), namespace)
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
        time.sleep(.025)

def click():
    for down in (True, False):
        command('input-send-event', {'events': [
            {'type': 'btn', 'data': {'button': 'left', 'down': down}}]})
        if down:
            time.sleep(.02)

def difference(a, b):
    assert len(a) == len(b) and a
    return sum(abs(x-y) > 20 for x, y in zip(a, b))/len(a)

def capture(label):
    command('screendump', {'filename': str(root/f'build/chrome-history-{label}.png'),
                         'format': 'png'})


assert command('query-status')['running']
relative(-2000, -1200)
relative(72, 72)
results = []
with tempfile.TemporaryDirectory(prefix='system-history-') as directory:
    filename = str(Path(directory)/'frame.ppm')

    def frame(check_geometry=False):
        command('screendump', {'filename': filename})
        with open(filename, 'rb') as image:
            assert image.readline().strip() == b'P6'
            width, height = map(int, image.readline().split())
            assert (width, height) == (1920, 1080)
            assert image.readline().strip() == b'255'
            pixels = image.read()
        if check_geometry:
            extent = namespace['browser_extent'](width, pixels)
            if extent is None or not (8 <= extent[0] <= 14 and 950 <= extent[1] <= 958):
                raise RuntimeError('Expected inspected normal browser, not another placement')
        return b''.join(pixels[(y*width+x)*3:(y*width+x)*3+3]
                        for y in range(160, 900, 8) for x in range(30, 900, 8))

    original = frame(True)
    capture('original-reference')
    click()
    started = time.monotonic()
    while difference(original, frame()) < .03:
        if time.monotonic()-started > 15:
            capture('failure')
            raise RuntimeError('Forward bootstrap did not produce a distinct page body')
        time.sleep(.02)
    # 初始页面基准必须先稳定；首次绘制可能尚未完成。
    previous = frame()
    stable_since = time.monotonic()
    while True:
        time.sleep(.05)
        next_page = frame()
        if difference(previous, next_page) > .002:
            stable_since = time.monotonic()
        if time.monotonic()-stable_since >= .5:
            break
        if time.monotonic()-started > 15:
            capture('failure')
            raise RuntimeError('Forward bootstrap body did not settle')
        previous = next_page
    capture('forward-reference')
    assert difference(original, next_page) > .03, 'Insufficient body contrast'
    relative(-36, 0)
    for cycle in range(1, args.cycles+1):
        for label, reference, motion in (('back', original, 36), ('forward', next_page, -36)):
            started = time.monotonic()
            click()
            while True:
                mismatch = difference(reference, frame())
                if mismatch < .005:
                    break
                if time.monotonic()-started > 15:
                    capture('failure')
                    raise RuntimeError(f'{label}: page body did not restore, difference={mismatch}')
                time.sleep(.01)
            record = {'cycle': cycle, 'button': label,
                      'body_restore_ms': (time.monotonic()-started)*1000,
                      'body_reference_difference': mismatch}
            results.append(record)
            print(json.dumps(record), flush=True)
            capture(f'{label}-{cycle}')
            relative(motion, 0)
            time.sleep(.1)
    click()
    started = time.monotonic()
    while difference(original, frame()) >= .005:
        if time.monotonic()-started > 15:
            raise RuntimeError('Final original page restoration failed')
        time.sleep(.01)
    capture('final')
(root/'build/chrome-history-roundtrip.json').write_text(json.dumps({
    'results': results,
    'scope': 'Actual toolbar clicks restoring both sampled page bodies; review reference screenshots',
    'includes': '20ms held click, QMP delivery, screenshot and sampling overhead'
}, indent=2)+'\n')
