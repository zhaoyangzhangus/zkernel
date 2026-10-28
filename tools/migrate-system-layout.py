#!/usr/bin/env python3
"""将旧系统配置迁入现有 OS 目录，并清理部署树中的空根目录。"""
import argparse
import filecmp
from pathlib import Path


def migrate(root):
    root = Path(root)
    if root.name != 'esp' or root.is_symlink() or not root.is_dir():
        raise ValueError('只能迁移真实的 ESP 暂存目录')
    old, destination = root/'etc', root/'EFI/OS'
    for directory in (old, root/'EFI', destination):
        if directory.is_symlink():
            raise ValueError('系统目录不能是符号链接：'+str(directory))
    if (root/'log').is_symlink() or ((root/'log').exists() and not (root/'log').is_dir()):
        raise ValueError('日志目录必须是实际目录')
    pending = []
    if old.exists():
        for source in old.rglob('*'):
            if source.is_symlink():
                raise ValueError('拒绝迁移配置符号链接：'+str(source))
            if source.is_file():
                pending.append((source, destination/source.relative_to(old)))
    for name in ('init', 'init-runtime'):
        source = root/name
        if source.exists() or source.is_symlink():
            pending.append((source, destination/name))
    for source in root.glob('*.log'):
        pending.append((source, root/'log'/source.name))
    source = destination/'realtest.log'
    if source.exists() or source.is_symlink():
        pending.append((source, root/'log/realtest.log'))
    targets = {}
    # 全部目标通过校验以后才移动，冲突时保留原文件。
    for source, target in pending:
        if source.is_symlink() or not source.is_file():
            raise ValueError('只能迁移普通文件：'+str(source))
        for parent in (target, *target.parents):
            if parent == root:
                break
            if parent.is_symlink():
                raise ValueError('迁移目标不能经过符号链接：'+str(parent))
        previous = targets.get(target, target)
        if previous.exists() and (not previous.is_file() or
                                  not filecmp.cmp(source, previous, shallow=False)):
            raise ValueError('新旧系统文件内容冲突，未删除旧文件：'+str(target))
        targets[target] = source
    for source, target in pending:
        target.parent.mkdir(parents=True, exist_ok=True)
        if target.exists():
            source.unlink()
        else:
            source.rename(target)
    if old.exists():
        for directory in sorted(old.rglob('*'), key=lambda p: len(p.parts), reverse=True):
            if directory.is_dir() and not any(directory.iterdir()):
                directory.rmdir()
    (root/'log').mkdir(exist_ok=True)
    for name in ('etc', 'native', 'boot', 'bin', 'tmp', 'Downloads'):
        directory = root/name
        if directory.is_dir() and not directory.is_symlink() and not any(directory.iterdir()):
            directory.rmdir()
    return len(pending)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('esp', type=Path)
    args = parser.parse_args()
    print('SYSTEM_LAYOUT_MIGRATED files='+str(migrate(args.esp)))
