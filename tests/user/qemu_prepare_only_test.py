"""Run the real launcher against disposable fixtures; never start a guest."""
from pathlib import Path
import fcntl
import os
import shutil
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
with tempfile.TemporaryDirectory(prefix='system-qemu-prepare-') as directory:
    fixture = Path(directory)
    shutil.copy2(root/'run-qemu.sh', fixture/'run-qemu.sh')
    (fixture/'tools').mkdir()
    for name in ('clean-runtime-aliases.py', 'migrate-system-layout.py',
                 'finalize-app-layout.py', 'preserve-qemu-app-data.py'):
        shutil.copy2(root/'tools'/name, fixture/'tools'/name)
    build = fixture/'build'
    kernel = build/'esp/EFI/OS/kernel.elf'
    kernel.parent.mkdir(parents=True)
    kernel.write_bytes(b'fixture kernel')
    (build/'esp/usr/apps').mkdir(parents=True)
    # 本夹具只验证启动器事务；转换器生成占位镜像，且没有待恢复的应用数据。
    mdir = fixture/'mdir'
    mdir.write_text('#!/bin/sh\nexit 0\n')
    mdir.chmod(0o755)
    firmware = fixture/'firmware.fd'
    firmware.write_bytes(b'fixture firmware')
    marker = fixture/'unexpected-launch'
    qemu = fixture/'qemu'
    qemu.write_text('#!/bin/bash\ntouch "$LAUNCH_MARKER"\nexit 99\n')
    qemu.chmod(0o755)
    converter = fixture/'qemu-img'
    converter.write_text('#!/bin/bash\n'
                         '[[ ${FAIL_CONVERT:-0} == 0 ]] || exit 42\n'
                         'printf "prepared-image" > "${@: -1}"\n')
    converter.chmod(0o755)
    env = dict(os.environ, BUILD=str(build), QEMU_PATH=str(qemu),
               QEMU_IMG=str(converter), OVMF_CODE=str(firmware),
               LAUNCH_MARKER=str(marker), PATH=str(fixture)+os.pathsep+os.environ['PATH'])

    def run(extra=(), **changes):
        return subprocess.run(['bash', str(fixture/'run-qemu.sh'),
                               '--no-build', '--prepare-only', *extra],
                              env=dict(env, **changes), text=True,
                              capture_output=True, timeout=30)

    image = build/'qemu-fat.img'
    result = run()
    assert result.returncode == 0, result.stderr
    assert image.read_bytes() == b'prepared-image'
    assert not marker.exists()
    image.write_bytes(b'previous-good-image')
    result = run(FAIL_CONVERT='1')
    assert result.returncode != 0 and '无法生成 QEMU FAT 磁盘镜像' in result.stderr, result.stderr
    assert image.read_bytes() == b'previous-good-image'
    assert not list(build.glob('.qemu-image.tmp.*'))
    with (build/'qemu-run.lock').open('a') as lock:
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        result = run()
        assert result.returncode != 0 and '已有 QEMU' in result.stderr
    with image.open('rb'):
        result = run()
        assert result.returncode != 0 and '正在使用' in result.stderr
    assert image.read_bytes() == b'previous-good-image'
    assert not marker.exists()
    # 显式复用时既不能调用转换器，也不要求暂存文件。
    kernel.unlink()
    identity = (image.stat().st_ino, image.stat().st_mtime_ns)
    result = run(('--reuse-image',), FAIL_CONVERT='1')
    assert result.returncode == 0, result.stderr
    assert (image.stat().st_ino, image.stat().st_mtime_ns) == identity
    assert image.read_bytes() == b'previous-good-image'
    with image.open('rb'):
        result = run(('--reuse-image',))
        assert result.returncode != 0 and '正在使用' in result.stderr
    with (build/'qemu-run.lock').open('a') as lock:
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        result = run(('--reuse-image',))
        assert result.returncode != 0 and '已有 QEMU' in result.stderr
    result = run(('--reuse-image','--all-disks'))
    assert result.returncode != 0 and '无法复用' in result.stderr
    secondary = build/'qemu-nvme-data.img'
    secondary.write_bytes(b'preserved-secondary-data')
    result = run(('--reuse-image','--all-disks'), FAIL_CONVERT='1')
    assert result.returncode == 0, result.stderr
    with secondary.open('rb'):
        result = run(('--reuse-image','--all-disks'))
        assert result.returncode != 0 and '正在使用' in result.stderr
    assert secondary.read_bytes() == b'preserved-secondary-data'
    result = subprocess.run(['bash',str(fixture/'run-qemu.sh'),
                             '--reuse-image','--prepare-only'],env=env,
                            text=True,capture_output=True,timeout=30)
    assert result.returncode == 2 and '--no-build' in result.stderr
    image.unlink()
    result = run(('--reuse-image',))
    assert result.returncode != 0 and '无法复用' in result.stderr
    assert not image.exists() and not marker.exists()
print('QEMU prepare-only: real launcher, no guest, atomic replacement, '
      'conversion failure preservation, run lock and active image rejection; '
      'reuse preserves inode/data without staging, rejects missing/in-use primary/secondary disks passed')
