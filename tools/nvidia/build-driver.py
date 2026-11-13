#!/usr/bin/env python3
"""Build the pinned, unmodified NVIDIA RM and current system adapters on Linux."""
import argparse
import concurrent.futures
import functools
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import threading

ROOT = Path(__file__).resolve().parents[2]
VENDOR = ROOT / 'third_party/nvidia/595.84'
SOURCE = VENDOR / 'open-gpu-kernel-modules'
ABI = ROOT / 'kernel/drivers/gpu/nvidia/vendor_abi'
RM = ROOT / 'kernel/drivers/gpu/nvidia/rm'
INCLUDES = '''src/nvidia/kernel/inc src/nvidia/interface
src/common/sdk/nvidia/inc src/common/sdk/nvidia/inc/hw
src/nvidia/arch/nvalloc/common/inc src/nvidia/arch/nvalloc/common/inc/gsp
src/nvidia/arch/nvalloc/common/inc/deprecated src/nvidia/arch/nvalloc/unix/include
src/nvidia/inc src/nvidia/inc/os src/common/shared/inc src/common/inc
src/common/uproc/os/libos-v2.0.0/include src/common/uproc/os/common/include
src/common/inc/swref src/common/inc/swref/published src/nvidia/generated
src/common/nvswitch/kernel/inc src/common/nvswitch/interface src/common/nvswitch/common/inc
src/common/inc/displayport src/common/nvlink/interface src/common/nvlink/inband/interface
src/nvidia/src/mm/uvm/interface src/nvidia/inc/libraries src/nvidia/src/libraries
src/nvidia/inc/kernel src/nvidia/src/libraries/libspdm/3.5.0/include
src/nvidia/src/libraries/libspdm/3.5.0/include/hal
src/nvidia/src/libraries/libspdm/3.5.0/os_stub/include
src/nvidia/src/libraries/libspdm/3.5.0/os_stub
src/nvidia/src/libraries/libspdm/3.5.0/os_stub/cryptlib_null
src/nvidia/src/libraries/libspdm/nvidia
src/common/mbedtls/3.6.2/include src/common/mbedtls/3.6.2/nvidia'''.split()
DEFINES = '''NVRM NV_LINUX NV_UNIX NV_X86_64 NV_ARCH_BITS=64 _LANGUAGE_C __NO_CTYPE
LOCK_VAL_ENABLED=0 PORT_ATOMIC_64_BIT_SUPPORTED=1 PORT_IS_KERNEL_BUILD=1
PORT_IS_CHECKED_BUILD=0 RS_STANDALONE=0 RS_STANDALONE_TEST=0
RS_COMPATABILITY_MODE=1 RS_PROVIDES_API_STATE=0 NV_CONTAINERS_NO_TEMPLATES
INCLUDE_NVLINK_LIB INCLUDE_NVSWITCH_LIB NV_PRINTF_STRINGS_ALLOWED=0
NV_ASSERT_FAILED_USES_STRINGS=0 PORT_ASSERT_FAILED_USES_STRINGS=0'''.split()
DEFINES += ['PORT_MODULE_' + m + '=1' for m in
            'atomic core cpu crypto debug memory safe string sync thread util'.split()]
DEFINES += ['PORT_MODULE_' + m + '=0' for m in 'example mmio time'.split()]
FLAGS = '''-O2 -ffreestanding -fno-stack-protector -fno-common -fno-pic -fno-pie
-fno-strict-aliasing -fno-omit-frame-pointer -ffunction-sections -fdata-sections
-msoft-float -mno-red-zone -mno-mmx -mno-sse -mno-sse2 -mno-3dnow -mcmodel=kernel
-Wformat=2 -Wno-unused-parameter -Werror-implicit-function-declaration
-Wwrite-strings -Wundef'''.split()


def sha(path):
    h = hashlib.sha256()
    with Path(path).open('rb') as f:
        for block in iter(lambda: f.read(1024 * 1024), b''):
            h.update(block)
    return h.hexdigest()


