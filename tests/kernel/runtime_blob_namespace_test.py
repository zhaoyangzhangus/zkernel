"""Internal runtime entry renaming preserves payload bytes and symbol offsets."""
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
cases = (
    ('tests/user/user_test_blob.S', 'user_runtime_', 'liteos_user_runtime_'),
    ('kernel/process/init_blob.S', 'INIT_RUN_SERVICE', 'LITEOS_RUN_SERVICE'),
)
with tempfile.TemporaryDirectory(prefix='runtime-namespace-') as directory:
    directory = Path(directory)
    for index, (name, current, previous) in enumerate(cases):
        source = (ROOT / name).read_text()
        assert current in source and previous not in source, name
        for autostart in (0, 1):
            outputs = []
            for variant, body in enumerate((source, source.replace(current, previous))):
                obj = directory / f'{index}-{autostart}-{variant}.o'
                subprocess.run(['cc', '-m64', '-x', 'assembler-with-cpp',
                                f'-DOS_AUTOSTART_CHROME={autostart}', '-c', '-',
                                '-o', str(obj)], input=body, text=True, check=True)
                code = subprocess.check_output(['objcopy', '-O', 'binary',
                                                '-j', '.rdata', str(obj), '/dev/stdout'])
                assert len(code) > 1000, name
                symbols = subprocess.check_output(['nm', '--defined-only', str(obj)], text=True)
                relocations = subprocess.check_output(['objdump', '-r', str(obj)], text=True)
                # 这些负载会直接复制到用户 ELF 映像中。
                assert not re.search(r'^RELOCATION RECORDS FOR', relocations, re.M), name
                # nm 会依据进程区域设置对原始名称排序。添加前缀可能改变排序，但不会更改
                # 符号地址；比较规范化后的行，同时保留重复项。
                normalized = '\n'.join(sorted(
                    symbols.replace(previous, current).splitlines()))
                outputs.append((code, normalized))
            assert outputs[0] == outputs[1], (name, autostart)
            if index == 0:
                exported = set(re.findall(r'\bR\s+(user_runtime_\w+)', outputs[0][1]))
                expected = {'user_runtime_' + suffix for suffix in (
                    'blob_start', 'blob_end', 'child', 'timer_helper', 'vm_worker',
                    'cow_worker', 'uaccess_worker', 'wait_race_worker')}
                assert exported == expected, exported
consumer = (ROOT / 'kernel/process/runtime_test.c').read_text()
assert 'liteos_user_runtime_' not in consumer
assert set(re.findall(r'extern const uint8_t (user_runtime_\w+)\[\];', consumer)) == {
    'user_runtime_' + suffix for suffix in (
        'blob_start', 'blob_end', 'child', 'timer_helper', 'vm_worker', 'uaccess_worker')}
assert 'nvkms_liteos_' not in (ROOT / 'kernel/drivers/display/nvidia/nv_nvkms_os.c').read_text()
print('Runtime namespace: identical payload bytes and offsets, no relocations, both Chrome autostart modes passed')
