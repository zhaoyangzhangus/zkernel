"""实际清理必须精确、可重复，并先验证迁移目标且不跟随链接。"""
import importlib.util
from pathlib import Path
import tempfile
import subprocess

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('cleanup', ROOT/'tools/clean-runtime-aliases.py')
cleanup = importlib.util.module_from_spec(spec)
spec.loader.exec_module(cleanup)

def put(esp, relative, data=b'new application payload'):
    path = esp/relative
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(data)
    return path


with tempfile.TemporaryDirectory(prefix='runtime-cleanup-') as directory:
    base = Path(directory)
    esp = base/'esp'
    for relative in cleanup.RETIRED:
        path = esp/relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(b'old generated alias')
    current = esp/'lib/libsystemc.so.1'
    current.write_bytes(b'current runtime')
    outside = base/'outside'
    outside.write_bytes(b'protected')
    alias = esp/cleanup.RETIRED[0]
    alias.unlink()
    alias.symlink_to(outside)
    assert len(cleanup.clean(esp)) == len(cleanup.RETIRED)
    assert outside.read_bytes() == b'protected'
    assert current.read_bytes() == b'current runtime'
    assert cleanup.clean(esp) == []

    # Chrome 顶层扩展资源也必须逐个验证新副本，不能按通配符直接删除。
    for name in ('extra.pak', 'extra.dat', 'extra.bin', 'extra.staging.json'):
        old = put(esp, 'sbin/'+name)
        assert cleanup.clean(esp) == [] and old.is_file()
        put(esp, 'usr/apps/chrome/'+name)
        assert cleanup.clean(esp) == [old]
    alias.write_bytes(b'retain on validation failure')
    invalid = esp/cleanup.RETIRED[-1]
    invalid.mkdir()
    try:
        cleanup.clean(esp)
        raise AssertionError('directory alias accepted')
    except ValueError:
        pass
    assert alias.is_file()
    invalid.rmdir()
    (esp/'lib').rename(esp/'real-lib')
    (esp/'lib').symlink_to(esp/'real-lib', target_is_directory=True)
    try:
        cleanup.clean(esp)
        raise AssertionError('symlinked staging parent accepted')
    except ValueError:
        pass
    assert (esp/'real-lib'/alias.name).is_file()



# 覆盖全部迁移规则：缺少新负载时保留旧副本；就绪后只删除明确列出的旧文件。
with tempfile.TemporaryDirectory(prefix='application-cleanup-') as directory:
    esp = Path(directory)/'esp'
    for old in cleanup.MIGRATED:
        put(esp, old, b'old payload')
    protected = ('sbin/netd', 'sbin/audiod', 'sbin/deviced', 'sbin/logd',
                 'sbin/crashd', 'sbin/custom-app',
                 'lib/libsystemc.so.1', 'lib/libgraphics.so.1',
                 'etc/fonts/default.ttf', 'etc/fonts/cjk.ttf')
    for relative in protected:
        put(esp, relative, b'keep')
    assert cleanup.clean(esp) == []
    destinations = {item for items in cleanup.MIGRATED.values() for item in items}
    for destination in destinations:
        put(esp, destination)
    # 图集必须等全部替代图标就绪；空文件不能作为迁移完成的证据。
    icon = esp/'usr/apps/gshell/icon.png'
    icon.write_bytes(b'')
    removed = cleanup.clean(esp)
    assert len(removed) == len(cleanup.MIGRATED)-1
    assert (esp/'etc/desktop/icons.png').is_file()
    icon.write_bytes(b'icon')
    assert cleanup.clean(esp) == [esp/'etc/desktop/icons.png']
    assert not (esp/'etc/desktop').exists()
    assert cleanup.clean(esp) == []
    for destination in destinations:
        assert (esp/destination).is_file()
    for relative in protected:
        assert (esp/relative).read_bytes() == b'keep'

# Chrome 目录逐文件核对新归属；未知资源及旧树独有文件保留。
with tempfile.TemporaryDirectory(prefix='chrome-resource-cleanup-') as directory:
    esp = Path(directory)/'esp'
    for name in cleanup.CHROME_RESOURCE_DIRS:
        for relative in ('known.pak', 'nested/data.bin'):
            put(esp, f'sbin/{name}/{relative}', b'old')
            put(esp, f'usr/apps/chrome/{name}/{relative}')
    unknown = put(esp, 'sbin/resources/private.txt', b'keep')
    outside = Path(directory)/'outside'
    outside.write_bytes(b'keep outside')
    link = esp/'sbin/locales/known.pak'
    link.unlink()
    link.symlink_to(outside)
    assert len(cleanup.clean(esp)) == 6
    assert not (esp/'sbin/locales').exists()
    assert not (esp/'sbin/MEIPreload').exists()
    assert unknown.read_bytes() == b'keep'
    assert outside.read_bytes() == b'keep outside'
    assert cleanup.clean(esp) == []

