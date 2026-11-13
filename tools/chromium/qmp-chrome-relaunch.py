"""Bounded real-X/dock relaunch trials in the sole QEMU; not a monitor."""
import argparse
import ast
import json
from pathlib import Path
import subprocess
import tempfile
import time

root = Path(__file__).resolve().parents[2]
import sys
sys.path.insert(0, str(root / 'tools'))
from qmp_client import QMPClient
parser = argparse.ArgumentParser()
parser.add_argument('--cycles', type=int, default=3, choices=range(1, 11))
parser.add_argument('--close-method', choices=('x', 'hotkey'), default='x')
args = parser.parse_args()
# 使用现有的已检查工具栏门禁，不导入其 CLI 副作用。
tree = ast.parse((root / 'tools/chromium/qmp-startup-latency.py').read_text())
scope = {}
node = next(n for n in tree.body if isinstance(n, ast.FunctionDef) and n.name == 'browser_extent')
exec(compile(ast.Module(body=[node], type_ignores=[]), '<toolbar gate>', 'exec'), scope)
extent = scope['browser_extent']
subprocess.run(['cc', '-g3', '-fno-eliminate-unused-debug-types', '-ffreestanding',
                '-DOS_KERNEL_BUILD', '-Iinclude', '-c',
                'tools/chromium/inspection-types.c', '-o',
                'build/current-inspection-types.o'], cwd=root, check=True)

qmp = QMPClient()

def command(name, arguments=None):
    return qmp.command(name, arguments)

def event(events):
    command('input-send-event', {'events': events})

def move(dx, dy):
    # USB 相对位移数据包需要间隔轮询；批量处理会丢失移动量。
    while dx or dy:
        x, y = max(-100, min(100, dx)), max(-100, min(100, dy))
        event([{'type': 'rel', 'data': {'axis': 'x', 'value': x}},
               {'type': 'rel', 'data': {'axis': 'y', 'value': y}}])
        dx, dy = dx-x, dy-y
        time.sleep(.03)

def click():
    event([{'type': 'btn', 'data': {'button': 'left', 'down': True}}])
    time.sleep(.02)
    event([{'type': 'btn', 'data': {'button': 'left', 'down': False}}])

def snapshot(old_pid=0):
    code = r'''
import gdb,json
server=gdb.parse_and_eval('g_window_server')
guard=int(server['desktop_chrome_launch_pid'])
owners=[int(server['windows'][i].dereference()['owner'].dereference()['pid'])
        for i in range(int(server['count']))]
head=gdb.parse_and_eval('&g_process_registry')
offset=int(gdb.parse_and_eval('(unsigned long)&((process_t*)0)->global_node'))
node=head.dereference()['next'];seen=set();old_threads=None
while int(node)!=int(head) and int(node) not in seen and len(seen)<256:
    seen.add(int(node))
    process=gdb.Value(int(node)-offset).cast(gdb.lookup_type('process_t').pointer()).dereference()
    if int(process['pid'])==OLD_PID:old_threads=int(process['thread_count'])
    node=process['global_node']['next']
print('RELAUNCH_STATE '+json.dumps({'guard_pid':guard,'window_owner_pids':owners,
    'pending':bool(server['desktop_chrome_relaunch_pending']),
    'closing_pid':int(server['desktop_chrome_closing_pid']), 'old_threads':old_threads}))
'''.replace('OLD_PID', str(old_pid))
    command('human-monitor-command', {'command-line':
        'gdbserver unix:/tmp/system-relaunch.sock,server=on,wait=off'})
    try:
        result = subprocess.run(['gdb', '-q', '-nx', '-batch',
            str(root / 'build/qemu-running-kernel.elf'),
            '-ex', 'add-symbol-file build/current-inspection-types.o 0',
            '-ex', 'target remote /tmp/system-relaunch.sock',
            '-ex', 'python exec(' + repr(code) + ')', '-ex', 'detach'],
            cwd=root, text=True, capture_output=True, timeout=15, check=True)
        rows = [json.loads(line.removeprefix('RELAUNCH_STATE '))
                for line in result.stdout.splitlines() if line.startswith('RELAUNCH_STATE ')]
        if len(rows) != 1:
            raise RuntimeError(result.stdout + result.stderr)
        return rows[0]
    finally:
        command('human-monitor-command', {'command-line': 'gdbserver none'})
        command('cont')
        assert command('query-status')['running']


