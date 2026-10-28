#!/usr/bin/env python3
"""核对应用私有运行时，成功后清理已被替代的全局兼容目录。"""
import argparse
import json
import os
from pathlib import Path
import re
import shutil
import subprocess


def inspect(path, option):
    return subprocess.check_output(['readelf', option, str(path)], text=True,
                                   env=dict(os.environ, LC_ALL='C'))


def check(root):
    apps = root/'usr/apps'
    checked = 0
    for app in sorted(apps.iterdir()):
        if not app.is_dir():
            raise ValueError('应用目录中出现散落文件：'+str(app))
        private = app/'lib'
        binaries = []
        for path in app.iterdir():
            if path.is_file():
                with path.open('rb') as stream:
                    if stream.read(4) == b'\x7fELF':
                        binaries.append(path)
        libraries = list(private.glob('*.so*')) if private.is_dir() else []
        for path in [*binaries, *libraries]:
            dynamic = inspect(path, '-dW')
            if '/compat/linux' in dynamic:
                raise ValueError('应用仍引用旧兼容目录：'+str(path))
            needed = re.findall(r'\(NEEDED\).*Shared library: \[([^\]]+)\]', dynamic)
            for name in needed:
                if '/' in name or not name:
                    raise ValueError('依赖名称必须属于当前应用或系统公共库：'+name)
                if not (private/name).is_file() and not (root/'lib'/name).is_file():
                    raise ValueError('应用依赖缺失：'+str(path)+' -> '+name)
            if path in binaries:
                headers = inspect(path, '-lW')
                for interpreter in re.findall(r'Requesting program interpreter: ([^\]]+)\]', headers):
                    allowed = ('/lib/ld-system.so.1', '/lib/ld-linux-system.so.1',
                               '/usr/apps/'+app.name+'/lib/ld-linux-x86-64.so.2')
                    if interpreter not in allowed or not (root/interpreter.lstrip('/')).is_file():
                        raise ValueError('应用加载器路径无效：'+str(path)+' -> '+interpreter)
                    if interpreter.startswith('/usr/apps/'):
                        paths = re.findall(r'\((?:RPATH|RUNPATH)\).*\[([^\]]+)\]', dynamic)
                        own = '/usr/apps/'+app.name+'/lib'
                        if paths != [own]:
                            raise ValueError('应用必须从自己的目录加载私有库：'+str(path))
                checked += 1
        for directory in ('vulkan', 'egl'):
            for path in (app/directory).glob('*.json'):
                library = json.loads(path.read_text())['ICD']['library_path']
                if ('..' in Path(library).parts or
                    not library.startswith('/usr/apps/'+app.name+'/lib/') or
                    not (root/library.lstrip('/')).is_file()):
                    raise ValueError('图形配置引用其他目录：'+str(path))
    return checked


def finalize(root, clean=False, strict=False):
    root = Path(root)
    if root.name != 'esp' or root.is_symlink() or not root.is_dir():
        raise ValueError('只允许处理真实 ESP 暂存目录')
    count = check(root)
    if not clean:
        if strict:
            for name in ('init', 'init-runtime', 'native', 'etc', 'compat', 'lib64', 'lib/x86_64-linux-gnu',
                         'usr/share/glvnd', 'usr/share/vulkan', 'etc/chrome-gpu-mode'):
                if (root/name).exists():
                    raise ValueError('部署树仍有旧的应用公共目录：'+name)
        return count
    # 清理前要求所有接收者已部署，避免半迁移状态误删仍需使用的运行库。
    for app in ('chrome', 'cube', 'vulkan-probe', 'vulkan-video'):
        target = root/'usr/apps'/app
        if not (target/app).is_file() or not (target/'lib/ld-linux-x86-64.so.2').is_file():
            raise ValueError('尚未完成私有运行时部署：'+app)
    retired = ('compat', 'lib64', 'lib/x86_64-linux-gnu',
               'usr/share/glvnd', 'usr/share/vulkan', 'usr/share/licenses/vulkan',
               'etc/chrome-gpu-mode', 'sbin/vulkan-probe', 'sbin/vulkan-video',
               'video-test.h264')
    targets = [root/name for name in retired]
    for target in targets:
        for parent in target.parents:
            if parent == root:
                break
            if parent.is_symlink():
                raise ValueError('拒绝清理经过符号链接的路径：'+str(target))
    for target in targets:
        if target.is_symlink() or target.is_file():
            target.unlink()
        elif target.is_dir():
            shutil.rmtree(target)
    return count


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('esp', type=Path)
    parser.add_argument('--clean', action='store_true')
    parser.add_argument('--strict', action='store_true')
    args = parser.parse_args()
    print('APP_LAYOUT_PASS binaries='+str(finalize(args.esp, args.clean, args.strict)))
