#!/usr/bin/env python3
"""Build the isolated native hardware decoder without staging a live VM."""
from pathlib import Path
import subprocess
import importlib.util
import shutil
import sys

root = Path(__file__).resolve().parents[2]
out = root / 'build/nvidia/vulkan'
ffmpeg = out / 'ffmpeg-7.1.1'
subprocess.run(['python3', str(root / 'tests/user/vulkan_video_query_patch_test.py')], check=True)
subprocess.run(['python3', str(root / 'tools/nvidia/patch-video-query.py')], check=True)
subprocess.run(['python3', str(root / 'tests/user/vulkan_video_bitstream_padding_test.py')], check=True)
subprocess.run(['python3', str(root / 'tests/user/vulkan_video_query_test.py')], check=True)
subprocess.run(['python3', str(root / 'tests/user/vulkan_video_query_status_test.py')], check=True)
subprocess.run(['make', '-j2', 'libavcodec/libavcodec.a', 'libavutil/libavutil.a'],
               cwd=ffmpeg, check=True)
command = ['python3', str(root / 'tools/nvidia/video-baseline-cc.py'),
           '-std=gnu11', '-O2', '-fPIC', '-Wall', '-Wextra', '-Werror',
           '-shared', '-Wl,-soname,libos-vulkan-video.so.1',
           '-Wl,--version-script=' + str(root / 'tools/nvidia/video-decoder.map'),
           '-Wl,-Bsymbolic', '-Wl,--exclude-libs,ALL', '-Wl,-z,defs',
           '-I' + str(root / 'include'), '-I' + str(ffmpeg),
           str(root / 'tools/nvidia/vulkan_video_decoder.c'),
           str(root / 'tools/nvidia/vulkan_video_device.c'),
           str(out / 'loader/loader/libvulkan.so.1'), '-Wl,--start-group',
           str(ffmpeg / 'libavcodec/libavcodec.a'),
           str(ffmpeg / 'libavutil/libavutil.a'), '-Wl,--end-group',
           '-pthread', '-o', str(out / 'libos-vulkan-video.so.1')]
spec=importlib.util.spec_from_file_location('video_link_cache',
    root/'tools/nvidia/video-build-cache.py')
cache=importlib.util.module_from_spec(spec)
spec.loader.exec_module(cache)
sources=[root/'tools/nvidia/vulkan_video_decoder.c',root/'tools/nvidia/vulkan_video_device.c']
dependencies=cache.compiler_dependencies(command[:command.index(str(sources[0]))],sources)
compiler=Path(shutil.which('x86_64-linux-gnu-gcc')).resolve()
toolchain=[compiler,Path(sys.executable).resolve()]
for program in ('cc1','collect2','as','ld'):
    value=subprocess.check_output([str(compiler),'-print-prog-name='+program],text=True).strip()
    toolchain.append(Path(shutil.which(value) or value).resolve(strict=True))
for library in ('libgcc.a','libgcc_eh.a'):
    value=subprocess.check_output([str(compiler),'-print-file-name='+library],text=True).strip()
    toolchain.append(Path(value).resolve(strict=True))
dev=root/'build/nvidia/host-deps/glibc/libc6-dev-extract/usr/lib/x86_64-linux-gnu'
runtime=root/'build/nvidia/host-deps/glibc/libc6-extract/lib/x86_64-linux-gnu'
inputs=[*dependencies,*toolchain,Path(__file__).resolve(),
        root/'tools/nvidia/video-build-cache.py',root/'tools/nvidia/video-baseline-cc.py',
        root/'tools/nvidia/video-decoder.map',out/'loader/loader/libvulkan.so.1',
        ffmpeg/'libavcodec/libavcodec.a',ffmpeg/'libavutil/libavutil.a',
        dev/'crti.o',dev/'crtn.o',dev/'libc_nonshared.a',
        runtime/'libm.so.6',runtime/'libc.so.6']
cache.cached_command(command,inputs,[out/'libos-vulkan-video.so.1'],
                     out/'video-decoder-link-cache.json',atomic_output=True)
subprocess.run(['python3', str(root / 'tests/user/vulkan_video_hardware_format_test.py')],
               check=True)
subprocess.run(['python3', str(root / 'tests/user/vulkan_video_decoder_lifecycle_test.py')],
               check=True)
subprocess.run(['python3', str(root / 'tests/user/vulkan_video_device_sharing_test.py')],
               check=True)
subprocess.run(['python3', str(root / 'tests/user/vulkan_video_packet_pool_test.py')],
               check=True)
subprocess.run(['python3', str(root / 'tests/user/vulkan_video_frame_layout_test.py')],
               check=True)
subprocess.run(['python3', str(root / 'tests/user/vulkan_video_queue_selection_test.py')],
               check=True)
subprocess.run(['python3', str(root / 'tools/nvidia/vulkan_runtime_abi.py'),
                '--runtime-dir', str(root / 'build/nvidia/host-deps/glibc/libc6-extract/lib/x86_64-linux-gnu'),
                str(out / 'libos-vulkan-video.so.1')], check=True)
subprocess.run(['python3', str(root / 'tests/user/vulkan_video_decoder_abi_test.py')],
               check=True)
print('VULKAN_VIDEO_DECODER_BUILT_NOT_DEPLOYED', flush=True)
