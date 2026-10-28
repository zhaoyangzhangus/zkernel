"""Linux reference for the exact probe deployed into the canonical QEMU."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
with tempfile.TemporaryDirectory(prefix='tcp-wake-reference-') as directory:
    binary = Path(directory) / 'probe'
    subprocess.run(['cc', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                    str(root/'tests/user/tcp_loopback_wake_probe.c'),
                    '-o', str(binary)], check=True)
    result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=15)
    assert result.returncode == 0, result.stdout + result.stderr
    for marker in ('TCP_WAKE_IPV4_PASS', 'TCP_WAKE_IPV6_PASS', 'TCP_LOOPBACK_WAKE_PASS'):
        assert marker in result.stdout, result.stdout
    print(result.stdout, end='')
    output = Path(directory) / 'guest-style-output.log'
    redirected = subprocess.run([str(binary), str(output)], capture_output=True,
                                text=True, timeout=15)
    assert redirected.returncode == 0, output.read_text()
    assert not redirected.stdout and not redirected.stderr
    assert 'TCP_LOOPBACK_WAKE_PASS' in output.read_text()
    assert subprocess.run([str(binary), directory], timeout=5).returncode == 2
