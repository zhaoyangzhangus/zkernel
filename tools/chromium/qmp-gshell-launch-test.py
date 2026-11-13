"""Fresh canonical desktop: repeated successful program launches in one shell."""
import argparse
from pathlib import Path
import sys
import tempfile
import time

root = Path(__file__).resolve().parents[2]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--background', action='store_true')
parser.add_argument('--count', type=int, default=20)
args = parser.parse_args()
if not 1 <= args.count <= 256:
    parser.error('--count must be between 1 and 256')
sys.path.insert(0, str(root / 'tools'))
from qmp_client import QMPClient
from qemu_frame import read_qemu_frame
qmp = QMPClient()
command, move, click, key = qmp.command, qmp.move, qmp.click, qmp.key
assert command('query-status')['running']

def enter(text):
    for character in text:
        if character == ' ':
            key('spc')
        elif character == '-':
            key('minus')
        elif character == '&':
            key('shift', '7')
        elif character.isupper():
            key('shift', character.lower())
        else:
            key(character)
    key('ret')
    time.sleep(.5)

def launch_and_verify(number, success, background):
    # 与上次启动相同的状态不能证明有进展。再次接受它之前，必须先出现不同且可见的输出。
    assert lines()[-2] == success, f'Launch {number}: initial status missing'
    enter(f'echo LAUNCH PROBE {number}')
    assert lines()[-2] != success, f'Launch {number}: probe did not change the output'
    enter('nasm -v &' if background else 'nasm -v')
    assert lines()[-2] == success, f'Launch {number}: expected status missing after probe'

with tempfile.TemporaryDirectory(prefix='system-shell-launch-') as directory:
    filename = str(Path(directory) / 'frame.ppm')
    def lines():
        move(1800, 960)
        command('screendump', {'filename': filename})
        pixels = read_qemu_frame(filename)
        bands, band = [], bytearray()
        white = b'\xff' * (850 * 3)
        for y in range(175, 918):
            start = (y * 1920 + 250) * 3
            row = pixels[start:start + len(white)]
            if row != white:
                band.extend(row)
            elif band:
                bands.append(bytes(band))
                band.clear()
        if band:
            bands.append(bytes(band))
        assert len(bands) >= 2
        return bands
    click(815, 1020)
    time.sleep(.8)
    # 使用相同的客户机字体绘制已知成功标签，不使用 OCR 或宿主图像库。
    # 后续真实子进程的退出结果必须与此文本完全一致。
    label = 'PROGRAM STARTED IN BACKGROUND' if args.background else 'PROGRAM EXIT STATUS 0'
    enter('echo ' + label)
    success = lines()[-2]
    serial_before = (root / 'build/qemu-serial.log').read_text(errors='replace').count('NASM version')
    for number in range(args.count):
        launch_and_verify(number + 1, success, args.background)
        print(f'{"Background" if args.background else "Foreground"} launch {number + 1}/{args.count}: {label}', flush=True)
    if args.background:
        # 仅创建进程还不够：验证所有请求的子进程确实使用快照参数向量 exec 了 NASM。
        serial_after = (root / 'build/qemu-serial.log').read_text(errors='replace').count('NASM version')
        assert serial_after - serial_before >= args.count, 'Background child exec/output missing'
    mode = 'background' if args.background else 'launch'
    name = f'shell-{mode}-{args.count}-verified.png'
    command('screendump', {'filename': str(root / 'build' / name),
                          'format': 'png'})
    click(1664, 155)  # Close only our idle, document-free test shell.
    assert command('query-status')['running']
qmp.close()
