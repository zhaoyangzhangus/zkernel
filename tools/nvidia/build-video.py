#!/usr/bin/env python3
"""Reproducible minimal Vulkan/H264 build in the canonical build directory."""
from pathlib import Path
import hashlib
import subprocess
import tarfile
import importlib.util
import shutil

spec=importlib.util.spec_from_file_location('video_cache',Path(__file__).with_name('video-build-cache.py'))
cache=importlib.util.module_from_spec(spec);spec.loader.exec_module(cache)

root=Path(__file__).resolve().parents[2]
out=root/'build/nvidia/vulkan'
archive=out/'ffmpeg-7.1.1.tar.xz'
digest='733984395e0dbbe5c046abda2dc49a5544e7e0e1e2366bba849222ae9e3a03b1'
if not archive.is_file():
    subprocess.run(['curl','--fail','-L','https://ffmpeg.org/releases/ffmpeg-7.1.1.tar.xz','-o',str(archive)],check=True)
if hashlib.sha256(archive.read_bytes()).hexdigest()!=digest:raise RuntimeError('FFmpeg source hash mismatch')
ff=out/'ffmpeg-7.1.1'
if not ff.is_dir():
    with tarfile.open(archive) as tar:tar.extractall(out,filter='data')
source=ff/'libavutil/hwcontext_vulkan.c'
old='''    for (int i = 0; i < planes; i++)
        av_image_copy_plane(vkbuf->mapped_mem + region[i].bufferOffset,
                            region[i].bufferRowLength,
                            swf->data[i],
                            swf->linesize[i],
                            swf->linesize[i],
                            region[i].imageExtent.height);'''
new='''    for (int i = 0; i < planes; i++) {
        uint8_t *gpu = vkbuf->mapped_mem + region[i].bufferOffset;
        if (upload)
            av_image_copy_plane(gpu, region[i].bufferRowLength,
                                swf->data[i], swf->linesize[i],
                                swf->linesize[i], region[i].imageExtent.height);
        else
            av_image_copy_plane(swf->data[i], swf->linesize[i],
                                gpu, region[i].bufferRowLength,
                                swf->linesize[i], region[i].imageExtent.height);
    }'''
text=source.read_text()
if old in text:source.write_text(text.replace(old,new,1))
elif new not in text:raise RuntimeError('Unrecognized Vulkan download implementation')
subprocess.run(['python3',str(root/'tests/user/vulkan_video_query_patch_test.py')],check=True)
subprocess.run(['python3',str(root/'tools/nvidia/patch-video-query.py')],check=True)
subprocess.run(['python3',str(root/'tests/user/vulkan_video_bitstream_padding_test.py')],check=True)
flags=['--cc=../../../../tools/nvidia/video-baseline-cc.py','--disable-autodetect',
       '--disable-everything','--disable-doc','--disable-debug','--disable-x86asm',
       '--disable-network','--disable-ffplay','--disable-ffprobe','--enable-ffmpeg',
       '--enable-avcodec','--enable-avformat','--enable-avfilter','--enable-swscale',
       '--enable-vulkan','--enable-pthreads','--enable-decoder=h264,hevc,av1',
       '--enable-parser=h264,hevc,av1','--enable-demuxer=h264',
       '--enable-hwaccel=h264_vulkan,hevc_vulkan,av1_vulkan',
       '--enable-protocol=file','--enable-encoder=rawvideo','--enable-muxer=rawvideo',
       '--enable-filter=hwdownload,format,scale,null']
cache.cached_command(['./configure',*flags],
                     [ff/'configure',root/'tools/nvidia/video-baseline-cc.py',
                      Path(shutil.which('x86_64-linux-gnu-gcc')).resolve(),
                      out/'sdk/include/vulkan/vulkan_core.h'],
                     [ff/'config.h',ff/'config_components.h',ff/'ffbuild/config.mak'],
                     out/'video-configure-cache.json',cwd=ff)
subprocess.run(['make','-j2','libavformat/libavformat.a','libavcodec/libavcodec.a',
                'libswscale/libswscale.a','libavutil/libavutil.a'],cwd=ff,check=True)
subprocess.run(['python3',str(root/'tests/user/vulkan_video_download_test.py')],check=True)
subprocess.run(['python3',str(root/'tests/user/vulkan_video_query_test.py')],check=True)
subprocess.run(['python3',str(root/'tests/user/vulkan_video_query_status_test.py')],check=True)
subprocess.run(['python3',str(root/'tools/nvidia/stage-video.py')],check=True)
