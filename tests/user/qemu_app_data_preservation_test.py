"""使用真实 FAT 镜像验证应用数据跨重建保存，且拒绝访问正在使用的镜像。"""
from pathlib import Path
import hashlib
import importlib.util
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('preservation', ROOT/'tools/preserve-qemu-app-data.py')
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)
with tempfile.TemporaryDirectory() as directory:
    base = Path(directory)
    image = base/'disk.img'
    subprocess.run(['mformat','-i',str(image),'-C','-T','32768','::'],check=True)
    subprocess.run(['mmd','-i',str(image),'::/usr','::/usr/apps',
                    '::/usr/apps/chrome','::/usr/apps/chrome/data',
                    '::/usr/apps/chrome/run'],check=True)
    preferences = base/'Preferences'
    content = '{"语言":"中文","startup_cache":"keep"}'.encode()
    preferences.write_bytes(content)
    subprocess.run(['mcopy','-i',str(image),str(preferences),
                    '::/usr/apps/chrome/data/Preferences'],check=True)
    subprocess.run(['mcopy','-i',str(image),str(preferences),
                    '::/usr/apps/chrome/run/transient'],check=True)
    root = base/'esp'
    app = root/'usr/apps/chrome'
    (app/'data').mkdir(parents=True)
    (app/'data/deleted').write_text('旧快照中已删除的数据不能复活')
    (app/'chrome').write_bytes(b'new program')
    before = hashlib.sha256(image.read_bytes()).digest()
    with image.open('rb'):
        try:
            module.preserve(image,root)
            raise AssertionError('不能读取使用中的镜像')
        except ValueError:
            pass
    assert (app/'data/deleted').exists()
    assert module.preserve(image,root)==1
    assert (app/'data/Preferences').read_bytes()==content
    assert not (app/'data/deleted').exists() and not (app/'run').exists()
    assert (app/'chrome').read_bytes()==b'new program'
    assert hashlib.sha256(image.read_bytes()).digest()==before
    assert module.preserve(image,root)==1
    assert module.preserve(base/'missing.img',root)==0
print('QEMU app data: actual FAT, exact bytes, stale-data removal, executable isolation, live-image rejection and read-only source PASS')
