"""Compile actual Ozone capture methods against a minimal delegate."""
from pathlib import Path as _TestPath
import sys as _test_sys
_test_sys.path.insert(0, str(_TestPath(__file__).resolve().parents[1]))
from chromium_source import chromium_source
import pathlib
import subprocess
import tempfile

source = (chromium_source() / 'ui/ozone/platform/nativeguest/nativeguest_window.cc').read_text()
methods = source[source.index('void NativeGuestWindow::SetCapture()'):source.index('void NativeGuestWindow::SetFullscreen(')]
program = '''
#include <cassert>
namespace nativeguest { enum { kCapture=7 }; struct WindowCommand { int operation=0, width=0; }; }
struct Delegate { int lost = 0; void OnLostCapture() { ++lost; } };
struct NativeGuestWindow {
 Delegate* delegate_; bool valid = true, closed_ = false, has_capture_ = false;
 bool visible_=true, capture_pending_=false;
 bool IsValid() const { return valid; }
 bool commit_ok=true; int commits=0, native_capture=0;
 bool Commit(nativeguest::WindowCommand command) {
   ++commits; assert(command.operation==nativeguest::kCapture);
   if (!commit_ok) return false;
   native_capture=command.width; return true;
 }
 void SetCapture(); void ReleaseCapture(); bool HasCapture() const;
};
''' + methods + '''
int main() {
 Delegate d; NativeGuestWindow w{&d};
 assert(!w.HasCapture()); w.ReleaseCapture(); assert(d.lost == 0);
 w.SetCapture(); assert(w.HasCapture());
 w.SetCapture(); assert(w.HasCapture());
 // Widget 的自动释放保护现在必须允许释放 Aura 捕获。
 if (w.HasCapture()) w.ReleaseCapture();
 assert(!w.HasCapture()); assert(d.lost == 1);
 w.ReleaseCapture(); assert(d.lost == 1);
 w.closed_ = true; w.SetCapture(); assert(!w.HasCapture());
 w.closed_ = false; w.valid = false; w.SetCapture(); assert(!w.HasCapture());
 w.valid=true; w.commit_ok=false; w.SetCapture(); assert(!w.HasCapture());
 w.commit_ok=true; w.SetCapture(); assert(w.HasCapture() && w.native_capture==1);
 w.commit_ok=false; int lost=d.lost;
 w.ReleaseCapture(); assert(w.HasCapture() && w.native_capture==1 && d.lost==lost);
 w.commit_ok=true;
 w.ReleaseCapture(); assert(!w.HasCapture() && w.native_capture==0);
 // 菜单会在 Show() 前请求捕获。窗口隐藏时不会尝试原生捕获，但 Aura 的释放保护
 // 仍须识别这一待处理请求。
 w.visible_=false;int before=w.commits;
 w.SetCapture();assert(w.HasCapture() && w.capture_pending_ && !w.has_capture_);
 assert(w.commits==before);
 w.SetCapture();assert(w.commits==before);
 lost=d.lost;w.ReleaseCapture();
 assert(!w.HasCapture() && !w.capture_pending_ && d.lost==lost+1);
 assert(w.commits==before);
}
'''
with tempfile.TemporaryDirectory() as directory:
    cpp = pathlib.Path(directory) / 'test.cc'
    cpp.write_text(program)
    exe = pathlib.Path(directory) / 'test'
    subprocess.run(['c++', '-std=c++17', '-Wall', '-Wextra', '-Werror', str(cpp), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
print('Capture state: acquire, auto-release guard, notification and idempotence passed')
