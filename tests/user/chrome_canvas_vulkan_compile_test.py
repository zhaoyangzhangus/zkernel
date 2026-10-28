"""Syntax-check the real canvas integration without changing build products."""
from pathlib import Path
import shlex
import subprocess
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from chromium_source import chromium_source

source = chromium_source()
out = source / 'out/liteos-debian'
for prefix in ('', 'clang_x64/'):
    for name in ('canvas_utils', 'canvas_2d_resource_provider',
                 'canvas_non_2d_resource_provider'):
        target = prefix + 'obj/third_party/blink/renderer/platform/platform/' + name + '.o'
        lines = subprocess.check_output(
            ['ninja', '-C', str(out), '-t', 'commands', '-s', target],
            text=True, timeout=20).splitlines()
        argv = shlex.split(lines[-1])
        if Path(argv[0]).name == 'sccache':
            argv = argv[1:]
        args, index = [], 0
        while index < len(argv):
            if argv[index] in ('-o', '-MF'):
                index += 2
                continue
            if argv[index] not in ('-c', '-MD', '-MMD'):
                args.append(argv[index])
            index += 1
        subprocess.run([*args, '-fsyntax-only'], cwd=out, check=True, timeout=60)
        print('CHROME_CANVAS_VULKAN_SYNTAX_PASS', target, flush=True)
