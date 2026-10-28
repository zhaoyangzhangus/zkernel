"""Refresh the twelve entry-symbol traces; never attach to a running guest.

The default is a read-only check. --apply mechanically updates symbol literals
and their build guards only after validating every input. This does not certify
instruction offsets or arbitrary object-layout diagnostics in other scripts.
"""
import argparse
from pathlib import Path
import re
import subprocess

from trace_build import ROOT, _chrome_build_id, validate_chrome_symbols

FIRST = 'trace-first-new-tab.gdb'
FILES = (
    FIRST, 'trace-newtab-frame-stages.gdb', 'trace-newtab-first-present.gdb',
    'trace-address-delivery.gdb', 'trace-newtab-syscall-cost.gdb',
    'trace-newtab-commit.gdb', 'trace-draw-paint-pair.gdb',
    'trace-root-render-pass.gdb', 'trace-newtab-futex.gdb',
    'trace-newtab-delivery.gdb', 'trace-native-keys.gdb',
    'trace-cache-read-error.gdb',
)

# Resolve exact entry symbols, not an assumed link-wide address delta. Each
# anchor names an existing semantic role; tests independently verify its ELF.
ANCHORS = (
    (FIRST, r"'renderer': \((0x[0-9a-f]+)", 'content::RenderProcessHostImpl::Init()'),
    (FIRST, r"'navigate': \((0x[0-9a-f]+)", 'Navigate(NavigateParams*)'),
    (FIRST, r"'tabstrip': \((0x[0-9a-f]+)", 'TabStripModel::AddTab(std::__Cr::unique_ptr<tabs::TabModel, std::__Cr::default_delete<tabs::TabModel>>, int, ui::PageTransition, int, std::__Cr::optional<tab_groups::TabGroupId>)'),
    (FIRST, r"'loadurl': \((0x[0-9a-f]+)", 'content::NavigationControllerImpl::LoadURLWithParams(content::NavigationController::LoadURLParams const&)'),
    (FIRST, r"'contents': \((0x[0-9a-f]+)", '(anonymous namespace)::CreateTargetContents(NavigateParams const&, GURL const&)'),
    (FIRST, r"'webcontents': \((0x[0-9a-f]+)", 'content::WebContents::Create(content::WebContents::CreateParams const&)'),
    (FIRST, r"'helpers': \((0x[0-9a-f]+)", 'TabHelpers::AttachTabHelpers(content::WebContents*, bool)'),
    (FIRST, r"NewTabEntry\('\*(0x[0-9a-f]+)'", 'chrome::NewTab(BrowserWindowInterface*, NewTabTypes)'),
    ('trace-newtab-first-present.gdb', r"'address':(0x[0-9a-f]+)", 'OmniboxViewViews::OnMousePressed(ui::MouseEvent const&)'),
    ('trace-newtab-first-present.gdb', r"'addressrelease':(0x[0-9a-f]+)", 'OmniboxViewViews::OnMouseReleased(ui::MouseEvent const&)'),
    ('trace-newtab-frame-stages.gdb', r"Stage\('\*(0x[0-9a-f]+)','DISPLAY_BEGIN_FRAME'", 'viz::DisplayScheduler::OnBeginFrame(viz::BeginFrameArgs const&)'),
    ('trace-newtab-frame-stages.gdb', r"Stage\('\*(0x[0-9a-f]+)','DRAW_AND_SWAP'", 'viz::Display::DrawAndSwap(viz::DrawAndSwapParams const&)'),
    ('trace-newtab-frame-stages.gdb', r"Stage\('\*(0x[0-9a-f]+)','BEGIN_PAINT'", 'viz::SoftwareOutputDeviceOzone::BeginPaint(gfx::Rect const&)'),
    ('trace-newtab-frame-stages.gdb', r"Stage\('\*(0x[0-9a-f]+)','END_PAINT'", 'viz::SoftwareOutputDeviceOzone::EndPaint()'),
    ('trace-newtab-commit.gdb', r"\((0x[0-9a-f]+),'BEGIN_MAIN_FRAME'", 'cc::SingleThreadProxy::BeginMainFrame(viz::BeginFrameArgs const&)'),
    ('trace-newtab-commit.gdb', r"\((0x[0-9a-f]+),'SEND_BEGIN_MAIN_FRAME'", 'cc::SingleThreadProxy::ScheduledActionSendBeginMainFrame(viz::BeginFrameArgs const&)'),
    ('trace-newtab-commit.gdb', r"\((0x[0-9a-f]+),'DO_COMMIT'", 'cc::SingleThreadProxy::DoCommit(viz::BeginFrameArgs const&)'),
    ('trace-newtab-commit.gdb', r"\((0x[0-9a-f]+),'ACTIVATE_SYNC_TREE'", 'cc::LayerTreeHostImpl::ActivateSyncTree()'),
    ('trace-draw-paint-pair.gdb', r"Decision\('\*(0x[0-9a-f]+)'", 'viz::RootCompositorFrameSinkImpl::DisplayWillDrawAndSwap(bool, std::__Cr::vector<std::__Cr::unique_ptr<viz::AggregatedRenderPass, std::__Cr::default_delete<viz::AggregatedRenderPass>>, std::__Cr::allocator<std::__Cr::unique_ptr<viz::AggregatedRenderPass, std::__Cr::default_delete<viz::AggregatedRenderPass>>>>*)'),
    ('trace-root-render-pass.gdb', r"Frame\('\*(0x[0-9a-f]+)'", 'viz::DirectRenderer::DrawFrame('),
    ('trace-root-render-pass.gdb', r"RenderPass\('\*(0x[0-9a-f]+)'", 'viz::SoftwareRenderer::BeginDrawingRenderPass('),
    ('trace-root-render-pass.gdb', r"Capture\('\*(0x[0-9a-f]+)'", 'ThumbnailTabHelper::CaptureThumbnailOnTabBackgrounded()'),
    ('trace-native-keys.gdb', r"Key\('\*(0x[0-9a-f]+)'", 'ui::NativeGuestWindow::CreateEvent(ui::nativeguest::InputEvent const&, gfx::Point const&)'),
    ('trace-cache-read-error.gdb', r"CacheError\('\*(0x[0-9a-f]+)'", 'net::HttpCache::Transaction::OnCacheReadError(int, bool)'),
)


