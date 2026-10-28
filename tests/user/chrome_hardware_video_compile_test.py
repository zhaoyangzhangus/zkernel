"""Syntax-check the actual media integration without changing build products."""
from pathlib import Path
import shlex
import subprocess
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from chromium_source import chromium_source

source = chromium_source()
out = source / 'out/liteos-debian'
for target in (
    'obj/media/renderers/renderers/native_vulkan_video_decoder.o',
    'obj/media/renderers/renderers/default_decoder_factory.o',
    'obj/media/base/base/decoder.o',
    'obj/media/base/base/supported_types.o',
    'obj/media/filters/filters/ffmpeg_video_decoder.o',
):
    lines = subprocess.check_output(['ninja', '-C', str(out), '-t', 'commands',
                                     '-s', target], text=True, timeout=15).splitlines()
    argv = shlex.split(lines[-1])
    if Path(argv[0]).name == 'sccache': argv = argv[1:]
    if 'build/toolchain/native_os/clang' not in argv[0]:
        raise RuntimeError('Hardware decoder check selected a host compiler')
    args, index = [], 0
    while index < len(argv):
        if argv[index] in ('-o', '-MF'):
            index += 2
            continue
        if argv[index] not in ('-c', '-MD', '-MMD'):
            args.append(argv[index])
        index += 1
    subprocess.run([*args, '-fsyntax-only'], cwd=out, check=True, timeout=60)
    print('CHROME_HARDWARE_VIDEO_SYNTAX_PASS', target, flush=True)
