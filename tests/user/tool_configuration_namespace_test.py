"""Exercise configuration resolution without launching QEMU or rebinding PCI."""
from pathlib import Path
import os
import re
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[2]

for name in ('audit-environment.sh', 'run-qemu-vfio-5070.sh'):
    script = (ROOT / 'tools' / name).read_text()
    assert 'LITEOS_GPU' not in script, name
    subprocess.run(['bash', '-n', str(ROOT / 'tools' / name)], check=True)
    initialization = script[:script.index('usage() {')]
    # 执行真实初始化，但只替换路径发现逻辑：否则 BASH_SOURCE 会指向 stdin 测试框架。
    initialization = re.sub(r'^script_dir=.*$',
                            'script_dir="$1/tools"', initialization,
                            count=1, flags=re.M)
    initialization += '\nprintf "%s\\n" "$build_dir" "$gpu_bdf" "$audio_bdf"\n'
    for values, expected in (
        ({}, ['build', '', '']),
        ({'BUILD': 'chosen', 'OS_GPU_PCI': '0000:01:00.0',
          'OS_GPU_AUDIO_PCI': '0000:01:00.1'},
         ['chosen', '0000:01:00.0', '0000:01:00.1']),
        ({'LITEOS_GPU_PCI': 'stale', 'LITEOS_GPU_AUDIO_PCI': 'stale'},
         ['build', '', '']),
        ({'OS_GPU_PCI': '', 'OS_GPU_AUDIO_PCI': '',
          'LITEOS_GPU_PCI': 'stale', 'LITEOS_GPU_AUDIO_PCI': 'stale'},
         ['build', '', '']),
    ):
        environment = {k: v for k, v in os.environ.items()
                       if k not in ('BUILD', 'OS_GPU_PCI', 'OS_GPU_AUDIO_PCI',
                                    'LITEOS_GPU_PCI', 'LITEOS_GPU_AUDIO_PCI')}
        environment.update(values)
        result = subprocess.run(['bash', '-s', '--', str(ROOT)],
                                input=initialization, text=True,
                                capture_output=True, check=True, env=environment)
        assert result.stdout.splitlines() == expected, (name, result.stdout)

    # 同时测试生产选项解析器；在宿主探测、模块加载、驱动更改或虚拟机启动之前停止。
    parser = script[:script.index('\ndone\n') + len('\ndone\n')]
    parser = re.sub(r'^script_dir=.*$', 'script_dir="$1/tools"; shift',
                    parser, count=1, flags=re.M)
    parser += '\nprintf "%s\\n" "$build_dir" "$gpu_bdf" "$audio_bdf"\n'
    environment.update(BUILD='environment', OS_GPU_PCI='environment-gpu',
                       OS_GPU_AUDIO_PCI='environment-audio')
    result = subprocess.run(
        ['bash', '-s', '--', str(ROOT), '--build', 'command-line',
         '--gpu-pci', '0000:02:00.0', '--gpu-audio-pci', '0000:02:00.1'],
        input=parser, text=True, capture_output=True, check=True, env=environment)
    assert result.stdout.splitlines() == ['command-line', '0000:02:00.0', '0000:02:00.1']
    for flag in ('--build', '--gpu-pci', '--gpu-audio-pci'):
        result = subprocess.run(['bash', '-s', '--', str(ROOT), flag],
                                input=parser, text=True, capture_output=True,
                                env=environment)
        assert result.returncode == 2, (name, flag, result.returncode)

pwsh = shutil.which('pwsh')
expressions = []
for name in ('build-windows.ps1', 'run-qemu-auto.ps1',
             'stop-qemu-auto-debug.ps1', 'stop-qemu-native-debug.ps1'):
    source = (ROOT / 'tools' / name).read_text()
    assert not re.search(r'\$env:LITEOS_(BUILD|TOOLPREFIX|ELFTOOLPREFIX)', source)
    # 有意使用相同优先级约定分别检查单行和多行形式：显式参数、OS_*、通用变量、默认值。
    start = source.index('$buildName = if ')
    end = source.index("'build'", start) + len("'build'")
    expression = source[start:end] + ' }'
    assert re.search(r'\$Build.*?\$env:OS_BUILD.*?\$env:BUILD', expression, re.S), name
    assert 'build-refactor' not in expression, name
    expressions.append(expression)
    for parameter, os_key, generic in (
        ('ToolPrefix', 'OS_TOOLPREFIX', 'TOOLPREFIX'),
        ('ElfToolPrefix', 'OS_ELFTOOLPREFIX', 'ELFTOOLPREFIX'),
    ):
        if '$env:' + os_key in source:
            assert re.search(r'if \(\$' + parameter + r'\).*?\$env:' + os_key
                             + r'.*?\$env:' + generic, source, re.S), name

if pwsh:
    for expression in expressions:
        for explicit, os_build, generic, expected in (
            ('', '', '', 'build'), ('explicit', 'os', 'generic', 'explicit'),
            ('', 'os', 'generic', 'os'), ('', '', 'generic', 'generic'),
        ):
            program = (f"$Build='{explicit}'; $env:OS_BUILD='{os_build}'; "
                       f"$env:BUILD='{generic}'; " + expression
                       + '; Write-Output $buildName')
            result = subprocess.run([pwsh, '-NoProfile', '-Command', '-'],
                                    input=program, text=True, capture_output=True,
                                    check=True)
            assert expected in result.stdout.splitlines(), result.stdout

print('Tool configuration: neutral GPU variables, Windows precedence and canonical build defaults passed; '
      + ('PowerShell execution passed' if pwsh else 'PowerShell unavailable: Windows source checks only'))
