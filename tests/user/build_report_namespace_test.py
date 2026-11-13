"""Check report producer/reader contracts without running a vendor build."""
from pathlib import Path
import os
import re
import subprocess

ROOT = Path(__file__).resolve().parents[2]
TOOLS = ROOT/'tools/nvidia'
producer = (TOOLS/'phase3_vendor_build.ps1').read_text()
gate = (TOOLS/'phase3_gate.ps1').read_text()
phase4 = (TOOLS/'phase4_build.ps1').read_text()
for name, text in (('producer', producer), ('gate', gate), ('phase4', phase4)):
    assert not re.search(r'liteos', text, re.I), name

def assigned_literal(text, variable):
    match = re.search(r'^\$' + variable + r" = Join-Path [^\n]* '([^']+)'$", text, re.M)
    assert match, variable
    return match[1]

for produced, consumed in (('systemObjectReport', 'systemReport'),
                           ('systemLinkLog', 'systemLinkLog'),
                           ('systemLinkObject', 'systemLinkObject')):
    assert assigned_literal(producer, produced) == assigned_literal(gate, consumed)
object_list = assigned_literal(producer, 'systemObjectList')
assert f"(Join-Path $phase3Root '{object_list}')" in phase4
assert assigned_literal(producer, 'systemObjectRoot') == 'objects\\system'
assert assigned_literal(phase4, 'systemObjectRoot') == 'system'

summary = re.search(r"Write-Output \('(SUMMARY [^']+)' -f ", producer)[1]
summary_pattern = re.search(r"\[regex\]::Match\(\$summaryRows\[0\], '([^']+)'\)", gate)[1]
values = (1026, 1026, 0, 12, 12, 11, 0, 321, 0, 0, 0)
record = summary.format(*values)
match = re.fullmatch(summary_pattern, record)
assert match and tuple(map(int, match.groups())) == values
assert not re.fullmatch(summary_pattern, record.replace('system_', 'liteos_'))
assert not re.fullmatch(summary_pattern, record + ';extra=1')
header = re.search(r"\$unresolvedLines.Add\(\('(# rm_compiled=[^']+)' -f ", producer)[1]
header_pattern = re.search(r"\[regex\]::Match\(\$unresolvedHeader\[0\], '([^']+)'\)", gate)[1]
values = (1026, 12, 11, 321, 0, 0, 0, 0)
record = header.format(*values)
match = re.fullmatch(header_pattern, record)
assert match and tuple(map(int, match.groups())) == values
assert not re.fullmatch(header_pattern, record.replace('system_', 'liteos_'))
assert "Write-Output 'NVIDIA_RM_OS_LINK=PASS'" in gate
assert "Write-Output 'W3_OS_DISPLAY_BACKEND=PASS'" in (TOOLS/'phase-w3_gate.ps1').read_text()
for name in ('run-qemu-auto.ps1', 'stop-qemu-auto-debug.ps1'):
    text = (ROOT/'tools'/name).read_text()
    assert "$preferred = @($env:OS_WSL_DISTRO, 'Debian', 'Ubuntu')" in text
    assert 'LITEOS_WSL_DISTRO' not in text

matrix = (ROOT/'tools/run-qemu-matrix.sh').read_text()
assert 'LITEOS_VERBOSE' not in matrix
blocks = re.findall(r'(?m)^ *if \[\[ "\$\{OS_VERBOSE:-0\}" == "1" \]\]; then\n.*?^ *fi$', matrix, re.S)
assert len(blocks) == 2
subprocess.run(['bash', '-n', str(ROOT/'tools/run-qemu-matrix.sh')], check=True)
for verbose, stale, expected in (('', '', ''), ('0', '1', ''), ('1', '', 'QEMU matrix:'), ('', '1', '')):
    environment = {k: v for k, v in os.environ.items()
                   if k not in ('OS_VERBOSE', 'LITEOS_VERBOSE')}
    environment.update(OS_VERBOSE=verbose, LITEOS_VERBOSE=stale)
    result = subprocess.run(['bash', '-s'], input='cpu=4; actual_accel=kvm\n'+'\n'.join(blocks),
                            text=True, capture_output=True, env=environment, timeout=5, check=True)
    assert (expected in result.stdout if expected else not result.stdout), result.stdout

for name in ('run-real-hardware-test.ps1', 'collect-realtest-log.ps1'):
    text = (ROOT/'tools'/name).read_text()
    assert 'public static class SystemFirmwareEnvironment {' in text, name
    assert '[SystemFirmwareEnvironment]::EnableFirmwarePrivilege()' in text, name
    assert '$runOnceName = "SystemRealTest_' in text, name
    assert 'LiteOsFirmwareEnvironment' not in text and 'LiteOSRealTest_' not in text
    # 升级后仍可读取持久化的固件变量。
    assert "'LiteOSRealTestState', $firmwareGuid," in text, name
firmware = (ROOT/'tools/run-real-hardware-test.ps1').read_text()
assert '@("System USB $letter", "LiteOS USB $letter")' in firmware
assert '# System refactor benchmark baseline v1' in (ROOT/'tools/refactor-benchmark.sh').read_text()
assert '# System native Windows refactor benchmark baseline v1' in (ROOT/'tools/refactor-benchmark-windows.ps1').read_text()
print('Build reports: producer/consumer formats, paths, WSL keys, verbosity, neutral firmware helpers and compatible persisted state passed (Windows source checks only)')
