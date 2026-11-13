"""Keep QEMU debug servers, GDB clients and recovery connections in agreement."""
import ast
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[2]
DIRECTORY = ROOT / 'tools' / 'chromium'
socket_pattern = r'/tmp/system-[a-z-]+\.sock'
gdb_count = python_count = paired_count = 0
for path in sorted(DIRECTORY.glob('*.gdb')):
    source = path.read_text()
    assert '/tmp/liteos-' not in source, path.name
    assert not re.search(r"environ\.get\(['\"]LITEOS_", source), path.name
    connections = re.findall(r'^target remote (\S+)', source, re.M)
    for connection in connections:
        assert re.fullmatch(socket_pattern, connection), (path.name, connection)
    if connections:
        gdb_count += 1

for path in sorted(DIRECTORY.glob('*.py')):
    source = path.read_text()
    assert '/tmp/liteos-' not in source, path.name
    tree = ast.parse(source, filename=str(path))
    for node in ast.walk(tree):
        if (isinstance(node, ast.Call) and isinstance(node.func, ast.Attribute)
                and node.func.attr == 'TemporaryDirectory'):
            for keyword in node.keywords:
                if keyword.arg == 'prefix' and isinstance(keyword.value, ast.Constant):
                    assert not keyword.value.value.startswith('liteos-'), path.name
    constants = [node.value for node in ast.walk(tree)
                 if isinstance(node, ast.Constant) and isinstance(node.value, str)]
    servers = set()
    clients = set()
    for value in constants:
        servers.update(re.findall(r'gdbserver unix:(' + socket_pattern + ')', value))
        clients.update(re.findall(r'target remote (' + socket_pattern + ')', value))
    if not servers:
        continue
    python_count += 1
    assert clients <= servers, (path.name, servers, clients)
    for value in constants:
        # 这既包括 argparse 默认值，也包括子进程的 -x 参数。
        if re.fullmatch(r'(?:tools/chromium/)?[a-z-]+\.gdb', value):
            script = DIRECTORY / Path(value).name
            assert script.is_file(), (path.name, value)
            connections = re.findall(r'^target remote (\S+)', script.read_text(), re.M)
            assert connections and set(connections) <= servers, (path.name, value, connections)
            paired_count += 1

assert gdb_count >= 50 and python_count >= 10 and paired_count >= 4, (
    gdb_count, python_count, paired_count)
for name in ('qmp-scroll-soak.py', 'qmp-scroll-cadence.py', 'qmp-scroll-latency.py'):
    tree = ast.parse((DIRECTORY / name).read_text())
    prefixes = [keyword.value.value for node in ast.walk(tree)
                if isinstance(node, ast.Call)
                and isinstance(node.func, ast.Attribute)
                and node.func.attr == 'TemporaryDirectory'
                for keyword in node.keywords if keyword.arg == 'prefix']
    assert prefixes and all(prefix.startswith('system-scroll-') for prefix in prefixes), name

inspection = (DIRECTORY/'inspection-types.c').read_text()
assert 'liteos_inspection_' not in inspection
for name in ('process', 'thread', 'wait'):
    assert f'system_inspection_{name}_type;' in inspection

print(f'Debug namespace: {gdb_count} GDB clients, {python_count} server producers, '
      f'{paired_count} script pairings, inline recovery clients and scroll temp names passed')
