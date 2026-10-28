"""Shared, synchronous control of the single canonical QEMU; no import effects."""
import atexit
import json
from pathlib import Path
import socket
import time

ROOT = Path(__file__).resolve().parents[1]


class QMPClient:
    def __init__(self, path=None, timeout=10):
        self.sock = socket.socket(socket.AF_UNIX)
        self._buffer = bytearray()
        self._closed = False
        self._sequence = 0
        try:
            self.sock.settimeout(timeout)
            self.sock.connect(str(path or ROOT / 'build/qemu-qmp.sock'))
            timeout = self.sock.gettimeout()
            deadline = time.monotonic() + timeout if timeout is not None else None
            greeting = self._read(deadline)
            if not isinstance(greeting.get('QMP'), dict):
                raise RuntimeError('QMP greeting missing')
            self.sock.settimeout(timeout)
            self.command('qmp_capabilities')
        except BaseException:
            self.close()
            raise
        atexit.register(self.close)

    def _read(self, deadline):
        searched = 0
        while True:
            if deadline is not None:
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    raise TimeoutError('QMP response exceeded its deadline')
                self.sock.settimeout(remaining)
            newline = self._buffer.find(b'\n', searched)
            if newline >= 0:
                line = bytes(self._buffer[:newline])
                del self._buffer[:newline + 1]
                break
            searched = len(self._buffer)
            chunk = self.sock.recv(65536)
            if not chunk:
                raise EOFError('QEMU closed the QMP connection')
            self._buffer.extend(chunk)
        reply = json.loads(line)
        if not isinstance(reply, dict):
            raise ValueError('QMP response must be an object')
        return reply

    def command(self, name, arguments=None):
        if self._closed:
            raise RuntimeError('QMP connection is closed')
        self._sequence += 1
        identifier = self._sequence
        timeout = self.sock.gettimeout()
        deadline = time.monotonic() + timeout if timeout is not None else None
        try:
            self.sock.sendall((json.dumps({
                'execute': name, 'arguments': arguments or {},
                'id': identifier}) + '\n').encode())
            while True:
                if deadline is not None:
                    remaining = deadline - time.monotonic()
                    if remaining <= 0:
                        raise TimeoutError(f'QMP command {name!r} exceeded its deadline')
                    self.sock.settimeout(remaining)
                reply = self._read(deadline)
                if 'event' in reply:
                    continue
                if reply.get('id') != identifier:
                    raise ValueError('Unexpected QMP response identifier')
                if 'error' in reply:
                    raise QMPCommandError(reply)
                if 'return' not in reply:
                    raise ValueError('QMP response has no result')
                return reply['return']
        except QMPCommandError:
            # 命令被拒绝不会使传输连接失效。
            raise
        except BaseException:
            self.close()
            raise
        finally:
            if not self._closed:
                self.sock.settimeout(timeout)

    def close(self):
        if self._closed:
            return
        self._closed = True
        atexit.unregister(self.close)
        self._buffer.clear()
        self.sock.close()

    def __enter__(self):
        return self

    def __exit__(self, *_):
        self.close()

    def move(self, x, y):
        # QMP acknowledges host enqueueing, not guest HID consumption. Large
        # rapid bursts can merge across the homing/placement direction change
        # while Chrome is closing, so a later click misses its target. Keep
        # reports small and allow each to drain before the next report.
        for dx, dy in [(-2500, -1500), (x, y)]:
            while dx or dy:
                a = max(-80, min(80, dx))
                b = max(-80, min(80, dy))
                self.command('input-send-event', {'events': [
                    {'type': 'rel', 'data': {'axis': 'x', 'value': a}},
                    {'type': 'rel', 'data': {'axis': 'y', 'value': b}}]})
                dx -= a
                dy -= b
                time.sleep(.15)
            time.sleep(.3)

    def click(self, x, y, button='left'):
        self.move(x, y)
        for down in (True, False):
            self.command('input-send-event', {'events': [
                {'type': 'btn', 'data': {'button': button, 'down': down}}]})
            time.sleep(.04)
        time.sleep(.2)

    def key(self, *codes):
        for down in (True, False):
            self.command('input-send-event', {'events': [
                {'type': 'key', 'data': {
                    'down': down, 'key': {'type': 'qcode', 'data': code}}}
                for code in (codes if down else reversed(codes))]})
            time.sleep(.04)

    def screenshot(self, path, image_format='png'):
        return self.command('screendump', {
            'filename': str(Path(path).resolve()), 'format': image_format})


class QMPCommandError(RuntimeError):
    """Full response envelope, including the error fields used by GDB retries."""
