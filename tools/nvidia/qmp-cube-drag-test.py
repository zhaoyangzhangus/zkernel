"""Guest-only visual regression on the single canonical fresh 1080p QEMU."""
from pathlib import Path
import sys
import tempfile
import time

root = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(root / 'tools'))
from qmp_client import QMPClient
from qemu_frame import read_qemu_frame
qmp = QMPClient()
command, move, click, key = qmp.command, qmp.move, qmp.click, qmp.key
assert command('query-status')['running']

def delta(x, y):
    command('input-send-event', {'events': [
        {'type': 'rel', 'data': {'axis': 'x', 'value': x}},
        {'type': 'rel', 'data': {'axis': 'y', 'value': y}}]})

def button(down):
    command('input-send-event', {'events': [
        {'type': 'btn', 'data': {'button': 'left', 'down': down}}]})

with tempfile.TemporaryDirectory(prefix='cube-drag-') as directory:
    frame_path = Path(directory) / 'frame.ppm'
    def frame():
        command('screendump', {'filename': str(frame_path)})
        return read_qemu_frame(frame_path)

    time.sleep(1)
    move(1800, 960)
    time.sleep(.3)
    baseline = frame()
    click(815, 1020)
    time.sleep(1)
    for code in ('c', 'u', 'b', 'e', 'ret'):
        key(code)
    time.sleep(3)
    # 响应正常的 shell 现在会在立方体运行时显示子进程输出。
    # 短暂将其置顶，以捕获稳定后的客户区，而非启动前的文本。
    click(1100, 155)
    time.sleep(.5)
    terminal_baseline = frame()
    click(264, 63)
    click(1600, 155)  # Minimize the terminal created by this test.
    time.sleep(.5)
    x, y = 64, 48
    for number, path in enumerate(([(12, 5)] * 40, [(-12, 7)] * 35,
                                    [(15, -10)] * 25, [(-15, -7)] * 25)):
        move(x + 200, y + 15)
        button(True)
        try:
            for dx, dy in path:
                delta(dx, dy)
                x += dx
                y += dy
                time.sleep(.01)
        finally:
            button(False)
        move(1800, 960)
        time.sleep(.6)
        pixels = frame()
        changed = 0
        for row in range(980):
            # 忽略立方体当前边界（800x600 客户区，加上 2px/30px 窗框）。
            for left, right in ((0, max(0, x - 8)),
                                (min(1920, x + 810), 1920)) if y - 8 <= row < y + 640 else ((0, 1920),):
                if 935 <= row < 985:
                    segments = ((left, min(right, 1770)), (max(left, 1835), right))
                else:
                    segments = ((left, right),)
                for start, end in segments:
                    if start >= end:
                        continue
                    a = (row * 1920 + start) * 3
                    b = (row * 1920 + end) * 3
                    changed += sum(pixels[i:i+3] != baseline[i:i+3]
                                   for i in range(a, b, 3))
        command('screendump', {'filename': str(root / f'build/cube-drag-fixed-{number}.png'),
                              'format': 'png'})
        print(f'Drag {number}: position={x},{y}, stale_pixels={changed}', flush=True)
        assert changed == 0, 'Window trail or background restoration failure'

    click(815, 1020)  # Restore our terminal, then raise the exposed cube title.
    time.sleep(.5)
    click(x + 200, y + 15)
    for number, (dx, dy) in enumerate(((12, 6), (-12, -6))):
        move(x + 200, y + 15)
        button(True)
        try:
            for _ in range(30):
                delta(dx, dy)
                x += dx
                y += dy
                time.sleep(.01)
        finally:
            button(False)
        move(1800, 960)
        time.sleep(.6)
        pixels = frame()
        changed = 0
        # 排除当前立方体，比较稳定后的终端客户区像素。
        for row in range(400, 910):
            for col in range(250, 1670):
                if x - 8 <= col < x + 810 and y - 8 <= row < y + 640:
                    continue
                offset = (row * 1920 + col) * 3
                changed += pixels[offset:offset+3] != terminal_baseline[offset:offset+3]
        command('screendump', {'filename': str(root / f'build/cube-drag-overlap-{number}.png'),
                              'format': 'png'})
        print(f'Overlap {number}: position={x},{y}, stale_pixels={changed}', flush=True)
        assert changed == 0, 'Underlying terminal was not restored'

qmp.close()
print('Cube drag: four wallpaper paths and two terminal overlap paths passed')
