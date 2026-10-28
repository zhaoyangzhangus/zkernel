#!/usr/bin/env python3
"""Reject diagnostic breakpoint addresses that do not match the current build."""
from pathlib import Path as _TestPath
import sys as _test_sys
_test_sys.path.insert(0, str(_TestPath(__file__).resolve().parents[1]))
from chromium_source import chromium_source
import ast
from pathlib import Path
import re
import subprocess

root = Path(__file__).resolve().parents[2]
script = (root / 'tools/chromium/trace-first-new-tab.gdb').read_text()
expected = re.search(r"expected = '(Build ID: [0-9a-f]+)'", script).group(1)
chromium = chromium_source()
binary = chromium / 'out/liteos-debian/chrome'
for artifact in (binary, root / 'build/esp/usr/apps/chrome/chrome'):
    assert expected in subprocess.check_output(['readelf', '-n', artifact], text=True)
stages = ast.literal_eval(re.search(r'stages = (\{.*?\n\})', script, re.S).group(1))
symbols = {
    'renderer': 'content::RenderProcessHostImpl::Init()',
    'navigate': 'Navigate(NavigateParams*)',
    'tabstrip': 'TabStripModel::AddTab(std::__Cr::unique_ptr<tabs::TabModel, std::__Cr::default_delete<tabs::TabModel>>, int, ui::PageTransition, int, std::__Cr::optional<tab_groups::TabGroupId>)',
    'loadurl': 'content::NavigationControllerImpl::LoadURLWithParams(content::NavigationController::LoadURLParams const&)',
    'newtab': 'chrome::NewTab(BrowserWindowInterface*, NewTabTypes)',
    'contents': '(anonymous namespace)::CreateTargetContents(NavigateParams const&, GURL const&)',
    'webcontents': 'content::WebContents::Create(content::WebContents::CreateParams const&)',
    'helpers': 'TabHelpers::AttachTabHelpers(content::WebContents*, bool)',
    'omnibox': 'OmniboxViewViews::OnMousePressed(ui::MouseEvent const&)',
    'omnibox_release': 'OmniboxViewViews::OnMouseReleased(ui::MouseEvent const&)',
    'create_event': 'ui::NativeGuestWindow::CreateEvent(ui::nativeguest::InputEvent const&, gfx::Point const&)',
    'scheduler_begin_frame': 'viz::DisplayScheduler::OnBeginFrame(viz::BeginFrameArgs const&)',
    'draw_swap': 'viz::Display::DrawAndSwap(viz::DrawAndSwapParams const&)',
    'begin_paint': 'viz::SoftwareOutputDeviceOzone::BeginPaint(gfx::Rect const&)',
    'end_paint': 'viz::SoftwareOutputDeviceOzone::EndPaint()',
    'cache_error': 'net::HttpCache::Transaction::OnCacheReadError(int, bool)',
    'single_main': 'cc::SingleThreadProxy::BeginMainFrame(viz::BeginFrameArgs const&)',
    'single_send': 'cc::SingleThreadProxy::ScheduledActionSendBeginMainFrame(viz::BeginFrameArgs const&)',
    'single_commit': 'cc::SingleThreadProxy::DoCommit(viz::BeginFrameArgs const&)',
    'activate_tree': 'cc::LayerTreeHostImpl::ActivateSyncTree()',
}
nm = chromium / 'third_party/llvm-build/Release+Asserts/bin/llvm-nm'
output = subprocess.check_output([str(nm), '-C', '--defined-only', str(binary)], text=True)
# Diagnostics target the actual staged binary, not uncompiled source names.
if symbols['create_event'] not in output:
    symbols['create_event'] = 'ui::LiteosWindow::CreateEvent(ui::liteos::InputEvent const&, gfx::Point const&)'
addresses = {}
for line in output.splitlines():
    fields = line.split(' ', 2)
    if len(fields) == 3 and fields[2] in symbols.values():
        addresses[fields[2]] = int(fields[0], 16)
bias = int(re.search(r'#define ELF_PIE_BIAS\s+(0x[0-9a-fA-F]+)',
                     (root / 'kernel/process/exec_internal.h').read_text()).group(1), 16)
