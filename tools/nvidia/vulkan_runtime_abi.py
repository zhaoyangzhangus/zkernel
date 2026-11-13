#!/usr/bin/env python3
"""Check versioned glibc imports before publishing Vulkan runtime libraries."""
import argparse
import os
from pathlib import Path
import re
import subprocess


def symbols(path, undefined):
    output = subprocess.check_output(['readelf', '--dyn-syms', '--wide', str(path)],
                                     text=True, env=dict(os.environ, LC_ALL='C'))
    result = set()
    for row in output.splitlines():
        fields = row.split()
        if len(fields) < 8 or not fields[0].endswith(':'):
            continue
        if (fields[6] == 'UND') != undefined:
            continue
        # 弱导入允许保持未解析状态。
        if undefined and fields[4] == 'WEAK':
            continue
        match = re.fullmatch(r'([^@]+)@@?(GLIBC_[A-Za-z0-9_.]+)', fields[7])
        if match:
            result.add(match.groups())
    return result


def check(runtime, libraries):
    available = set()
    providers = sorted(Path(runtime).glob('*.so*'))
    for provider in providers:
        if provider.is_file():
            with provider.open('rb') as stream:
                is_elf = stream.read(4) == b'\x7fELF'
            if is_elf:
                available.update(symbols(provider, False))
    if not available:
        raise ValueError('runtime has no versioned glibc exports')
    missing = {}
    for library in libraries:
        absent = symbols(library, True) - available
        if absent:
            missing[str(library)] = sorted(f'{name}@{version}' for name, version in absent)
    if missing:
        details = '; '.join(f'{path}: {", ".join(names)}' for path, names in missing.items())
        raise ValueError('Vulkan runtime ABI mismatch: ' + details)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--runtime-dir', required=True, type=Path)
    parser.add_argument('libraries', nargs='+', type=Path)
    args = parser.parse_args()
    try:
        check(args.runtime_dir, args.libraries)
    except (ValueError, OSError, subprocess.CalledProcessError) as error:
        parser.exit(1, str(error) + '\n')
    print('VULKAN_RUNTIME_ABI_PASS')
