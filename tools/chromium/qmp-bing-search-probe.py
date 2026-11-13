"""Submit Bing's visible home form without assuming a fixed banner height.

This captures evidence, not a semantic pass: inspect typed/result screenshots.
Requires the single canonical Chrome at its inspected 945x1060 placement.
"""
import argparse
import json
from pathlib import Path
import tempfile
import time

def find_search_band(width, pixels):
    runs = []
    start = None
    for y in range(200, 451):
        white = sum(min(pixels[(y*width+x)*3:(y*width+x)*3+3]) >= 248
                    for x in range(400, 700, 4))
        # 空表单占位符位于此条带左侧。不要按字形行拆分输入矩形，也不要依赖横幅高度。
        if white >= 71 and y < 450:
            if start is None:
                start = y
        elif start is not None:
            runs.append((start, y))
            start = None
    candidates = [(a, b) for a, b in runs if 30 <= b-a <= 65]
    if len(candidates) != 1:
        raise RuntimeError(f'No unique home search band: {runs}; do not click blindly')
    return candidates[0]

parser = argparse.ArgumentParser()
parser.add_argument('--query', default='chromium')
args = parser.parse_args()
if not args.query or not all(c in 'abcdefghijklmnopqrstuvwxyz ' for c in args.query):
    parser.error('query must contain lowercase ASCII letters/spaces')
root = Path(__file__).resolve().parents[2]
import sys
sys.path.insert(0, str(root / 'tools'))
from qmp_client import QMPClient
qmp = QMPClient()

def command(name, arguments=None):
    return qmp.command(name, arguments)

def key(*codes):
    command('send-key', {'keys': [{'type': 'qcode', 'data': c} for c in codes],
                         'hold-time': 40})
    time.sleep(.1)

def relative(dx, dy):
    while dx or dy:
        x, y = max(-100, min(100, dx)), max(-100, min(100, dy))
        command('input-send-event', {'events': [
            {'type': 'rel', 'data': {'axis': 'x', 'value': x}},
            {'type': 'rel', 'data': {'axis': 'y', 'value': y}}]})
        dx, dy = dx-x, dy-y
        time.sleep(.05)


assert command('query-status')['running']
with tempfile.TemporaryDirectory(prefix='system-bing-form-') as directory:
    path = str(Path(directory) / 'home.ppm')
    command('screendump', {'filename': path})
    with open(path, 'rb') as image:
        assert image.readline().strip() == b'P6'
        width, height = map(int, image.readline().split())
        assert (width, height) == (1920, 1080)
        assert image.readline().strip() == b'255'
        pixels = image.read()
    top, bottom = find_search_band(width, pixels)
    target_y = (top+bottom)//2

relative(-2000, -1200)
relative(300, target_y)
for down in (True, False):
    command('input-send-event', {'events': [
        {'type': 'btn', 'data': {'button': 'left', 'down': down}}]})
    time.sleep(.05)
key('ctrl', 'a')
for char in args.query:
    key('spc' if char == ' ' else char)
command('screendump', {'filename': str(root/'build/chrome-bing-form-typed.png'), 'format': 'png'})
key('ret')
time.sleep(12)
command('screendump', {'filename': str(root/'build/chrome-bing-form-result.png'), 'format': 'png'})
print(json.dumps({'query': args.query, 'search_band': [top, bottom],
                  'click': [300, target_y],
                  'verification': 'Inspect typed query and rendered results; input delivery alone is not a pass'}))
