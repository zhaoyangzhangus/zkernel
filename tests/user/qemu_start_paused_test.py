"""Check the real launcher pause branch without starting a VM or TCP listener."""
from pathlib import Path
import subprocess

root = Path(__file__).resolve().parents[2]
source = (root / 'run-qemu.sh').read_text()
assert 'OS_INTERACTIVE_DESKTOP ?= 1' in (root / 'makefile').read_text()
assert 'OS_INTERACTIVE_DESKTOP="${OS_INTERACTIVE_DESKTOP:-1}"' in source
assert 'start_paused=0' in source
assert '--start-paused) start_paused=1; shift ;;' in source
start = source.index('if ((debug)); then qemu_args+=')
end = source.index('\nfor vfio_device', start)
branch = source[start:end]
for debug in (0, 1):
    for paused in (0, 1):
        script = f'set -eu\ndebug={debug};start_paused={paused};gdb_port=1234;qemu_args=()\n'
        script += branch + '\nprintf "%s\\0" "${qemu_args[@]}"'
        args = subprocess.check_output(['bash', '-c', script]).decode().rstrip('\0').split('\0')
        assert args.count('-S') == int(bool(debug or paused))
        assert ('-gdb' in args) == bool(debug)
print('QEMU start-paused: four debug/pause combinations, no duplicate pause or implicit GDB listener PASS')

# 执行真实参数构造，不启动虚拟机；常驻桌面不能把来宾重启截成停止状态。
start = source.index('qemu_args=(')
end = source.index('\nif ((stdvga)); then', start)
for keep_open in (0, 1):
    script = '''set -eu
qemu_machine=q35; qemu_memory=4096M; qemu_cpu_model=qemu64; cpu_count=4
firmware=/firmware; firmware_vars_args=(); serial_device=stdio; build_dir=/tmp
'''
    script += f'keep_open={keep_open}\n' + source[start:end]
    script += '\nprintf "%s\\0" "${qemu_args[@]}"'
    args = subprocess.check_output(['bash', '-c', script]).decode().rstrip('\0').split('\0')
    assert args.count('-no-reboot') == 1 - keep_open
    assert args.count('-no-shutdown') == 1 - keep_open
print('QEMU lifecycle: persistent desktop honors guest reboot/poweroff; bounded diagnostics retain stop state PASS')
