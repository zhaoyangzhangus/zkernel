"""Guest-only resize QA after cube drag; optionally test its busy terminal."""
import argparse
from pathlib import Path
import sys
import tempfile
import time

root = Path(__file__).resolve().parents[2]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--terminal', action='store_true',
                    help='resize the terminal while its foreground cube is running')
args = parser.parse_args()
sys.path.insert(0, str(root / 'tools'))
from qmp_client import QMPClient
from qemu_frame import read_qemu_frame
qmp = QMPClient()
command, move, click = qmp.command, qmp.move, qmp.click
assert command('query-status')['running']

def drag(x, y, dx, dy):
    move(x, y)
    command('input-send-event', {'events': [
        {'type': 'btn', 'data': {'button': 'left', 'down': True}}]})
    try:
        for _ in range(5):
            command('input-send-event', {'events': [
                {'type': 'rel', 'data': {'axis': 'x', 'value': dx // 5}},
                {'type': 'rel', 'data': {'axis': 'y', 'value': dy // 5}}]})
            time.sleep(.03)
    finally:
        command('input-send-event', {'events': [
            {'type': 'btn', 'data': {'button': 'left', 'down': False}}]})
    move(1800, 960)
    time.sleep(.4)

with tempfile.TemporaryDirectory(prefix='system-window-resize-') as directory:
    filename = str(Path(directory) / 'frame.ppm')
    def frame():
        command('screendump', {'filename': filename})
        return read_qemu_frame(filename)
    def pixel(pixels, point):
        x, y = point
        offset = (y * 1920 + x) * 3
        return pixels[offset:offset + 3]
    # 所有采样点都在留于 (124,68)、大小为 802x632 的立方体之外。
    # 直接记录实际露出的背景，不依赖 PNG/图像处理库。
    click(1600, 155)  # Minimize the terminal waiting synchronously for cube exit.
    move(1800, 960)
    background = frame()
    # 复用 QA 终端，或创建空白编辑器；绝不打开用户文档。
    click(815 if args.terminal else 912, 1020)
    time.sleep(.8)
    move(1800, 960)
    original = frame()
    points = {'left': (250, 800), 'right': (1669, 800),
              'top': (1100, 140), 'bottom': (1100, 934)}
    # 从每条边向内 5 像素：超出旧的 3px 拖拽区域，处于扩大的 8px 区域内，
    # 并避开圆角透明像素。
    routes = [(245, 800, 30, 0, ('left',)),
              (1674, 800, -30, 0, ('right',)),
              (1100, 140, 0, 20, ('top',)),
              (1100, 939, 0, -20, ('bottom',)),
              (245, 140, 30, 20, ('left', 'top')),
              (1674, 140, -30, 20, ('right', 'top')),
              (245, 939, 30, -20, ('left', 'bottom')),
              (1674, 939, -30, -20, ('right', 'bottom'))]
    for x, y, dx, dy, sides in routes:
        for side in sides:
            assert pixel(original, points[side]) != pixel(background, points[side])
        drag(x, y, dx, dy)
        smaller = frame()
        for side in sides:
            assert pixel(smaller, points[side]) == pixel(background, points[side]), side
        drag(x + dx, y + dy, -dx, -dy)
        restored = frame()
        for side in sides:
            assert pixel(restored, points[side]) == pixel(original, points[side]), side
        print('Resize ' + '/'.join(sides) + ': shrink, background repair and restore passed', flush=True)
    click(324, 83)  # Return the running cube to the foreground.
    command('screendump', {'filename': str(root / 'build/resize-geometry-verified.png'),
                          'format': 'png'})
    assert command('query-status')['running']
qmp.close()
