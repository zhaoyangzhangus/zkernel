#!/usr/bin/env python3
"""Build guest video tools against the same private glibc baseline as Vulkan."""
from pathlib import Path
import subprocess
import sys

root = Path(__file__).resolve().parents[2]
dev = root/'build/nvidia/host-deps/glibc/libc6-dev-extract/usr'
runtime = root/'build/nvidia/host-deps/glibc/libc6-extract/lib/x86_64-linux-gnu'
sdk = root/'build/nvidia/vulkan/sdk/include'
args = sys.argv[1:]
app = 'vulkan-video'
if args[:1] == ['--app-name']:
    if len(args) < 3 or not args[1] or not all(c.isascii() and (c.isalnum() or c in '-_') for c in args[1]):
        raise SystemExit('应用名只能包含 ASCII 字母、数字、横线和下划线')
    app, args = args[1], args[2:]
command = ['x86_64-linux-gnu-gcc', '-isystem', str(dev/'include'),
           '-isystem', str(dev/'include/x86_64-linux-gnu'), '-I', str(sdk)]
if any(x in args for x in ('-c', '-E', '-S', '--version', '-v')):
    command += args
else:
    lib = dev/'lib/x86_64-linux-gnu'
    shared = '-shared' in args
    command += ['-nostdlib', '-nodefaultlibs', '-nostartfiles']
    if not shared:
        command += ['-no-pie', '-Wl,-dynamic-linker,/usr/apps/'+app+'/lib/ld-linux-x86-64.so.2']
    command += [
                '-Wl,-rpath,' + ('$ORIGIN' if shared else '/usr/apps/'+app+'/lib'),
                '-Wl,--disable-new-dtags', '-Wl,-rpath-link,'+str(runtime),
                *([] if shared else [str(lib/'crt1.o')]), str(lib/'crti.o')]
    command += [x for x in args if x not in ('-lm','-ldl','-lpthread','-lc')]
    command += [str(runtime/'libm.so.6'), str(runtime/'libc.so.6'),
                str(lib/'libc_nonshared.a'), '-lgcc', '-lgcc_eh', str(lib/'crtn.o')]
sys.exit(subprocess.call(command))
