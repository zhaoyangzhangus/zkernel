"""真实 ELF 验证私有加载器、依赖闭包以及清理前的完整性门禁。"""
import importlib.util
import json
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('app_layout', root/'tools/finalize-app-layout.py')
layout = importlib.util.module_from_spec(spec)
spec.loader.exec_module(layout)
with tempfile.TemporaryDirectory(prefix='app-runtime-layout-') as directory:
    base = Path(directory)
    esp = base/'esp'
    app = esp/'usr/apps/demo'
    private = app/'lib'
    private.mkdir(parents=True)
    source = base/'test.c'
    source.write_text('void _start(void) {}\n')
    library = private/'libtest.so.1'
    subprocess.run(['cc', '-nostdlib', '-shared', '-fPIC', str(source),
                    '-Wl,-soname,libtest.so.1', '-o', str(library)], check=True)
    loader = private/'ld-linux-x86-64.so.2'
    loader.write_bytes(library.read_bytes())
    def build(interpreter='/usr/apps/demo/lib/ld-linux-x86-64.so.2', rpath='/usr/apps/demo/lib'):
        subprocess.run(['cc', '-nostdlib', '-no-pie', str(source), '-Wl,--no-as-needed',
                        str(library), '-Wl,-e,_start', '-Wl,-dynamic-linker,'+interpreter,
                        '-Wl,-rpath,'+rpath, '-o', str(app/'demo')], check=True)
    def rejected(action):
        try:
            action()
        except ValueError:
            return
        raise AssertionError('错误布局未被拒绝')
    build()
    assert layout.finalize(esp) == 1
    loader.unlink()
    rejected(lambda: layout.finalize(esp))
    loader.write_bytes(library.read_bytes())
    saved = library.read_bytes()
    library.unlink()
    rejected(lambda: layout.finalize(esp))
    library.write_bytes(saved)
    build('/lib64/ld-linux-x86-64.so.2')
    rejected(lambda: layout.finalize(esp))
    build(rpath='/compat/linux/lib/x86_64-linux-gnu')
    rejected(lambda: layout.finalize(esp))
    build(rpath='/usr/apps/other/lib')
    rejected(lambda: layout.finalize(esp))
    build()
    config = app/'vulkan/nvidia_icd.json'
    config.parent.mkdir()
    config.write_text(json.dumps({'ICD': {'library_path': '/usr/apps/demo/lib/libtest.so.1'}}))
    assert layout.finalize(esp) == 1
    config.write_text(json.dumps({'ICD': {'library_path': '/usr/apps/other/lib/libtest.so.1'}}))
    rejected(lambda: layout.finalize(esp))
    config.unlink()
    old = esp/'compat/linux'
    old.mkdir(parents=True)
    sentinel = old/'must-survive'
    sentinel.touch()
    rejected(lambda: layout.finalize(esp, clean=True))
    assert sentinel.exists()
    for name in ('chrome', 'cube', 'vulkan-probe', 'vulkan-video'):
        target = esp/'usr/apps'/name
        (target/'lib').mkdir(parents=True)
        (target/'lib/ld-linux-x86-64.so.2').write_bytes(saved)
        subprocess.run(['cc', '-nostdlib', '-no-pie', str(source), '-Wl,-e,_start',
                        '-Wl,-dynamic-linker,/usr/apps/'+name+'/lib/ld-linux-x86-64.so.2',
                        '-Wl,-rpath,/usr/apps/'+name+'/lib', '-o', str(target/name)], check=True)
    public = esp/'EFI/OS/fonts/system.ttf'
    public.parent.mkdir(parents=True)
    public.write_bytes(b'system font')
    assert layout.finalize(esp, clean=True) == 5
    assert not old.exists() and public.read_bytes() == b'system font'
    assert layout.finalize(esp, strict=True) == 5
print('PASS: actual ELF private interpreter/closure/RPATH, cross-app ICD rejection and incomplete migration cannot remove old payload')
