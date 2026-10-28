#!/usr/bin/env python3
"""重做统一 QEMU 镜像前，将离线磁盘中的应用数据同步回部署树。"""
import argparse
from pathlib import Path
import re
import subprocess
import tempfile


def offline(image):
    result = subprocess.run(['fuser', '-s', str(image)], capture_output=True)
    if result.returncode != 1:
        raise ValueError('磁盘正在使用或无法确认离线状态，拒绝读取应用数据')


def identity(image):
    info = image.stat()
    return info.st_dev, info.st_ino, info.st_size, info.st_mtime_ns, info.st_ctime_ns


def preserve(image, root):
    image, root = Path(image), Path(root)
    if root.name != 'esp' or root.is_symlink() or not root.is_dir():
        raise ValueError('只能更新真实 ESP 暂存目录')
    if not image.exists():
        return 0
    if image.is_symlink() or not image.is_file():
        raise ValueError('磁盘镜像必须是普通文件')
    offline(image)
    initial = identity(image)
    subprocess.run(['mdir', '-i', str(image), '::/'], check=True,
                   stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
    apps = root/'usr/apps'
    if apps.is_symlink() or apps.parent.is_symlink():
        raise ValueError('应用目录不能经过符号链接')
    count = 0
    for app in sorted(apps.iterdir()):
        if app.is_symlink():
            raise ValueError('应用目录不能是符号链接')
        if not app.is_dir() or not re.fullmatch(r'[A-Za-z0-9_-]+', app.name):
            continue
        source = '::/usr/apps/'+app.name+'/data'
        result = subprocess.run(['mdir', '-b', '-i', str(image), source],
                                stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        if result.returncode == 1:
            continue
        result.check_returncode()
        target = app/'data'
        if target.is_symlink() or (target.exists() and not target.is_dir()):
            raise ValueError('应用数据目标必须是实际目录')
        with tempfile.TemporaryDirectory(prefix='.data-sync-', dir=app) as directory:
            temporary = Path(directory)
            payload = temporary/'payload'
            subprocess.run(['mcopy', '-s', '-i', str(image), source, str(payload)],
                           check=True, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
            if not payload.is_dir() or any(p.is_symlink() for p in payload.rglob('*')):
                raise ValueError('磁盘应用数据目录结构无效')
            offline(image)
            if identity(image) != initial:
                raise ValueError('磁盘在读取期间发生变化，保留现有部署数据')
            # 先完整提取再替换；失败时恢复原目录，避免留下半份配置或数据库。
            previous = temporary/'previous'
            if target.exists():
                target.rename(previous)
            try:
                payload.rename(target)
            except BaseException:
                if previous.exists():
                    previous.rename(target)
                raise
            count += 1
    return count


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('image', type=Path)
    parser.add_argument('esp', type=Path)
    args = parser.parse_args()
    print('QEMU_APP_DATA_PRESERVED apps='+str(preserve(args.image, args.esp)))
