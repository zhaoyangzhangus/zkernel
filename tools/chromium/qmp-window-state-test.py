"""Real maximize/restore clicks in the single canonical QEMU.

Requires an inspected normal browser at (10,10), 945x1060 on 1920x1080.
Checks exposed desktop pixels rather than accepting a changed caption glyph
as proof of restored geometry. Leaves Chrome open after testing.
"""
import argparse
import ast
import json
from pathlib import Path
import tempfile
import time

parser = argparse.ArgumentParser()
parser.add_argument('--cycles', type=int, default=3)
parser.add_argument('--minimize', action='store_true',
                    help='Also verify native minimize and dock restore; requires updated Chrome')
parser.add_argument('--fullscreen', action='store_true',
                    help='Verify F11 restores both normal and maximized geometry')
args = parser.parse_args()
if not 1 <= args.cycles <= 10:
    parser.error('cycles must be 1..10')
root = Path(__file__).resolve().parents[2]
import sys
sys.path.insert(0, str(root / 'tools'))
from qmp_client import QMPClient
tree = ast.parse((root/'tools/chromium/qmp-startup-latency.py').read_text())
extent_function = next(node for node in tree.body if isinstance(node, ast.FunctionDef)
                       and node.name == 'browser_extent')
namespace = {}
exec(compile(ast.Module(body=[extent_function], type_ignores=[]), '<browser-extent>', 'exec'), namespace)
browser_extent = namespace['browser_extent']
qmp = QMPClient()

def command(name, arguments=None):
    return qmp.command(name, arguments)

def relative(dx, dy):
    while dx or dy:
        x, y = max(-100, min(100, dx)), max(-100, min(100, dy))
        command('input-send-event', {'events': [
            {'type': 'rel', 'data': {'axis': 'x', 'value': x}},
            {'type': 'rel', 'data': {'axis': 'y', 'value': y}}]})
        dx -= x
        dy -= y
        time.sleep(.05)

position = [0, 0]

def move(x, y):
    relative(x-position[0], y-position[1])
    position[:] = [x, y]

def click():
    for down in (True, False):
        command('input-send-event', {'events': [
            {'type': 'btn', 'data': {'button': 'left', 'down': down}}]})
        time.sleep(.04)

def capture(label):
    command('screendump', {'filename': str(root/f'build/chrome-window-state-{label}.png'),
                         'format': 'png'})

def desktop_roi(width, pixels):
    # 紧邻预期的普通窗口边缘；全宽的“普通”窗口（旧的实际恢复缺陷）必须无法通过此检查。
    return b''.join(pixels[(y*width+960)*3:(y*width+1200)*3]
                    for y in range(200, 700))

def difference(a, b):
    assert len(a) == len(b) and a
    return sum(abs(x-y) > 20 for x, y in zip(a, b))/len(a)


