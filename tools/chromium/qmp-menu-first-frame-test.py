"""Check transparent margins throughout menu fade-in, not just its final frame.

Requires the canonical Chrome window at (10,10), on about:blank.
"""
import ast
import json
from pathlib import Path
import tempfile
import time

root = Path(__file__).resolve().parents[2]
import sys
sys.path.insert(0, str(root / 'tools'))
from qmp_client import QMPClient
tree = ast.parse((root / 'tools/chromium/qmp-app-menu-test.py').read_text())
functions = [n for n in tree.body if isinstance(n, ast.FunctionDef)
             and n.name == 'menu_is_painted']
namespace = {}
exec(compile(ast.Module(body=functions, type_ignores=[]), 'menu-gate', 'exec'), namespace)
qmp = QMPClient()

def command(name, arguments=None):
    return qmp.command(name, arguments)

def escape():
    command('send-key', {'keys': [{'type': 'qcode', 'data': 'esc'}], 'hold-time': 40})


assert command('query-status')['running']
escape()
time.sleep(.3)
for dx, dy in [(-2000, -1200), (928, 72)]:
    while dx or dy:
        x, y = max(-100, min(100, dx)), max(-100, min(100, dy))
        command('input-send-event', {'events': [
            {'type': 'rel', 'data': {'axis': 'x', 'value': x}},
            {'type': 'rel', 'data': {'axis': 'y', 'value': y}}]})
        dx -= x
        dy -= y
        time.sleep(.015)
with tempfile.TemporaryDirectory(prefix='system-menu-first-frame-') as directory:
    path = str(Path(directory) / 'frame.ppm')

    def frame():
        command('screendump', {'filename': path})
        with open(path, 'rb') as image:
            assert image.readline().strip() == b'P6'
            width, height = map(int, image.readline().split())
            assert (width, height) == (1920, 1080)
            assert image.readline().strip() == b'255'
            pixels = image.read()
        # 位于面板和有效阴影之外、原生弹出窗口范围之内。错误的动画会将整个条带绘制成蓝色。
        margin = b''.join(pixels[(y * width + 535) * 3:(y * width + 540) * 3]
                          for y in range(200, 650, 5))
        return margin, namespace['menu_is_painted'](width, pixels)

    try:
        for cycle in range(3):
            baseline, painted = frame()
            assert not painted, 'Menu baseline is not closed/neutral'
            for down in (True, False):
                command('input-send-event', {'events': [
                    {'type': 'btn', 'data': {'button': 'left', 'down': down}}]})
                if down:
                    time.sleep(.02)
            started = time.monotonic()
            samples = []
            seen_menu = False
            while time.monotonic() - started < .7:
                margin, painted = frame()
                changed = sum(abs(a - b) > 20 for a, b in zip(baseline, margin)) / len(baseline)
                samples.append({'ms': round((time.monotonic() - started) * 1000),
                                'margin_difference': changed})
                seen_menu |= painted
                if changed > .05:
                    command('screendump', {'filename': str(root / 'build/menu-first-frame-failure.png'),
                                          'format': 'png'})
                    raise AssertionError(f'Transient opaque menu margin: {samples}')
                time.sleep(.01)
            assert seen_menu, 'Menu did not become visible; margin-only test would be a false pass'
            assert len(samples) >= 5, 'Too few opening samples'
            print(json.dumps({'cycle': cycle + 1, 'sample_count': len(samples),
                              'first_sample_ms': samples[0]['ms'],
                              'last_sample_ms': samples[-1]['ms'],
                              'max_margin_difference': max(
                                  sample['margin_difference'] for sample in samples)}), flush=True)
            escape()
            time.sleep(.35)
            assert not frame()[1], 'Menu failed to close'
    finally:
        command('input-send-event', {'events': [
            {'type': 'btn', 'data': {'button': 'left', 'down': False}}]})
        escape()
