"""Execute actual nativeguest frame policy and bounded title metadata sender."""
from pathlib import Path
import sys
import subprocess
import tempfile
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from chromium_source import chromium_source
root = chromium_source()
source = (root/'ui/ozone/platform/nativeguest/nativeguest_window.cc').read_text()
policy = source[source.index('  const bool toplevel = properties.type'):source.index('  create.background =', source.index('  const bool toplevel = properties.type'))]
title = source[source.index('void NativeGuestWindow::SetTitle('):source.index('void NativeGuestWindow::SetCapture(')]
native = (source[source.index('void NativeGuestWindow::SetUseNativeFrame('):source.index('void NativeGuestWindow::DispatchHostWindowDragMovement(')] +
          source[source.index('bool NativeGuestWindow::ShouldUseNativeFrame()'):source.index('void NativeGuestWindow::SetCursor(')])
platform = (root/'ui/ozone/platform/nativeguest/ozone_platform_nativeguest.cc').read_text()
assert 'custom_frame_pref_default = true' in platform
assert 'supports_server_side_window_decorations = true' in platform
properties_begin = platform.index('  const PlatformProperties& GetPlatformProperties()')
properties = platform[properties_begin:platform.index('  bool IsNativePixmapConfigSupported(', properties_begin)]
browser = (root/'chrome/browser/ui/views/frame/browser_native_widget_aura_linux.cc').read_text()
assert 'use_custom_frame_pref_.GetValue()' in browser
assert '== "nativeguest"' not in browser[browser.index('bool BrowserNativeWidgetAuraLinux::UseCustomFrame()'):]
browser_policy = browser[browser.index('bool BrowserNativeWidgetAuraLinux::UseCustomFrame()'):browser.index('void BrowserNativeWidgetAuraLinux::TabDraggingKindChanged(')]
host = (root/'ui/views/widget/desktop_aura/desktop_window_tree_host_platform.cc').read_text()
host_begin = host.index('  platform_window()->SetUseNativeFrame(', host.index('void DesktopWindowTreeHostPlatform::OnNativeWidgetCreated('))
host_policy = host[host_begin:host.index(';', host_begin)+1]
code = r'''
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <string>
#include <atomic>
#include <barrier>
#include <thread>
#include <vector>
#include "base/no_destructor.h"
static std::atomic<unsigned> property_writes{0};
struct PropertyBool {
 std::atomic<bool> value{false};
 void operator=(bool next){value=next;++property_writes;}
 operator bool() const{return value.load();}
};
struct PlatformProperties {
 PlatformProperties()=default;
 PlatformProperties(const PlatformProperties&)=delete;
 ~PlatformProperties(){}
 PropertyBool custom_frame_pref_default,supports_vulkan_swap_chain,
              platform_shows_drag_image,supports_global_screen_coordinates;
};
struct PlatformRuntimeProperties {
 PropertyBool supports_overlays,supports_server_side_window_decorations,supports_native_pixmaps;
};
struct PropertySource {
 virtual ~PropertySource()=default;
 virtual const PlatformProperties& GetPlatformProperties()=0;
 virtual const PlatformRuntimeProperties& GetPlatformRuntimeProperties()=0;
};
struct GuestProperties : PropertySource {
''' + properties + r'''
};
enum class PlatformWindowType { kWindow,kPopup,kMenu,kTooltip,kDrag,kBubble };
namespace nativeguest {
constexpr unsigned kWindowResizable=2,kWindowClientDecorations=4,kWindowStandardCaptionControls=16,kWindowExplicitMoveResize=64,kTitle=8,kDecorations=9;
struct WindowCommand {unsigned operation,reserved;int x,y;unsigned width,height,stride,reserved2;uint64_t address,length;};
static unsigned commits;
static char received[32];
static int CommitWindowCommand(unsigned widget,WindowCommand *c){
 assert(widget==7&&c->operation==8&&c->length==32&&!c->reserved&&!c->x&&!c->y&&!c->width&&!c->height&&!c->stride);
 ++commits;memcpy(received,(void *)c->address,32);return 0;
}
}
namespace base {static std::string next_utf8;static std::string UTF16ToUTF8(const std::u16string&){return next_utf8;}}
struct NativeGuestWindow {
 bool server_decorated_=false,top_level_=false,closed_=false,valid=true,commit_ok=true;
 unsigned toggles=0;
 unsigned widget_=7;
 std::u16string title_;
 bool IsValid(){return valid;}
 bool Commit(nativeguest::WindowCommand& c){assert(c.operation==9&&c.width<=1);++toggles;return commit_ok;}
 void SetTitle(const std::u16string&);
 void SetUseNativeFrame(bool);
 bool ShouldUseNativeFrame() const;
 unsigned flags(PlatformWindowType type,bool remove_standard_frame){
  struct {PlatformWindowType type;bool remove_standard_frame;} properties{type,remove_standard_frame};
  struct {unsigned flags;} create{};
''' + policy + r'''
  return create.flags;
 }
};
''' + title + native + r'''
namespace ui {
struct OzonePlatform {
 struct Properties {bool supports_server_side_window_decorations=true;} properties;
 static OzonePlatform* GetInstance(){static OzonePlatform platform;return &platform;}
 const Properties& GetPlatformRuntimeProperties(){return properties;}
};
}
struct BrowserView {
 bool normal=true,web_app=false,pip=false;
 bool GetIsNormalType(){return normal;}
 bool GetIsWebAppType(){return web_app;}
 bool GetIsPictureInPictureType(){return pip;}
};
struct BrowserNativeWidgetAuraLinux {
 BrowserView view;
 bool has_view=true;
 struct Pref {bool value=false;bool GetValue() const{return value;}} use_custom_frame_pref_;
 BrowserView* browser_view() const{return has_view?const_cast<BrowserView*>(&view):nullptr;}
 bool UseCustomFrame() const;
};
''' + browser_policy + r'''
struct Widget {
 struct InitParams {enum Type {TYPE_WINDOW,TYPE_MENU};Type type;bool remove_standard_frame;};
};
struct Host {
 NativeGuestWindow window;
 NativeGuestWindow* platform_window(){return &window;}
 void created(const Widget::InitParams& params){
''' + host_policy + r'''
 }
};
int main(){
 // 并发首次访问和重复查询都只能配置每个属性一次，不能先发布默认值，再在读取路径
 // 上修改它们。
 std::barrier start(16);
 std::vector<std::thread> readers;
 const PlatformProperties* platform_addresses[16]{};
 const PlatformRuntimeProperties* runtime_addresses[16]{};
 for(unsigned n=0;n<16;++n)readers.emplace_back([&,n]{
  GuestProperties source;
  start.arrive_and_wait();
  for(unsigned i=0;i<10000;++i){
   const auto& p=source.GetPlatformProperties();
   const auto& r=source.GetPlatformRuntimeProperties();
   assert(p.custom_frame_pref_default&&!p.supports_vulkan_swap_chain);
   assert(!p.platform_shows_drag_image&&p.supports_global_screen_coordinates);
   assert(!r.supports_overlays&&r.supports_server_side_window_decorations&&!r.supports_native_pixmaps);
   if(i){assert(platform_addresses[n]==&p&&runtime_addresses[n]==&r);}
   platform_addresses[n]=&p;runtime_addresses[n]=&r;
  }
 });
 for(auto& reader:readers)reader.join();
 for(unsigned n=1;n<16;++n){
  assert(platform_addresses[n]==platform_addresses[0]);
  assert(runtime_addresses[n]==runtime_addresses[0]);
 }
 assert(property_writes==7);
 // 同时执行真实的浏览器偏好判定和 Views 宿主初始化：自定义边框不得重新出现
 // 服务端标题栏。
 for(bool supported:{false,true})for(bool pref:{false,true})
 for(bool normal:{false,true})for(bool app:{false,true})for(bool pip:{false,true}){
  ui::OzonePlatform::GetInstance()->properties.supports_server_side_window_decorations=supported;
  BrowserNativeWidgetAuraLinux browser;
  browser.use_custom_frame_pref_.value=pref;
  browser.view={normal,app,pip};
  bool custom=!supported||(pref&&normal)||app||pip;
  assert(browser.UseCustomFrame()==custom);
  Host host;
  unsigned flags=host.window.flags(PlatformWindowType::kWindow,custom);
  host.created({Widget::InitParams::TYPE_WINDOW,custom});
  assert(host.window.ShouldUseNativeFrame()==!custom);
  assert(!host.window.toggles);
  assert(bool(flags&nativeguest::kWindowClientDecorations)==custom);
  assert(bool(flags&nativeguest::kWindowStandardCaptionControls)==!custom);
  browser.has_view=false;assert(!browser.UseCustomFrame());
 }
 NativeGuestWindow w;
 for(auto type:{PlatformWindowType::kWindow,PlatformWindowType::kPopup,PlatformWindowType::kMenu,PlatformWindowType::kTooltip,PlatformWindowType::kDrag,PlatformWindowType::kBubble}){
  for(bool custom:{false,true}) {
   unsigned f=w.flags(type,custom);
   bool server=type==PlatformWindowType::kWindow&&!custom;
   assert(w.ShouldUseNativeFrame()==server);
   // 弹出窗口缓冲区也必须接受 Chromium 发起的程序化调整大小操作。
   assert(f==((type==PlatformWindowType::kWindow&&!custom?18:6)|
              (type==PlatformWindowType::kWindow?64:0)));
   unsigned before=w.toggles;
   w.commit_ok=false;w.SetUseNativeFrame(!server);assert(w.ShouldUseNativeFrame()==server);
   w.commit_ok=true;w.SetUseNativeFrame(!server);
   bool switched=type==PlatformWindowType::kWindow;
   assert(w.ShouldUseNativeFrame()==(switched?!server:server));
   assert(w.toggles==before+(switched?2:0));
  }
 }
 w.flags(PlatformWindowType::kWindow,false);
 base::next_utf8="Chrome";w.SetTitle(u"Chrome");assert(!strcmp(nativeguest::received,"Chrome"));
 for(unsigned i=7;i<32;++i)assert(!nativeguest::received[i]);
 base::next_utf8=std::string(100,'x');w.SetTitle(u"long");assert(strlen(nativeguest::received)==31);
 for(unsigned prefix=27;prefix<=31;++prefix){
  base::next_utf8=std::string(prefix,'x')+"\xe4\xb8\xad";
  w.SetTitle(u"unicode");
  assert(strlen(nativeguest::received)==(prefix+3<=31?prefix+3:prefix));
 }
 auto n=nativeguest::commits;
 w.closed_=true;w.SetTitle(u"ignored");assert(nativeguest::commits==n);w.closed_=false;
 w.valid=false;w.SetTitle(u"ignored");assert(nativeguest::commits==n);w.valid=true;
 w.flags(PlatformWindowType::kMenu,false);w.SetTitle(u"ignored");assert(nativeguest::commits==n);
}
'''
with tempfile.TemporaryDirectory(prefix='chrome-server-frame-') as directory:
    base = Path(directory)
    (base/'test.cc').write_text(code)
    command = ['c++','-std=c++20','-O2','-Wall','-Wextra','-Werror','-pthread',
                    '-fsanitize=address,undefined','-fno-pie','-no-pie',
                    '-I'+str(root),
                    str(base/'test.cc'),'-o',str(base/'test')]
    subprocess.run(command,check=True)
    subprocess.run([str(base/'test')],check=True)
    # 负对照：复现旧运行时 getter 的重复写入。
    # 返回值相同也不能掩盖每次读取都会发生的不安全修改。
    assert '    return properties;' in properties
    broken_properties = properties.replace('static const Properties properties;',
                                          'static Properties properties;').replace(
        '    return properties;',
        '    properties.supports_overlays = false; return properties;')
    broken = code.replace(properties,broken_properties).replace(
        'assert(property_writes==7);','if(property_writes!=7)return 42;')
    (base/'test.cc').write_text(broken)
    subprocess.run(command,check=True)
    assert subprocess.run([str(base/'test')]).returncode==42
print('Chrome frame: immutable properties (16 concurrent readers, negative control), browser/Views/native policy chain, borderless popups and UTF8 titles passed')
