"""Run actual stream mirrors: complete evidence survives terminal filtering."""
from pathlib import Path
import subprocess
import tempfile

root=Path(__file__).resolve().parents[2]
source=(root/'run-qemu.sh').read_text()
begin=source.index('stdout_log="$build_dir/qemu-stdout.log"')
end=source.index('# Ctrl+S',begin)
mirrors=source[begin:end]
with tempfile.TemporaryDirectory(prefix='qemu-stream-') as directory:
    base=Path(directory)
    for direct in (0,1):
        local=base/str(direct);external=base/('usb'+str(direct))
        local.mkdir();external.mkdir()
        script='set -euo pipefail\nbuild_dir="$1"\nstream_log_run="$2"\nserial_to_file="$3"\n'+mirrors+'''
printf 'boot normal\\nGPU ready\\nERROR fixture\\n' | mirror_qemu_stdout
printf 'ordinary diagnostic\\nqemu: invalid fixture\\nterminating on signal 15 from pid 42\\n' | mirror_qemu_stderr
'''
        result=subprocess.run(['bash','-c',script,'mirror',str(local),str(external),str(direct)],
                              capture_output=True,text=True,check=True)
        assert result.stdout=='ERROR fixture\n'
        assert result.stderr=='qemu: invalid fixture\n'
        assert (external/'stdout.log').read_text()=='boot normal\nGPU ready\nERROR fixture\n'
        assert (external/'stderr.log').read_text()=='ordinary diagnostic\nqemu: invalid fixture\nterminating on signal 15 from pid 42\n'
        assert (external/'stdout.log').read_bytes()==(local/'qemu-stdout.log').read_bytes()
        assert (external/'stderr.log').read_bytes()==(local/'qemu-stderr.log').read_bytes()
        if direct:assert not (local/'qemu-serial.log').exists()
        else:assert (local/'qemu-serial.log').read_bytes()==(external/'stdout.log').read_bytes()
    # 启用该选项不得更改现有启动器，也不得要求外接磁盘。
    local=base/'default';local.mkdir()
    subprocess.run(['bash','-c','set -euo pipefail\nbuild_dir="$1"\nstream_log_run=""\nserial_to_file=0\n'+mirrors+
                    '\nprintf "boot\\n" | mirror_qemu_stdout','default',str(local)],check=True)
    assert (local/'qemu-serial.log').read_text()=='boot\n'
print('QEMU stream mirrors: full evidence, terminal filtering, headless direct-file policy and default compatibility passed')
