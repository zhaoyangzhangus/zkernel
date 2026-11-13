"""Exercise production bounds/state methods with native screen-fit checks."""
from pathlib import Path as _TestPath
import sys as _test_sys
_test_sys.path.insert(0, str(_TestPath(__file__).resolve().parents[1]))
from chromium_source import chromium_source
from pathlib import Path
import subprocess
import tempfile

source = (chromium_source() / 'ui/ozone/platform/nativeguest/nativeguest_window.cc').read_text()

def method(name):
    start = source.index('void NativeGuestWindow::' + name + '(')
    brace = source.index('{', start)
    depth = 1
    end = brace + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]

program = r'''
#include <algorithm>
#include <cassert>
#include <optional>
#include <memory>
#include <vector>
#include <cstdint>
namespace gfx {
struct Point { int x=0,y=0; bool operator==(const Point&) const = default; };
struct Size { int w,h; Size(int a,int b):w(a),h(b){} int width() const{return w;} int height() const{return h;} bool operator==(const Size&) const = default; };
struct Rect {
 int a,b,w,h;
 Point origin() const { return {a,b}; } Size size() const { return {w,h}; }
 int x() const{return a;} int y() const{return b;}
 int width() const{return w;} int height() const{return h;}
 void set_origin(Point p){a=p.x;b=p.y;} void set_size(Size s){w=s.w;h=s.h;}
 void set_x(int v){a=v;}void set_y(int v){b=v;}void set_width(int v){w=v;}void set_height(int v){h=v;}
 bool operator==(const Rect&) const = default;
};
}
namespace display {
struct Display { gfx::Rect work_area(){return {0,0,1920,1080};} };
struct Screen { static Screen* Get(){static Screen s;return &s;} Display GetPrimaryDisplay(){return {};} };
}
namespace nativeguest {
enum {kMove=1,kResize=2,kMinimize=6,kMaximize=11,kFullscreen=12};
struct WindowCommand {int operation=0,x=0,y=0;uint32_t width=0,height=0;};
}
enum class PlatformWindowState {kNormal,kMaximized,kFullScreen,kMinimized};
struct Delegate {bool CanMaximize(){return true;} bool CanFullscreen(){return true;} int activation_calls=0;bool active=true; void OnActivationChanged(bool a){active=a;++activation_calls;}};
class NativeGuestWindow {
 public:
 gfx::Rect bounds_{10,10,945,1060}, native=bounds_;
 std::optional<gfx::Rect> restored_bounds_;
 PlatformWindowState window_state_=PlatformWindowState::kNormal;
 PlatformWindowState fullscreen_restore_state_=PlatformWindowState::kNormal;
 PlatformWindowState pre_minimize_state_=PlatformWindowState::kNormal;
 struct Weak {
   std::shared_ptr<bool> alive;
   explicit operator bool() const {return *alive;}
 };
 struct Factory {
   std::shared_ptr<bool> alive=std::make_shared<bool>(true);
   Weak GetWeakPtr(){return {alive};}
 } weak_ptr_factory_;
 bool destroy_on_bounds=false;
 bool delete_on_bounds=false;
 bool delete_on_state=false;
 bool native_minimized=false;
 bool server_decorated_=false;
 bool top_level_=false;
 ~NativeGuestWindow(){*weak_ptr_factory_.alive=false;}
 bool visible_=true;
 Delegate delegate,*delegate_=&delegate;
 int fail_at=0,calls=0,notifications=0;
 std::vector<int> operations;
 gfx::Rect NormalizeBounds(gfx::Rect r){return r;}
 bool Commit(nativeguest::WindowCommand c) {
   ++calls;operations.push_back(c.operation);
   if(calls==fail_at)return false;
   if(c.operation==nativeguest::kMinimize){native_minimized=c.width!=0;return true;}
   if(c.operation==nativeguest::kMaximize){assert(c.width<=1);return true;}
   if(c.operation==nativeguest::kFullscreen){assert(c.width<=1);return true;}
   gfx::Rect next=native;
   if(c.operation==nativeguest::kMove)next.set_origin({c.x+(server_decorated_?1:0),c.y+(server_decorated_?41:0)});
   else next.set_size({static_cast<int>(c.width),static_cast<int>(c.height)});
   if(next.x()<(server_decorated_?1:0) || next.y()<(server_decorated_?41:0) || next.x()+next.width()+(server_decorated_?1:0)>1920 || next.y()+next.height()+(server_decorated_?1:0)>1080)return false;
   native=next;return true;
 }
 void UpdateBounds(gfx::Rect r){bounds_=r;++notifications;if(destroy_on_bounds)*weak_ptr_factory_.alive=false;if(delete_on_bounds)delete this;}
 void UpdateWindowState(PlatformWindowState s){assert(*weak_ptr_factory_.alive);window_state_=s;if(delete_on_state)delete this;}
 void SetBoundsInPixels(const gfx::Rect&);
 void Maximize();void Restore();
 void SetFullscreen(bool,int64_t);
 void Minimize();void HandleMinimized(bool);
};
''' + '\n'.join(method(n) for n in ('SetBoundsInPixels', 'Maximize', 'Restore', 'SetFullscreen', 'Minimize', 'HandleMinimized')) + r'''
int main(){
 const gfx::Rect normal{10,10,945,1060}, full{0,0,1920,1080};
 for(bool server_style:{false,true}){
  NativeGuestWindow top;top.top_level_=true;top.server_decorated_=server_style;
  top.SetFullscreen(true,0);assert(top.operations.back()==12&&top.window_state_==PlatformWindowState::kNormal);
  top.window_state_=PlatformWindowState::kFullScreen;top.Restore();assert(top.operations.back()==12&&top.window_state_==PlatformWindowState::kFullScreen);
  top.window_state_=PlatformWindowState::kNormal;top.Maximize();assert(top.operations.back()==11&&top.window_state_==PlatformWindowState::kNormal);
 }
 NativeGuestWindow server;server.server_decorated_=true;server.bounds_=server.native={241,176,640,360};
 for(unsigned i=0;i<50;++i){
  unsigned before=server.operations.size();server.Maximize();
  assert(server.operations.size()==before+1&&server.operations.back()==11);
  assert(server.window_state_==PlatformWindowState::kNormal); // state waits for native resize event
  server.window_state_=PlatformWindowState::kMaximized;server.Restore();
  assert(server.operations.size()==before+2&&server.operations.back()==11);
  assert(server.window_state_==PlatformWindowState::kMaximized);
  server.window_state_=PlatformWindowState::kNormal;
 }
 server.SetBoundsInPixels({301,241,800,500});assert((server.native==gfx::Rect{301,241,800,500}));
 // 复现旧原生实现的拒绝行为：全尺寸窗口无法移动到 (10,10)。
 NativeGuestWindow old;old.bounds_=old.native=full;
 nativeguest::WindowCommand old_move{};old_move.operation=nativeguest::kMove;
 old_move.x=10;old_move.y=10;
 assert(!old.Commit(old_move) && old.native==full);
 NativeGuestWindow w;
 for(int i=0;i<20;++i){
   w.Maximize();assert(w.bounds_==full && w.native==full);
   assert(w.window_state_==PlatformWindowState::kMaximized && w.restored_bounds_==normal);
   w.Restore();assert(w.bounds_==normal && w.native==normal);
   assert(w.window_state_==PlatformWindowState::kNormal && !w.restored_bounds_);
 }
 // 恢复失败时必须保留状态和已保存的边界，以便重试。
 for(int fail=1;fail<=2;++fail){
   NativeGuestWindow f;f.Maximize();f.calls=0;f.fail_at=fail;
   f.Restore();assert(f.window_state_==PlatformWindowState::kMaximized);
   assert(f.restored_bounds_==normal && f.bounds_==f.native);
   f.fail_at=0;f.Restore();assert(f.bounds_==normal && f.native==normal);
 }
 // 移动成功后扩展失败，应发布实际的部分边界。
 NativeGuestWindow f;f.fail_at=2;f.Maximize();
 assert(f.window_state_==PlatformWindowState::kNormal && f.bounds_==f.native);
 assert((f.bounds_==gfx::Rect{0,0,945,1060}));
 f.fail_at=0;f.Maximize();f.Restore();
 assert(f.bounds_==normal && f.native==normal);
 // 同时缩小和扩展时必须依次执行缩小、移动、扩展，不能经过无效的中间状态。
 NativeGuestWindow mixed;mixed.bounds_=mixed.native={1000,0,920,500};
 mixed.SetBoundsInPixels({0,500,1500,580});
 assert((mixed.native==gfx::Rect{0,500,1500,580}) && mixed.bounds_==mixed.native);
 NativeGuestWindow mixed2;mixed2.bounds_=mixed2.native={0,500,1500,580};
 mixed2.SetBoundsInPixels({1000,0,920,1000});
 assert((mixed2.operations==std::vector<int>{nativeguest::kResize,nativeguest::kMove,nativeguest::kResize}));
 assert((mixed2.native==gfx::Rect{1000,0,920,1000}) && mixed2.bounds_==mixed2.native);
 NativeGuestWindow fs;fs.SetFullscreen(true,0);
 assert(fs.window_state_==PlatformWindowState::kFullScreen && fs.bounds_==full);
 fs.SetFullscreen(false,0);
 assert(fs.window_state_==PlatformWindowState::kNormal && fs.bounds_==normal);
 NativeGuestWindow failed_fs;failed_fs.fail_at=2;failed_fs.SetFullscreen(true,0);
 assert(failed_fs.window_state_==PlatformWindowState::kNormal);
 assert(failed_fs.bounds_==failed_fs.native && failed_fs.restored_bounds_==normal);
 failed_fs.fail_at=0;failed_fs.SetFullscreen(true,0);failed_fs.SetFullscreen(false,0);
 assert(failed_fs.bounds_==normal && failed_fs.native==normal);
 NativeGuestWindow max_fs;max_fs.Maximize();max_fs.SetFullscreen(true,0);
 max_fs.SetFullscreen(false,0);
 assert(max_fs.window_state_==PlatformWindowState::kMaximized && max_fs.bounds_==full);
 assert(max_fs.restored_bounds_==normal);
 max_fs.Restore();assert(max_fs.bounds_==normal && max_fs.native==normal);
 // OnBoundsChanged 可以销毁平台窗口。弱指针失效后，不得再调用后续状态回调。
 NativeGuestWindow gone;gone.destroy_on_bounds=true;gone.Maximize();
 assert(gone.window_state_==PlatformWindowState::kNormal);
 NativeGuestWindow gone_fs;gone_fs.destroy_on_bounds=true;gone_fs.SetFullscreen(true,0);
 assert(gone_fs.window_state_==PlatformWindowState::kNormal);
 NativeGuestWindow gone_restore;gone_restore.Maximize();gone_restore.destroy_on_bounds=true;
 gone_restore.Restore();assert(gone_restore.window_state_==PlatformWindowState::kMaximized);
 for(int action=0;action<3;++action){
   auto* heap=new NativeGuestWindow;
   if(action==2)heap->Maximize();
   auto lifetime=heap->weak_ptr_factory_.alive;
   heap->delete_on_bounds=true;
   if(action==0)heap->Maximize();
   else if(action==1)heap->SetFullscreen(true,0);
   else heap->Restore();
   assert(!*lifetime);
 }
 for(int initial=0;initial<3;++initial){
   NativeGuestWindow m;
   if(initial==1)m.Maximize();
   if(initial==2)m.SetFullscreen(true,0);
   const auto state=m.window_state_;const auto geometry=m.bounds_;
   const auto saved=m.restored_bounds_;
   m.Minimize();assert(m.native_minimized && !m.visible_ && !m.delegate.active);
   assert(m.bounds_==geometry && m.restored_bounds_==saved);
   const int notified=m.delegate.activation_calls;
   m.HandleMinimized(true);assert(m.delegate.activation_calls==notified);
   m.Restore();assert(!m.native_minimized && m.visible_ && m.delegate.active);
   assert(m.window_state_==state && m.bounds_==geometry && m.restored_bounds_==saved);
   // Dock 恢复事件具有权威性，不得再发送其他命令。
   m.Minimize();const int calls=m.calls;m.native_minimized=false;m.HandleMinimized(false);
   assert(m.calls==calls && m.window_state_==state && m.visible_ && m.delegate.active);
 }
 NativeGuestWindow failed_min;failed_min.fail_at=1;failed_min.Minimize();
 assert(failed_min.visible_ && !failed_min.native_minimized && failed_min.window_state_==PlatformWindowState::kNormal);
 failed_min.fail_at=0;failed_min.Minimize();failed_min.fail_at=failed_min.calls+1;failed_min.Restore();
 assert(!failed_min.visible_ && failed_min.native_minimized && failed_min.window_state_==PlatformWindowState::kMinimized);
 auto* destroyed_min=new NativeGuestWindow;
 auto min_lifetime=destroyed_min->weak_ptr_factory_.alive;
 destroyed_min->delete_on_state=true;destroyed_min->Minimize();assert(!*min_lifetime);
}
'''
with tempfile.TemporaryDirectory(prefix='system-window-bounds-') as directory:
    path = Path(directory)
    (path / 'test.cc').write_text(program)
    subprocess.run(['c++', '-std=c++20', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                    str(path / 'test.cc'), '-o', str(path / 'test')], check=True)
    subprocess.run([str(path / 'test')], check=True)
print('window bounds: maximize/fullscreen/restore, native fit, failures/retry and callback destruction passed (ASan/UBSan)')