class FileDigests:
    """Invocation-local content checks; invalidate even when mtime is restored."""
    def __init__(self, snapshot=False):
        self.cache = {}
        self.paths = {}
        self.lock = threading.Lock()
        self.reads = 0
        self.hits = 0
        self.snapshot = snapshot

    @staticmethod
    def fingerprint(path):
        stat = path.stat()
        return stat.st_dev, stat.st_ino, stat.st_size, stat.st_mtime_ns, stat.st_ctime_ns

    def __call__(self, path):
        raw_path = os.fspath(path)
        # 单飞机制：多个编译工作线程会共享许多头文件。
        with self.lock:
            path = self.paths.get(raw_path)
            if path is None:
                path = Path(os.path.abspath(raw_path))
                self.paths[raw_path] = path
            cached = self.cache.get(path)
            if self.snapshot and cached is not None:
                self.hits += 1
                return cached[1]
            for _ in range(3):
                before = self.fingerprint(path)
                cached = self.cache.get(path)
                if cached is not None and cached[0] == before:
                    self.hits += 1
                    return cached[1]
                digest = sha(path)
                self.reads += 1
                if self.fingerprint(path) == before:
                    self.cache[path] = before, digest
                    return digest
        raise RuntimeError('input changed during checksum: ' + str(path))

    def validate(self):
        # 发布任何驱动之前都要验证冻结的输入快照。
        # 若构建期间头文件发生更改，则中止构建，而不是为依赖关系基于其他版本检查过的
        # 对象签发有效标记。
        with self.lock:
            for path, (fingerprint, _digest) in self.cache.items():
                if self.fingerprint(path) != fingerprint:
                    raise RuntimeError('input changed during build: ' + str(path))


def read_receipt(path):
    try:
        value = json.loads(Path(path).read_text())
    except (ValueError, OSError):
        return {}
    return value if isinstance(value, dict) else {}


def verify_vendor(digest=sha):
    version = (VENDOR / 'VENDOR_VERSION').read_text()
    for expected in ('version=595.84', 'semantic_vendor_patches=0'):
        if expected not in version:
            raise RuntimeError('unexpected NVIDIA source version')
    count = 0
    for line in (VENDOR / 'VENDOR_SHA256SUMS').read_text().splitlines():
        if not line or line.startswith('#'):
            continue
        expected_digest, name = line.split('  ', 1)
        path = (VENDOR / name).resolve()
        if not path.is_relative_to(SOURCE.resolve()) or digest(path) != expected_digest.lower():
            raise RuntimeError('NVIDIA source checksum mismatch: ' + name)
        count += 1
    print('NVIDIA_SOURCE_VERIFIED=' + str(count), flush=True)


def write_changed(path, data):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    if not path.exists() or path.read_text() != data:
        temporary = path.with_suffix(path.suffix + '.tmp')
        temporary.write_text(data)
        os.replace(temporary, path)


def publish_binary(temporary, target):
    """Atomically publish changed bytes; identical output keeps its timestamp."""
    temporary, target = Path(temporary), Path(target)
    digest = sha(temporary)
    if target.exists() and sha(target) == digest:
        temporary.unlink()
    else:
        os.replace(temporary, target)
    return digest


def compiler_identity(compiler):
    executable = shutil.which(compiler)
    if executable is None:
        raise FileNotFoundError('tool not found: ' + str(compiler))
    return _compiler_identity(compiler, str(Path(executable).absolute()),
                              FileDigests.fingerprint(Path(executable)))


@functools.lru_cache(maxsize=128)
def _compiler_identity(compiler, _executable, _fingerprint):
    # 保留现有回执格式，但当 PATH 或实际可执行文件发生变化时刷新，包括替换后恢复原时间戳的情况。
    return subprocess.check_output([compiler, '--version'], text=True)


