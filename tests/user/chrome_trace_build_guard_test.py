"""The shared diagnostic guard rejects mismatched, ambiguous and missing ELF IDs."""
from pathlib import Path
import os
import re
import subprocess
import sys
from unittest.mock import patch

ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT/'tools/chromium'))
from trace_build import validate_chrome_build, validate_chrome_symbols

expected='Build ID: abc123'
def reject(notes, configuration=None):
    with patch.dict(os.environ,configuration or {'CHROMIUM_OUT':'/fixture/output'},clear=True), \
         patch('trace_build.subprocess.check_output',side_effect=notes):
        try: validate_chrome_build(expected)
        except RuntimeError: pass
        else: raise AssertionError('unsafe diagnostic identity accepted')

for configuration, output in (({'CHROMIUM_OUT':'/fixture/custom output'},'/fixture/custom output'),
                               ({'CHROMIUM_SRC':'/fixture/source'},'/fixture/source/out/liteos-debian'),
                               ({'CHROMIUM_OUT':'/fixture/explicit','CHROMIUM_SRC':'/fixture/ignored'},'/fixture/explicit')):
    with patch.dict(os.environ,configuration,clear=True), \
         patch('trace_build.subprocess.check_output',return_value='Build ID: ABC123\n') as read:
        validate_chrome_build(expected)
        assert [call.args[0] for call in read.call_args_list]==[
            ['readelf','-n',str(ROOT/'build/esp/usr/apps/chrome/chrome')],
            ['readelf','-n',str(Path(output)/'chrome')]]
reject(['Build ID: abc1230\n'])
reject(['Build ID: abc123\n','Build ID: 123abc\n'])
reject(['Build ID: 123abc\n'])
reject(['no build identity'])
reject(['Build ID: abc123xyz\n'])
reject(['prefix Build ID: abc123\n'])
reject(['Build ID: abc123\nBuild ID: invalid\n'])
reject(['Build ID: abc123\nBuild ID: abc123\n'])
reject([FileNotFoundError('fixture')])
reject([subprocess.CalledProcessError(1,['readelf'])])
reject([],{'CHROMIUM_OUT':'','CHROMIUM_SRC':''})
for identity in ('','abc123','Build ID: abc123 extra','Build ID: xyz'):
    with patch.dict(os.environ,{'CHROMIUM_OUT':'fixture'},clear=True), \
         patch('trace_build.subprocess.check_output') as read:
        try: validate_chrome_build(identity)
        except RuntimeError: pass
        else: raise AssertionError('malformed expected identity accepted')
        assert not read.called
for name in ('trace-first-new-tab.gdb','trace-newtab-syscall-cost.gdb',
             'trace-newtab-frame-stages.gdb','trace-newtab-commit.gdb',
             'trace-draw-paint-pair.gdb','trace-root-render-pass.gdb',
             'trace-newtab-first-present.gdb','trace-address-delivery.gdb',
             'trace-newtab-futex.gdb','trace-newtab-delivery.gdb',
             'trace-native-keys.gdb','trace-cache-read-error.gdb',
             'trace-chrome-key-delivery.gdb', 'trace-http-cache-hits.gdb',
             'trace-menu-button-state.gdb', 'trace-menu-paint.gdb',
             'trace-newtab-debug-write.gdb'):
    script=(ROOT/'tools/chromium'/name).read_text()
    assert script.count('validate_chrome_build(expected)')==1,name
    assert script.index('validate_chrome_build(expected)') < script.index('target remote'),name
    assert '/home/' not in script,name
    assert 'Refusing stale' in script and 'gdb.GdbError' in script,name
for configuration in ({'CHROMIUM_OUT':'/fixture/custom output'},
                      {'CHROMIUM_SRC':'/fixture/source'}):
    with patch.dict(os.environ, configuration, clear=True), \
         patch('trace_build.subprocess.check_output', side_effect=[
             'Build ID: ABC123\n', 'Build ID: abc123\n']):
        binary = validate_chrome_symbols()
        assert binary.name == 'chrome'
for notes in (['Build ID: abc123', 'Build ID: abc1230'],
              ['Build ID: abc123', 'Build ID: abc123\nBuild ID: abc123'],
              ['missing'], [FileNotFoundError('fixture')]):
    with patch.dict(os.environ, {'CHROMIUM_OUT':'/fixture/output'}, clear=True), \
         patch('trace_build.subprocess.check_output', side_effect=notes):
        try: validate_chrome_symbols()
        except RuntimeError: pass
        else: raise AssertionError('mismatched symbol executable accepted')

quarantine = (ROOT/'tools/chromium/inspect-quarantine-delays.gdb').read_text()
assert quarantine.index('validate_chrome_symbols()') < quarantine.index('target remote')
assert '/home/' not in quarantine and 're.search' not in quarantine
skia = (ROOT/'tools/chromium/skia-cpu-probe.py').read_text()
assert '/home/' not in skia and "os.environ.get('CHROMIUM_SRC')" in skia
assert skia.index('validate_chrome_build(expected)') < skia.index('qmp = QMPClient()')
assert "str(binary)" in skia
compile(skia, 'skia-cpu-probe.py', 'exec')
for name in ('inspect-guest-waits.gdb', 'inspect-renderer-observers.gdb',
             'snapshot-cpu-stacks.gdb'):
    script = (ROOT/'tools/chromium'/name).read_text()
    assert not re.search(r'LITEOS_(INSPECT|CAPTURE|BROWSER_PID|OBSERVER_LIST)', script)
    assert 'OS_' in script
    for block in re.findall(r'^python\n(.*?)^end$', script, re.M | re.S):
        compile(block, name, 'exec')
print('Chrome trace guard: exact IDs, both artifacts, output precedence, '
      'seventeen pre-connect consumers, dynamic symbol guard, Skia configuration and neutral inspection keys passed')
