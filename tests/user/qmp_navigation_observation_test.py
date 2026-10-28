"""用可控时钟验证实际观察循环，不连接虚拟机。"""
import ast
import json
from pathlib import Path
import tempfile
from types import SimpleNamespace

root = Path(__file__).resolve().parents[2]
tree = ast.parse((root / 'tools/chromium/qmp-open-url.py').read_text())
helper = next(node for node in tree.body if isinstance(node, ast.FunctionDef)
              and node.name == 'observe_navigation')


def exercise(save, capture_cost, snapshot_after=0, snapshot_cost=0, fail=False):
    with tempfile.TemporaryDirectory() as directory:
        workspace = Path(directory)
        (workspace / 'build').mkdir()
        now = [0.0]
        captures, inspections, sleeps = [], [], []

        def sleep(delay):
            assert delay > 0
            sleeps.append(delay)
            now[0] += delay

        def command(name, arguments=None):
            if name == 'query-status':
                return {'running': True}
            assert name == 'screendump'
            captures.append((now[0], Path(arguments['filename'])))
            now[0] += capture_cost

        def inspect():
            inspections.append(now[0])
            now[0] += snapshot_cost
            if fail:
                raise RuntimeError('inspection failed')

        env = {'args': SimpleNamespace(url='about:blank', label='bounded',
                    observe_seconds=6, save_samples=save,
                    network_snapshot_after=snapshot_after),
               'root': workspace, 'json': json,
               'time': SimpleNamespace(monotonic=lambda: now[0], sleep=sleep),
               'command': command, 'capture_network_snapshot': inspect,
               'print': lambda *args, **kwargs: None}
        exec(compile(ast.Module(body=[helper], type_ignores=[]),
                     '<production-navigation-observer>', 'exec'), env)
        try:
            report = env['observe_navigation']()
        except RuntimeError:
            assert fail
            assert len(inspections) == 1
            return
        assert not fail
        assert report['diagnostic_pause_seconds'] == snapshot_cost
        assert inspections == ([float(snapshot_after)] if snapshot_after else [])
        final = workspace / 'build/chrome-bounded.png'
        assert captures[-1][1] == final
        assert sum(path == final for _, path in captures) == 1
        assert captures[-1][0] == report['observation_seconds']
        assert 6 <= report['observation_seconds'] <= max(6 + capture_cost,
                                                         snapshot_after + snapshot_cost)
        if save:
            samples = report['samples']
            assert len(captures) == len(samples) + 1
            for (started, path), sample in zip(captures, samples):
                assert sample['file'] == str(path.relative_to(workspace))
                assert sample['capture_start_seconds'] == started
                assert sample['capture_end_seconds'] == started + capture_cost
            assert json.loads((workspace / 'build/chrome-bounded-samples/timing.json')
                              .read_text()) == report
            # 截图耗时超过一秒时，不能追赶已错过的采样点。
            if capture_cost > 1:
                assert len(samples) == 3
        else:
            assert len(captures) == 1
            assert not (workspace / 'build/chrome-bounded-samples').exists()
            assert report['observation_seconds'] == max(6, snapshot_after + snapshot_cost)


for save in (False, True):
    for cost in (0, .25, 1.5):
        exercise(save, cost)
    exercise(save, .25, 2, .25)
    exercise(save, .25, 2, 10)
    exercise(save, .25, 2, .25, fail=True)

# 观察失败也必须关闭当前 QMP 所有者，避免后续工具无法连接。
cleanup = next(node for node in tree.body if isinstance(node, ast.Try)
               and any(isinstance(item, ast.Assign) and
                       ast.unparse(item.value) == 'observe_navigation()'
                       for item in node.body))
for failure in (False, True):
    closed = []

    def observe():
        if failure:
            raise RuntimeError('capture failed')
        return {}

    try:
        exec(compile(ast.Module(body=[cleanup], type_ignores=[]), '<observer-cleanup>', 'exec'),
             {'observe_navigation': observe,
              'qmp': SimpleNamespace(close=lambda: closed.append(True))})
    except RuntimeError:
        assert failure
    assert closed == [True]

print('Navigation observation: deadline, single final capture, optional samples, slow capture, diagnostic overrun and failure cleanup PASS')
