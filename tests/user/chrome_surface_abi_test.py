"""Compare compiled kernel/Chromium surface layouts and ioctl numbers."""
from pathlib import Path
import subprocess
import sys
import tempfile
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from chromium_source import chromium_source

root = Path(__file__).resolve().parents[2]
chrome = chromium_source()
c = r'''
#include <stdio.h>
#include <stddef.h>
#include "uapi/linux_abi.h"
#include "uapi/window.h"
int main(void) {
 printf("%u %u %zu %zu %zu %zu %zu %zu %zu %zu %zu %zu %zu %zu %zu\n",
  LINUX_WINDOW_IOCTL_SURFACE, OS_WINDOW_SURFACE_EXCHANGE,
  sizeof(os_surface_command_t),sizeof(os_surface_frame_t),
  offsetof(os_surface_command_t,frame),offsetof(os_surface_frame_t,token),
  offsetof(os_surface_frame_t,address),offsetof(os_surface_frame_t,capacity),
  offsetof(os_surface_frame_t,width),offsetof(os_surface_frame_t,stride),
  offsetof(os_surface_frame_t,reserved),
  offsetof(os_surface_frame_t,damage_x),offsetof(os_surface_frame_t,damage_y),
  offsetof(os_surface_frame_t,damage_width),offsetof(os_surface_frame_t,damage_height));
}
'''
cpp = r'''
#include <cstdio>
#include <cstddef>
#include "ui/ozone/platform/nativeguest/nativeguest_window_abi.h"
using namespace ui::nativeguest;
int main() {
 std::printf("%u %u %zu %zu %zu %zu %zu %zu %zu %zu %zu %zu %zu %zu %zu\n",
  kWindowIoctlSurface,kWindowSurfaceExchange,
  sizeof(SurfaceCommand),sizeof(SurfaceFrame),
  offsetof(SurfaceCommand,frame),offsetof(SurfaceFrame,token),
  offsetof(SurfaceFrame,address),offsetof(SurfaceFrame,capacity),
  offsetof(SurfaceFrame,width),offsetof(SurfaceFrame,stride),
  offsetof(SurfaceFrame,reserved),
  offsetof(SurfaceFrame,damage_x),offsetof(SurfaceFrame,damage_y),
  offsetof(SurfaceFrame,damage_width),offsetof(SurfaceFrame,damage_height));
}
'''
with tempfile.TemporaryDirectory(prefix='surface-abi-') as directory:
    path = Path(directory)
    results = []
    for compiler, standard, name, body, include in (
            ('cc', '-std=c11', 'kernel', c, root / 'include'),
            ('c++', '-std=c++20', 'chrome', cpp, chrome)):
        source = path / (name + ('.c' if compiler == 'cc' else '.cc'))
        source.write_text(body)
        binary = path / name
        subprocess.run([compiler, standard, '-Wall', '-Wextra', '-Werror',
                        '-I', str(include), str(source), '-o', str(binary)], check=True)
        results.append(subprocess.check_output([str(binary)], text=True).strip())
    assert results[0] == results[1], results
print('Kernel/Chrome compiled surface ABI matches:', results[0])