def refreshed(scripts, nm_output, identity, bias):
    if not re.fullmatch(r'Build ID: [0-9a-f]+', identity):
        raise RuntimeError('Invalid target build identity')
    if bias != 0x40000000:
        raise RuntimeError('PIE bias changed; review the diagnostic address range')
    old_ids = set()
    for name in FILES:
        script = scripts[name]
        guard = re.findall(r"^expected = '(Build ID: [0-9a-f]+)'$", script, re.M)
        if len(guard) != 1 or script.count('validate_chrome_build(expected)') != 1:
            raise RuntimeError(f'Missing or ambiguous build guard: {name}')
        if script.index('validate_chrome_build(expected)') > script.index('target remote'):
            raise RuntimeError(f'Build guard follows guest connection: {name}')
        old_ids.add(guard[0])
    if len(old_ids) != 1:
        raise RuntimeError('Trace set has mixed build identities; review before refreshing')
    symbols = []
    for line in nm_output.splitlines():
        match = re.fullmatch(r'([0-9a-fA-F]+) [tT] (.+)', line)
        if match:
            symbols.append((int(match[1], 16) + bias, match[2]))
    replacements = {}
    for name, pattern, symbol in ANCHORS:
        old = {int(x, 16) for x in re.findall(pattern, scripts[name])}
        if len(old) != 1:
            raise RuntimeError(f'Missing or ambiguous anchor: {name}: {symbol}')
        candidates = [address for address, label in symbols
                      if (label.startswith(symbol) and '::$_' not in label
                          if symbol.endswith('(') else label == symbol)]
        if len(candidates) != 1 or not bias < candidates[0] < 0x80000000:
            raise RuntimeError(f'Missing, ambiguous or out-of-range ELF symbol: {symbol}')
        address = old.pop()
        if address in replacements and replacements[address] != candidates[0]:
            raise RuntimeError('Conflicting source symbol anchors')
        replacements[address] = candidates[0]
    result = {}
    for name in FILES:
        def replace(match):
            value = int(match[0], 16)
            if not bias < value < 0x80000000:
                return match[0]
            if value not in replacements:
                raise RuntimeError(f'Unrecognized user address in {name}: {match[0]}')
            return hex(replacements[value])
        text = re.sub(r'0x[0-9a-fA-F]+', replace, scripts[name])
        text = text.replace(next(iter(old_ids)), identity)
        text = re.sub(r'(# ONLY Chrome build ID )[0-9a-f]+',
                      lambda m: m[1] + identity.split()[-1], text)
        result[name] = text
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--apply', action='store_true')
    args = parser.parse_args()
    binary = validate_chrome_symbols()
    identity = _chrome_build_id(binary)
    directory = Path(__file__).resolve().parent
    scripts = {name: (directory / name).read_text() for name in FILES}
    bias = int(re.search(r'#define ELF_PIE_BIAS\s+(0x[0-9a-fA-F]+)',
                        (ROOT / 'kernel/process/exec_internal.h').read_text())[1], 16)
    output = subprocess.check_output(
        ['llvm-nm', '-C', '--defined-only', str(binary)], text=True)
    updates = refreshed(scripts, output, identity, bias)
    # Recheck after the slow symbol read; never stamp addresses onto a new ELF
    # if staging or linking raced this invocation.
    validate_chrome_symbols()
    if _chrome_build_id(binary) != identity:
        raise RuntimeError('Chrome changed while resolving symbols')
    changed = [name for name in FILES if scripts[name] != updates[name]]
    if args.apply:
        # Validate the entire set before the first mechanical rewrite.
        if any((directory / n).read_text() != scripts[n] for n in FILES):
            raise RuntimeError('Trace scripts changed while resolving symbols')
        for name in changed:
            (directory / name).write_text(updates[name])
    print(f'{identity}: {len(FILES)} traces, {len(ANCHORS)} symbols; '
          f'{len(changed)} {"updated" if args.apply else "stale"}')
    return 0 if args.apply or not changed else 1


if __name__ == '__main__':
    raise SystemExit(main())
