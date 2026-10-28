"""验证系统目录迁移的完整性、重复执行和冲突保护。"""
from pathlib import Path
import importlib.util
import tempfile

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('migration', ROOT/'tools/migrate-system-layout.py')
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)

with tempfile.TemporaryDirectory() as directory:
    root = Path(directory)/'esp'
    (root/'etc/fonts').mkdir(parents=True)
    (root/'etc/fonts/default.ttf').write_bytes(b'font')
    (root/'init').write_bytes(b'init')
    (root/'init-runtime').write_bytes(b'runtime')
    (root/'netd.log').write_bytes(b'log')
    (root/'native').mkdir()
    (root/'Downloads').mkdir()
    (root/'Downloads/keep').write_text('keep')
    assert module.migrate(root) == 4
    assert (root/'EFI/OS/fonts/default.ttf').read_bytes() == b'font'
    assert (root/'EFI/OS/init').read_bytes() == b'init'
    assert (root/'EFI/OS/init-runtime').read_bytes() == b'runtime'
    assert (root/'log/netd.log').read_bytes() == b'log'
    assert not (root/'etc').exists() and not (root/'native').exists()
    assert (root/'Downloads/keep').read_text() == 'keep'
    assert module.migrate(root) == 0
    (root/'etc').mkdir()
    (root/'etc/new').write_text('new')
    (root/'init').write_text('conflict')
    try:
        module.migrate(root)
        raise AssertionError('应拒绝覆盖冲突文件')
    except ValueError:
        pass
    assert (root/'etc/new').is_file() and (root/'init').read_text() == 'conflict'
    (root/'init').unlink()
    (root/'EFI/OS/fonts/default.ttf').unlink()
    (root/'EFI/OS/fonts').rmdir()
    (root/'EFI/OS/fonts').symlink_to(Path(directory), target_is_directory=True)
    (root/'etc/fonts').mkdir()
    (root/'etc/fonts/new').write_text('new')
    try:
        module.migrate(root)
        raise AssertionError('应拒绝符号链接目标')
    except ValueError:
        pass
    assert (root/'etc/fonts/new').is_file()
print('system layout migration passed')
