"""Validate the keyboard trace's 24-byte metadata ABI against both real headers."""
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
#include <stddef.h>
#include <uapi/input.h>
#include <uapi/window.h>
#include "ui/ozone/platform/nativeguest/nativeguest_window_abi.h"
static_assert(sizeof(os_input_event_t)==24);
static_assert(sizeof(ui::nativeguest::InputEvent)==24);
static_assert(sizeof(os_window_event_t)==64);
static_assert(sizeof(ui::nativeguest::WindowEvent)==sizeof(os_window_event_t));
static_assert(sizeof(ui::nativeguest::ControlEvent)==sizeof(os_control_event_t));
static_assert(offsetof(ui::nativeguest::WindowEvent,pointer_x)==offsetof(os_window_event_t,pointer_x));
static_assert(offsetof(ui::nativeguest::WindowEvent,pointer_y)==offsetof(os_window_event_t,pointer_y));
'''
for field,offset in [('timestamp',0),('device_id',8),('type',12),('flags',14),('code',16),('value',20)]:
    for kind in ('os_input_event_t','ui::nativeguest::InputEvent'):
        program += f'static_assert(offsetof({kind},{field})=={offset});\n'
for field in ('timestamp','target','code','value','action','flags','node_index','node_kind','tree_revision','user_data'):
    program += f'static_assert(offsetof(os_control_event_t,{field})==offsetof(ui::nativeguest::ControlEvent,{field}));\n'
program += 'int main() { return 0; }\n'
with tempfile.TemporaryDirectory(prefix='system-native-trace-abi-') as directory:
    path=Path(directory)
    (path/'test.cc').write_text(program)
    subprocess.run(['c++','-std=c++17','-Wall','-Wextra','-Werror',
                    '-I'+str(root/'include'),'-I'+str(chromium),
                    str(path/'test.cc'),'-o',str(path/'test')],check=True)
    subprocess.run([str(path/'test')],check=True)
print('Native event ABI: kernel/Chromium input, indexed control payload, 64-byte window envelope and every control field offset PASS')