def compile_one(record, digest=sha, input_digest=None):
    input_digest = input_digest or digest
    source, output, compiler, flags = record
    source_path = str((ROOT / source).resolve())
    cmd = [compiler, *flags, '-MMD', '-MP', '-MF', str(output) + '.d',
           '-c', str(source), '-o', str(output) + '.tmp']
    key = hashlib.sha256((json.dumps(cmd) + compiler_identity(compiler)).encode()).hexdigest()
    signature = output.with_suffix('.json')
    if output.exists() and signature.exists():
        previous = read_receipt(signature)
        dependencies = previous.get('dependencies')
        if (previous.get('command') == key and previous.get('object') == digest(output) and
                isinstance(dependencies, dict) and source_path in dependencies):
            try:
                unchanged = isinstance(dependencies, dict) and dependencies and all(
                    isinstance(p, str) and isinstance(h, str) and
                    input_digest(p) == h for p, h in dependencies.items())
            except OSError:
                unchanged = False
            if unchanged:
                return False
    output.parent.mkdir(parents=True, exist_ok=True)
    result = subprocess.run(cmd, cwd=ROOT, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, text=True)
    output.with_suffix('.log').write_text(result.stdout)
    if result.returncode:
        raise RuntimeError(str(source) + '\n' + result.stdout[-12000:])
    dep_text = Path(str(output) + '.d').read_text().replace('\\\n', '')
    # 所有生成输入都使用不含空白的仓库相对路径。
    deps = dep_text.split(':', 1)[1].split('\n', 1)[0].split()
    dependencies = {str((ROOT / p).resolve()): input_digest(ROOT / p) for p in deps}
    dependencies[source_path] = input_digest(source_path)
    output_digest = publish_binary(str(output) + '.tmp', output)
    write_changed(signature, json.dumps({'command': key, 'object': output_digest,
                                        'dependencies': dependencies}, sort_keys=True))
    return True


