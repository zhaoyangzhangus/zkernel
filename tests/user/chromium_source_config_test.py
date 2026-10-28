"""Source regressions never guess machine-specific checkout locations."""
from pathlib import Path
from contextlib import redirect_stdout
import io
import os
import signal
import subprocess
import sys
import tempfile
from unittest.mock import patch

TEST_ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(TEST_ROOT))
from chromium_source import chromium_source
from run_chromium_source_tests import TESTS, DEPLOYED_TESTS, run_test, selected_tests, main

with tempfile.TemporaryDirectory(prefix='chromium-source-config-') as directory:
    base=Path(directory)
    for configured in ('',str(base/'missing'),str(base),'x'*5000):
        with patch.dict(os.environ,CHROMIUM_SRC=configured):
            try:
                chromium_source()
            except RuntimeError as error:
                assert 'CHROMIUM_SRC' in str(error)
            else:
                raise AssertionError('invalid source configuration accepted')
    root=base/'checkout with spaces'
    root.mkdir();(root/'BUILD.gn').write_text('# fixture root\n')
    (base/'not a directory').write_text('fixture')
    with patch.dict(os.environ,CHROMIUM_SRC=str(base/'not a directory')):
        try: chromium_source()
        except RuntimeError: pass
        else: raise AssertionError('file accepted as source root')
    with patch.dict(os.environ,CHROMIUM_SRC=str(root)):
        assert chromium_source()==root.resolve()
    link=base/'configured-link';link.symlink_to(root,target_is_directory=True)
    with patch.dict(os.environ,CHROMIUM_SRC=str(link)):
        assert chromium_source()==root.resolve()
    cwd=Path.cwd()
    try:
        os.chdir(base)
        with patch.dict(os.environ,CHROMIUM_SRC=root.name):
            assert chromium_source()==root.resolve()
    finally:
        os.chdir(cwd)
    env=dict(os.environ);env.pop('CHROMIUM_SRC',None)
    for entry in ('run_chromium_source_tests.py',TESTS[0],TESTS[4]):
        result=subprocess.run([sys.executable,str(TEST_ROOT/entry)],env=env,capture_output=True,text=True)
        assert result.returncode!=0 and 'CHROMIUM_SRC' in result.stderr,entry
        assert 'FileNotFoundError' not in result.stderr,entry
    result=subprocess.run([sys.executable,str(TEST_ROOT/'run_chromium_source_tests.py'),'--list'],
                          env=env,capture_output=True,text=True,check=True)
    assert result.stdout.splitlines()==list(TESTS)
    for phase in ('all', 'source', 'deployed'):
        result = subprocess.run([sys.executable, str(TEST_ROOT/'run_chromium_source_tests.py'),
                                 '--list', '--phase', phase], env=env,
                                capture_output=True, text=True, check=True)
        assert result.stdout.splitlines() == list(selected_tests(phase))
        for succeeds in (True, False):
            with patch('run_chromium_source_tests.chromium_source', return_value=root), \
                 patch('run_chromium_source_tests.run_test', return_value=succeeds) as launch, \
                 patch.object(sys, 'argv', ['runner', '--phase', phase]), \
                 redirect_stdout(io.StringIO()) as output:
                assert main() == (0 if succeeds else 1)
                assert [call.args[0] for call in launch.call_args_list] == list(selected_tests(phase))
                assert 'regressions (' + phase + ')' in output.getvalue()
    assert set(selected_tests('source')).isdisjoint(DEPLOYED_TESTS)
    assert set(selected_tests('source')) | set(DEPLOYED_TESTS) == set(TESTS)
    try: selected_tests('unknown')
    except ValueError: pass
    else: raise AssertionError('unknown phase silently skipped tests')
    for results, expected, signals in (
        ([0],True,[]), ([1],False,[]),
        ([subprocess.TimeoutExpired('fixture',90),0],False,[signal.SIGTERM]),
        ([subprocess.TimeoutExpired('fixture',90),subprocess.TimeoutExpired('fixture',5),0],
         False,[signal.SIGTERM,signal.SIGKILL])):
        with patch('run_chromium_source_tests.subprocess.Popen') as launch, \
             patch('run_chromium_source_tests.os.killpg') as kill, redirect_stdout(io.StringIO()):
            launch.return_value.pid=43210
            launch.return_value.wait.side_effect=results
            assert run_test(TESTS[0],root)==expected
            kwargs=launch.call_args.kwargs
            assert kwargs['start_new_session'] and kwargs['env']['CHROMIUM_SRC']==str(root)
            assert [call.args for call in kill.call_args_list]==[(43210,sig) for sig in signals]
    with patch('run_chromium_source_tests.subprocess.Popen') as launch, \
         patch('run_chromium_source_tests.os.killpg'), redirect_stdout(io.StringIO()):
        launch.return_value.pid=43210
        launch.return_value.wait.side_effect=[subprocess.TimeoutExpired('fixture',5)]*3
        try: run_test(TESTS[0],root)
        except RuntimeError as error: assert 'still live' in str(error)
        else: raise AssertionError('unterminated test treated as stopped')
# Keep the current coverage floor while allowing new regressions to be added.
assert len(TESTS)==len(set(TESTS))>=40
assert 'user/chrome_server_frame_test.py' in TESTS
assert 'user/chrome_guest_library_policy_test.py' in TESTS
assert 'user/chromium_toolchain_root_test.py' in TESTS
for name in TESTS:
    source=(TEST_ROOT/name).read_text()
    assert 'from chromium_source import chromium_source' in source,name
    assert "'/home/" not in source and '"/home/' not in source,name
print(f'Chromium source configuration: no legacy fallback, missing/invalid/file roots, spaces, symlinks, relative paths, direct entry errors and {len(TESTS)}-test registry passed')
