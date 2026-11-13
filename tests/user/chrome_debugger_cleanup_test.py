"""Exercise the real bounded-trace crash recovery without a guest/debugger."""
import ast
from pathlib import Path
from types import SimpleNamespace

root = Path(__file__).resolve().parents[2]
tree = ast.parse((root/'tools/chromium/qmp-trace-keys.py').read_text())
function = next(node for node in tree.body if isinstance(node, ast.FunctionDef)
                and node.name == 'recover_failed_debugger')
calls = []
namespace = {'root': root, 'process': None,
             'command': lambda name, args: calls.append((name, args)),
             'subprocess': SimpleNamespace(run=lambda args, **kw: calls.append((args, kw)))}
exec(compile(ast.Module(body=[function], type_ignores=[]), 'recovery', 'exec'), namespace)
recover = namespace['recover_failed_debugger']
for process in (None, SimpleNamespace(poll=lambda: None)):
    namespace['process'] = process
    recover()
    assert calls == [], 'Absent/live debugger must not be replaced'
for code in (0, -11, -15, 1):
    namespace['process'] = SimpleNamespace(poll=lambda: code)
    recover()
    assert calls[0][1]['command-line'] == 'gdbserver none'
    assert 'unix:/tmp/system-keys.sock' in calls[1][1]['command-line']
    argv, kwargs = calls[2]
    assert argv[:4] == ['gdb', '-q', '-nx', '-batch']
    assert argv[-1] == 'detach' and 'target remote /tmp/system-keys.sock' in argv
    assert kwargs == {'cwd': root, 'check': True, 'timeout': 10}
    calls.clear()
outer = next(node for node in tree.body if isinstance(node, ast.Try))
recovery_try = next(node for node in outer.finalbody if isinstance(node, ast.Try))
assert 'recover_failed_debugger()' in ast.unparse(recovery_try.body[0])
cleanup = ast.unparse(ast.Module(body=recovery_try.finalbody, type_ignores=[]))
assert 'gdbserver none' in cleanup and "command('cont')" in cleanup
assert "'down': False" in cleanup and "'button': 'left'" in cleanup
# 执行真实输入重试策略：只有被拒绝的事件可以重试；成功投递只发生一次，其他错误仍按错误处理。
retry = next(n for n in tree.body if isinstance(n, ast.FunctionDef) and n.name=='command')
clock=[0.0]
attempts=[]
def once(name,args):
    attempts.append((name,args))
    if len(attempts)<3:
        raise RuntimeError({'error':{'desc':'VM not running'}})
    return 'delivered'
env={'time':SimpleNamespace(monotonic=lambda:clock[0],
       sleep=lambda duration:clock.__setitem__(0,clock[0]+duration)),
     'command_once':once}
exec(compile(ast.Module(body=[retry],type_ignores=[]),'retry','exec'),env)
assert env['command']('input-send-event',{'events':[]})=='delivered'
assert len(attempts)==3
attempts.clear()
try:env['command']('query-status')
except RuntimeError:pass
else:raise AssertionError('non-input errors must propagate')
assert len(attempts)==1
def stopped(name,args):raise RuntimeError({'error':{'desc':'VM not running'}})
env['command_once']=stopped
try:env['command']('input-send-event')
except RuntimeError:pass
else:raise AssertionError('retry must be bounded')
assert 3<=clock[0]<3.1
print('Debugger cleanup: zero/nonzero exits reset stale traps, live debugger preserved, input retry bounded, buttons released and resume passed')

# 点击时序与按键时序相互独立：100ms 的诊断按压时间会干扰正在测量的按键释放触发地址选择操作。
click = next(n for n in ast.walk(outer) if isinstance(n,ast.If)
             and ast.unparse(n.test)=='args.click')
buttons = next(n for n in click.body if isinstance(n,ast.For)
               and ast.unparse(n.target)=='down')
events=[]
click_env={'args':SimpleNamespace(mouse_hold_ms=20),
           'command':lambda n,a:events.append(a),
           'time':SimpleNamespace(sleep=lambda d:events.append(d))}
exec(compile(ast.Module(body=[buttons],type_ignores=[]),'click-hold','exec'),click_env)
assert events[0]['events'][0]['data']['down'] is True
assert events[1]==.02 and events[2]['events'][0]['data']['down'] is False
assert len(events)==3
print('Address click trace: configurable 20ms hold, immediate release, independent of key hold passed')
