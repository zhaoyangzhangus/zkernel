"""Production pixel gates and wake-record reader layout regression."""
import ast
from pathlib import Path
import re
from types import SimpleNamespace

root = Path(__file__).resolve().parents[2]
tab_source=(root/'tools/chromium/qmp-tab-close-test.py').read_text()
tab_tree=ast.parse(tab_source)
logo=next(n for n in tab_tree.body if isinstance(n,ast.FunctionDef) and n.name=='google_ntp_logo')
logo_namespace={}
exec(compile(ast.Module(body=[logo],type_ignores=[]),'logo','exec'),logo_namespace)
logo_gate=logo_namespace['google_ntp_logo']
pixels=bytearray([255])*(1920*1080*3)
assert not logo_gate(1920,1080,pixels), 'Blank NTP body must fail'
for y in range(270,375):
    for x in range(340,630,3):
        color=((66,133,244),(234,67,53),(251,188,5),(52,168,83))[(x-340)//75 % 4]
        offset=(y*1920+x)*3
        pixels[offset:offset+3]=bytes(color)
assert logo_gate(1920,1080,pixels)
for y in range(270,375):
    for x in range(340,630,3):
        offset=(y*1920+x)*3
        pixels[offset:offset+3]=bytes((66,133,244))
assert not logo_gate(1920,1080,pixels), 'One colored block is not the four-color logo'
pixels[:]=bytes([255])*len(pixels)
for y in range(230,305):
    for x in range(340,630,3):
        color=((66,133,244),(234,67,53),(251,188,5),(52,168,83))[(x-340)//75 % 4]
        offset=(y*1920+x)*3
        pixels[offset:offset+3]=bytes(color)
assert logo_gate(1920,1080,pixels), 'NTP without the original tab infobar must pass'
pixels[:]=bytes([255])*len(pixels)
for y in range(380,415):
    for x in range(340,630,3):
        color=((66,133,244),(234,67,53),(251,188,5),(52,168,83))[(x-340)//75 % 4]
        pixels[(y*1920+x)*3:(y*1920+x)*3+3]=bytes(color)
assert not logo_gate(1920,1080,pixels)
assert logo_gate(1920,1080,pixels,40), 'Compositor titlebar offset must be included'
regions=next(n for n in tab_tree.body if isinstance(n,ast.FunctionDef) and n.name=='tab_header_regions')
exec(compile(ast.Module(body=[regions],type_ignores=[]),'regions','exec'),logo_namespace)
region_gate=logo_namespace['tab_header_regions']
pixels[:]=bytes([255])*len(pixels)
before=region_gate(1920,1080,pixels,70)
for y in range(16,40):
    pixels[(y*1920+320)*3:(y*1920+450)*3]=bytes((20,20,20))*130
assert region_gate(1920,1080,pixels,70)==before, 'Caption repaint must not count as tab change'
assert region_gate(1920,1080,pixels,30)!=before
for y in range(56,80):
    pixels[(y*1920+320)*3:(y*1920+450)*3]=bytes((20,20,20))*130
assert region_gate(1920,1080,pixels,70)!=before
try:
    logo_gate(1920,1080,b'')
except AssertionError:
    pass
else:
    raise AssertionError('Truncated body must fail')
capture=next(n for n in tab_tree.body if isinstance(n,ast.FunctionDef) and n.name=='capture_png')
calls=[]
clock=iter((1.0,1.125))
namespace={'args':SimpleNamespace(no_png_evidence=True),'root':root,'png_captures':[],
           'time':SimpleNamespace(monotonic=lambda:next(clock)),
           'command':lambda name,arguments:calls.append((name,arguments))}
exec(compile(ast.Module(body=[capture],type_ignores=[]),'capture', 'exec'),namespace)
namespace['capture_png']('probe')
assert not calls and not namespace['png_captures']
namespace['args'].no_png_evidence=False
namespace['capture_png']('probe')
assert calls[0][0]=='screendump' and calls[0][1]['format']=='png'
assert namespace['png_captures']==[{'name':'probe','capture_ms':125.0}]
frame=next(n for n in ast.walk(tab_tree) if isinstance(n,ast.FunctionDef) and n.name=='frame')
assert 'no_png_evidence' not in ast.unparse(frame)
assert "format': 'png'" not in ast.unparse(frame)
for filename in ('qmp-scroll-cadence.py', 'qmp-history-roundtrip.py', 'qmp-tab-close-test.py'):
    tree = ast.parse((root/'tools/chromium'/filename).read_text())
    function = next(n for n in tree.body if isinstance(n, ast.FunctionDef)
                    and n.name == 'difference')
    namespace = {}
    exec(compile(ast.Module(body=[function], type_ignores=[]), filename, 'exec'), namespace)
    difference = namespace['difference']
    page = bytearray([255]*10000)
    other = page.copy()
    other[:800] = bytes(800)
    # 大面积白色背景会使旧的 15% 引导检查失效。
    assert .03 < difference(page, other) < .15
    assert difference(page, page) == 0
    slightly_changed = page.copy()
    slightly_changed[:40] = bytes(40)
    assert difference(page, slightly_changed) < .005
    assert difference(page, other) > .005
    try:
        difference(b'', b'')
    except AssertionError:
        pass
    else:
        raise AssertionError('An empty frame must not pass')

source = (root/'kernel/sched/core.c').read_text()
definition = source[source.index('typedef struct sched_wake_delay'):]
definition = definition[:definition.index('} sched_wake_delay_t;')]
fields = re.findall(r'uint64_t\s+([^;]+);', definition)
count = sum(len(field.split(',')) for field in fields)
assert count == 25, f'Wake reader must be updated for {count} columns'
reader = (root/'tools/chromium/inspect-scheduler-wake-delays.gdb').read_text()
assert '$index*12' not in reader
assert '$index*16' not in reader
assert '$index*21' not in reader
assert '$index*22' not in reader
assert '$index*25+24' in reader
assert 'sizeof(g_sched_wake_delays) != 16*25*8' in reader
profiler = (root/'tools/chromium/guest-delay-profile.py').read_text()
assert "gdb.parse_and_eval('sizeof('+records+')')" in profiler
assert "gdb.parse_and_eval('sizeof(g_sched_wake_trace)')" in profiler
assert "gdb.parse_and_eval('sizeof(g_sched_wake_stall)')" in profiler
assert "inferior.write_memory(address('g_sched_wake_stall'),bytes(14*8))" in profiler
assert "gdb.parse_and_eval('sizeof(g_vm_lock_diagnostics)')" in profiler
assert "inferior.write_memory(address('g_vm_lock_diagnostics'),bytes(8*8))" in profiler
begin = profiler[profiler.index("if action=='begin':"):profiler.index("elif action=='end':")]
assert begin.index("address('g_vm_lock_diagnostic_guard')") < begin.index('inferior.write_memory(')
assert "'command_release_queues'" in profiler
for name,columns in (('g_sched_inbox_stall',17),('g_sched_ipi_send',7)):
    assert "('%s',%s)"%(name,columns) in profiler
    assert "inferior.write_memory(address('%s'),bytes(%s*8))"%(name,columns) in profiler
    assert "raw('%s',%s)"%(name,columns) in profiler
for phase in ('notify_publish_ns', 'publish_ready_ns', 'source_cpu', 'target_cpu',
              'publish_interrupt_ns', 'interrupt_ready_ns', 'interrupt_active', 'interrupt_preempt',
              'interrupt_preempt_caller', 'publish_request_ns', 'request_interrupt_ns', 'request_send'):
    assert phase in reader
print('Latency probes: white-page contrast, strict reference restore, empty-frame rejection and 25-column wake layout passed')
