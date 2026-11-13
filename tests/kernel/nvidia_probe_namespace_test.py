"""Keep probe producers, gate consumers and staged file data in one namespace."""
from pathlib import Path
import re
import subprocess

ROOT = Path(__file__).resolve().parents[2]
TOOLS = ROOT / 'tools/nvidia'
probes = {
    7: 'phase7_glibc_test.c',
    8: 'phase8_frontend_test.c',
    9: 'phase9_userspace_load_test.c',
    10: 'phase10_egl_context_test.c',
}
for phase, filename in probes.items():
    source = (TOOLS / filename).read_text()
    gate = (TOOLS / f'phase{phase}_gate.ps1').read_text()
    assert 'LITEOS_P' not in source + gate
    emitted = set(re.findall(r'OS_P\d+_[A-Z0-9_]+=?', source))
    required = set(re.findall(r'OS_P\d+_[A-Z0-9_]+=?', gate))
    assert f'OS_P{phase}_PASS' in emitted if phase in (7, 8, 10) else 'OS_P9_LOADER_PASS' in emitted
    assert emitted & required, filename
    # 门禁输出按记录检查，而不是匹配过期 LITEOS 标签中的子串。
    assert ". (Join-Path $PSScriptRoot 'log-records.ps1')" in gate
    assert 'Test-LogRecord $realText $requiredMarker' in gate
    assert 'Require-Text $realText $requiredMarker' not in gate
    subprocess.run(['cc', '-std=c11', '-D_GNU_SOURCE', '-Wall', '-Wextra',
                    '-Werror', '-fsyntax-only', '-I' + str(ROOT / 'include'),
                    str(TOOLS / filename)], check=True)

for filename in ('phase11_build.ps1', 'phase11_gate.ps1'):
    source = (TOOLS / filename).read_text()
    assert 'LITEOS_P10' not in source
    assert "Test-LogRecord $candidateText 'OS_P10_PASS'" in source

source = (TOOLS / probes[7]).read_text()
fixture = (TOOLS / 'phase7-data.txt').read_bytes()
literal = re.search(r'expected_data\[\] = "([^"]+)";', source).group(1)
assert literal.encode().decode('unicode_escape').encode() == fixture
assert b'liteos' not in fixture.lower()

matcher = (TOOLS / 'log-records.ps1').read_text()
assert "(?m)(?:^|[ \\t])" in matcher
assert "(?=[ \\t\\r\\n]|$)" in matcher
for phase in probes:
    marker = f'OS_P{phase}_PASS'
    pattern = re.compile(r'(?m)(?:^|[ \t])' + re.escape(marker) + r'(?=[ \t\r\n]|$)')
    for text in (marker, marker + '\r\n', 'prefix ' + marker + '\n'):
        assert pattern.search(text)
    for text in ('LITE' + marker, marker + '_FAIL', marker + 'Extra'):
        assert not pattern.search(text)
print('nvidia probe namespace: PASS')
