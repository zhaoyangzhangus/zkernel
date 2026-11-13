"""Inspect real compiler recipes without building or touching the live image."""
from pathlib import Path
import re
import shlex
import subprocess

ROOT = Path(__file__).resolve().parents[2]
FEATURES = ('DEBUG_SERIAL', 'REALTEST', 'REALTEST_FAILURE_TEST',
            'ENABLE_FRAME_TRACE', 'PHASE7', 'PHASE8', 'PHASE9', 'PHASE10',
            'XHCI_DIAGNOSTIC_BOT', 'AUTOSTART_CHROME', 'INTERACTIVE_DESKTOP')


def inspect_recipe(values, nvidia):
    overrides = [f'OS_{name}={value}' for name, value in values.items()]
    command = ['make', '-s', '-n', '-B', '-f', 'GNUmakefile',
               'build/kernel/entry.o', 'BUILD=build', f'OS_NVIDIA={nvidia}',
               *overrides]
    output = subprocess.check_output(command, cwd=ROOT, text=True)
    recipes = [shlex.split(line) for line in output.splitlines()
               if '-c kernel/init/main.c ' in line]
    assert len(recipes) == 1, output
    recipe = recipes[0]
    definitions = [value[2:] for value in recipe if value.startswith('-D')]
    assert not any(value.startswith('LITE' + 'OS_') for value in definitions)
    macros = {}
    for definition in definitions:
        name, _, value = definition.partition('=')
        macros[name] = value or '1'
    assert macros['OS_KERNEL_BUILD'] == '1'
    for name, value in values.items():
        assert macros['OS_' + name] == str(value), (name, macros)
    variable = 'ELFCC' if nvidia else 'CC'
    printer = "config-toolchain-print: ; @printf '%s\\n' '$(" + variable + ")'"
    configured = subprocess.check_output(
        ['make', '-s', '-f', 'GNUmakefile', '--eval=' + printer,
         'config-toolchain-print', f'OS_NVIDIA={nvidia}', *overrides],
        cwd=ROOT, text=True)
    compiler = shlex.split(configured)
    assert compiler and recipe[:len(compiler)] == compiler, (recipe, compiler)
    if nvidia:
        assert macros['OS_NVIDIA_PHASE5'] == '1'
    else:
        assert 'OS_NVIDIA_PHASE5' not in macros
    # 在预处理器中实际测试 -D 参数，而不只是检查字符串。
    checks = '\n'.join(f'#if OS_{name} != {value}\n#error {name}\n#endif'
                       for name, value in values.items())
    checks += '\n#if OS_KERNEL_BUILD != 1\n#error kernel_mode\n#endif\n'
    subprocess.run(['cc', '-E', '-P', '-x', 'c', '-',
                    *['-D' + value for value in definitions]],
                   input=checks, text=True, check=True, stdout=subprocess.PIPE)


disabled = dict.fromkeys(FEATURES, 0)
desktop = {**disabled, 'DEBUG_SERIAL': 1, 'INTERACTIVE_DESKTOP': 1}
diagnostic = dict.fromkeys(FEATURES, 1)
for nvidia in (0, 1):
    for values in (disabled, desktop, diagnostic):
        inspect_recipe(values, nvidia)
make = (ROOT / 'makefile').read_text()
assert 'DEBUG_SERIAL_CONFIG_VALUE = os-config-v1:' in make
assert '$(OS_DEBUG_SERIAL)' in make and '$(OS_NVIDIA)' in make
assert '$(DEBUG_SERIAL_STAMP): FORCE' in make
for name in ('run-qemu.sh', 'tools/nvidia/driver.mk',
             'tools/nvidia/build-driver.py', '.vscode/tasks.json'):
    text = (ROOT / name).read_text()
    assert not re.search(r'(?:(?<=-D)|\b)(?:LITE' +
                         r'OS_)(?:KERNEL_BUILD|NVIDIA|DEBUG_SERIAL)\b', text), name
print('Build config: six real recipes, both compilers, feature on/off values and cache-generation marker passed')
