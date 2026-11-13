"""Evaluate real launcher topology branches without opening a second VM."""
from pathlib import Path
import re
import subprocess

ROOT = Path(__file__).resolve().parents[2]
source = (ROOT / 'run-qemu.sh').read_text()
storage = source[source.index('if ((usb_storage)); then', source.index('qemu_args=(')):
                 source.index('if [[ -n "$monitor_socket" ]]; then')]
inputs = source[source.index('if ((usb_nested)); then', source.index('qemu_args=(')):
                source.index('if ((debug)); then qemu_args+=')]

def verify(args):
    ids = set()
    drives = set()
    for kind, value in zip(args[::2], args[1::2]):
        options = dict(pair.split('=', 1) for pair in value.split(',') if '=' in pair)
        if 'id' in options:
            assert options['id'] not in ids, options['id']
            ids.add(options['id'])
            if kind == '-drive':
                drives.add(options['id'])
    for value in args[1::2]:
        options = dict(pair.split('=', 1) for pair in value.split(',') if '=' in pair)
        if 'drive' in options:
            assert options['drive'] in drives, value
        if 'bus' in options:
            assert options['bus'].split('.')[0] in ids, value
        assert not re.search(r'\bliteos-', value), value

for usb, nested, all_disks in ((0, 0, 0), (0, 1, 0), (1, 0, 0), (1, 0, 1)):
    script = f'''set -eu
usb_storage={usb}; usb_nested={nested}; all_disks={all_disks}
qemu_fat_image=/fixture/root.img; qemu_secondary_image=/fixture/data.img
qemu_args=()
''' + storage + inputs + '\nprintf "%s\\0" "${qemu_args[@]}"\n'
    args = subprocess.check_output(['bash', '-c', script]).decode().rstrip('\0').split('\0')
    verify(args)
    controllers = [value for kind, value in zip(args[::2], args[1::2])
                   if kind == '-device' and value.split(',')[0] == 'qemu-xhci']
    assert len(controllers) == 1
    options = dict(pair.split('=', 1) for pair in controllers[0].split(',')[1:])
    assert options['id'] == 'vm-xhci' and options['msix'] == 'on'
    assert options['p2'] == '8' and options['p3'] == '8'
    if nested:
        assert 'usb-mouse,id=vm-nested-mouse,bus=vm-xhci.0,port=1.1.1' in args
    damaged = [value.replace('drive=vm-', 'drive=missing-') for value in args]
    try:
        verify(damaged)
    except AssertionError:
        pass
    else:
        raise AssertionError('Broken storage reference accepted')
windows = (ROOT / 'tools/run-realtest-qemu.ps1').read_text()
assert not re.search(r'(?:id|bus|drive|serial)=liteos-', windows)
assert 'device_del vm-usb-boot' in windows
print('QEMU device names: four real topology branches, unique controllers and broken-reference rejection passed')