# 新归属链接不算就绪，目录链接则整批拒绝，先前验证的文件也不能被删除。
with tempfile.TemporaryDirectory(prefix='cleanup-destination-') as directory:
    esp = Path(directory)/'esp'
    old = put(esp, 'sbin/fileman', b'old')
    destination = put(esp, 'usr/apps/fileman/fileman')
    outside = Path(directory)/'outside'
    outside.write_bytes(b'protected')
    destination.unlink()
    destination.symlink_to(outside)
    assert cleanup.clean(esp) == [] and old.is_file()
    destination.unlink()
    destination.write_bytes(b'new')
    put(esp, cleanup.RETIRED[0])
    (esp/'usr/apps/fileman').rename(esp/'usr/apps/real-fileman')
    (esp/'usr/apps/fileman').symlink_to(esp/'usr/apps/real-fileman', target_is_directory=True)
    try:
        cleanup.clean(esp)
        raise AssertionError('symlinked destination parent accepted')
    except ValueError:
        pass
    assert old.is_file() and (esp/cleanup.RETIRED[0]).is_file()
    assert outside.read_bytes() == b'protected'

# 旧应用路径意外成为目录时，整批停止，不能误递归或先删除其余旧副本。
with tempfile.TemporaryDirectory(prefix='cleanup-application-directory-') as directory:
    esp = Path(directory)/'esp'
    alias = put(esp, cleanup.RETIRED[0])
    put(esp, 'usr/apps/fileman/fileman')
    (esp/'sbin/fileman').mkdir(parents=True)
    try:
        cleanup.clean(esp)
        raise AssertionError('application directory accepted as retired file')
    except ValueError:
        pass
    assert alias.is_file() and (esp/'sbin/fileman').is_dir()

# 旧资源树本身是链接时拒绝，不进入树外目录。
with tempfile.TemporaryDirectory(prefix='cleanup-resource-link-') as directory:
    esp = Path(directory)/'esp'
    put(esp, cleanup.RETIRED[0])
    (esp/'sbin').mkdir()
    (esp/'sbin/resources').symlink_to(Path(directory), target_is_directory=True)
    try:
        cleanup.clean(esp)
        raise AssertionError('symlinked resource tree accepted')
    except ValueError:
        pass
    assert (esp/cleanup.RETIRED[0]).is_file()

with tempfile.TemporaryDirectory(prefix='cleanup-empty-locale-metadata-') as directory:
    esp = Path(directory)/'esp'
    old = put(esp, 'sbin/locales/en-US_FEMININE.pak.info', b'')
    current = put(esp, 'usr/apps/chrome/locales/en-US_FEMININE.pak.info', b'')
    cleanup.clean(esp)
    assert not old.exists() and current.is_file()

make = (ROOT/'makefile').read_text()
assert 'DESKTOP_ASSET_DIR = $(BUILD)/esp/usr/apps/desktop' in make
assert 'assets/desktop/vfsmap.tst | $(BUILD)/esp\n' in make
with tempfile.TemporaryDirectory(prefix='legacy-diagnostics-') as directory:
    build = Path(directory)/'build'
    esp = build/'esp'
    old = put(esp, 'sbin/taskmgr-cpu-probe', b'legacy probe')
    put(esp, 'sbin/tcp-wake-probe', b'old tcp probe')
    current = put(esp, 'usr/apps/tcp-wake-probe/tcp-wake-probe', b'new tcp probe')
    recipe = make[make.index('LEGACY_DIAGNOSTIC_NAMES ='):make.index('esp: rtl8126-payload')]
    program = f'override BUILD := {build}\n{recipe}'
    for _ in range(2):
        subprocess.run(['make', '-s', '-f', '-', 'legacy-diagnostic-apps'],
                       input=program, text=True, check=True)
    assert (esp/'usr/apps/taskmgr-cpu-probe/taskmgr-cpu-probe').read_bytes() == b'legacy probe'
    assert current.read_bytes() == b'new tcp probe'
    assert old.read_bytes() == b'legacy probe'
    assert len(cleanup.clean(esp)) == 2
    assert not old.exists() and current.read_bytes() == b'new tcp probe'
assert 'python3 tools/clean-runtime-aliases.py "$(BUILD)/esp"' in make
runner = (ROOT/'run-qemu.sh').read_text()
call = 'python3 "$script_dir/tools/clean-runtime-aliases.py" "$build_dir/esp"'
assert call in runner
assert runner.index(call) < runner.index('materialize_fat_image()')
print('Runtime alias cleanup: exact files, idempotence, current runtime retention, '
      'external symlink protection, application migration readiness, Chrome resource '
      'ownership, unknown-file retention and validate-before-delete passed')
