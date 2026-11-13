"""Exercise the production feature selector, without launching another guest."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
baseline = 'qemu64,rdrand=on,xsave=off,avx=off,avx2=off'
with tempfile.TemporaryDirectory(prefix='system-qemu-cpu-') as directory:
    fixture = Path(directory) / 'cpuinfo'
    def select(text, kvm=1, chrome=1):
        fixture.write_text(text)
        return subprocess.check_output(['bash', '-c',
            'source "$1"; qemu_select_cpu_model "$2" "$3" "$4"', 'cpu-test',
            str(root/'tools/qemu-cpu-model.sh'), str(fixture), str(kvm), str(chrome)], text=True).strip()
    all_flags = 'flags\t\t: sse sse2 ssse3 sse4_1 sse4_2 popcnt avx avx2 xsave\n'
    assert select(all_flags) == baseline+',ssse3=on,sse4.1=on,sse4.2=on,popcnt=on'
    assert select(all_flags, kvm=0) == baseline
    assert select(all_flags, chrome=0) == baseline
    assert select('') == baseline
    assert select('flags : sse sse2\n') == baseline
    assert select(all_flags+'flags : sse ssse3 popcnt\n') == baseline+',ssse3=on,popcnt=on'
    assert select('flags : ssse3_fake sse4_1_fake popcnt_fake\n') == baseline
    assert select(all_flags*4) == select(all_flags)
    stable = 'flags : constant_tsc nonstop_tsc sse sse2\n'
    assert select(stable, chrome=0) == baseline+',invtsc=on'
    assert select(stable, kvm=0) == baseline
    assert select(stable*4, chrome=0) == baseline+',invtsc=on'
    assert select(stable+'flags : constant_tsc\n', chrome=0) == baseline
    assert select('flags : constant_tsc_fake nonstop_tsc_fake\n', chrome=0) == baseline
    assert select(all_flags+stable) == baseline
    assert select(all_flags.rstrip()+' constant_tsc nonstop_tsc\n') == \
        baseline+',ssse3=on,sse4.1=on,sse4.2=on,popcnt=on,invtsc=on'
launcher = (root/'run-qemu.sh').read_text()
assert '-cpu "$qemu_cpu_model"' in launcher
assert 'if ((qemu_use_kvm)); then' in launcher
assert 'qemu_select_cpu_model /proc/cpuinfo "$qemu_use_kvm" "$chrome_fast"' in launcher
print('QEMU CPU: Chrome/KVM host intersection, exact flags, TCG/legacy fallback and AVX/XSAVE exclusion passed')
