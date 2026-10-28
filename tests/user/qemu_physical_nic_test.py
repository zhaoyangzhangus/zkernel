"""Execute actual canonical launcher's emulated/physical network arguments."""
from pathlib import Path
import subprocess

root = Path(__file__).resolve().parents[2]
source = (root / 'run-qemu.sh').read_text()
assert '\nemulated_net=0\n' in source
assert 'physical_nic="${QEMU_PHYSICAL_NIC:-0000:07:00.0}"' in source
selection = source[source.index('# Canonical physical network selection.'):
                   source.index('# Chrome maps large read-only assets')]
for enabled, nic, existing, expected in [
    (0, '0000:07:00.0', [], ['0000:07:00.0']),
    (0, '07:00.0', ['0000:01:00.0'], ['0000:01:00.0', '0000:07:00.0']),
    (0, '0000:07:00.0', ['07:00.0'], ['07:00.0']),
    (0, '0000:0A:00.0', ['0000:0a:00.0'], ['0000:0a:00.0']),
    (1, 'invalid', [], []),
]:
    script = (f'emulated_net={enabled}\nphysical_nic={nic}\nvfio_devices=(' +
              ' '.join(existing) + ')\n' + selection +
              '\nfor device in "${vfio_devices[@]}"; do printf "%s\\0" "$device"; done\n')
    result = subprocess.run(['bash', '-eu', '-c', script], check=True,
                            capture_output=True).stdout.split(b'\0')[:-1]
    assert result == [x.encode() for x in expected], result
bad_nic = subprocess.run(['bash', '-eu', '-c',
    'emulated_net=0; physical_nic=invalid; vfio_devices=()\n' + selection],
    capture_output=True)
assert bad_nic.returncode == 2 and b'QEMU_PHYSICAL_NIC' in bad_nic.stderr
start = source.index('if ((emulated_net)); then')
end = source.index('if [[ -n "$net_dump" ]]; then', start)
branch = source[start:end]
for enabled in (0, 1):
    script = (f'emulated_net={enabled}\nqemu_args=()\n' + branch +
              '\nprintf "%s\\0" "${qemu_args[@]}"\n')
    result = subprocess.run(['bash', '-c', script], check=True,
                            capture_output=True).stdout.split(b'\0')[:-1]
    expected = ([b'-netdev', b'user,id=vm-net,host=10.0.2.2', b'-device',
                 b'e1000e,netdev=vm-net,mac=52:54:00:12:34:56'] if enabled
                else [b'-nic', b'none'])
    assert result == expected, result
bad = subprocess.run([str(root / 'run-qemu.sh'), '--no-emulated-net',
                      '--net-dump', '/tmp/unused-physical-nic-test.pcap',
                      '--no-build', '--prepare-only'], capture_output=True)
assert bad.returncode == 2
assert b'--net-dump' in bad.stderr and b'--no-emulated-net' in bad.stderr
print('Canonical QEMU: physical NIC default, normalized/deduplicated selection, explicit emulation, invalid NIC and incompatible capture rejection passed')

start = source.index('for vfio_device in "${vfio_devices[@]}"; do')
end = source.index('\nif [[ -n "$qemu_trace" ]]', start)
for nic in ('0000:07:00.0', '0000:0A:00.0', '07:00.0'):
    script = ('physical_nic=' + nic + '\nvfio_primary=""\n'
              'vfio_devices=(0000:01:00.0 ' + nic.lower() + ')\nqemu_args=()\n' +
              source[start:end] + '\nprintf "%s\\0" "${qemu_args[@]}"\n')
    result = subprocess.run(['bash', '-eu', '-c', script], check=True,
                            capture_output=True).stdout.split(b'\0')[:-1]
    assert result == [b'-device', b'vfio-pci,host=0000:01:00.0', b'-device',
                      ('vfio-pci,host=' + nic.lower() + ',rombar=0').encode()]
print('VFIO NIC: network expansion ROM disabled, GPU options unchanged passed')