results = []
try:
    with tempfile.TemporaryDirectory(prefix='system-relaunch-') as directory:
        frame_path = Path(directory) / 'frame.ppm'
        def frame():
            command('screendump', {'filename': str(frame_path)})
            with frame_path.open('rb') as f:
                assert f.readline().strip() == b'P6'
                width, height = map(int, f.readline().split())
                assert f.readline().strip() == b'255'
                assert (width, height) == (1920, 1080)
                return width, f.read()

        for cycle in range(args.cycles):
            before = snapshot()
            width, pixels = frame()
            bounds = extent(width, pixels)
            if bounds is None or not (8 <= bounds[0] <= 14 and 950 <= bounds[1] <= 958):
                raise RuntimeError('Require inspected normal Chrome geometry; refusing blind close')
            if not before['guard_pid'] or before['guard_pid'] not in before['window_owner_pids']:
                raise RuntimeError('Visible Chrome has no owning launch guard')
            move(-2000, -1200)
            time.sleep(.05)
            move(935, 30) if args.close_method == 'x' else move(1200, 1020)
            time.sleep(.05)
            if args.close_method == 'x':
                click()  # Real Chrome X, not window-server forced removal.
            else:
                keys = ('ctrl', 'shift', 'w')
                event([{'type': 'key', 'data': {'down': True,
                       'key': {'type': 'qcode', 'data': key}}} for key in keys])
                time.sleep(.02)
                event([{'type': 'key', 'data': {'down': False,
                       'key': {'type': 'qcode', 'data': key}}} for key in reversed(keys)])
            deadline = time.monotonic()+10
            while extent(*frame()) is not None:
                if time.monotonic() > deadline:
                    raise RuntimeError('Chrome X did not remove the visible window')
                time.sleep(.01)
            closed_at = time.monotonic()
            if args.close_method == 'x':
                move(265, 990)
            time.sleep(.02)
            click()
            clicked_at = time.monotonic()
            deadline = clicked_at+30
            while extent(*frame()) is None:
                if time.monotonic() > deadline:
                    raise RuntimeError('Lost dock relaunch: no Chrome window within 30 seconds')
                time.sleep(.05)
            time.sleep(1)
            after = snapshot(before['guard_pid'])
            if (after['guard_pid'] == before['guard_pid'] or
                after['guard_pid'] not in after['window_owner_pids'] or
                after['pending'] or after['closing_pid'] or
                after['old_threads'] not in (None, 0)):
                raise RuntimeError({'before': before, 'after': after})
            result = {'cycle': cycle+1, 'before': before, 'after': after,
                      'visible_close_to_dock_click_ms': (clicked_at-closed_at)*1000,
                      'close_method': args.close_method,
                      'verification': 'Chrome close, dock click, new guarded window, old process has no live threads'}
            results.append(result)
            print(json.dumps(result), flush=True)
            command('screendump', {'filename': str(root / 'build/chrome-relaunch.png'), 'format': 'png'})
finally:
    event([{'type': 'key', 'data': {'down': False,
           'key': {'type': 'qcode', 'data': key}}} for key in ('w', 'shift', 'ctrl')])
    event([{'type': 'btn', 'data': {'button': 'left', 'down': False}}])
    (root / ('build/chrome-relaunch-' + args.close_method + '.json')).write_text(json.dumps(results, indent=2)+'\n')
    qmp.close()
