"""Neutral browser tools retain the existing output and safe check-only mode."""
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
for name in ('build-chrome.sh', 'stage-chrome.sh', 'chrome-prebuild.sh'):
    path = ROOT / 'tools' / name
    assert os.access(path, os.X_OK), name
    subprocess.run(['bash', '-n', str(path)], check=True)
    assert 'native_os_root=' not in path.read_text()
for name in ('build-chrome-liteos.sh', 'stage-chrome-liteos.sh', 'liteos-chrome-prebuild.sh'):
    assert not (ROOT / 'tools' / name).exists(), name
builder = ROOT / 'tools/build-chrome.sh'
env = dict(os.environ, CHROMIUM_SRC='')
result = subprocess.run(['bash', str(builder), '--check'], env=env,
                        capture_output=True, text=True)
assert result.returncode == 2 and 'CHROMIUM_SRC' in result.stderr
with tempfile.TemporaryDirectory(prefix='chrome-entrypoints-') as directory:
    base = Path(directory)
    src = base / 'source with spaces'
    out = src / 'out/liteos-debian'
    out.mkdir(parents=True)
    gn = src / 'buildtools/linux64/gn'
    gn.parent.mkdir(parents=True)
    gn.write_text('#!/bin/sh\nexit 99\n'); gn.chmod(0o755)
    tools = base / 'tools'
    tools.mkdir()
    for name in ('ninja', 'sccache', 'systemd-run', 'systemctl', 'flock'):
        path = tools / name
        path.write_text('#!/bin/sh\nexit 99\n'); path.chmod(0o755)
    args = out / 'args.gn'
    sdk_header = src / 'build/linux/debian_bullseye_amd64-sysroot/usr/include/linux/limits.h'
    sdk_header.parent.mkdir(parents=True)
    sdk_header.write_text('/* Fixed SDK fixture; no host headers. */\n')
    original = (f'native_os_root = "{ROOT}"\nnative_os_build_dir = "{ROOT}/build"\n'
                'use_sysroot = true\n'
                'sysroot = "//build/linux/debian_bullseye_amd64-sysroot"\n'
                'use_qt5 = false\nuse_qt6 = false\nrtc_use_pipewire = false\n')
    args.write_text(original)
    env.update(CHROMIUM_SRC=str(src), PATH=str(tools) + os.pathsep + os.environ['PATH'])
    result = subprocess.run(['bash', str(builder), '--check'], env=env,
                            capture_output=True, text=True)
    assert result.returncode == 0, result.stderr
    assert str(out) in result.stdout and args.read_text() == original
    for key in ('native_os_root', 'native_os_build_dir'):
        args.write_text(original.replace(f'{key} = "', f'{key} = "/wrong/'))
        result = subprocess.run(['bash', str(builder), '--check'], env=env,
                                capture_output=True, text=True)
        assert result.returncode != 0 and key in result.stderr
    assert sorted(p.name for p in out.iterdir()) == ['args.gn']
    obj = out / 'obj'
    obj.mkdir()
    incomplete = obj / 'incomplete.a'; incomplete.touch()
    valid = obj / 'valid.a'; valid.write_bytes(b'!<arch>\n')
    result = subprocess.run(['bash', str(ROOT / 'tools/chrome-prebuild.sh'),
                            '--chromium-out', str(out), '--dry-run'],
                            capture_output=True, text=True)
    assert result.returncode == 0 and 'would remove' in result.stdout
    assert incomplete.exists() and valid.read_bytes() == b'!<arch>\n'
print('Chrome entrypoints: neutral executable paths, explicit source, unchanged output, check-only GN roots and non-mutating prebuild passed')
