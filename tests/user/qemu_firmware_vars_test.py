"""Exercise the launcher's split-firmware setup without launching QEMU."""
from pathlib import Path
import os
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'run-qemu.sh').read_text()
start = source.index('# Split OVMF images need')
end = source.index('# A non-interactive headless launch', start)
setup = source[start:end]
assert '"${firmware_vars_args[@]}"' in source
assert source.index('if ((prepare_only)); then') < start
assert source.index('flock -n') < start

with tempfile.TemporaryDirectory(prefix='qemu-vars-test-') as directory:
    fixture = Path(directory)
    build = fixture / 'build'
    build.mkdir()
    code = fixture / 'OVMF_CODE_4M.fd'
    template = fixture / 'OVMF_VARS_4M.fd'
    template.write_bytes(b'pristine-variable-flash')

    def run(firmware=code, override=''):
        env = dict(os.environ, firmware=str(firmware), build_dir=str(build),
                   OVMF_VARS_TEMPLATE=str(override))
        return subprocess.run(['bash', '-euc', setup +
            '\nprintf "%s\\n" "${firmware_vars_args[@]}"'],
            env=env, capture_output=True, text=True, timeout=10)

    result = run()
    assert result.returncode == 0, result.stderr
    store = build / 'OVMF_VARS.fd'
    assert store.read_bytes() == template.read_bytes()
    assert f'if=pflash,format=raw,file={store}' in result.stdout
    saved = b'X' * store.stat().st_size
    store.write_bytes(saved)
    assert run().returncode == 0
    assert store.read_bytes() == saved  # Firmware settings survive restart.
    assert run(fixture / 'OVMF_CODE_4M.secboot.fd').returncode == 0
    store.write_bytes(b'truncated')
    assert run().returncode != 0
    assert store.read_bytes() == b'truncated'
    store.unlink()
    template.unlink()
    assert run().returncode != 0
    assert not store.exists()
    # Custom combined images retain their existing code-only behavior.
    result = run(fixture / 'combined.fd')
    assert result.returncode == 0 and '-drive' not in result.stdout
    template.write_bytes(b'custom-vars')
    assert run(fixture / 'custom-code.fd', template).returncode == 0
    assert store.read_bytes() == b'custom-vars'
    store.unlink()
    legacy = fixture / 'OVMF_VARS.fd'
    legacy.write_bytes(b'legacy-vars')
    assert run(fixture / 'OVMF_CODE.fd').returncode == 0
    assert store.read_bytes() == b'legacy-vars'

print('QEMU firmware variables: paired flash, persistent settings, size checks, '
      'missing template, custom and legacy firmware passed')
