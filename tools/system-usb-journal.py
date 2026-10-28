#!/usr/bin/env python3
"""Stream journal records to a mounted USB, rotate and fsync every second."""
import argparse
import datetime
import os
from pathlib import Path
import selectors
import signal
import subprocess
import time

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--mount', default='/run/media/zzy/CCDC-EFE7')
parser.add_argument('--user-only', action='store_true')
args = parser.parse_args()
mount = Path(args.mount).resolve()
if not os.path.ismount(mount):
    raise SystemExit('USB is not mounted; refusing to log onto the system disk')
if not args.user_only and os.geteuid() != 0:
    raise SystemExit('Full system/kernel journal capture requires root')
boot = Path('/proc/sys/kernel/random/boot_id').read_text().strip()
directory = mount / 'system-logs' / boot
directory.mkdir(parents=True, exist_ok=True)
prefix = 'user' if args.user_only else 'system'
stamp = datetime.datetime.now(datetime.timezone.utc).strftime('%Y%m%dT%H%M%SZ')
stem = prefix + '-' + stamp
sequence = 0
output = None
stopping = False


def stop(*_):
    global stopping
    stopping = True


def open_segment():
    global output, sequence
    if output is not None:
        os.fsync(output.fileno())
        output.close()
    sequence += 1
    output = (directory / f'{stem}-{sequence:05d}.jsonl').open('ab', buffering=0)
    # 每个作用域中，本次启动的日志流最多保留八个 32 MiB 分段。此处不删除其他启动目录，
    # 以便事后检查。
    segments = sorted(directory.glob(prefix + '-*.jsonl'))
    for old in segments[:-8]:
        old.unlink()


signal.signal(signal.SIGTERM, stop)
signal.signal(signal.SIGINT, stop)
command = ['journalctl', '--boot=0', '--follow', '--lines=all',
           '--output=json', '--no-pager', '--quiet']
if args.user_only:
    command.append('--user')
child = subprocess.Popen(command, stdout=subprocess.PIPE)
selector = selectors.DefaultSelector()
selector.register(child.stdout, selectors.EVENT_READ)
try:
    open_segment()
    print(f'USB_JOURNAL scope={prefix} directory={directory} fsync=1s', flush=True)
    synced = time.monotonic()
    while not stopping:
        if not os.path.ismount(mount):
            raise RuntimeError('USB mount disappeared; capture stopped')
        events = selector.select(timeout=1)
        if events:
            chunk = os.read(child.stdout.fileno(), 65536)
            if not chunk:
                raise RuntimeError(f'journalctl ended: {child.wait()}')
            # 仅在记录边界轮转，确保单条 JSON 行完整，即使读取将异常大的日志记录拆成多段也不例外。
            for piece in chunk.splitlines(keepends=True):
                output.write(piece)
                if piece.endswith(b'\n') and output.tell() >= 32 * 1024 * 1024:
                    open_segment()
        if time.monotonic() - synced >= 1:
            os.fsync(output.fileno())
            synced = time.monotonic()
finally:
    child.terminate()
    try:
        child.wait(timeout=3)
    except subprocess.TimeoutExpired:
        child.kill()
        child.wait()
    selector.close()
    if output is not None:
        os.fsync(output.fileno())
        output.close()