def weaken_object(obj, objcopy, digest=sha):
    weak = obj.with_name(obj.stem + '-weak.o')
    receipt = weak.with_suffix('.json')
    inputs = {'source': digest(obj), 'tool': compiler_identity(objcopy), 'command': objcopy}
    previous = read_receipt(receipt)
    if (not weak.exists() or previous.get('inputs') != inputs or
            previous.get('output') != digest(weak)):
        temporary = str(weak) + '.tmp'
        subprocess.run([objcopy, '--weaken', str(obj), temporary], check=True)
        output_digest = publish_binary(temporary, weak)
        write_changed(receipt, json.dumps({'inputs': inputs, 'output': output_digest}, sort_keys=True))
    return weak


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--build', default='build')
    parser.add_argument('--cc', default='x86_64-linux-gnu-gcc')
    parser.add_argument('--cxx', default='x86_64-linux-gnu-g++')
    parser.add_argument('--ld', default='x86_64-linux-gnu-ld')
    parser.add_argument('--objcopy', default='x86_64-linux-gnu-objcopy')
    parser.add_argument('--jobs', type=int, default=4)
    parser.add_argument('--irq-mode', choices=('msi', 'msix'), default='msix')
    args = parser.parse_args()
    os.chdir(ROOT)
    build = Path(args.build)
    if build.is_absolute() or '..' in build.parts or re.search(r'\s', str(build)):
        raise RuntimeError('build must be a whitespace-free repository-relative path')
    out = build / 'nvidia/driver'
    digests = FileDigests(snapshot=True)
    verify_vendor(digests)
    generated = out / 'generated/g_nvid_string.c'
    write_changed(generated, 'const char NVRM_ID[] = "nvidia id: UNIX Open Kernel Module for kernel RM, baseline 595.84";\nconst char *const pNVRM_ID = NVRM_ID + 11;\n')
    # 使用相对路径也能让依赖文件在 USB 迁移后保持可移植。
    vendor = SOURCE.relative_to(ROOT)
    inc = ['-I' + str(vendor / p) for p in INCLUDES]
    base = ['-include', str(vendor / 'src/common/sdk/nvidia/inc/cpuopsys.h'),
            *inc, *['-D' + d for d in DEFINES], *FLAGS]
    sources = []
    for line in (SOURCE / 'src/nvidia/srcs.mk').read_text().splitlines():
        match = re.match(r'\s*(SRCS|SRCS_CXX)\s*\+=\s+(\S+)\s*$', line)
        if match:
            if '$(' in match[2]:
                raise RuntimeError('unexpanded vendor source: ' + match[2])
            sources.append(vendor / 'src/nvidia' / match[2])
    sources.append(generated)
    records = []
    for index, source in enumerate(sources):
        cxx = source.suffix == '.cpp'
        language = ['-std=gnu++11', '-fno-operator-names', '-fno-rtti',
                    '-fno-exceptions', '-fcheck-new'] if cxx else ['-std=gnu11']
        cflags = [f for f in base if not (cxx and f == '-Werror-implicit-function-declaration')]
        records.append((source, out / 'rm' / f'{index:04d}.o',
                        args.cxx if cxx else args.cc, [*cflags, *language]))
    abi_inc = ['-Iinclude', '-Ikernel/drivers/gpu/nvidia/port/include',
               '-I' + str(ABI.relative_to(ROOT) / 'include'),
               '-I' + str(vendor / 'kernel-open'),
               '-I' + str(vendor / 'kernel-open/common/inc')]
    for source in sorted(ABI.glob('*')):
        if source.suffix not in ('.c', '.S'):
            continue
        flags = FLAGS if source.suffix == '.S' else [
            '-include', str(vendor / 'src/common/sdk/nvidia/inc/cpuopsys.h'),
            *abi_inc, *inc, *['-D' + d for d in DEFINES], *FLAGS, '-std=gnu11',
            '-DOS_KERNEL_BUILD', '-DOS_NVIDIA_PHASE5=1']
        records.append((source.relative_to(ROOT), out / 'abi' / (source.stem + '.o'),
                        args.cc, flags))
    bridge_inc = ['-Iinclude', '-I' + str(RM.relative_to(ROOT) / 'include')]
    for name in ('nv_rm_vendor_bridge', 'nv_rm_workqueue'):
        records.append((RM.relative_to(ROOT) / (name + '.c'), out / (name + '.o'),
                        args.cc, [*bridge_inc, *base, '-std=gnu11',
                                  '-DOS_KERNEL_BUILD', '-DOS_NVIDIA_PHASE5=1',
                                  '-DOS_NVIDIA_PHASE6=0',
                                  '-DOS_NVIDIA_PREFER_MSI=' + str(int(args.irq_mode == 'msi'))]))
    records.append((Path('boot/uefi/sha256.c'), out / 'sha256.o', args.cc,
                    [*FLAGS, '-Iinclude']))
    rebuilt = 0
    with concurrent.futures.ThreadPoolExecutor(max_workers=max(1, min(args.jobs, 4))) as pool:
        for index, changed in enumerate(pool.map(
                lambda record: compile_one(record, input_digest=digests), records), 1):
            rebuilt += changed
            if index % 25 == 0 or index == len(records):
                print(f'NVIDIA_COMPILE={index}/{len(records)} rebuilt={rebuilt}', flush=True)
    digests.validate()
    objects = []
    for source, obj, _compiler, _flags in records:
        if source.name.endswith('_unsupported.S'):
            # 失败关闭的占位项绝不能遮蔽实际存在的当前适配器。
            weak = weaken_object(obj, args.objcopy)
            objects.append(str(weak))
        else:
            objects.append(str(obj))
    response = out / 'objects.list'
    write_changed(response, '\n'.join(objects) + '\n')
    target = out / 'driver.o'
    link_signature = json.dumps({'recipe': sha(__file__), 'linker': compiler_identity(args.ld),
                                'objcopy': [args.objcopy, compiler_identity(args.objcopy)],
                                'objects': {p: sha(p) for p in objects}}, sort_keys=True)
    receipt = out / 'driver.link.json'
    previous = read_receipt(receipt)
    if (rebuilt or not target.exists() or previous.get('inputs') != link_signature
            or previous.get('output') != sha(target)):
        subprocess.run([args.ld, '-r', '-o', str(target) + '.tmp', '@' + str(response)],
                       check=True, cwd=ROOT)
        # RM 自带 libc 回退实现；这些函数由内核实现提供。
        subprocess.run([args.objcopy, '--weaken-symbol=memcpy', '--weaken-symbol=memset',
                        str(target) + '.tmp'], check=True)
        digests.validate()
        output_digest = publish_binary(str(target) + '.tmp', target)
        write_changed(receipt, json.dumps({'inputs': link_signature, 'output': output_digest}))
    print(f'NVIDIA_CHECKSUMS reads={digests.reads} reused={digests.hits}', flush=True)
    print('NVIDIA_DRIVER_READY=' + str(target), flush=True)


if __name__ == '__main__':
    main()
