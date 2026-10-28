#!/usr/bin/env python3
"""Validate Vulkan's complete ELF closure before publishing guest runtime files."""
import argparse
import importlib.util
import json
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]


def module(name, filename):
    import sys
    spec = importlib.util.spec_from_file_location(name, ROOT/'tools/nvidia'/filename)
    result = importlib.util.module_from_spec(spec)
    sys.modules[name] = result
    spec.loader.exec_module(result)
    return result


stage = module('vulkan_stage_helpers', 'stage-driver.py')
check = module('vulkan_elf_check', 'check_userspace_59584.py')
abi = module('vulkan_abi_check', 'vulkan_runtime_abi.py')

# These compiler modules are loaded by NVIDIA via dlopen, not DT_NEEDED.
# Keep them explicit roots so their own dependencies are validated as well.
NVIDIA_VULKAN_MODULES = ('libnvidia-glvkspirv.so.595.84',
                         'libnvidia-gpucomp.so.595.84',
                         'libnvidia-rtcore.so.595.84',
                         'libnvidia-eglcore.so.595.84')


def vulkan_closure(catalog, package):
    catalog = dict(catalog)
    for name in NVIDIA_VULKAN_MODULES:
        source = package/name
        if not source.is_file():
            raise RuntimeError('missing NVIDIA Vulkan runtime module: '+name)
        catalog[name] = source
    # NVIDIA 的无显示 Vulkan 初始化也会动态加载 EGL，不能只检查 DT_NEEDED。
    return closure(catalog, ('libvulkan.so.1', 'libGLX_nvidia.so.0',
                             'libEGL.so.1', 'libEGL_nvidia.so.0',
                             *NVIDIA_VULKAN_MODULES))


def closure(catalog, names):
    selected = {}
    pending = list(names)
    while pending:
        name = pending.pop()
        if name in selected:
            continue
        if name not in catalog:
            raise RuntimeError('missing Vulkan DT_NEEDED dependency: '+name)
        selected[name] = catalog[name]
        pending.extend(check.Elf64(catalog[name]).needed())
    return selected


