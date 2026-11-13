"""Exercise the actual Chromium compiler wrapper's target header selection."""
import argparse
import os
from pathlib import Path
import subprocess
import sys
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from chromium_source import chromium_source

parser = argparse.ArgumentParser()
parser.add_argument('--chromium-source', type=Path)
args = parser.parse_args()
if args.chromium_source is None:
    args.chromium_source = chromium_source()
root = Path(__file__).resolve().parents[2]
wrapper = args.chromium_source / 'build/toolchain/native_os/clang'
gn = args.chromium_source / 'build/toolchain/native_os/BUILD.gn'
assert '--native-os-root=\\"$native_os_root\\"' in gn.read_text()
toolchain = gn.read_text()
assert '-D__NATIVE_OS__' in toolchain and '__LITEOS__' not in toolchain
assert 'native_os_root = ""' in toolchain
assert '/build-debian' not in toolchain
assert 'libsystemc.so.1' in toolchain and 'libliteosc.so.1' not in toolchain
assert '-Wl,-e,libc_crt_entry' in toolchain
assert '-dynamic-linker,/lib/ld-linux-system.so.1' in toolchain
assert 'native_os_chromium = false' in (args.chromium_source/'build/config/BUILDCONFIG.gn').read_text()
assert 'native_os_ozone_graph_only = false' in (args.chromium_source/'BUILD.gn').read_text()
windows = (args.chromium_source/'build/toolchain/gcc_toolchain.gni').read_text()
assert 'getenv("OS_ROOT")' in windows
assert 'C:/Users/Admin/Desktop' not in windows
rust_wrapper = (args.chromium_source/'build/toolchain/native_os/host_rust_link.bat').read_text()
assert '%~dp0host_rust_link.py' in rust_wrapper
assert 'C:\\Users\\Admin' not in rust_wrapper

def probe(extra, environment):
    return subprocess.run([str(wrapper), *extra, '-E', '-x', 'c', '/dev/null', '-v'],
                          env={**os.environ, **environment}, capture_output=True,
                          text=True, timeout=30)

# 未选择工作区时应明确失败，不能猜测使用历史副本。
result = probe([], {'OS_ROOT': ''})
assert result.returncode != 0 and 'Invalid native OS header root' in result.stderr

# GN 参数必须覆盖继承自环境、指向其他工作区的值。Clang 不得直接收到仅供包装器
# 使用的标志。
result = probe([f'--native-os-root={root}'], {'OS_ROOT': '/nonexistent/old-root'})
assert result.returncode == 0, result.stderr
assert str(root / 'user/libc/include') in result.stderr
assert str(root / 'include') in result.stderr
assert '/nonexistent/old-root' not in result.stderr

# 已有的显式配置构建脚本仍保留环境变量支持。
result = probe([], {'OS_ROOT': str(root)})
assert result.returncode == 0, result.stderr
assert str(root / 'user/libc/include') in result.stderr

# 缓存编译器发现过程不会使用 GN 专用的根目录参数。
# 使用与构建器相同的显式环境变量测试其版本探测。
result = subprocess.run([str(wrapper), '--version'],
                        env={**os.environ, 'OS_ROOT': str(root)},
                        capture_output=True, text=True, timeout=30)
assert result.returncode == 0, result.stderr
assert 'clang version' in result.stdout

# 所选根目录不可用时，绝不能静默回退。
result = probe(['--native-os-root=/nonexistent/selected-root'],
               {'OS_ROOT': str(root)})
assert result.returncode != 0
assert 'Invalid native OS header root' in result.stderr
print('Chromium selected target header root: PASS')
