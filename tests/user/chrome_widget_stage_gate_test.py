"""Exercise the single-pass widget gate and reject retired namespace logs."""
from pathlib import Path
import os
import subprocess
import tempfile
import threading

ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT/'tools/verify-chrome-widget-stages.sh'
STAGES = ('WIDGET_INIT_PLATFORM_BEGIN', 'WIDGET_INIT_PLATFORM_DONE',
          'WIDGET_INIT_COMPOSITOR_BEGIN', 'WIDGET_INIT_COMPOSITOR_DONE',
          'WIDGET_INIT_ACCEL_BEGIN', 'WIDGET_INIT_ACCEL_DONE',
          'WIDGET_INIT_HOST_BEGIN', 'WIDGET_INIT_HOST_DONE',
          'WIDGET_INIT_DONE', 'SURFACE_PRESENT')
cases = 0
with tempfile.TemporaryDirectory(prefix='system-widget-gate-') as directory:
    log = Path(directory)/'serial with spaces.log'
    def run(text, expected):
        global cases
        log.write_text(text)
        result = subprocess.run(['bash', str(SCRIPT), str(log)],
                                capture_output=True, text=True, timeout=5)
        assert result.returncode == expected, (text, result.stdout, result.stderr)
        cases += 1
    for family in ('OS_', 'LITEOS_'):
        markers = [family + stage for stage in STAGES]
        expected = 0 if family == 'OS_' else 1
        run('\n'.join(markers), expected)
        run('\r\n'.join('timestamp\t' + marker + ' extra=1' for marker in markers)+'\r\n', expected)
        run('\n'.join(marker+'\n'+marker for marker in markers), expected)
        run(' '.join(markers), 1)
        for index in range(len(markers)):
            run('\n'.join(markers[:index]+markers[index+1:]), 1)
            for altered in ('PREFIX' + markers[index], markers[index] + '_FAIL', markers[index] + 'Extra'):
                broken = markers.copy(); broken[index] = altered
                run('\n'.join(broken), 1)
        for index in range(len(markers)-1):
            broken = markers.copy()
            broken[index], broken[index+1] = broken[index+1], broken[index]
            run('\n'.join(broken), 1)
    run('\n'.join(('OS_' if index % 2 else 'LITEOS_') + marker
                  for index, marker in enumerate(STAGES)), 1)
    run('', 1)
    run('unrelated diagnostics\n', 1)
    assert subprocess.run(['bash', str(SCRIPT)], capture_output=True).returncode == 2
    assert subprocess.run(['bash', str(SCRIPT), str(log)+'.missing'], capture_output=True).returncode == 2
    # 完整且有效的跟踪记录必须立即返回，不得等待日志到达 EOF。
    fifo = Path(directory)/'live-log'
    os.mkfifo(fifo)
    release = threading.Event()
    def writer():
        try:
            with fifo.open('w') as stream:
                # 填满使用块缓冲的 awk 实现。当检查器仍在提前关闭时，此生产者还会
                # 继续写入填充数据。
                stream.write('\n'.join('OS_' + stage for stage in STAGES)+'\n' + 'noise\n'*200000)
                stream.flush()
                release.wait(5)
        except BrokenPipeError:
            pass
    thread = threading.Thread(target=writer, daemon=True)
    thread.start()
    try:
        result = subprocess.run(['bash', str(SCRIPT), str(fifo)],
                                capture_output=True, text=True, timeout=2)
        assert result.returncode == 0, result.stderr
    finally:
        release.set(); thread.join(timeout=5)
    assert not thread.is_alive()

source = SCRIPT.read_text()
assert 'grep ' not in source and 'cut ' not in source
assert source.count('awk -v stages=') == 1
print(f'Widget stage gate: {cases} real checker cases; complete records, ordering, duplicates, '
      'CRLF, namespace isolation, retired namespace rejection, one scan and pre-EOF early completion passed')
