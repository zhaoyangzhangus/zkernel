#!/usr/bin/env python3
"""Stage verified NVIDIA runtime, GSP firmware and a freshly built cube into BUILD/esp."""
import argparse
import importlib.util
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('nvidia_builder', ROOT / 'tools/nvidia/build-driver.py')
builder = importlib.util.module_from_spec(spec)
spec.loader.exec_module(builder)


def copy_changed(source, target, digest=builder.sha):
    expected = digest(source)
    if target.exists() and expected == digest(target):
        return False
    target.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='.' + target.name + '.nvidia-',
                                     dir=target.parent) as directory:
        temporary = Path(directory) / target.name
        shutil.copyfile(source, temporary)
        if digest(temporary) != expected:
            raise RuntimeError('deployment source changed during copy: ' + str(source))
        os.replace(temporary, target)
    return True


def link_cube(cube, command, inputs):
    signature = {'command': command, 'compiler': builder.compiler_identity(command[0]),
                 'inputs': {str(p): builder.sha(p) for p in inputs}}
    stamp = cube.with_suffix('.link.json')
    previous = builder.read_receipt(stamp)
    if (cube.exists() and previous.get('inputs') == signature and
            previous.get('output') == builder.sha(cube)):
        return False
    subprocess.run(command, check=True)
    output_digest = builder.publish_binary(str(cube) + '.tmp', cube)
    builder.write_changed(stamp, json.dumps({'inputs': signature, 'output': output_digest}, sort_keys=True))
    return True


def build_cube(out, compiler, glibc):
    """Build the canonical cube without copying into any deployment tree."""
    dev = Path(glibc) / 'libc6-dev-extract/usr'
    run = Path(glibc) / 'libc6-extract/lib/x86_64-linux-gnu'
    lib = dev / 'lib/x86_64-linux-gnu'
    cube = out / 'cube'
    obj = out / 'cube.o'
    flags = ['-std=gnu11', '-O2', '-Wall', '-Wextra', '-Werror', '-D_GNU_SOURCE',
             '-DCUBE_INTERACTIVE=1', '-fno-pie', '-isystem', str(dev / 'include'),
             '-isystem', str(dev / 'include/x86_64-linux-gnu'), '-Iinclude']
    builder.compile_one((Path('tools/nvidia/opengl_cube_demo.c'), obj, compiler, flags))
    inputs = [lib / 'crt1.o', lib / 'crti.o', obj, run / 'libc.so.6',
              lib / 'libc_nonshared.a', lib / 'crtn.o']
    link = [compiler, '-nostdlib', '-nodefaultlibs', '-nostartfiles', '-no-pie',
            '-Wl,--build-id=none', '-Wl,-dynamic-linker,/usr/apps/cube/lib/ld-linux-x86-64.so.2',
            '-Wl,-rpath,/usr/apps/cube/lib',
            '-Wl,--disable-new-dtags', '-Wl,--allow-shlib-undefined',
            '-Wl,-rpath-link,' + str(run), '-Wl,-z,now',
            *map(str, inputs), '-o', str(cube) + '.tmp']
    link_cube(cube, link, inputs)
    return cube


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--build', default='build')
    parser.add_argument('--cc', default='x86_64-linux-gnu-gcc')
    parser.add_argument('--userspace', default='build/nvidia/userspace')
    parser.add_argument('--manifest', default='build/nvidia/userspace-595.84.json')
    parser.add_argument('--package', default='build/nvidia/official-595.84/extracted-gitbash')
    parser.add_argument('--glibc', default='build/nvidia/host-deps/glibc')
    parser.add_argument('--build-only', action='store_true',
                        help='Build cube only; do not stage runtime, firmware or ESP files')
    args = parser.parse_args()
    os.chdir(ROOT)
    build = Path(args.build)
    if build.is_absolute() or '..' in build.parts:
        raise RuntimeError('BUILD must stay inside the repository')
    esp = build / 'esp'
    out = build / 'nvidia/runtime'
    out.mkdir(parents=True, exist_ok=True)
    if args.build_only:
        cube = build_cube(out, args.cc, args.glibc)
        print('CUBE_BUILD_READY=' + str(cube), flush=True)
        return
    checksum = builder.FileDigests()
    manifest = json.loads(Path(args.manifest).read_text(encoding='utf-8-sig'))
    normalized = out / 'userspace-595.84.json'
    builder.write_changed(normalized, json.dumps(manifest, sort_keys=True))
    subprocess.run(['python3', 'tools/nvidia/check_userspace_59584.py',
                    '--root', args.userspace, '--manifest', str(normalized),
                    '--package-root', args.package], check=True)
    # Only manifest-approved files and aliases are copied, never an old ESP/kernel.
    entries = [entry['target'] for entry in manifest['files']]
    entries += [entry['alias'] for entry in manifest['aliases']]
    for name in sorted(set(entries)):
        relative = Path(name.replace('\\', '/'))
        if relative.is_absolute() or '..' in relative.parts:
            raise RuntimeError('invalid userspace manifest target: ' + name)
        if relative.parts[0] not in ('lib', 'lib64'):
            continue
        copy_changed(Path(args.userspace) / relative, esp / 'usr/apps/cube/lib' / relative.name, checksum)
    copy_changed(Path('tools/nvidia/phase10_glvnd_10_nvidia.json'),
                 esp / 'usr/apps/cube/egl/10_nvidia.json', checksum)
    firmware_hashes = {
        'gsp_ga10x.bin': '1c107d18cd63a08fd4ba78706809825c756db2b4a556c453fb5c003976839946',
        'gsp_tu10x.bin': '76e4d216b8f80c995f97e114e7aa1b87475f6d694cb78d78ce2f52070fb30739',
    }
    for name, digest in firmware_hashes.items():
        source = Path(args.package) / 'firmware' / name
        if checksum(source) != digest:
            raise RuntimeError('firmware checksum mismatch: ' + name)
        copy_changed(source, esp / 'EFI/OS/firmware' / name, checksum)
    run = Path(args.glibc) / 'libc6-extract/lib/x86_64-linux-gnu'
    for name in ('ld-linux-x86-64.so.2', 'libc.so.6', 'libdl.so.2',
                 'libm.so.6', 'libpthread.so.0', 'librt.so.1'):
        source = run / name
        verified = esp / 'usr/apps/cube/lib' / name
        if not verified.exists() or checksum(source) != checksum(verified):
            raise RuntimeError('glibc does not match verified closure: ' + name)
    cube = build_cube(out, args.cc, args.glibc)
    copy_changed(cube, esp / 'usr/apps/cube/cube', checksum)
    deployed = [esp / 'usr/apps/cube/cube', esp / 'usr/apps/cube/lib/ld-linux-x86-64.so.2',
                esp / 'usr/apps/cube/egl/10_nvidia.json']
    deployed += [esp / 'EFI/OS/firmware' / name for name in firmware_hashes]
    deployed += [p for p in (esp / 'usr/apps/cube/lib').iterdir() if p.is_file()]
    copy_changed(Path(args.package) / 'LICENSE', esp / 'usr/apps/cube/licenses/NVIDIA-LICENSE', checksum)
    deployed.append(esp / 'usr/apps/cube/licenses/NVIDIA-LICENSE')
    inventory = {str(p.relative_to(esp)): checksum(p) for p in deployed}
    builder.write_changed(out / 'deployed.json', json.dumps(inventory, indent=2, sort_keys=True))
    print('NVIDIA_PAYLOAD_READY=' + str(esp), flush=True)


if __name__ == '__main__':
    main()