for stage, (address, _) in stages.items():
    assert address == addresses[symbols[stage]] + bias, stage
newtab = int(re.search(r"NewTabEntry\('\*(0x[0-9a-f]+)'", script).group(1), 16)
assert newtab == addresses[symbols['newtab']] + bias
# A return callback may destroy its own object. Never remove a live GDB BP
# from within stop(); the bounded outer helper performs detach and recovery.
assert '.delete()' not in script
assert 'self.done = True' in script and 'not init_traced' in script
assert 'gdb.post_event' in script

frame_script = (root / 'tools/chromium/trace-newtab-frame-stages.gdb').read_text()
input_script = (root / 'tools/chromium/trace-newtab-first-present.gdb').read_text()
for diagnostic in (frame_script, input_script):
    assert expected in diagnostic
    assert '.delete()' not in diagnostic and 'gdb.post_event' in diagnostic
for name, label in [('scheduler_begin_frame', 'DISPLAY_BEGIN_FRAME'), ('draw_swap', 'DRAW_AND_SWAP'),
                    ('begin_paint', 'BEGIN_PAINT'), ('end_paint', 'END_PAINT')]:
    value = re.search(r"Stage\('\*(0x[0-9a-f]+)','" + label + r"'\)", frame_script)
    assert value and int(value.group(1), 16) == addresses[symbols[name]] + bias, name
frame_entry = re.search(r"TabEntry\('\*(0x[0-9a-f]+)'", frame_script)
assert frame_entry and int(frame_entry.group(1), 16) == addresses[symbols['newtab']] + bias
assert "TabEntry('*%#x'" in input_script
for name in ('newtab', 'omnibox', 'omnibox_release'):
    assert hex(addresses[symbols[name]] + bias) in input_script, name
print('Current-build identity, all stage symbols, PIE bias and bounded breakpoint ownership passed')

delivery_script = (root / 'tools/chromium/trace-address-delivery.gdb').read_text()
assert expected in delivery_script and '.delete()' not in delivery_script
assert "Press('*%s'" % hex(addresses[symbols['omnibox']] + bias) in delivery_script
assert "Input('input_core_push'" in delivery_script
assert "Present('window_server_present'" in delivery_script
assert 'self.done = True' in delivery_script and 'gdb.post_event' in delivery_script
assert delivery_script.count('gdb.BP_HARDWARE_BREAKPOINT') == 4
assert 'event[name]' in delivery_script and "('type','code','value')" in delivery_script
print('Address delivery trace: current symbol, bounded one-click stages and event metadata only passed')

cost_script = (root / 'tools/chromium/trace-newtab-syscall-cost.gdb').read_text()
assert expected in cost_script and '.delete()' not in cost_script
assert "Entry('*%s'" % hex(addresses[symbols['newtab']] + bias) in cost_script
assert cost_script.count('gdb.BP_HARDWARE_BREAKPOINT') == 2
assert "write('g_syscall_cost_tid',0)" in cost_script
assert "sizeof(g_syscall_costs)" in cost_script and "g_syscall_cost_guard" in cost_script
assert "int(current()['tid']) != owner" in cost_script
assert "gdb.post_event" in cost_script
print('NewTab-only syscall cost: identity, layout/guard, owner and selector cleanup checks passed')

wait_script = (root / 'tools/chromium/trace-newtab-futex.gdb').read_text()
assert expected in wait_script and '.delete()' not in wait_script
assert "Entry('*%s'" % hex(addresses[symbols['newtab']] + bias) in wait_script
assert wait_script.count('gdb.BP_HARDWARE_BREAKPOINT') == 4
assert 'samples>=8' in wait_script and 'range(8)' in wait_script
assert 'return_pc==wait_return_pc' in wait_script and 'pending = None' in wait_script
assert '(unsigned long long)$rcx' in wait_script
print('Futex return trace: current entry, four-address bound, owner/pairing and unsigned timeout passed')

