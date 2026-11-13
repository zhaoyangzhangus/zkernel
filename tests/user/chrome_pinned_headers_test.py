"""Preprocess only: guest and generator headers must not come from the host."""
import os
from pathlib import Path
import re
import subprocess
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from chromium_source import chromium_source

root = Path(__file__).resolve().parents[2]
source = chromium_source()
sdk = source/'build/linux/debian_bullseye_amd64-sysroot'
# Windows 工作区可能会把符号链接保留为无效的重解析标记字符串。
# 在大型 SDK 重建/链接之前，先检查基础运行时链接。
for relative in ('lib64/ld-linux-x86-64.so.2',
                 'usr/lib/x86_64-linux-gnu/libutil.so',
                 'usr/lib/x86_64-linux-gnu/librt.so',
                 'usr/lib/x86_64-linux-gnu/libpthread.so',
                 'usr/lib/x86_64-linux-gnu/libdl.so',
                 'usr/lib/x86_64-linux-gnu/libexpat.so',
                 'usr/lib/x86_64-linux-gnu/libresolv.so',
                 'usr/lib/x86_64-linux-gnu/libz.so'):
    path = sdk / relative
    assert path.is_file(), 'Broken fixed SDK runtime link: ' + str(path)
    assert path.resolve().is_relative_to(sdk.resolve()), \
        'Fixed SDK link escapes into host filesystem: ' + str(path)
args = (source/'out/liteos-debian/args.gn').read_text()
assert re.search(r'^use_sysroot\s*=\s*true$', args, re.M)
assert re.search(r'^sysroot = "//build/linux/debian_bullseye_amd64-sysroot"$', args, re.M)
for option in ('use_qt5', 'use_qt6', 'rtc_use_pipewire'):
    assert re.search(r'^' + option + r'\s*=\s*false$', args, re.M), \
        'Fixed SDK must not implicitly enable a Linux desktop backend: ' + option
compiler = source/'third_party/llvm-build/Release+Asserts/bin/clang'
wrapper = source/'build/toolchain/native_os/clang'
environment = dict(os.environ, CPATH='/usr/include',
                   C_INCLUDE_PATH='/usr/include', CPLUS_INCLUDE_PATH='/usr/include')
probe = '#include <stdint.h>\n#include <linux/limits.h>\n'
for name, command in (
    ('generator', [str(compiler), '--sysroot='+str(sdk)]),
    ('guest', [str(wrapper), '--native-os-root='+str(root),
               '--native-os-glibc-root='+str(root/'build/nvidia/host-deps/glibc')]),
):
    env = environment.copy()
    if name == 'generator':
        # 统一启动器会为标准 Chromium 工具清理这些环境变量。
        for key in ('CPATH', 'C_INCLUDE_PATH', 'CPLUS_INCLUDE_PATH'):
            env.pop(key)
    result = subprocess.run([*command, '-M', '-x', 'c', '-'], input=probe,
                            text=True, capture_output=True, env=env,
                            check=True, timeout=15)
    assert str(sdk/'usr/include/linux/limits.h') in result.stdout
    assert not re.search(r'(?<!\S)/usr/include(?:/|\s)', result.stdout)
    print(name + ': pinned SDK dependency paths; no host /usr/include')
result = subprocess.run([str(wrapper), '--native-os-root='+str(root),
                         '-E', '-x', 'c', '-'], input=probe, text=True,
                        capture_output=True,
                        env=dict(environment, OS_CLANG='/nonexistent/compiler'),
                        timeout=15)
assert result.returncode != 0 and 'Missing pinned Chromium compiler' in result.stderr
print('Missing pinned compiler rejected; no host compiler fallback; no build performed')
