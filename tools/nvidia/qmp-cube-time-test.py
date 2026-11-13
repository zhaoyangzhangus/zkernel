"""Read the actual cube overlay and compare its clock to host elapsed time.

Run after qmp-cube-drag-test.py; no input, restart or guest modification.
"""
from pathlib import Path
import re
import sys
import tempfile
import time

root = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(root / 'tools'))
from qmp_client import QMPClient
from qemu_frame import read_qemu_frame

source = (root / 'tools/nvidia/opengl_cube_demo.c').read_text()
digits = re.search(r'digits\[10\]\[7\] = \{(.*?)\};', source, re.S).group(1)
values = [int(n, 16) for n in re.findall(r'0x[0-9A-Fa-f]+', digits)]
patterns = {tuple(values[i * 7:(i + 1) * 7]): str(i) for i in range(10)}
for character, rows in re.findall(r"case '(.)': \{ static const uint8_t glyph\[7\] =\s*\{([^}]+)\}", source):
    patterns[tuple(int(n, 16) for n in re.findall(r'0x[0-9A-Fa-f]+', rows))] = character
patterns[(0,) * 7] = ' '

def decode(pixels):
    # 立方体拖拽回归测试将外窗留在 (124,68)，客户区偏移为 +1,+31。
    for origin_y in range(100, 105):
        for origin_x in range(127, 132):
            text = ''
            for index in range(40):
                rows = []
                for row in range(7):
                    bits = 0
                    for column in range(5):
                        x, y = origin_x + index * 12 + column * 2, origin_y + row * 2
                        offset = (y * 1920 + x) * 3
                        if pixels[offset:offset + 3] == b'\xf0\xf0\xf0':
                            bits |= 1 << (4 - column)
                    rows.append(bits)
                if tuple(rows) not in patterns:
                    break
                text += patterns[tuple(rows)]
                match = re.fullmatch(r'FPS:(\d+) TIME:(\d+\.\d)S', text)
                if match:
                    return text, float(match.group(2))
    raise AssertionError('Actual foreground cube overlay could not be decoded')

with QMPClient() as qmp, tempfile.TemporaryDirectory(prefix='cube-clock-') as directory:
    filename = str(Path(directory) / 'frame.ppm')
    def sample():
        qmp.command('screendump', {'filename': filename})
        timestamp = time.monotonic()
        return timestamp, decode(read_qemu_frame(filename))
    assert qmp.command('query-status')['running']
    start, (first, guest_start) = sample()
    time.sleep(3)
    finish, (last, guest_finish) = sample()
    wall, guest = finish - start, guest_finish - guest_start
    print(f'{first} -> {last}; host elapsed={wall:.3f}s, displayed elapsed={guest:.1f}s', flush=True)
    assert abs(guest - wall) < .5, 'Guest cube elapsed clock differs from wall time'
print('Cube TIME is elapsed rendering runtime in seconds, not frame latency; live clock rate verified')
