"""Long-path prompt and command mouse insertion in the canonical guest Shell."""
from pathlib import Path
import sys
import tempfile
import time

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools'))
from qmp_client import QMPClient
from qemu_frame import read_qemu_frame

with tempfile.TemporaryDirectory(prefix='shell-path-qa-') as directory, QMPClient() as q:
    assert q.command('query-status')['running']
    frame_path = Path(directory) / 'frame.ppm'

    def frame():
        q.command('screendump', {'filename': str(frame_path)})
        return read_qemu_frame(frame_path)

    def enter(text):
        for char in text:
            q.key({' ': 'spc', '/': 'slash', '-': 'minus'}.get(char, char))
        q.key('ret')
        time.sleep(.3)

    # 新建仅供客户机使用的目录，绝不覆盖用户文档或二进制文件。
    path = '/tmp/' + 'a' * 180 + str(time.monotonic_ns())
    assert len(path) < 240
    enter('mkdir ' + path)
    enter('cd ' + path)
    enter('clear')
    text = 'a' * 100 + 'xyz'
    for char in 'echo ' + text:
        q.key('spc' if char == ' ' else char)
    pixels = frame()
    cursor = [(x, y) for y in range(175, 195) for x in range(250, 1670)
              if pixels[(y * 1920 + x) * 3:(y * 1920 + x) * 3 + 3] == bytes((26, 115, 232))]
    assert cursor, 'No input cursor after the long path'
    x, y = min(cursor)
    q.click(x + 1, y + 8)
    q.key('b')
    q.key('ret')
    time.sleep(.3)
    actual = frame()
    q.command('screendump', {'filename': str(ROOT / 'build/shell-long-path.png'), 'format': 'png'})
    # 短路径下同一行的预期输出可作为像素基准。
    enter('cd /')
    enter('clear')
    enter('echo ' + text + 'b')
    expected = frame()
    def output_row(pixels, start=195):
        # 排除右边缘的指针以及其后的命令提示符行。
        return b''.join(pixels[(y * 1920 + 250) * 3:(y * 1920 + 1500) * 3]
                        for y in range(start, start + 17))
    assert output_row(expected) != bytes((255,)) * (1250 * 17 * 3), 'Expected echo output is blank'
    # 完整命令回显可能换行，因此结果可能出现在更多行之后。
    assert any(output_row(actual, y) == output_row(expected) for y in range(195, 400)), \
        'Long-path click inserted at a truncated offset'
    print('QEMU Shell long path: scrolled input, end-click insertion and exact output pixels PASS')