assert command('query-status')['running']
relative(-2000, -1200)
results = []
with tempfile.TemporaryDirectory(prefix='system-window-state-') as directory:
    frame_path = str(Path(directory)/'frame.ppm')

    def frame_info():
        command('screendump', {'filename': frame_path})
        with open(frame_path, 'rb') as image:
            assert image.readline().strip() == b'P6'
            width, height = map(int, image.readline().split())
            assert (width, height) == (1920, 1080)
            assert image.readline().strip() == b'255'
            pixels = image.read()
            assert len(pixels) == width*height*3
        focus_roi = b''.join(pixels[(y*width+140)*3:(y*width+350)*3]
                             for y in range(56, 145))
        return desktop_roi(width, pixels), browser_extent(width, pixels), focus_roi

    def frame():
        return frame_info()[0]

    baseline, original_extent, _ = frame_info()
    if original_extent is None or not (8 <= original_extent[0] <= 14 and
                                       950 <= original_extent[1] <= 958):
        capture('invalid-baseline')
        raise RuntimeError('Expected inspected normal browser geometry; do not click blindly')
    capture('baseline')

    def wait_changed(maximized, label):
        started = time.monotonic()
        while time.monotonic()-started < 5:
            changed = difference(baseline, frame())
            if (changed > .3 if maximized else changed < .005):
                capture(label)
                return {'paint_ms': (time.monotonic()-started)*1000,
                        'desktop_changed_fraction': changed}
            time.sleep(.025)
        capture(label+'-failure')
        raise RuntimeError(label+': native geometry did not return to expected desktop coverage')

    def fullscreen_roundtrip(was_maximized, label):
        def header():
            frame_info()
            with open(frame_path, 'rb') as image:
                image.readline()
                width, _ = map(int, image.readline().split())
                image.readline()
                pixels = image.read()
            return b''.join(pixels[(y*width+20)*3:(y*width+800)*3]
                            for y in range(12, 48))

        expected_header = header()

        def wait_header(entered):
            started = time.monotonic()
            while time.monotonic()-started < 5:
                changed = difference(expected_header, header())
                if (changed > .1 if entered else changed < .03):
                    return
                time.sleep(.025)
            capture(label+'-header-failure')
            raise RuntimeError('Fullscreen chrome header did not disappear/return')

        command('send-key', {'keys': [{'type': 'qcode', 'data': 'f11'}],
                             'hold-time': 100})
        wait_header(True)
        entered = wait_changed(True, label+'-entered')
        # Chrome 的全屏转换尚未结束时，覆盖率信息本身就会先发生变化。
        # 不要在转换保护期间注入下一个快捷键。
        time.sleep(2)
        capture(label+'-entered')
        command('send-key', {'keys': [{'type': 'qcode', 'data': 'f11'}],
                             'hold-time': 100})
        time.sleep(2)
        wait_header(False)
        exited = wait_changed(was_maximized, label+'-exited')
        return {'entered': entered, 'exited': exited}

    for cycle in range(1, args.cycles+1):
        fullscreen = {}
        if args.fullscreen:
            fullscreen['normal'] = fullscreen_roundtrip(False, f'{cycle}-normal-fullscreen')
        move(903, 30)
        click()
        maximized = wait_changed(True, f'{cycle}-maximized')
        if args.fullscreen:
            fullscreen['maximized'] = fullscreen_roundtrip(True, f'{cycle}-max-fullscreen')
        move(1868, 20)
        click()
        restored = wait_changed(False, f'{cycle}-restored')
        result = {'cycle': cycle, 'maximize': maximized, 'restore': restored}
        if args.fullscreen:
            result['fullscreen'] = fullscreen
        if args.minimize:
            move(870, 30)
            click()
            started = time.monotonic()
            while time.monotonic()-started < 5:
                _, extent, _ = frame_info()
                if extent is None:
                    break
                time.sleep(.025)
            else:
                capture(f'{cycle}-minimize-failure')
                raise RuntimeError('Minimize retained the native Chrome surface over the desktop')
            capture(f'{cycle}-minimized')
            result['minimize_ms'] = (time.monotonic()-started)*1000
            move(1200, 1020)
            click()
            started = time.monotonic()
            while time.monotonic()-started < 5:
                roi, extent, _ = frame_info()
                if extent == original_extent and difference(baseline, roi) < .005:
                    break
                time.sleep(.025)
            else:
                capture(f'{cycle}-dock-restore-failure')
                raise RuntimeError('Dock restore failed to repaint the original Chrome geometry')
            capture(f'{cycle}-dock-restored')
            result['dock_restore_ms'] = (time.monotonic()-started)*1000
            # 仅保留旧像素还不够：确认恢复后的合成器和键盘能够绘制真实的地址栏焦点状态。
            move(920, 400)
            click()
            time.sleep(.15)
            focus_before = frame_info()[2]
            command('send-key', {'keys': [{'type': 'qcode', 'data': code}
                                         for code in ('ctrl', 'l')],
                                 'hold-time': 100})
            started = time.monotonic()
            while time.monotonic()-started < 5:
                focused = frame_info()[2]
                changed = sum(abs(a-b) > 20 for a, b in zip(focus_before, focused))
                if changed > 200:
                    break
                time.sleep(.025)
            else:
                capture(f'{cycle}-restored-input-failure')
                raise RuntimeError('Restored window retained pixels but failed to paint keyboard focus')
            result['restored_focus_ms'] = (time.monotonic()-started)*1000
            capture(f'{cycle}-restored-input')
            command('send-key', {'keys': [{'type': 'qcode', 'data': 'esc'}],
                                 'hold-time': 80})
            time.sleep(.2)
        results.append(result)
        print(json.dumps(result), flush=True)
(root/'build/chrome-window-state-results.json').write_text(json.dumps(results, indent=2)+'\n')
