"""Fail closed before using build-specific Chromium breakpoint addresses."""
import os
from pathlib import Path
import re
import subprocess

ROOT = Path(__file__).resolve().parents[2]


def chrome_binary_path():
    output = os.environ.get('CHROMIUM_OUT')
    if not output:
        source = os.environ.get('CHROMIUM_SRC')
        if not source:
            raise RuntimeError('Set CHROMIUM_OUT or CHROMIUM_SRC for Chrome diagnostics')
        output = str(Path(source)/'out/liteos-debian')
    return Path(output)/'chrome'


def _chrome_build_id(binary):
    try:
        notes = subprocess.check_output(['readelf', '-n', str(binary)], text=True)
    except (OSError, subprocess.CalledProcessError) as error:
        raise RuntimeError(f'Cannot verify Chrome build: {binary}') from error
    identities = re.findall(r'^[ \t]*Build ID: ([0-9a-fA-F]+)[ \t]*$', notes, re.M)
    if len(identities) != 1 or notes.count('Build ID:') != 1:
        raise RuntimeError(f'Missing or ambiguous Chrome build identity: {binary}')
    return 'Build ID: '+identities[0].lower()


def validate_chrome_build(expected):
    if not re.fullmatch(r'Build ID: [0-9a-f]+', expected):
        raise RuntimeError('Invalid expected Chrome build identity')
    for binary in (ROOT/'build/esp/usr/apps/chrome/chrome', chrome_binary_path()):
        if _chrome_build_id(binary) != expected:
            raise RuntimeError(f'Refusing stale Chrome breakpoint addresses: {binary}')


def validate_chrome_symbols():
    """For symbol-derived addresses, require exactly matching staged/output ELFs."""
    binary = chrome_binary_path()
    if _chrome_build_id(ROOT/'build/esp/usr/apps/chrome/chrome') != _chrome_build_id(binary):
        raise RuntimeError('Symbol executable does not match staged Chrome Build ID')
    return binary
