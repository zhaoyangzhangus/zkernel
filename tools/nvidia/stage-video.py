#!/usr/bin/env python3
"""Build and ABI-check the strict Vulkan Video player before deployment."""
from pathlib import Path
import importlib.util
import subprocess
import sys
import tempfile
import json
import shutil

root=Path(__file__).resolve().parents[2]
out=root/'build/nvidia/vulkan'
ff=out/'ffmpeg-7.1.1'
spec=importlib.util.spec_from_file_location('video_stage',root/'tools/nvidia/stage-vulkan.py')
stage=importlib.util.module_from_spec(spec);sys.modules[spec.name]=stage;spec.loader.exec_module(stage)
cache=stage.module('video_build_cache','video-build-cache.py')
command=['python3',str(root/'tools/nvidia/video-baseline-cc.py'),'-std=gnu11','-O2','-Wall','-Wextra','-Werror',
         '-I'+str(root/'include'),'-I'+str(ff),str(root/'tools/nvidia/vulkan_video_player.c'),
         str(root/'tools/nvidia/vulkan_video_device.c'),str(out/'loader/loader/libvulkan.so.1'),
         '-Wl,--start-group',str(ff/'libavformat/libavformat.a'),str(ff/'libavcodec/libavcodec.a'),
         str(ff/'libswscale/libswscale.a'),str(ff/'libavutil/libavutil.a'),'-Wl,--end-group',
         '-pthread','-o',str(out/'vulkan-video')]
dev=root/'build/nvidia/host-deps/glibc/libc6-dev-extract/usr'
runtime=root/'build/nvidia/host-deps/glibc/libc6-extract/lib/x86_64-linux-gnu'
# Include header sets as well as their contents: adding/removing an included
# header invalidates the receipt even when older timestamps are restored.
headers=[*ff.rglob('*.h'),*(out/'sdk/include').rglob('*.h'),
         *(dev/'include').rglob('*.h'),*(root/'include/uapi').rglob('*.h')]
inputs=[root/'tools/nvidia/video-baseline-cc.py',root/'tools/nvidia/stage-video.py',
        root/'tools/nvidia/vulkan_video_player.c',root/'tools/nvidia/vulkan_video_device.c',
        root/'tools/nvidia/video_pacing.h',
        out/'loader/loader/libvulkan.so.1',ff/'libavformat/libavformat.a',
        ff/'libavcodec/libavcodec.a',ff/'libswscale/libswscale.a',ff/'libavutil/libavutil.a',
        runtime/'libm.so.6',runtime/'libc.so.6',dev/'lib/x86_64-linux-gnu/crt1.o',
        dev/'lib/x86_64-linux-gnu/crti.o',dev/'lib/x86_64-linux-gnu/crtn.o',
        dev/'lib/x86_64-linux-gnu/libc_nonshared.a',*sorted(headers)]
inputs.append(Path(shutil.which('x86_64-linux-gnu-gcc')).resolve())
for tool_input in ('libgcc.a','libgcc_eh.a'):
    inputs.append(Path(subprocess.check_output(
        ['x86_64-linux-gnu-gcc','-print-file-name='+tool_input],text=True).strip()))
inputs.append(Path(subprocess.check_output(
    ['x86_64-linux-gnu-gcc','-print-prog-name=cc1'],text=True).strip()))
compiler=subprocess.check_output(['x86_64-linux-gnu-gcc','--version'],text=True)
stage.stage.builder.write_changed(out/'video-compiler.txt',compiler)
inputs.append(out/'video-compiler.txt')
cache.cached_command(command,inputs,[out/'vulkan-video'],out/'video-link-cache.json')
stage.abi.check(runtime,[out/'vulkan-video'])
libdir=root/'build/esp/usr/apps/chrome/lib'
catalog={p.name:p for p in libdir.glob('*.so*') if p.is_file()}
needed=stage.closure(catalog,stage.check.Elf64(out/'vulkan-video').needed())
stage.abi.check(runtime,needed.values())
with tempfile.TemporaryDirectory(prefix='.video-verify-',dir=out) as directory:
    temporary=Path(directory)
    stage.stage.copy_changed(out/'vulkan-video',temporary/'vulkan-video')
    for name,path in needed.items():stage.stage.copy_changed(path,temporary/name)
    stage.check.verify_elves(temporary)
fixture=root/'tools/nvidia/make-video-fixture.py'
cache.cached_command(['python3',str(fixture),str(out/'video-test.h264')],
                     [fixture],[out/'video-test.h264'],out/'video-fixture-cache.json')
esp=root/'build/esp'
for name, path in needed.items():
    stage.stage.copy_changed(path,esp/'usr/apps/vulkan-video/lib'/name)
stage.stage.copy_changed(runtime/'ld-linux-x86-64.so.2',esp/'usr/apps/vulkan-video/lib/ld-linux-x86-64.so.2')
icd=json.loads((esp/'usr/apps/chrome/vulkan/nvidia_icd.json').read_text())
icd['ICD']['library_path']='/usr/apps/vulkan-video/lib/libGLX_nvidia.so.0'
for name,path in stage.vulkan_closure(catalog,libdir).items():
    stage.stage.copy_changed(path,esp/'usr/apps/vulkan-video/lib'/name)
stage.stage.builder.write_changed(esp/'usr/apps/vulkan-video/vulkan/nvidia_icd.json',json.dumps(icd,indent=2)+'\n')
stage.stage.builder.write_changed(esp/'usr/apps/vulkan-video/egl/10_nvidia.json',json.dumps({
    'file_format_version':'1.0.0','ICD':{
        'library_path':'/usr/apps/vulkan-video/lib/libEGL_nvidia.so.0'}},indent=2)+'\n')
stage.stage.copy_changed(out/'vulkan-video',esp/'usr/apps/vulkan-video/vulkan-video')
stage.stage.copy_changed(out/'video-test.h264',esp/'usr/apps/vulkan-video/video-test.h264')
stage.stage.copy_changed(ff/'COPYING.LGPLv2.1',esp/'usr/apps/vulkan-video/licenses/ffmpeg/COPYING.LGPLv2.1')
for path in (esp/'usr/apps/chrome/licenses').rglob('*'):
    if path.is_file():
        stage.stage.copy_changed(path,esp/'usr/apps/vulkan-video/licenses'/path.relative_to(esp/'usr/apps/chrome/licenses'))
app=esp/'usr/apps/vulkan-video'
stage.stage.builder.write_changed(out/'video-deployed.json',json.dumps({
    str(path.relative_to(esp)):stage.stage.builder.sha(path)
    for path in app.rglob('*') if path.is_file()},indent=2,sort_keys=True)+'\n')
print('VULKAN_VIDEO_PAYLOAD_READY',flush=True)
