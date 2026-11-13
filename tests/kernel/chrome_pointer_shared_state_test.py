"""Compile real Ozone event conversion; press/release can cross window queues."""
from pathlib import Path as _TestPath
import sys as _test_sys
_test_sys.path.insert(0, str(_TestPath(__file__).resolve().parents[1]))
from chromium_source import chromium_source
from pathlib import Path
import argparse
import resource
import subprocess
import tempfile

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--negative-controls', action='store_true')
args = parser.parse_args()
root = (chromium_source() / 'ui/ozone/platform/nativeguest')
source = (root/'nativeguest_window.cc').read_text()
method = source[source.index('std::unique_ptr<Event> NativeGuestWindow::CreateEvent('):
                source.index('\nvoid NativeGuestWindow::Show(')]
button_helper = source[source.index('int MouseButtonFlag('):
                       source.index('\ngfx::Rect NormalizeBounds(')]
header = (root/'nativeguest_window_manager.h').read_text()
accessor = header[header.index('  int& mouse_button_flags()'):
                  header.index('\n', header.index('  int& mouse_button_flags()'))]
program = r'''
#include <cassert>
#include <memory>
#include <cstdint>
namespace gfx {
struct Point { int x=0,y=0; void Offset(int a,int b){x+=a;y+=b;} };
struct Rect { int a=0,b=0; int x()const{return a;} int y()const{return b;} };
struct Vector2d { int x,y; };
}
namespace base { struct TimeTicks { static TimeTicks Now(){return {};} }; }
namespace nativeguest {
enum { kRelease=0,kPress=1,kRepeat=2,kKey=3,kButton=4,kRelative=5,kAbsolute=6,
       kRelativeWheel=7,kButtonLeft=8,kButtonMiddle=9,kButtonRight=10 };
struct InputEvent { uint32_t type,code; int value; uint32_t device_id=1; };
}
enum { EF_NONE=0,EF_LEFT_MOUSE_BUTTON=1,EF_MIDDLE_MOUSE_BUTTON=2,
       EF_RIGHT_MOUSE_BUTTON=4,EF_IS_REPEAT=16 };
enum class EventType { kKeyReleased,kKeyPressed,kMousePressed,kMouseReleased,kMouseMoved };
enum class DomCode { NONE,A };
using KeyboardCode=int;
struct KeycodeConverter { static DomCode UsbKeycodeToDomCode(uint32_t){return DomCode::A;} };
KeyboardCode DomCodeToUsLayoutKeyboardCode(DomCode){return 1;}
int ModifierFlagForDomCode(DomCode){return 0;}
struct Event {
 int flags; EventType type;
 Event(EventType t,int f):flags(f),type(t){} virtual ~Event()=default;
 void set_source_device_id(int){}
};
struct KeyEvent:Event {
 KeyEvent(EventType t,int,DomCode,int f,base::TimeTicks):Event(t,f){}
 void set_scan_code(uint32_t){}
};
struct MouseEvent:Event {
 MouseEvent(EventType t,gfx::Point,gfx::Point,base::TimeTicks,int f,int):Event(t,f){}
};
struct MouseWheelEvent:Event {
 static constexpr int kWheelDelta=120;
 MouseWheelEvent(gfx::Vector2d,gfx::Point,gfx::Point,base::TimeTicks,int f,int):Event(EventType::kMouseMoved,f){}
};
struct NativeGuestWindowManager {
 int mouse_button_flags_=0;
 gfx::Point cursor;
 void SetCursorScreenPoint(gfx::Point p){cursor=p;}
''' + accessor + r'''
};
struct NativeGuestWindow {
 NativeGuestWindowManager* manager_;
 gfx::Rect bounds_;
 int keyboard_modifier_flags_=0,mouse_button_flags_=0;
 std::unique_ptr<Event> CreateEvent(const nativeguest::InputEvent&,const gfx::Point&);
};
''' + button_helper + method + r'''
int main(){
 NativeGuestWindowManager manager,independent;
 NativeGuestWindow browser{&manager,{10,10}},popup{&manager,{557,90}},other{&independent,{0,0}},fallback{nullptr,{0,0}};
 auto button=[&](NativeGuestWindow& w,uint32_t code,int value){
  return w.CreateEvent({nativeguest::kButton,code,value},{3,4});
 };
 auto move=[&](NativeGuestWindow& w){return w.CreateEvent({nativeguest::kRelative,0,0},{5,6});};
 // 打开菜单的按下事件命中浏览器，但释放事件由菜单接收。
 assert(button(browser,nativeguest::kButtonLeft,nativeguest::kPress)->flags==1);
 assert(manager.mouse_button_flags()==1);
 assert(button(popup,nativeguest::kButtonLeft,nativeguest::kRelease)->flags==1);
 assert(manager.mouse_button_flags()==0);
 assert(move(browser)->flags==0);
 assert(manager.cursor.x==15 && manager.cursor.y==16);
 // 反转队列顺序，并验证捕获重路由时的转换操作具有幂等性。
 button(popup,nativeguest::kButtonLeft,nativeguest::kPress);
 button(browser,nativeguest::kButtonLeft,nativeguest::kPress);
 assert(manager.mouse_button_flags()==1);
 button(browser,nativeguest::kButtonLeft,nativeguest::kRelease);
 assert(move(popup)->flags==0);
 // 切换窗口时保留另一枚仍被物理按住的按键状态。
 button(browser,nativeguest::kButtonLeft,nativeguest::kPress);
 button(popup,nativeguest::kButtonRight,nativeguest::kPress);
 assert(button(popup,nativeguest::kButtonLeft,nativeguest::kRelease)->flags==5);
 assert(move(browser)->flags==4);
 assert(move(other)->flags==0);
 button(browser,nativeguest::kButtonRight,nativeguest::kRelease);
 assert(move(popup)->flags==0);
 // 销毁菜单后，不能遗留一份独立的物理状态副本。
 {
  auto transient=std::make_unique<NativeGuestWindow>(NativeGuestWindow{&manager,{20,20}});
  button(browser,nativeguest::kButtonLeft,nativeguest::kPress);
  button(*transient,nativeguest::kButtonLeft,nativeguest::kRelease);
 }
 assert(move(browser)->flags==0);
 assert(!button(browser,999,nativeguest::kPress));
 assert(manager.mouse_button_flags()==0);
 button(fallback,nativeguest::kButtonLeft,nativeguest::kPress);
 assert(move(fallback)->flags==1);
 button(fallback,nativeguest::kButtonLeft,nativeguest::kRelease);
 assert(move(fallback)->flags==0);
}
'''
with tempfile.TemporaryDirectory(prefix='liteos-pointer-shared-') as directory:
    path=Path(directory)
    def run(name,text,negative=False):
        cpp=path/(name+'.cc'); exe=path/name; cpp.write_text(text)
        subprocess.run(['c++','-std=c++20','-Wall','-Wextra','-Werror',
                        '-fsanitize=address,undefined','-fno-pie','-no-pie',
                        str(cpp),'-o',str(exe)],check=True)
        def no_core(): resource.setrlimit(resource.RLIMIT_CORE,(0,0))
        result=subprocess.run([str(exe)],capture_output=True,text=True,preexec_fn=no_core)
        if negative:
            assert result.returncode!=0 and 'manager.mouse_button_flags()==1' in result.stderr,result.stderr
            print('Negative control rejected: per-window pointer state')
        else:
            assert result.returncode==0,result.stderr
    run('production',program)
    if args.negative_controls:
        shared='manager_ ? manager_->mouse_button_flags() : mouse_button_flags_'
        assert program.count(shared)==1
        run('per-window',program.replace(shared,'mouse_button_flags_'),True)
print('Shared pointer state: cross-window press/release, capture reroute, multi-button, popup destruction and fallback passed')
