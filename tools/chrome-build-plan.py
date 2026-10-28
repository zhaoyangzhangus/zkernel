#!/usr/bin/env python3
"""Read Ninja's existing graph without building or regenerating it."""
import argparse
from collections import deque
from pathlib import Path
import re
import subprocess
import sys


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--max-steps', type=int, default=500)
    args = parser.parse_args()
    if args.max_steps < 1:
        parser.error('--max-steps must be positive')
    if not (args.out/'build.ninja').is_file():
        parser.error('build.ninja is missing; read-only planning never runs GN')
    process = subprocess.Popen(['ninja', '-C', str(args.out), '-n', '-d', 'explain',
                                'chrome'], stdout=subprocess.PIPE,
                               stderr=subprocess.STDOUT, text=True)
    steps = 0
    examples = []
    reasons = deque(maxlen=12)
    recognized = False
    for line in process.stdout:
        line = line.rstrip()
        match = re.match(r'^\[(\d+)/(\d+)\] ', line)
        if match:
            recognized = True
            steps = max(steps, int(match.group(2)))
            if len(examples) < 8:
                examples.append(line)
        elif 'ninja: no work to do.' in line:
            recognized = True
        elif line.startswith('ninja explain:') or 'error:' in line:
            reasons.append(line)
    status = process.wait()
    for line in reasons:
        print(line)
    for line in examples:
        print(line)
    if status:
        print(f'CHROME_BUILD_PLAN_FAILED exit={status}', file=sys.stderr)
        return status if 0 < status < 126 else 1
    if not recognized:
        print('CHROME_BUILD_PLAN_FAILED unrecognized Ninja output', file=sys.stderr)
        return 1
    print(f'CHROME_BUILD_PLAN steps={steps} limit={args.max_steps}')
    if steps > args.max_steps:
        print('构建范围超过预检上限，未启动编译。确认依赖原因后，可显式设置 '
              'OS_CHROME_MAX_BUILD_STEPS 调整上限；不会清缓存或跳过依赖。',
              file=sys.stderr)
        return 2
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
