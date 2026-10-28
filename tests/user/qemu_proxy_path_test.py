"""Exercise the real launcher verifier without booting another guest."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
launcher = (root/'run-qemu.sh').read_text()
verifier = root/'tools/verify-qemu-network.sh'
alias = '-netdev "user,id=vm-net,host=10.0.2.2"'
assert launcher.count(alias) == 1
assert 'command -v socat' not in launcher
with tempfile.TemporaryDirectory(prefix='system-qemu-proxy-path-') as directory:
    fixture = Path(directory)/'run-qemu.sh'
    for label,text,expected in [
        ('actual launcher',launcher,True),
        ('NAT default',launcher.replace('emulated_net=0\n', 'emulated_net=1\n', 1),False),
        ('physical NIC omitted',launcher.replace('vfio_devices+=("$physical_nic")', ':'),False),
        ('implicit NIC enabled',launcher.replace('qemu_args+=( -nic none )', ':'),False),
        ('missing alias',launcher.replace(alias,'-netdev "user,id=vm-net"'),False),
        ('old relay',launcher.replace(alias,
            '-netdev "user,id=vm-net,guestfwd=tcp:10.0.2.100:7890-cmd:socat - TCP4:127.0.0.1:7890"'),False),
        ('network disabled',launcher+'\n -net none\n',False),
        ('relay reintroduced',launcher+'\n guestfwd=tcp:10.0.2.100:7890-cmd:socat\n',False),
    ]:
        fixture.write_text(text)
        result = subprocess.run(['bash',str(verifier),str(fixture)],
                                capture_output=True,text=True)
        assert (result.returncode == 0) == expected, (label,result.stdout,result.stderr)
print('QEMU proxy path: actual launcher accepted; missing alias, subprocess relay and disabled network rejected')
