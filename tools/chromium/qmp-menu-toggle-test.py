"""Two real anchor clicks must dismiss the menu and release its highlight.

Canonical 1920x1080 guest, unmaximized Chrome at 10,10 with width 945.
The pointer moves away after the second click without any further click.
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
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--cycles', type=int, default=5)
parser.add_argument('--dismiss', choices=('anchor', 'outside', 'escape'), default='anchor')
args = parser.parse_args()
assert 1 <= args.cycles <= 20
tree = ast.parse((root/'tools/chromium/qmp-app-menu-test.py').read_text())
functions = [node for node in tree.body if isinstance(node, ast.FunctionDef)
             and node.name == 'menu_is_painted']
namespace = {}
exec(compile(ast.Module(body=functions, type_ignores=[]), 'menu-gate', 'exec'), namespace)
qmp = QMPClient()

def command(name, arguments=None):
    return qmp.command(name, arguments)

def move(x, y):
    for dx, dy in [(-2000, -1200), (x, y)]:
        while dx or dy:
            a, b = max(-100, min(100, dx)), max(-100, min(100, dy))
            command('input-send-event', {'events': [
                {'type': 'rel', 'data': {'axis': 'x', 'value': a}},
                {'type': 'rel', 'data': {'axis': 'y', 'value': b}}]})
            dx -= a
            dy -= b
            time.sleep(.015)

def click():
    for down in (True, False):
        command('input-send-event', {'events': [
            {'type': 'btn', 'data': {'button': 'left', 'down': down}}]})
        time.sleep(.025)

def png(label):
    command('screendump', {'filename': str(root/f'build/chrome-menu-toggle-{label}.png'),
                           'format': 'png'})


assert command('query-status')['running']
try:
    command('send-key', {'keys': [{'type': 'qcode', 'data': 'esc'}], 'hold-time': 40})
    # 建立参考状态前先修复先前失败运行遗留的悬停状态，然后确认悬停后的画面确实与基线不同。
    move(928, 72)
    time.sleep(.3)
    move(450, 125)
    time.sleep(.3)
    with tempfile.TemporaryDirectory(prefix='system-menu-toggle-') as directory:
        path = str(Path(directory)/'frame.ppm')

        def frame():
            command('screendump', {'filename': path})
            with open(path, 'rb') as file:
                assert file.readline().strip() == b'P6'
                width, height = map(int, file.readline().split())
                assert (width, height) == (1920, 1080)
                assert file.readline().strip() == b'255'
                pixels = file.read()
            button = b''.join(pixels[(y*width+910)*3:(y*width+946)*3]
                              for y in range(55, 91))
            panel = b''.join(pixels[(y*width+600)*3:(y*width+900)*3]
                             for y in range(200, 650))
            painted = namespace['menu_is_painted'](width, pixels)
            # Google 页面图案可能满足绝对墨迹/白色门槛。
            # 还要确认面板相对于关闭菜单时的页面发生了变化。
            visible = (painted and panel_reference is not None and
                       sum(abs(a-b) > 20 for a, b in zip(panel, panel_reference)) > 3000)
            return button, visible, panel

        def difference(a, b):
            return sum(abs(x-y) > 8 for x, y in zip(a, b))/len(a)

        panel_reference = None
        baseline, _, panel_reference = frame()
        move(928, 72)
        time.sleep(.3)
        assert difference(baseline, frame()[0]) > .05, 'Reference is already highlighted'
        for cycle in range(args.cycles):
            if cycle:
                move(928, 72)
            click()
            start = time.monotonic()
            while not frame()[1]:
                assert time.monotonic()-start < 3, 'First click failed to open menu'
                time.sleep(.03)
            # 锚点模式下，两次点击之间不允许发生其他指针移动。
            # 其他模式会验证外部点击和 Escape 是否执行相同清理。
            if args.dismiss == 'anchor':
                click()
            elif args.dismiss == 'outside':
                move(450, 125)
                click()
            else:
                command('send-key', {'keys': [{'type': 'qcode', 'data': 'esc'}],
                                     'hold-time': 40})
            time.sleep(.3)
            assert not frame()[1], f'{args.dismiss} failed to close menu'
            move(450, 125)
            start = time.monotonic()
            while True:
                current, visible, _ = frame()
                delta = difference(baseline, current)
                if not visible and delta < .01:
                    break
                if time.monotonic()-start > 2:
                    png('failure')
                    raise RuntimeError(f'Stale anchor highlight after second click: {delta:.3f}')
                time.sleep(.03)
            png('passed')
            print(json.dumps({'cycle': cycle+1, 'menu_closed': True,
                              'dismiss': args.dismiss,
                              'button_difference': delta}), flush=True)
finally:
    command('input-send-event', {'events': [
        {'type': 'btn', 'data': {'button': 'left', 'down': False}}]})
    command('send-key', {'keys': [{'type': 'qcode', 'data': 'esc'}], 'hold-time': 40})
    qmp.close()
