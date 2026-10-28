"""Exercise the actual URL observer's optional, verified snapshot handoff."""
import ast
from pathlib import Path
import socket
import subprocess
import sys
import tempfile
from types import SimpleNamespace

root = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(root / 'tools/chromium'))
from guest_inspection import restore_run_state

source = (root / 'tools/chromium/qmp-open-url.py').read_text()
helper = next(node for node in ast.parse(source).body
              if isinstance(node, ast.FunctionDef)
              and node.name == 'capture_network_snapshot')


def exercise(node, running, existing, failure):
    with tempfile.TemporaryDirectory(prefix='qmp-network-handoff-') as directory:
        workspace = Path(directory)
        (workspace / 'build').mkdir()
        endpoint = workspace / 'build/chrome-debug-gdb.sock'
        if existing:
            with socket.socket(socket.AF_UNIX) as server:
                server.bind(str(endpoint))
        machine = {'running': running, 'status': 'running' if running else 'paused'}
        events = []
        owner = []

        class QMP:
            def __init__(self):
                assert not owner, 'QMP must be yielded before inspector/reconnect'
                owner.append(self)

            def command(self, name, arguments=None):
                assert owner == [self]
                events.append(name)
                if name == 'query-status':
                    return machine.copy()
                if name == 'human-monitor-command':
                    assert not existing
                    assert arguments['command-line'] == (
                        'gdbserver unix:' + str(endpoint) + ',server=on,wait=off')
                    return ''
                assert name in ('cont', 'stop')
                assert machine['status'] in ('running', 'paused')
                machine.update(running=name == 'cont',
                               status='running' if name == 'cont' else 'paused')

            def close(self):
                assert owner == [self]
                owner.clear()
                events.append('yield-qmp')

        def run(argv, *, cwd, stdout, stderr, timeout):
            assert not owner, 'GDB inspector must be able to acquire QMP'
            assert argv == ['gdb', '-q', '-nx', '-batch', '-x', str(
                workspace / 'tools/chromium/inspect-current-tcp.gdb')]
            assert cwd == workspace and stderr == subprocess.STDOUT and timeout == 60
            events.append('inspect')
            # Model interruption after attach; the observer must recover the
            # original state even if batch GDB cannot execute its own finally.
            machine.update(running=not running,
                           status='paused' if running else 'running')
            if failure == 'timeout':
                raise subprocess.TimeoutExpired(argv, timeout)
            if failure == 'spawn':
                raise OSError('cannot launch debugger')
            if failure == 'terminal':
                machine.update(running=False, status='shutdown')
            stdout.write(('' if failure == 'text' else 'GUEST_KERNEL_TEXT_MATCH bytes=42\n') +
                         ('' if failure == 'marker' else 'GUEST_INSPECTION_OK\n'))
            return SimpleNamespace(returncode=1 if failure == 'exit' else 0)

        env = {'root': workspace, 'args': SimpleNamespace(label='handoff'),
               'QMPClient': QMP, 'qmp': QMP(),
               'subprocess': SimpleNamespace(run=run, STDOUT=subprocess.STDOUT),
               'restore_run_state': restore_run_state}
        env['command'] = lambda name, arguments=None: env['qmp'].command(name, arguments)
        exec(compile(ast.Module(body=[node], type_ignores=[]), '<url-snapshot>', 'exec'), env)
        try:
            env['capture_network_snapshot']()
        except (RuntimeError, OSError, subprocess.TimeoutExpired):
            assert failure is not None
        else:
            assert failure is None
        assert events.index('yield-qmp') < events.index('inspect')
        if failure == 'terminal':
            assert machine['status'] == 'shutdown' and 'cont' not in events
        else:
            assert machine['running'] == running, 'original guest state not restored'
        assert len(owner) == 1 and owner[0] is env['qmp']
        assert events.count('human-monitor-command') == (0 if existing else 1)
        env['qmp'].close()


for running in (False, True):
    for existing in (False, True):
        for failure in (None, 'exit', 'text', 'marker', 'timeout', 'spawn', 'terminal'):
            exercise(helper, running, existing, failure)

# Reject an already terminal guest before creating a debugger, closing QMP,
# or even inspecting an endpoint on disk.
for status in ('shutdown', 'internal-error'):
    events = []
    env = {'command': lambda name: {'status': status, 'running': False}}
    exec(compile(ast.Module(body=[helper], type_ignores=[]), '<terminal-guest>', 'exec'), env)
    try:
        env['capture_network_snapshot']()
    except RuntimeError as error:
        assert 'not available' in str(error)
    else:
        raise AssertionError('already terminal guest accepted')

# A regression restoring the old held-QMP or unconditional-resume behavior
# must fail the same behavioral oracle, not merely a source-string check.
for old, new in [('    qmp.close()', '    pass'),
                 ("restore_run_state(qmp, state['running'])", "qmp.command('cont')")]:
    mutated = ast.unparse(helper)
    assert old in mutated
    candidate = ast.parse(mutated.replace(old, new)).body[0]
    try:
        exercise(candidate, False, True, None)
    except AssertionError:
        pass
    else:
        raise AssertionError('unsafe handoff negative control accepted')
print('URL network snapshot: 30 state/socket/failure cases; exclusive QMP handoff, '
      'verified success markers, timeout recovery, terminal rejection and negative controls PASS')
