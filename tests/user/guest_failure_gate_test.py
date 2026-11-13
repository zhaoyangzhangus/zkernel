"""Execute the real post-QEMU failure gate without running or binding a VM."""
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
source = (ROOT/'tools/run-qemu-vfio-5070.sh').read_text()
gate = source[source.index('guest_failure=""\n'):]
assert gate.endswith('exit "$qemu_status"\n')
with tempfile.TemporaryDirectory(prefix='system-failure-gate-') as directory:
    path = Path(directory)
    for log, status, expected in (
        ('OS_DESKTOP_CACHE_OK\r\n', 0, 0),
        ('OS_STAGE_FAIL phase=USER result=-5\r\n', 0, 1),
        ('OS_FRAMEBUFFER_MAP_FAIL\r\n', 0, 1),
        ('OS_STAGE_FAIL phase=USER result=-5\r\n', 7, 7),
        ('OS_DESKTOP_CACHE_OK\r\n', 7, 7),
        ('OS_STAGE_FAILED_NOT_A_MARKER\r\n', 0, 0),
        ('prefix OS_STAGE_FAIL phase=USER\r\n', 0, 0),
    ):
        (path/'qemu-serial.log').write_text(log)
        result = subprocess.run(['bash', '-s', '--', str(path), str(status)],
                                input='build_dir="$1"; qemu_status="$2"\n'+gate,
                                text=True, capture_output=True, timeout=5)
        assert result.returncode == expected, (log, status, result.returncode, result.stderr)
        if re.match(r'^OS_(STAGE_FAIL|FRAMEBUFFER_MAP_FAIL)(\s|$)', log):
            assert 'guest' in result.stderr and log.strip() in result.stderr
    (path/'qemu-serial.log').unlink()
    result = subprocess.run(['bash', '-s', '--', str(path), '9'],
                            input='build_dir="$1"; qemu_status="$2"\n'+gate,
                            text=True, capture_output=True, timeout=5)
    assert result.returncode == 9

benchmark = (ROOT/'tools/refactor-benchmark.sh').read_text()
pattern = re.search(r"if grep -Eq '([^']+)' \\\n\s*\"\$serial_path\"", benchmark)[1]
for log, expected in (
    ('OS_USER_RUNTIME_FAIL_STAGE=3\r\n', 0),
    ('OS_USER_RUNTIME_EXEC_WARN\r\n', 0),
    ('OS_STAGE_FAIL phase=USER result=-5\r\n', 0),
    ('OS_USER_RUNTIME_OK\r\n', 1),
    ('OS_USER_RUNTIME_EXEC_WARNING_NOT_A_MARKER\r\n', 1),
    ('prefix OS_STAGE_FAIL\r\n', 1),
):
    result = subprocess.run(['grep', '-Eq', pattern], input=log,
                            text=True, timeout=5)
    assert result.returncode == expected, (log, pattern, result.returncode)

realtest = (ROOT/'tools/run-realtest-qemu.ps1').read_text()
collector = (ROOT/'tools/collect-realtest-log.ps1').read_text()
assert 'LITEOS_REALTEST_' not in realtest
assert 'LITEOS_HOST_FIRMWARE_' not in collector
assert 'LITEOS_REALTEST_' not in collector
assert '"OS_REALTEST_$expectedMarker"' in realtest
assert '"OS_DESKTOP_CACHE_OK\\r\\n"' in (ROOT/'kernel/graphics/assets.c').read_text()
assert 'realtest_capture(text);' in (ROOT/'kernel/console.c').read_text()
desktop_patterns = re.findall(r"'(OS_DESKTOP_CACHE_OK[^']*)'", realtest)
assert len(desktop_patterns) == 4
log = 'OS_DESKTOP_CACHE_OK\r\nOS_NOTEPAD_READY SIZE=22\r\nOS_TEXT_ZOOM_SIZE=26\r\n'
assert all(re.search(pattern, log) for pattern in desktop_patterns)
assert not any(re.search(pattern, 'OS_DESKTOP_CACHE_PENDING\r\n')
               for pattern in desktop_patterns)
assert collector.count("'OS_HOST_FIRMWARE_STATE'") == 2
# 此固件持久化键是有意保留的兼容身份，而不是日志标记：必须仍能读取已有机器状态。
assert "'LiteOSRealTestState'" in collector
print('Guest failure gate: current stage/framebuffer failures rejected, QEMU status preserved, clean and missing logs handled passed')
print('Benchmark failure records and Windows real-test/firmware marker contracts passed (Windows source checks only)')
