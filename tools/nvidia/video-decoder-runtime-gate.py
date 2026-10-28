#!/usr/bin/env python3
"""Verify the decoder dlopen root against the actual guest library closure."""
from pathlib import Path
import argparse
import importlib.util
import subprocess
import sys

root = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('video_decoder_runtime_stage',
    root / 'tools/nvidia/stage-vulkan.py')
stage = importlib.util.module_from_spec(spec)
spec.loader.exec_module(stage)

def check(runtime, library):
    runtime, library = Path(runtime), Path(library)
    catalog = {path.name: path for path in runtime.iterdir() if path.is_file()}
    catalog['libos-vulkan-video.so.1'] = library
    closure = stage.closure(catalog, ['libos-vulkan-video.so.1'])
    stage.abi.check(runtime, closure.values())
    exports = subprocess.check_output(['nm', '-D', '--defined-only', str(library)], text=True)
    expected = {'OS_VIDEO_1', 'os_video_decoder_abi', 'os_video_decoder_create_h264',
                'os_video_decoder_create',
                'os_video_decoder_query',
                'os_video_decoder_send', 'os_video_decoder_receive',
                'os_video_decoder_reset', 'os_video_decoder_destroy'}
    names = {line.split()[-1].split('@')[0] for line in exports.splitlines()}
    if names != expected:
        raise ValueError('Decoder exports mismatch or leaked FFmpeg symbols')
    return closure

if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--runtime-dir', type=Path, required=True)
    parser.add_argument('library', type=Path)
    args = parser.parse_args()
    try:
        selected = check(args.runtime_dir, args.library)
    except (OSError, ValueError, RuntimeError, subprocess.CalledProcessError) as error:
        parser.exit(1, str(error) + '\n')
    print('CHROME_HARDWARE_VIDEO_RUNTIME_PASS', len(selected))
