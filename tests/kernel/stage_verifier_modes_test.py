"""Verify explicit desktop mode without weakening strict mode or fault checks."""
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
script = ROOT / 'tools/verify-qemu-stages.sh'
source = script.read_text()
def array(name):
    body = re.search(r'^' + name + r'=\(\n(.*?)^\)', source, re.M | re.S)[1]
    return re.findall(r"^\s*'([^']+)'", body, re.M)
markers = array('required_markers')
stages = array('required_stages')
line = next(i for i, text in enumerate((ROOT / 'kernel/init/core.c').read_text().splitlines(), 1)
            if 'kernel_debug_stage_enter(OS_DEBUG_PHASE_SPEC_3)' in text)
for row in (ROOT / 'tools/roadmap-stage-contracts.tsv').read_text().splitlines():
    if row and not row.startswith('#'):
        phase = row.split('|')[2]
        if not any('phase=' + phase + ' ' in stage for stage in stages):
            stages.append('OS_STAGE phase=' + phase + ' step=1 value=0')
def records(items):
    return [f'{text} seq={i} loc=kernel/init/core.c:{line}' for i, text in enumerate(items,1)]
skipped = {'OS_LIBC_TEST_OK', 'OS_USER_RUNTIME_OK', 'OS_USER_RUNTIME_SUBTESTS_OK', 'OS_USERMODE_OK'}
desktop = ['OS_USER_RUNTIME_SKIPPED_FOR_INTERACTIVE_DESKTOP',
           'OS_SMP_USER_DISPATCH_SKIPPED_FOR_INTERACTIVE_DESKTOP',
           'OS_DESKTOP_CACHE_OK', 'OS_FRAMEBUFFER_UC_OK', 'OS_DISPLAY_COMMIT_OK']
interactive_markers = [text for text in markers if text not in skipped] + desktop
interactive_stages = [text for text in stages if text != 'OS_STAGE phase=USER_RUNTIME step=5 value=1']
with tempfile.TemporaryDirectory(prefix='stage-verifier-modes-') as tmp:
    log = Path(tmp) / 'serial.log'
    def run(lines, mode=False):
        log.write_text('\n'.join(lines) + '\n')
        return subprocess.run(['bash', str(script), *(['--interactive'] if mode else []), str(log)],
                              cwd=ROOT, capture_output=True, text=True)
    strict = markers + records(stages)
    assert run(strict).returncode == 0
    assert run(strict,True).returncode == 1
    current = interactive_markers + records(interactive_stages)
    result = run(current,True); assert result.returncode == 0, result.stderr
    result = run(current); assert result.returncode == 1 and 'OS_LIBC_TEST_OK' in result.stderr
    for marker in desktop:
        result = run([text for text in current if text != marker],True)
        assert result.returncode == 1 and marker in result.stderr, marker
        for replacement in ('LITE' + marker, 'NOT_' + marker, marker + '_EXTRA'):
            result = run([replacement if text == marker else text for text in current],True)
            assert result.returncode == 1 and marker in result.stderr, replacement
    assert run(['[12:34:56] ' + text for text in current],True).returncode == 0
    stage = next(text for text in current if 'OS_STAGE phase=SPEC_P3_SLAB ' in text)
    for replacement in ('LITE' + stage, stage.replace('value=1 ', 'value=10 ')):
        result = run([replacement if text == stage else text for text in current],True)
        assert result.returncode == 1, replacement
    for marker, reason in (('OS_STAGE_FAIL phase=fixture', 'explicit stage failure'),
                          ('OS_DIAG_RT_FAIL needed', 'dynamic loader failed'),
                          ('OS_LIBC_TEST_FAIL stage=fixture', 'native libc test failed')):
        result = run(current + [marker],True)
        assert result.returncode == 1 and reason in result.stderr, marker
    for arguments in (['--unknown'], [str(log),str(log)], ['--interactive',str(log),str(log)]):
        result = subprocess.run(['bash',str(script),*arguments],cwd=ROOT,capture_output=True,text=True)
        assert result.returncode == 2 and 'usage:' in result.stderr
    # 只替换静态前置条件桩函数，测试真实的 make 配方。
    makefile = Path(tmp) / 'Makefile'
    makefile.write_text(f'override BUILD := {tmp}/unused\nSTAGE_LOG := {log}\n'
                        f'include {ROOT / "tools/host-tests.mk"}\nroadmap-stage-layout:\n')
    log.write_text('\n'.join(current) + '\n')
    for mode, expected in (('interactive',0), ('strict',2), ('invalid',2)):
        result = subprocess.run(['make','--no-print-directory','-j1','-f',str(makefile),
                                 'stage-sanity','STAGE_VERIFY_MODE=' + mode],
                                cwd=ROOT,capture_output=True,text=True)
        assert result.returncode == expected, (mode,result.stderr)
    result = subprocess.run(['make','--no-print-directory','-j1','-f',str(makefile),'stage-sanity'],
                            cwd=ROOT,capture_output=True,text=True)
    assert result.returncode == 2 and 'OS_LIBC_TEST_OK' in result.stderr
print('STAGE_VERIFIER_MODES_TEST_OK')
