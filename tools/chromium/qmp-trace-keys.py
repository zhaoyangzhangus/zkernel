"""Send Ctrl+L, a, b to the existing guest under a bounded GDB trace."""
import argparse
from pathlib import Path
import signal
import subprocess
import time

parser = argparse.ArgumentParser()
parser.add_argument('--script', default='tools/chromium/trace-native-keys.gdb')
parser.add_argument('--kernel-elf', default='build/qemu-running-kernel.elf',
                    help='Symbols matching the kernel actually booted by QEMU')
parser.add_argument('--label', default='native-key-trace')
parser.add_argument('--new-tab', action='store_true',
                    help='Trace one Ctrl+T instead of editing the address field')
parser.add_argument('--fullscreen', action='store_true',
                    help='Trace three F11 presses, three seconds apart')
parser.add_argument('--observe-seconds', type=int, default=5,
                    help='Bounded debugger observation after input (1..60)')
parser.add_argument('--key-hold-ms', type=int, default=100,
                    help='QEMU key hold time (1..200); included in delivery diagnostics')
parser.add_argument('--click', nargs=2, type=int, metavar=('X', 'Y'),
                    help='Trace a calibrated guest mouse click instead of keys')
parser.add_argument('--mouse-hold-ms', type=int, default=100,
                    help='Mouse click hold time (1..200), separate from key hold')
args = parser.parse_args()
if not args.label.replace('-', '').isalnum():
    parser.error('invalid output label')
if not 1 <= args.observe_seconds <= 60:
    parser.error('observe-seconds must be 1..60')
if not 1 <= args.key_hold_ms <= 200:
    parser.error('key-hold-ms must be 1..200')
if not 1 <= args.mouse_hold_ms <= 200:
    parser.error('mouse-hold-ms must be 1..200')
if args.fullscreen and (args.new_tab or args.click):
    parser.error('fullscreen cannot be combined with new-tab or click')
if args.click and (args.new_tab or not 0 <= args.click[0] < 1920 or
                  not 0 <= args.click[1] < 1080):
    parser.error('click requires a 1920x1080 guest coordinate, without --new-tab')
root = Path(__file__).resolve().parents[2]
import sys
sys.path.insert(0, str(root / 'tools'))
from qmp_client import QMPClient
kernel_elf = Path(args.kernel_elf)
if not kernel_elf.is_absolute():
    kernel_elf = root / kernel_elf
if not kernel_elf.is_file():
    parser.error('kernel-elf does not exist')
qmp = QMPClient()

def command(name, arguments=None):
    deadline = time.monotonic() + 3
    while True:
        try:
            return command_once(name, arguments)
        except RuntimeError as error:
            reply = error.args[0] if error.args else {}
            # 断点可能会在组合键的两个成员之间短暂暂停客户机。被拒绝的事件尚未送达。
            if (name != 'input-send-event' or not isinstance(reply, dict) or
                    reply.get('error', {}).get('desc') != 'VM not running' or
                    time.monotonic() >= deadline):
                raise
            time.sleep(.02)

def command_once(name, arguments=None):
    return qmp.command(name, arguments)


if not command('query-status')['running']:
    raise RuntimeError('Guest already stopped; leave its debugger state alone')
process = None

def stop_debugger():
    if process is None or process.poll() is not None:
        return
    # 退出前先中断 `continue`，让 GDB 可以移除远程断点。运行中发送 SIGTERM 无法保证
    # 与目标正确分离。
    process.send_signal(signal.SIGINT)
    try:
        process.wait(timeout=3)
    except subprocess.TimeoutExpired:
        process.terminate()
        process.wait(timeout=3)

def recover_failed_debugger():
    if process is None or process.poll() is None:
        return
    # 批处理 GDB 在 `continue` 被中断后仍可能以零状态退出，同时保留硬件断点。仅凭退出码
    # 无法证明已分离。
    # 恢复运行前始终重置远程调试会话，并断开新建连接，包括看似成功的超时退出。
    command('human-monitor-command', {'command-line': 'gdbserver none'})
    command('human-monitor-command', {'command-line':
        'gdbserver unix:/tmp/system-keys.sock,server=on,wait=off'})
    subprocess.run(['gdb', '-q', '-nx', '-batch',
                    '-ex', 'target remote /tmp/system-keys.sock',
                    '-ex', 'detach'], cwd=root, check=True, timeout=10)

try:
    command('human-monitor-command', {'command-line':
        'gdbserver unix:/tmp/system-keys.sock,server=on,wait=off'})
    with (root / f'build/chrome-{args.label}.log').open('w') as log:
        process = subprocess.Popen(['gdb', '-q', '-nx', '-batch',
            str(kernel_elf), '-x', args.script], cwd=root,
            stdout=log, stderr=subprocess.STDOUT)
        time.sleep(1)
        for attempt in range(60):
            if command('query-status')['running']:
                break
            time.sleep(.1)
        else:
            raise RuntimeError('Debugger did not resume guest')
        sequence = [] if args.click else ([('f11',)] * 3 if args.fullscreen else
                                         [('ctrl', 't')] if args.new_tab else
                                         [('ctrl', 'l'), ('a',), ('b',)])
        if args.click:
            for dx, dy in [(-2000, -1200), tuple(args.click)]:
                while dx or dy:
                    x, y = max(-100, min(100, dx)), max(-100, min(100, dy))
                    command('input-send-event', {'events': [
                        {'type': 'rel', 'data': {'axis': 'x', 'value': x}},
                        {'type': 'rel', 'data': {'axis': 'y', 'value': y}}]})
                    dx, dy = dx - x, dy - y
                    time.sleep(.1)
            for down in (True, False):
                command('input-send-event', {'events': [
                    {'type': 'btn', 'data': {'button': 'left', 'down': down}}]})
                if down:
                    time.sleep(args.mouse_hold_ms/1000)
        for keys in sequence:
            command('send-key', {'keys': [
                {'type': 'qcode', 'data': key} for key in keys], 'hold-time': args.key_hold_ms})
            time.sleep(3 if args.fullscreen else .4)
        try:
            process.wait(timeout=args.observe_seconds)
        except subprocess.TimeoutExpired:
            stop_debugger()
finally:
    stop_debugger()
    try:
        recover_failed_debugger()
    finally:
        command('human-monitor-command', {'command-line': 'gdbserver none'})
        command('cont')
        # 组合键/点击只送达一部分时，不能让客户机中的按键保持按下。
        command('input-send-event', {'events': [
            {'type':'btn','data':{'button':'left','down':False}},
            *[{'type':'key','data':{'down':False,
               'key':{'type':'qcode','data':code}}}
              for code in ('ctrl','shift','alt','meta_l')]]})
        print(command('query-status'))