delivery_script = (root / 'tools/chromium/trace-newtab-delivery.gdb').read_text()
assert expected in delivery_script and '.delete()' not in delivery_script
assert "NewTab('*%s'" % hex(addresses[symbols['newtab']] + bias) in delivery_script
assert delivery_script.count('gdb.BP_HARDWARE_BREAKPOINT') == 4
assert "int(event['code'])!=0x17" in delivery_script
assert 'tab_done and present_done' in delivery_script and 'current()' in delivery_script
assert 'gdb.post_event' in delivery_script
native_script = (root / 'tools/chromium/trace-native-keys.gdb').read_text()
assert expected in native_script and '.delete()' not in native_script
assert "Key('*%s'" % hex(addresses[symbols['create_event']] + bias) in native_script
assert native_script.count('gdb.BP_HARDWARE_BREAKPOINT') == 1
assert "read_memory(pointer,24)" in native_script and 'samples>=4' in native_script
assert 'gdb.post_event' in native_script
print('Input timelines/default key trace: current symbols, one-shot limits and bounded ABI prefix reads passed')

pair_script = (root / 'tools/chromium/trace-draw-paint-pair.gdb').read_text()
assert expected in pair_script and '.delete()' not in pair_script
for label,name in [('Input','newtab'),('Draw','draw_swap'),('Paint','begin_paint')]:
    assert "%s('*%s'" % (label,hex(addresses[symbols[name]]+bias)) in pair_script
# Five construction sites, but return and decision modes are mutually exclusive.
# The default host suite executes these callbacks with a fake debugger and
# verifies that each mode creates at most four live breakpoints.
assert pair_script.count('gdb.BP_HARDWARE_BREAKPOINT') == 5
assert 'calls>=16' in pair_script and "pending['sp']+8" in pair_script
assert "int(current()['tid'])!=pending['tid']" in pair_script
assert "'draw_call':pending['call'] if tracked else None" in pair_script
assert 'pending=None' in pair_script and 'gdb.post_event' in pair_script
assert "all_processes = os.environ.get('OS_FRAME_SCOPE') == 'all'" in frame_script
assert "frame_stage not in ('paint','pipeline','schedule')" in frame_script
print('Draw/paint pairing: build identity, four-address bound, owner/return-stack and skipped-call attribution passed')

cache_script = (root / 'tools/chromium/trace-cache-read-error.gdb').read_text()
assert expected in cache_script and '.delete()' not in cache_script
assert "CacheError('*%s'" % hex(addresses[symbols['cache_error']] + bias) in cache_script
assert cache_script.count('gdb.BP_HARDWARE_BREAKPOINT') == 1
assert 'samples>=8' in cache_script and 'read_memory(rsp,8)' in cache_script
assert "process['name'].string().rsplit('/',1)[-1]!='chrome'" in cache_script
assert 'gdb.post_event' in cache_script and 'result-=0x100000000' in cache_script
print('Cache error trace: current-build symbol, one hardware address, signed errno and eight-record bound passed')

commit_script = (root / 'tools/chromium/trace-newtab-commit.gdb').read_text()
assert expected in commit_script and '.delete()' not in commit_script
commit_stages = ast.literal_eval(re.search(r'stages=(\{.*?\n\})',commit_script,re.S).group(1))
labels={'BEGIN_MAIN_FRAME':'single_main','SEND_BEGIN_MAIN_FRAME':'single_send',
        'DO_COMMIT':'single_commit','ACTIVATE_SYNC_TREE':'activate_tree'}
for name,pairs in commit_stages.items():
    assert len(pairs)+2==4
    for address,label in pairs:
        assert address==addresses[symbols[labels[label]]]+bias,(name,label)
assert "NewTab('*%s'" % hex(addresses[symbols['newtab']]+bias) in commit_script
assert "Stage('*%s'" % hex(addresses[symbols['begin_paint']]+bias) in commit_script
assert 'samples>=40' in commit_script and 'gdb.post_event' in commit_script
assert "dereference()['pid'])!=browser" in commit_script
print('Browser UI commit trace: actual SingleThreadProxy symbols, three modes, four slots and owner attribution passed')
