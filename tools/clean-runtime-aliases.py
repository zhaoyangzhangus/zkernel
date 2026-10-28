#!/usr/bin/env python3
"""精确清理暂存树中的过期别名和已迁移的应用副本。"""
import argparse
from pathlib import Path

RETIRED = (
    'lib/ld-liteos.so.1',
    'lib/ld-linux-liteos.so.1',
    'lib/libliteosc.so.1',
    'lib/libliteosgfx.so.1',
    'etc/fonts/liteos.ttf',
)

# 只有新归属的普通文件已就绪，才允许删除旧副本；不搬动系统服务。
MIGRATED = {
    **{f'sbin/{app}': (f'usr/apps/{app}/{app}',) for app in (
        'gshell', 'notepad', 'fileman', 'settings', 'taskmgr', 'imageview',
        'dyn-gfx', 'cube', 'ui-file-demo', 'control-tree-demo',
        'window-frame-probe', 'audio-test',
        'nasm', 'ndisasm', 'wget', 'libc-test', 'linux-events-test', 'fat-fsync-probe',
        'taskmgr-cpu-probe', 'tcp-wake-probe', 'chrome-glibc-probe',
        'vulkan-probe', 'vulkan-video',
    )},
    'sbin/fm': ('usr/apps/fileman/fileman',),
    'sbin/netmgr': ('usr/apps/settings/settings',),
    'sbin/wallpaper': ('usr/apps/settings/wallpaper',),
    'sbin/file-picker': ('usr/apps/settings/file-picker',),
    **{f'sbin/{app}.d': (f'usr/apps/{app}/{app}',) for app in (
        'control-tree-demo', 'window-frame-probe')},
    'etc/desktop/wall.png': ('usr/apps/desktop/wall.png',),
    'etc/desktop/fm.png': ('usr/apps/fileman/icon.png',),
    'etc/desktop/icons.png': tuple(f'usr/apps/{app}/icon.png' for app in (
        'gshell', 'notepad', 'settings', 'taskmgr', 'chrome', 'imageview')),
    **{f'sbin/{name}': (f'usr/apps/chrome/{name}',) for name in (
        'chrome', 'chrome-start', 'chrome.staging.json', 'staging.json', 'chrome_crashpad_handler',
        'chrome_sandbox', 'chrome_100_percent.pak', 'chrome_200_percent.pak',
        'headless_command_resources.pak', 'resources.pak', 'icudtl.dat',
        'snapshot_blob.bin', 'v8_context_snapshot.bin',
    )},
}
# 这三个目录由 Chrome 暂存脚本发布。只清理新目录中也存在的文件，保留未知项。
CHROME_RESOURCE_DIRS = ('locales', 'resources', 'MEIPreload')


def checked_path(root, relative):
    target = root / relative
    for parent in target.parents:
        if parent == root:
            break
        if parent.is_symlink():
            raise ValueError(f'refusing symlinked staging directory: {parent}')
    return target


def replacement_ready(root, relative):
    target = checked_path(root, relative)
    # 部分语言包的构建元数据合法地为空；可执行文件和实际资源仍必须非空。
    return not target.is_symlink() and target.is_file() and (
        target.stat().st_size > 0 or relative.endswith('.pak.info'))


def clean(root):
    root = Path(root)
    if root.is_symlink() or not root.is_dir() or root.name != 'esp':
        raise ValueError('expected a real ESP staging directory')
    targets = []
    migrations = dict(MIGRATED)
    sbin = checked_path(root, 'sbin')
    if sbin.is_symlink():
        raise ValueError(f'refusing symlinked staging directory: {sbin}')
    if sbin.is_dir():
        for entry in sorted(sbin.iterdir()):
            if entry.suffix in ('.pak', '.dat', '.bin') or entry.name.endswith('.staging.json'):
                migrations[entry.relative_to(root).as_posix()] = (
                    f'usr/apps/chrome/{entry.name}',)
    resource_dirs = []
    for name in CHROME_RESOURCE_DIRS:
        directory = checked_path(root, f'sbin/{name}')
        if directory.is_symlink():
            raise ValueError(f'refusing symlinked resource directory: {directory}')
        if not directory.exists():
            continue
        if not directory.is_dir():
            raise ValueError(f'refusing non-directory resource tree: {directory}')
        pending = [directory]
        while pending:
            current = pending.pop()
            resource_dirs.append(current)
            for entry in sorted(current.iterdir()):
                # 不跟随链接；链接本身只有在新归属存在时才加入删除清单。
                if entry.is_dir() and not entry.is_symlink():
                    pending.append(entry)
                else:
                    relative = entry.relative_to(root).as_posix()
                    destination = 'usr/apps/chrome/' + entry.relative_to(root/'sbin').as_posix()
                    migrations[relative] = (destination,)
    for relative in (*RETIRED, *migrations):
        target = checked_path(root, relative)
        if relative in migrations and not all(
                replacement_ready(root, path) for path in migrations[relative]):
            continue
        if target.is_symlink() or target.is_file():
            targets.append(target)
        elif target.exists():
            raise ValueError(f'refusing non-file alias: {target}')
    # 全部验证完成后才删除文件，最后只移除空的旧资源目录。
    for target in targets:
        target.unlink()
    for directory in sorted(resource_dirs, key=lambda path: len(path.parts), reverse=True):
        if not any(directory.iterdir()):
            directory.rmdir()
    desktop = checked_path(root, 'etc/desktop')
    if desktop.is_dir() and not desktop.is_symlink() and not any(desktop.iterdir()):
        desktop.rmdir()
    return targets


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('esp', type=Path)
    args = parser.parse_args()
    for path in clean(args.esp):
        print(f'Removed retired generated alias: {path}')
