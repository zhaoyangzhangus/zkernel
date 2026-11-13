"""Compare native UAPI and Chromium's actual mirrored minimize ABI."""
from pathlib import Path as _TestPath
import sys as _test_sys
_test_sys.path.insert(0, str(_TestPath(__file__).resolve().parents[1]))
from chromium_source import chromium_source
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
chromium = chromium_source()
program = r'''
#include <cassert>
#include <cstring>
#include <cstddef>
#include <initializer_list>
#include "uapi/window.h"
#include "uapi/window_state.h"
// 原生头文件使用 C11 写法；在宿主 g++ 下也执行相同检查。
#define _Static_assert static_assert
#include "uapi/linux_abi.h"
#undef _Static_assert
#include "ui/ozone/platform/nativeguest/nativeguest_window_abi.h"
namespace bridge=ui::nativeguest;
static_assert(sizeof(os_window_event_t)==sizeof(bridge::WindowEvent));
static_assert(sizeof(os_window_minimized_event_t)==sizeof(bridge::MinimizedEvent));
static_assert(offsetof(os_window_event_t,minimized)==offsetof(bridge::WindowEvent,minimized));
static_assert(offsetof(os_window_event_t,pointer_x)==offsetof(bridge::WindowEvent,pointer_x));
static_assert(sizeof(linux_window_command_t)==sizeof(bridge::WindowCommand));
static_assert(bridge::kMinimize==LINUX_WINDOW_OPERATION_MINIMIZE);
static_assert(bridge::kMinimizedEvent==OS_WINDOW_EVENT_MINIMIZED);
static_assert(bridge::kMovedEvent==OS_WINDOW_EVENT_MOVED);
static_assert(bridge::kFullscreen==LINUX_WINDOW_OPERATION_FULLSCREEN);
static_assert(sizeof(os_window_moved_event_t)==sizeof(bridge::MovedEvent));
static_assert(offsetof(os_window_event_t,moved)==offsetof(bridge::WindowEvent,moved));
int main(){
 for(int32_t x:{INT32_MIN,-1,0,INT32_MAX}){
   os_window_event_t native{};native.type=OS_WINDOW_EVENT_MOVED;
   native.moved.x=x;native.moved.y=123;
   bridge::WindowEvent chrome{};std::memcpy(&chrome,&native,sizeof(native));
   assert(chrome.type==bridge::kMovedEvent&&chrome.moved.x==x&&chrome.moved.y==123);
   assert(!chrome.moved.reserved&&!chrome.moved.reserved2);
 }
 for(unsigned state=0;state<=1;++state){
   os_window_event_t native{};
   native.identifier=8;native.type=OS_WINDOW_EVENT_MINIMIZED;
   native.minimized.minimized=state;
   bridge::WindowEvent chrome{};std::memcpy(&chrome,&native,sizeof(native));
   assert(chrome.type==bridge::kMinimizedEvent && chrome.minimized.minimized==state);
   assert(chrome.minimized.reserved==0 && chrome.minimized.reserved2==0 && chrome.minimized.reserved3==0);
   bridge::WindowCommand command{};command.operation=bridge::kMinimize;command.width=state;
   linux_window_command_t kernel{};std::memcpy(&kernel,&command,sizeof(kernel));
   assert(kernel.operation==LINUX_WINDOW_OPERATION_MINIMIZE && kernel.width==state);
   assert(kernel.height==0 && kernel.reserved==0 && kernel.reserved2==0 && kernel.address==0 && kernel.length==0);
 }
}
'''
with tempfile.TemporaryDirectory(prefix='system-minimize-abi-') as directory:
    path = Path(directory)
    (path/'test.cc').write_text(program)
    subprocess.run(['c++', '-std=c++20', '-Wall', '-Wextra', '-Werror',
                    '-I', str(root/'include'), '-I', str(chromium),
                    str(path/'test.cc'), '-o', str(path/'test')], check=True)
    subprocess.run([str(path/'test')], check=True)
print('Minimize ABI: native/Chromium command and event sizes, offsets and payloads match')
