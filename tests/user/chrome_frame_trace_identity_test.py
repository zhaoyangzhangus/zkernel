#!/usr/bin/env python3
"""Validate only the current NewTab diagnostics against the deployed binary.

Older diagnostics remain build guarded; this does not certify their addresses.
"""
from pathlib import Path as _TestPath
import sys as _test_sys
_test_sys.path.insert(0, str(_TestPath(__file__).resolve().parents[1]))
from chromium_source import chromium_source
import ast
from pathlib import Path
import re
import subprocess

root = Path(__file__).resolve().parents[2]
chromium = chromium_source()
binary = chromium / 'out/liteos-debian/chrome'
build_id = re.search(r'Build ID: ([0-9a-f]+)', subprocess.check_output(
    ['readelf', '-n', str(binary)], text=True)).group(1)
assert f'Build ID: {build_id}' in subprocess.check_output(
    ['readelf', '-n', str(root / 'build/esp/usr/apps/chrome/chrome')], text=True)
bias = int(re.search(r'#define ELF_PIE_BIAS\s+(0x[0-9a-fA-F]+)',
                    (root / 'kernel/process/exec_internal.h').read_text()).group(1), 16)
symbols = (
    'chrome::NewTab(BrowserWindowInterface*, NewTabTypes)',
    'content::RenderProcessHostImpl::Init()',
    'Navigate(NavigateParams*)',
    'TabStripModel::AddTab(std::__Cr::unique_ptr<tabs::TabModel, std::__Cr::default_delete<tabs::TabModel>>, int, ui::PageTransition, int, std::__Cr::optional<tab_groups::TabGroupId>)',
    'content::NavigationControllerImpl::LoadURLWithParams(content::NavigationController::LoadURLParams const&)',
    '(anonymous namespace)::CreateTargetContents(NavigateParams const&, GURL const&)',
    'content::WebContents::Create(content::WebContents::CreateParams const&)',
    'TabHelpers::AttachTabHelpers(content::WebContents*, bool)',
    'viz::DisplayScheduler::OnBeginFrame(viz::BeginFrameArgs const&)',
    'viz::Display::DrawAndSwap(viz::DrawAndSwapParams const&)',
    'viz::SoftwareOutputDeviceOzone::BeginPaint(gfx::Rect const&)',
    'viz::SoftwareOutputDeviceOzone::EndPaint()',
    'cc::SingleThreadProxy::BeginMainFrame(viz::BeginFrameArgs const&)',
    'cc::SingleThreadProxy::ScheduledActionSendBeginMainFrame(viz::BeginFrameArgs const&)',
    'cc::SingleThreadProxy::DoCommit(viz::BeginFrameArgs const&)',
    'cc::LayerTreeHostImpl::ActivateSyncTree()',
    'viz::RootCompositorFrameSinkImpl::DisplayWillDrawAndSwap(bool, std::__Cr::vector<std::__Cr::unique_ptr<viz::AggregatedRenderPass, std::__Cr::default_delete<viz::AggregatedRenderPass>>, std::__Cr::allocator<std::__Cr::unique_ptr<viz::AggregatedRenderPass, std::__Cr::default_delete<viz::AggregatedRenderPass>>>>*)',
    'ThumbnailTabHelper::CaptureThumbnailOnTabBackgrounded()',
)
output = subprocess.check_output(
    ['llvm-nm', '-C', '--defined-only', str(binary)], text=True)
addresses = {}
for line in output.splitlines():
    fields = line.split(' ', 2)
    if len(fields) == 3 and fields[1] in ('t', 'T') and fields[2] in symbols:
        addresses[fields[2]] = int(fields[0], 16) + bias
assert set(addresses) == set(symbols), set(symbols) - set(addresses)
for prefix in ('viz::DirectRenderer::DrawFrame(', 'viz::SoftwareRenderer::BeginDrawingRenderPass('):
    matches = [line.split(' ', 2) for line in output.splitlines()
               if len(line.split(' ', 2)) == 3 and line.split(' ', 2)[1] in ('t','T')
               and line.split(' ', 2)[2].startswith(prefix)
               and '::$_' not in line.split(' ', 2)[2]]
    assert len(matches) == 1, prefix
    addresses[prefix] = int(matches[0][0], 16) + bias
