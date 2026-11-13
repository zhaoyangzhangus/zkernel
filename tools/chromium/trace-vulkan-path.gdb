set pagination off
set confirm off
python
import gdb
import os
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path('tools/chromium').resolve()))
from trace_build import validate_chrome_symbols

binary = validate_chrome_symbols()
# 从匹配的可执行文件中解析入口，而不是保留会在下次 Chromium 增量构建后
# 悄然过期的地址。
methods = {
    'content::VizProcessTransportFactory::OnEstablishedGpuChannel(': 'CHANNEL',
    'gpu::SharedContextState::InitializeVulkanOnly(': 'VULKAN_CONTEXT',
    'viz::SkiaOutputDeviceVulkanCanvas::Present(': 'VULKAN_PRESENT',
    'viz::SoftwareOutputSurface::SoftwareOutputSurface(': 'SOFTWARE_SURFACE',
}
if os.environ.get('OS_VULKAN_TRACE_STAGE', 'present') == 'policy':
    methods = {
        'content::VizProcessTransportFactory::OnEstablishedGpuChannel(': 'CHANNEL',
        'content::VizProcessTransportFactory::TryCreateContextsForGpuCompositing(': 'TRY_CONTEXT',
        'content::VizProcessTransportFactory::DisableGpuCompositing(ui::Compositor*)': 'DISABLE_COMPOSITING',
        'gpu::SharedContextState::InitializeVulkanOnly(': 'VULKAN_CONTEXT',
    }
elif os.environ.get('OS_VULKAN_TRACE_STAGE', 'present') == 'contexts':
    methods = {
        'content::VizProcessTransportFactory::TryCreateContextsForGpuCompositing(': 'TRY_CONTEXT',
        'gpu::RasterCommandBufferStub::Initialize(': 'RASTER_INITIALIZE',
        'gpu::SharedContextState::InitializeSkia(': 'SKIA_INITIALIZE',
    }
elif os.environ.get('OS_VULKAN_TRACE_STAGE', 'present') == 'display':
    methods = {
        'viz::OutputSurfaceProviderImpl::CreateOutputSurface(': 'OUTPUT_SELECTION',
        'viz::SkiaOutputSurfaceImplOnGpu::Initialize()': 'DISPLAY_INITIALIZE',
        'viz::SkiaOutputSurfaceImplOnGpu::InitializeForVulkan()': 'DISPLAY_VULKAN_INITIALIZE',
        'viz::SoftwareOutputSurface::SoftwareOutputSurface(': 'SOFTWARE_SURFACE',
    }
elif os.environ.get('OS_VULKAN_TRACE_STAGE', 'present') != 'present':
    raise gdb.GdbError('Unknown Vulkan trace stage')
entries = {method: set() for method in methods}
process = subprocess.Popen(['nm', '-C', str(binary)], stdout=subprocess.PIPE,
                           text=True)
for line in process.stdout:
    fields = line.strip().split(None, 2)
    if len(fields) != 3 or fields[1] not in ('t', 'T'):
        continue
    for method in methods:
        if fields[2].startswith(method):
            entries[method].add(int(fields[0], 16))
if process.wait() != 0:
    raise gdb.GdbError('Cannot read Chrome symbols')
if any(len(addresses) != 1 for addresses in entries.values()):
    raise gdb.GdbError('Missing or ambiguous Vulkan path entry symbols')

gdb.execute('target remote ' + os.environ.get(
    'OS_VULKAN_GDB_SOCKET', '/tmp/system-vulkan.sock'))
capture_returns = os.environ.get('OS_VULKAN_TRACE_STAGE') in ('contexts', 'display')
if capture_returns:
    gdb.execute('add-symbol-file build/current-inspection-types.o 0')
    cpus = gdb.Value(int(gdb.parse_and_eval('&g_cpus'))).cast(
        gdb.lookup_type('scheduler_cpu_t').pointer())

def current_thread():
    return cpus[gdb.selected_thread().num - 1]['queue']['current'].dereference()

def return_address(thread, address):
    process = thread['process'].dereference()
    table = int(process['vm'].dereference()['root_table']['value'])
    mask = 0x000ffffffffff000
    memory = gdb.selected_inferior()
    for shift in (39, 30, 21, 12):
        entry = int.from_bytes(bytes(memory.read_memory(
            0xffff800000000000 + table + ((address >> shift) & 511) * 8, 8)), 'little')
        if entry & 5 != 5:
            raise gdb.GdbError('Unmapped return address')
        if shift in (30, 21) and entry & 128:
            physical = (entry & mask & ~((1 << shift) - 1)) + (address & ((1 << shift) - 1))
            break
        table = entry & mask
    else:
        physical = table + (address & 4095)
    if address & 4095 > 4088:
        raise gdb.GdbError('Return address crosses page boundary')
    return int.from_bytes(bytes(memory.read_memory(
        0xffff800000000000 + physical, 8)), 'little')

class ReturnHit(gdb.Breakpoint):
    def __init__(self, address, label, tid, sp):
        super().__init__('*%#x' % address, type=gdb.BP_HARDWARE_BREAKPOINT)
        self.label, self.tid, self.sp = label, tid, sp

    def stop(self):
        if (int(current_thread()['tid']) != self.tid or
                int(gdb.parse_and_eval('$rsp')) != self.sp):
            return False
        mask = 0xff if self.label in ('SKIA_INITIALIZE', 'DISPLAY_INITIALIZE',
                                    'DISPLAY_VULKAN_INITIALIZE') else 0xffffffff
        print('VULKAN_PATH_RETURN label=%s tid=%d result=%d' % (
            self.label, self.tid, int(gdb.parse_and_eval('$rax')) & mask), flush=True)
        self.enabled = False
        return False

class PathHit(gdb.Breakpoint):
    def __init__(self, address, label):
        super().__init__('*%#x' % address, type=gdb.BP_HARDWARE_BREAKPOINT)
        self.label = label

    def stop(self):
        print('VULKAN_PATH_HIT label=%s pc=%#x' % (
            self.label, int(gdb.parse_and_eval('$pc'))), flush=True)
        self.enabled = False
        if self.label == 'OUTPUT_SELECTION':
            print('VULKAN_OUTPUT_SELECTION gpu=%d surface=%#x' % (
                int(gdb.parse_and_eval('$rdx')) & 0xff,
                int(gdb.parse_and_eval('$rsi'))), flush=True)
        if capture_returns and self.label not in ('OUTPUT_SELECTION', 'SOFTWARE_SURFACE'):
            thread = current_thread()
            sp = int(gdb.parse_and_eval('$rsp'))
            ReturnHit(return_address(thread, sp), self.label,
                      int(thread['tid']), sp + 8)
        return False

for method, addresses in entries.items():
    PathHit(next(iter(addresses)) + 0x40000000, methods[method])
print('VULKAN_PATH_TRACE_READY', flush=True)
end
continue
detach
quit