def compile_shaders(out):
    arrays = []
    for stage_name in ('vert', 'frag'):
        output = out/('triangle-'+stage_name+'.inc')
        subprocess.run(['glslc', '--target-env=vulkan1.0', '-mfmt=c',
                        str(ROOT/('tools/nvidia/vulkan_triangle.'+stage_name)),
                        '-o', str(output)], check=True)
        arrays.append('static const uint32_t triangle_'+stage_name+'[] = '+output.read_text().strip()+';')
    stage.builder.write_changed(out/'vulkan_triangle_spirv.h', '\n'.join(arrays)+'\n')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', default='build', type=Path)
    parser.add_argument('--cc', default='x86_64-linux-gnu-gcc')
    args = parser.parse_args()
    if args.build.is_absolute() or '..' in args.build.parts:
        parser.error('BUILD must stay inside the project')
    build = ROOT/args.build
    out = build/'nvidia/vulkan'
    esp = build/'esp'
    runtime = build/'nvidia/host-deps/glibc/libc6-extract/lib/x86_64-linux-gnu'
    dev = build/'nvidia/host-deps/glibc/libc6-dev-extract/usr'
    package = build/'nvidia/official-595.84/extracted-gitbash'
    catalog = {p.name: p for p in (esp/'usr/apps/cube/lib').glob('*.so*') if p.is_file()}
    for directory in ('usr/lib/x86_64-linux-gnu', 'lib/x86_64-linux-gnu'):
        catalog.update({p.name: p for p in (out/'deps/root'/directory).glob('*.so*') if p.is_file()})
    catalog['libvulkan.so.1'] = out/'loader/loader/libvulkan.so.1'
    catalog['libGLX_nvidia.so.0'] = package/'libGLX_nvidia.so.595.84'
    selected = vulkan_closure(catalog, package)
    abi.check(runtime, selected.values())
    # Check relocation and symbol closure on a private tree before modifying ESP.
    with tempfile.TemporaryDirectory(prefix='.verify-', dir=out) as directory:
        temporary = Path(directory)
        for name, source in selected.items():
            stage.copy_changed(source, temporary/name)
        check.verify_elves(temporary)
    flags = ['-std=gnu11', '-O2', '-Wall', '-Wextra', '-Werror', '-D_POSIX_C_SOURCE=200809L',
             '-fno-pie', '-isystem', str(dev/'include'),
             '-isystem', str(dev/'include/x86_64-linux-gnu'), '-I', str(out/'sdk/include'), '-I', str(out), '-I', str(ROOT/'include')]
    compile_shaders(out)
    render_obj = out/'render-probe.o'
    stage.builder.compile_one((ROOT/'tools/nvidia/vulkan_render_probe.c', render_obj, args.cc, flags))
    shared_obj = out/'shared-probe.o'
    stage.builder.compile_one((ROOT/'tools/nvidia/vulkan_shared_probe.c', shared_obj, args.cc, flags+['-D_GNU_SOURCE']))
    lifetime_obj = out/'lifetime-probe.o'
    stage.builder.compile_one((ROOT/'tools/nvidia/vulkan_import_lifetime_probe.c', lifetime_obj, args.cc, flags))
    obj, binary = out/'probe.o', out/'vulkan-probe'
    stage.builder.compile_one((ROOT/'tools/nvidia/vulkan_probe.c', obj, args.cc, flags))
    lib = dev/'lib/x86_64-linux-gnu'
    inputs = [lib/'crt1.o', lib/'crti.o', obj, render_obj, shared_obj, lifetime_obj, selected['libvulkan.so.1'],
              runtime/'libc.so.6', lib/'libc_nonshared.a', lib/'crtn.o']
    command = [args.cc, '-nostdlib', '-nodefaultlibs', '-nostartfiles', '-no-pie',
               '-Wl,--build-id=none,-dynamic-linker,/usr/apps/vulkan-probe/lib/ld-linux-x86-64.so.2',
               '-Wl,-rpath,/usr/apps/vulkan-probe/lib', '-Wl,--disable-new-dtags',
               '-Wl,-rpath-link,'+str(runtime), *map(str, inputs), '-o', str(binary)+'.tmp']
    stage.link_cube(binary, command, inputs)
    abi.check(runtime, [binary])
    icd = json.loads((package/'nvidia_icd.json').read_text())
    if icd['ICD']['library_path'] != 'libGLX_nvidia.so.0':
        raise RuntimeError('unexpected NVIDIA Vulkan ICD library')
    files = {}
    for app in ('chrome', 'vulkan-probe'):
        appdir = esp/'usr/apps'/app
        icd['ICD']['library_path'] = '/usr/apps/'+app+'/lib/libGLX_nvidia.so.0'
        icd_source = out/(app+'-nvidia_icd.json')
        stage.builder.write_changed(icd_source, json.dumps(icd, indent=2)+'\n')
        files.update({appdir/'lib'/name: source for name, source in selected.items()})
        files[appdir/'lib/ld-linux-x86-64.so.2'] = runtime/'ld-linux-x86-64.so.2'
        files[appdir/'vulkan/nvidia_icd.json'] = icd_source
        egl_source = out/(app+'-egl.json')
        stage.builder.write_changed(egl_source, json.dumps({
            'file_format_version': '1.0.0', 'ICD': {
                'library_path': '/usr/apps/'+app+'/lib/libEGL_nvidia.so.0'}
        }, indent=2)+'\n')
        files[appdir/'egl/10_nvidia.json'] = egl_source
        for path in (out/'deps/root/usr/share/doc').glob('*/copyright'):
            files[appdir/'licenses'/path.parent.name/'copyright'] = path
        files[appdir/'licenses/nvidia/LICENSE'] = package/'LICENSE'
        for path in (out/'licenses/loader').rglob('*'):
            if path.is_file():
                files[appdir/'licenses/loader'/path.relative_to(out/'licenses/loader')] = path
    files[esp/'usr/apps/vulkan-probe/vulkan-probe'] = binary
    if not (out/'licenses/loader/LICENSE.txt').is_file():
        raise RuntimeError('missing Vulkan loader licenses; rebuild loader first')
    for target, source in files.items():
        if not source.is_file():
            raise RuntimeError('missing deployment input: '+str(source))
    for target, source in files.items():
        stage.copy_changed(source, target)
    inventory = {str(target.relative_to(esp)): stage.builder.sha(target) for target in files}
    stage.builder.write_changed(out/'deployed.json', json.dumps(inventory, indent=2, sort_keys=True))
    print('VULKAN_PAYLOAD_READY='+str(esp), flush=True)


if __name__ == '__main__':
    main()
