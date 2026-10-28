#!/usr/bin/env python3
"""Stage local RTL8126 firmware, validating both files before publication."""
import argparse
import os
from pathlib import Path
import struct
import tempfile


def validate(data):
    if len(data) < 45 or struct.unpack_from('<I', data)[0] != 0:
        raise ValueError('invalid RTL8126 firmware header')
    offset, count = struct.unpack_from('<II', data, 36)
    if count == 0 or offset > len(data) or count > (len(data) - offset) // 4 or sum(data) % 256:
        raise ValueError('invalid RTL8126 firmware actions/checksum')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', default='build')
    parser.add_argument('--source', default='/usr/lib/firmware/rtl_nic')
    args = parser.parse_args()
    target = Path(args.build) / 'esp/EFI/OS/firmware/rtl8126'
    payloads = {}
    for name in ('rtl8126a-2.fw', 'rtl8126a-3.fw'):
        source = Path(args.source) / name
        if not source.is_file():
            source = target / name
        data = source.read_bytes()
        validate(data)
        payloads[name] = data
    target.mkdir(parents=True, exist_ok=True)
    for name, data in payloads.items():
        destination = target / name
        if destination.is_file() and destination.read_bytes() == data:
            continue
        with tempfile.NamedTemporaryFile(dir=target, delete=False) as output:
            temporary = Path(output.name)
            try:
                output.write(data)
                output.flush()
                os.fsync(output.fileno())
                os.chmod(temporary, 0o644)
                os.replace(temporary, destination)
            finally:
                temporary.unlink(missing_ok=True)
        print(f'Staged {name}: {len(data)} bytes')


if __name__ == '__main__':
    main()
