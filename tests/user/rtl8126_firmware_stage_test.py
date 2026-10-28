"""Validate firmware staging, idempotence and failure before publication."""
import importlib.util
from pathlib import Path
import struct
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
script = root / 'tools/stage-rtl8126-firmware.py'
spec = importlib.util.spec_from_file_location('firmware_stage', script)
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)
with tempfile.TemporaryDirectory(prefix='rtl8126-stage-') as directory:
    base = Path(directory)
    source = base / 'source'
    source.mkdir()
    data = bytearray(48)
    struct.pack_into('<II', data, 36, 44, 1)
    data[47] = (-sum(data)) % 256
    names = ('rtl8126a-2.fw', 'rtl8126a-3.fw')
    for name in names:
        (source / name).write_bytes(data)
    command = ['python3', str(script), '--build', str(base / 'build'),
               '--source', str(source)]
    subprocess.run(command, check=True, capture_output=True)
    targets = [base / 'build/esp/EFI/OS/firmware/rtl8126' / name for name in names]
    stamps = [path.stat().st_mtime_ns for path in targets]
    subprocess.run(command, check=True, capture_output=True)
    assert stamps == [path.stat().st_mtime_ns for path in targets]
    # A valid changed first file must not publish if the second is corrupt.
    changed = bytearray(data)
    changed[4], changed[5] = 1, 255
    (source / names[0]).write_bytes(changed)
    (source / names[1]).write_bytes(b'corrupt')
    assert subprocess.run(command, capture_output=True).returncode != 0
    assert all(path.read_bytes() == data for path in targets)
    for name in names:
        (source / name).unlink()
    subprocess.run(command, check=True, capture_output=True)  # staged fallback
    for invalid in (b'', bytes(45), data[:44]):
        try:
            module.validate(invalid)
        except ValueError:
            pass
        else:
            raise AssertionError('accepted malformed firmware')
print('RTL8126 staging: format/checksum, unchanged timestamps, staged fallback, no partial publication PASS')
