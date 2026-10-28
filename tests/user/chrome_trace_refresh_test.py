"""Entry-symbol refresh is repeatable and refuses incomplete/ambiguous input."""
from pathlib import Path
import re
import sys

root = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(root / 'tools/chromium'))
from refresh_trace_addresses import ANCHORS, FILES, refreshed

scripts = {name: (root / 'tools/chromium' / name).read_text() for name in FILES}
identity = 'Build ID: 123abc'
symbols = []
for index, (_, _, name) in enumerate(ANCHORS):
    label = name + 'fixture)' if name.endswith('(') else name
    symbols.append(f'{0x1000 + index * 0x100:x} T {label}')
output = '\n'.join(symbols)
updated = refreshed(scripts, output, identity, 0x40000000)
assert set(updated) == set(scripts)
for index, (name, pattern, _) in enumerate(ANCHORS):
    expected = 0x40001000 + index * 0x100
    assert {int(x, 16) for x in re.findall(pattern, updated[name])} == {expected}
for name in FILES:
    assert f"expected = '{identity}'" in updated[name]
    # Non-address behavior, offsets, limits and kernel expressions are intact.
    normalize = lambda s: re.sub(r'0x[0-9a-fA-F]+|Build ID: [0-9a-f]+|ONLY Chrome build ID [0-9a-f]+', '_', s)
    assert normalize(scripts[name]) == normalize(updated[name]), name
assert refreshed(updated, output, identity, 0x40000000) == updated
second = '\n'.join(f'{0x8000 + i * 0x100:x} T {line.split(" T ", 1)[1]}'
                   for i, line in enumerate(symbols))
again = refreshed(updated, second, 'Build ID: 456def', 0x40000000)
for index, (name, pattern, _) in enumerate(ANCHORS):
    assert {int(x, 16) for x in re.findall(pattern, again[name])} == {0x40008000 + index * 0x100}

def reject(source=scripts, nm=output, target=identity, bias=0x40000000):
    try:
        refreshed(source, nm, target, bias)
    except RuntimeError:
        return
    raise AssertionError('Unsafe trace refresh accepted')

reject(nm='\n'.join(symbols[1:]))
reject(nm=output + '\n' + symbols[0])
reject(nm=output + '\n9000 T viz::DirectRenderer::DrawFrame(second)')
reject(target='Build ID: invalid!')
reject(bias=0x50000000)
reject(nm=output.replace('1000 T ', 'ffffffff T ', 1))
for changed in (
    scripts[FILES[0]] + '\n# unknown entry 0x76543210\n',
    scripts[FILES[0]].replace('validate_chrome_build(expected)', 'pass'),
    scripts[FILES[0]] + "\nexpected = 'Build ID: 1111'\n",
    re.sub(r"Build ID: [0-9a-f]+", 'Build ID: 9999', scripts[FILES[0]]),
    scripts[FILES[0]].replace("'renderer':", "'lost':"),
):
    reject(source=dict(scripts, **{FILES[0]: changed}))
print('Trace refresh: 24 exact/prefix symbols, twelve scripts, repeat relink, '
      'unchanged behavior and missing/ambiguous/unknown/mixed-identity refusals passed')