valid = set(addresses.values())
for filename in ('trace-first-new-tab.gdb', 'trace-newtab-syscall-cost.gdb',
                 'trace-newtab-frame-stages.gdb', 'trace-newtab-commit.gdb',
                 'trace-draw-paint-pair.gdb', 'trace-root-render-pass.gdb'):
    script = (root / 'tools/chromium' / filename).read_text()
    assert f'Build ID: {build_id}' in script, filename
    # User text addresses live in the PIE range, unlike record sizes/constants.
    literals = {int(x, 16) for x in re.findall(r'0x[0-9a-fA-F]+', script)
                if 0x40000000 < int(x, 16) < 0x80000000}
    assert literals and literals <= valid, (filename, literals - valid)
    assert addresses[symbols[0]] in literals, filename
    assert '.delete()' not in script and 'gdb.post_event' in script, filename
    assert 'Refusing stale' in script, filename
    if filename == 'trace-first-new-tab.gdb':
        stages = ast.literal_eval(re.search(r'stages = (\{.*?\n\})', script, re.S).group(1))
        names = ('renderer', 'navigate', 'tabstrip', 'loadurl',
                 'contents', 'webcontents', 'helpers')
        for name, symbol in zip(names, symbols[1:8]):
            assert stages[name][0] == addresses[symbol], (filename, name)
    elif filename == 'trace-newtab-frame-stages.gdb':
        for label, symbol in zip(('DISPLAY_BEGIN_FRAME', 'DRAW_AND_SWAP',
                                  'BEGIN_PAINT', 'END_PAINT'), symbols[8:12]):
            entries = re.findall(r"Stage\('\*(0x[0-9a-f]+)','" + label + r"'\)", script)
            assert entries and all(int(x, 16) == addresses[symbol] for x in entries), label
    elif filename == 'trace-newtab-commit.gdb':
        stages = ast.literal_eval(re.search(r'stages=(\{.*?\n\})', script, re.S).group(1))
        labels = dict(zip(('BEGIN_MAIN_FRAME', 'SEND_BEGIN_MAIN_FRAME',
                           'DO_COMMIT', 'ACTIVATE_SYNC_TREE'), symbols[12:16]))
        for pairs in stages.values():
            assert len(pairs) + 2 == 4
            for address, label in pairs:
                assert address == addresses[labels[label]], label
    elif filename == 'trace-draw-paint-pair.gdb':
        for label, symbol in zip(('Input', 'Draw', 'Paint', 'Decision'),
                                 (symbols[0], symbols[9], symbols[10], symbols[16])):
            assert f"{label}('*{hex(addresses[symbol])}'" in script, label
        assert "mode=='return' and return_pc is None" in script
        assert "if mode=='decision':Decision" in script
    elif filename == 'trace-newtab-syscall-cost.gdb':
        assert f"Entry('*{hex(addresses[symbols[0]])}'" in script
        assert "write('g_syscall_cost_tid',0)" in script
    elif filename == 'trace-root-render-pass.gdb':
        for label, symbol in (('Frame', 'viz::DirectRenderer::DrawFrame('),
                               ('RenderPass', 'viz::SoftwareRenderer::BeginDrawingRenderPass(')):
            assert f"{label}('*{hex(addresses[symbol])}'" in script
        assert script.count('gdb.BP_HARDWARE_BREAKPOINT') == 5
        assert "if mode=='passes':RenderPass" in script
        assert f"else:Capture('*{hex(addresses[symbols[17]])}'" in script
        assert 'calls>=16' in script and 'samples>=32' in script
        assert '1<=count<=1024' in script and 'root+16' in script
print('Six used NewTab traces: deployed build identity, resolved PIE symbols and deferred detach passed')
