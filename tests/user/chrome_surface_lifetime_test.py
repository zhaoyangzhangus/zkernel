"""Execute native surface lifetime and concurrent unregister production code."""
from pathlib import Path
import re
import subprocess
import sys
import tempfile
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from chromium_source import chromium_source
chromium = chromium_source()
root = chromium/'ui/ozone/platform/nativeguest'
body = '\n'.join((root/name).read_text() for name in
                 ('nativeguest_surface_factory.h', 'nativeguest_surface_factory.cc'))
body = re.sub(r'^#include.*$', '', body, flags=re.M)
abi = (root / 'nativeguest_window_abi.h').read_text()
surface_abi = abi[abi.index('enum SurfaceOperation :'):abi.index('constexpr uint32_t kWindowIoctlSurface')]
ozone = (chromium/'components/viz/service/display_embedder/software_output_device_ozone.cc').read_text()
renderer = (chromium/'components/viz/service/display/software_renderer.cc').read_text()
def function(source, signature):
    start = source.index(signature)
    return source[start:source.index('\n}', start) + 2]
software_policy = '\n'.join((
    function(ozone, 'bool SoftwareOutputDeviceOzone::RequiresFullPaint() const'),
    function(renderer, 'bool SoftwareRenderer::CanPartialSwap()')))
code = r'''
#include <cassert>
#include <atomic>
#include <algorithm>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <cstdint>
#include <cstring>
#include <array>
#include <unistd.h>
#define BUILDFLAG(x) 0
#define UNSAFE_BUFFERS(x) x
#define LOG(x) NullLog()
struct NullLog{template<class T>NullLog& operator<<(T){return *this;}};
namespace base{using Lock=std::mutex;using AutoLock=std::lock_guard<Lock>;}
namespace base {
static unsigned repair_copies=0;
static uint64_t repair_bytes=0;
template<class T> struct span {
 T* pointer;size_t size;
 span(T* p,size_t n):pointer(p),size(n){}
 span subspan(size_t offset,size_t count){assert(offset<=size&&count<=size-offset);return {pointer+offset,count};}
 template<class U>void copy_from(span<U> other){assert(size==other.size);++repair_copies;repair_bytes+=size;std::copy_n(other.pointer,size,pointer);}
};
}
template<class T>struct raw_ptr{T* p=nullptr;raw_ptr(T* v=nullptr):p(v){}raw_ptr& operator=(T* v){p=v;return *this;}T* get()const{return p;}explicit operator bool()const{return p;}};
template<class T>using sk_sp=std::shared_ptr<T>;
namespace gfx{
using AcceleratedWidget=unsigned;
struct Size{int w=0,h=0;Size()=default;Size(int a,int b):w(a),h(b){}int width()const{return w;}int height()const{return h;}bool IsEmpty()const{return w<=0||h<=0;}void SetToMax(Size s){w=std::max(w,s.w);h=std::max(h,s.h);}bool operator==(const Size&)const=default;};
struct Rect{int a=0,b=0,w=0,h=0;Rect()=default;explicit Rect(Size s):w(s.w),h(s.h){}Rect(int x,int y,int width,int height):a(x),b(y),w(width),h(height){}int x()const{return a;}int y()const{return b;}int width()const{return w;}int height()const{return h;}int bottom()const{return b+h;}bool IsEmpty()const{return w<=0||h<=0;}void Intersect(Rect r){int right=std::min(a+w,r.a+r.w),bottom=std::min(b+h,r.b+r.h);a=std::max(a,r.a);b=std::max(b,r.b);w=std::max(0,right-a);h=std::max(0,bottom-b);}};
struct VSyncProvider{};
}
struct SkCanvas{void* pixels=nullptr;};struct SkSurfaceProps{};
struct SkImageInfo{int w,h;static SkImageInfo MakeN32Premul(int w,int h){return {w,h};}};
struct SkSurface{int w,h;SkCanvas c;int width()const{return w;}int height()const{return h;}SkCanvas* getCanvas(){return &c;}};
namespace SkSurfaces{static sk_sp<SkSurface> WrapPixels(SkImageInfo i,void* p,uint64_t stride,SkSurfaceProps*){assert(p&&stride==unsigned(i.w)*4);return std::make_shared<SkSurface>(SkSurface{i.w,i.h,{p}});}}
namespace nativeguest{
''' + surface_abi + r'''
enum{kResize=1,kPresent=2,kEagain=-11};
struct WindowCommand{unsigned operation=0,width=0,height=0,stride=0;int x=0,y=0;uint64_t address=0,length=0;};
std::atomic<unsigned> calls{0};std::atomic<bool> block{false},entered{false},release{false};
int64_t CommitWindowCommand(unsigned widget,WindowCommand* c){assert(widget==7);++calls;if(c->operation==kPresent&&block){entered=true;while(!release)std::this_thread::yield();}return 0;}
bool Failed(int64_t n){return n<0;}
static uint8_t* surface_pixels;
static unsigned front=0,acquires=0,commits=0,cancels=0;
static gfx::Rect submitted_damage;
static bool leased=false;
static int begin_error=0,commit_error=0;
static unsigned begin_width=16,begin_height=16,begin_stride=64;
int64_t CommitSurfaceCommand(unsigned widget,SurfaceCommand* command){
 assert(widget==7);
 if(command->operation==kSurfaceBegin){
  if(begin_error)return begin_error;
  assert(!leased);leased=true;++acquires;
  command->frame={acquires,reinterpret_cast<uint64_t>(surface_pixels+(front^1U)*4096),4096,begin_width,begin_height,begin_stride,0};return 0;
 }
 if(command->operation==kSurfaceCommit){assert(leased);if(commit_error)return commit_error;
  auto& f=command->frame;
  assert(f.damage_x>=0&&f.damage_y>=0&&f.damage_width&&f.damage_height);
  assert(uint64_t(f.damage_x)+f.damage_width<=f.width&&uint64_t(f.damage_y)+f.damage_height<=f.height);
  submitted_damage=gfx::Rect(f.damage_x,f.damage_y,f.damage_width,f.damage_height);
  leased=false;front^=1;++commits;return 0;}
 assert(command->operation==kSurfaceCancel&&leased);leased=false;++cancels;return 0;
}
}
namespace ui{
struct SurfaceOzoneCanvas{virtual ~SurfaceOzoneCanvas()=default;virtual void ResizeCanvas(const gfx::Size&,float)=0;virtual SkCanvas* GetCanvas()=0;virtual void PresentCanvas(const gfx::Rect&)=0;virtual std::unique_ptr<gfx::VSyncProvider> CreateVSyncProvider()=0;
 virtual bool CanvasFrameNeedsRetry()const{return false;}
 virtual void EnablePartialFrameUpdates(){}
 virtual bool RequiresFullFrame()const{return false;}
 virtual bool PresentCanvasChecked(const gfx::Rect& r){PresentCanvas(r);return true;}
 virtual void CancelCanvasFrame(){}
};
struct SurfaceFactoryOzone{virtual ~SurfaceFactoryOzone()=default;virtual std::unique_ptr<SurfaceOzoneCanvas> CreateCanvasForWidget(gfx::AcceleratedWidget)=0;};
}
''' + body + r'''
namespace viz {
struct SoftwareOutputDeviceOzone {
 ui::SurfaceOzoneCanvas* surface_ozone_;
 bool RequiresFullPaint() const;
};
struct SoftwareRenderer {
 SoftwareOutputDeviceOzone* output_device_;
 bool CanPartialSwap();
};
''' + software_policy + r'''
}
int main(){
 ui::NativeGuestSurfaceFactory f;uint8_t pixels[4096]{};
 auto state=f.RegisterWindow(7,pixels,sizeof(pixels),{16,16});
 auto c=f.CreateCanvasForWidget(7);assert(c);c->ResizeCanvas({16,16},1);assert(c->GetCanvas());
 c->PresentCanvas(gfx::Rect(0,0,16,16));assert(state->has_presented);
 {base::AutoLock l(state->lock);state->size={8,8};}
 auto before=nativeguest::calls.load();c->PresentCanvas(gfx::Rect(0,0,8,8));assert(nativeguest::calls==before);
 c->ResizeCanvas({8,8},1);assert(c->GetCanvas());
 nativeguest::block=true;std::thread presenter([&]{c->PresentCanvas(gfx::Rect(0,0,8,8));});
 while(!nativeguest::entered)std::this_thread::yield();
 std::atomic<bool> started{false},finished{false};
 std::thread closer([&]{started=true;f.UnregisterWindow(7);finished=true;});
 while(!started){std::this_thread::yield();}
 assert(!finished);
 nativeguest::release=true;presenter.join();closer.join();assert(finished&&!state->registered);
 before=nativeguest::calls.load();c->PresentCanvas(gfx::Rect(0,0,8,8));assert(nativeguest::calls==before);
 c->ResizeCanvas({8,8},1);assert(!c->GetCanvas());assert(!f.CreateCanvasForWidget(7));
 auto newer=f.RegisterWindow(7,pixels,sizeof(pixels),{16,16});
 c->ResizeCanvas({16,16},1);assert(!c->GetCanvas());assert(newer->registered);
 auto next=f.CreateCanvasForWidget(7);next->ResizeCanvas({INT32_MAX,INT32_MAX},1);assert(!next->GetCanvas());
 assert(nativeguest::calls==before);next->ResizeCanvas({16,16},1);assert(next->GetCanvas());
 auto replaced=f.RegisterWindow(7,pixels,sizeof(pixels),{16,16});assert(!newer->registered&&replaced->registered);
 next->PresentCanvas(gfx::Rect(0,0,16,16));assert(nativeguest::calls==before);
 f.UnregisterWindow(7);f.UnregisterWindow(7);
 // No external state owner: unlock must still use a live mutex after erase.
 f.RegisterWindow(7,pixels,sizeof(pixels),{16,16});f.UnregisterWindow(7);
 nativeguest::block=false;
 for(int i=0;i<1000;i++){
  f.RegisterWindow(7,pixels,sizeof(pixels),{16,16});
  std::thread lookup([&]{auto canvas=f.CreateCanvasForWidget(7);if(canvas){canvas->ResizeCanvas({16,16},1);canvas->PresentCanvas(gfx::Rect(0,0,16,16));}});
  f.UnregisterWindow(7);lookup.join();
 }
 alignas(uint32_t) uint8_t pair[8192]{};nativeguest::surface_pixels=pair;
 auto paired=f.RegisterWindow(7,pair,sizeof(pair),{16,16},true);
 auto gpu=f.CreateCanvasForWidget(7);gpu->ResizeCanvas({16,16},1);
 assert(gpu->RequiresFullFrame());
 auto* back=gpu->GetCanvas();assert(back&&back->pixels==pair+4096);
 assert(gpu->GetCanvas()==back&&nativeguest::acquires==1);
 static_cast<uint32_t*>(back->pixels)[0]=42;
 assert(*reinterpret_cast<uint32_t*>(pair)==0); // Front untouched during write.
 assert(gpu->PresentCanvasChecked(gfx::Rect(0,0,16,16)));
 assert(nativeguest::front==1&&nativeguest::commits==1&&paired->has_presented);
 assert(gpu->GetCanvas()->pixels==pair);
 gpu->CancelCanvasFrame();assert(!nativeguest::leased&&nativeguest::cancels==1);
 assert(gpu->GetCanvas()->pixels==pair);
 assert(!gpu->PresentCanvasChecked(gfx::Rect(1,1,2,2)));
 assert(nativeguest::front==1&&!nativeguest::leased&&nativeguest::cancels==2);
 nativeguest::commit_error=-11;
 assert(gpu->GetCanvas());assert(!gpu->PresentCanvasChecked(gfx::Rect(0,0,16,16)));
 assert(gpu->CanvasFrameNeedsRetry());
 assert(nativeguest::front==1&&!nativeguest::leased&&nativeguest::cancels==3);
 nativeguest::commit_error=0;nativeguest::begin_error=-110;
 assert(!gpu->GetCanvas()&&!nativeguest::leased);
 assert(!gpu->CanvasFrameNeedsRetry()); // A timeout is not a resize race.
 nativeguest::begin_error=-11;
 assert(!gpu->GetCanvas()&&gpu->CanvasFrameNeedsRetry());
 nativeguest::begin_error=0;assert(gpu->GetCanvas());
 assert(!gpu->CanvasFrameNeedsRetry());
 gpu->ResizeCanvas({16,16},1);assert(!nativeguest::leased&&nativeguest::cancels==4);
 assert(gpu->GetCanvas());gpu.reset();assert(!nativeguest::leased&&nativeguest::cancels==5);
 gpu=f.CreateCanvasForWidget(7);gpu->ResizeCanvas({16,16},1);assert(gpu->GetCanvas());
 // Exercise the real Ozone -> software renderer policy on alternating slots.
 // Only one pixel is logically damaged, but every new slot must be repainted.
 viz::SoftwareOutputDeviceOzone device{gpu.get()};
 viz::SoftwareRenderer renderer{&device};
 for(uint32_t generation=1;generation<=2000;++generation){
  assert(device.RequiresFullPaint()&&!renderer.CanPartialSwap());
  auto* pixels=static_cast<uint32_t*>(gpu->GetCanvas()->pixels);
  unsigned count=renderer.CanPartialSwap()?1:16*16;
  std::fill_n(pixels,count,generation);
  assert(gpu->PresentCanvasChecked(gfx::Rect(0,0,16,16)));
  auto* displayed=reinterpret_cast<const uint32_t*>(pair+nativeguest::front*4096);
  for(unsigned i=0;i<16*16;++i)assert(displayed[i]==generation);
 }
 viz::SoftwareOutputDeviceOzone legacy{next.get()};
 viz::SoftwareRenderer legacy_renderer{&legacy};
 assert(!legacy.RequiresFullPaint()&&legacy_renderer.CanPartialSwap());
 // Kernel resize can precede the UI-thread geometry notification. Only a
 // well-formed different-sized lease is retryable; malformed metadata is not.
 nativeguest::begin_width=32;nativeguest::begin_stride=128;
 assert(!gpu->GetCanvas()&&gpu->CanvasFrameNeedsRetry()&&!nativeguest::leased);
 nativeguest::begin_stride=64;
 assert(!gpu->GetCanvas()&&!gpu->CanvasFrameNeedsRetry()&&!nativeguest::leased);
 nativeguest::begin_width=0;nativeguest::begin_stride=0;
 assert(!gpu->GetCanvas()&&!gpu->CanvasFrameNeedsRetry()&&!nativeguest::leased);
 nativeguest::begin_width=16;nativeguest::begin_stride=64;
 assert(gpu->GetCanvas());
 f.UnregisterWindow(7);unsigned cancelled=nativeguest::cancels;
 nativeguest::leased=false; // Kernel window close cancels independently.
 gpu.reset();assert(nativeguest::cancels==cancelled); // No stale fd command.
 // Opted-in GPU-style partial writes still publish complete frames on both
 // alternating slots, without allocating another pixel buffer.
 auto partial_state=f.RegisterWindow(7,pair,sizeof(pair),{16,16},true);
 auto partial=f.CreateCanvasForWidget(7);
 partial->EnablePartialFrameUpdates();
 assert(partial->RequiresFullFrame());
 auto* p=static_cast<uint32_t*>(partial->GetCanvas()->pixels);
 std::array<uint32_t,256> oracle{};oracle.fill(17);
 std::copy(oracle.begin(),oracle.end(),p);
 assert(partial->PresentCanvasChecked(gfx::Rect(0,0,16,16)));
 assert(!partial->RequiresFullFrame());
 for(unsigned generation=0;generation<4096;++generation){
  unsigned copies=base::repair_copies;uint64_t bytes=base::repair_bytes;
  p=static_cast<uint32_t*>(partial->GetCanvas()->pixels);
  assert(base::repair_copies==copies+1);
  assert(base::repair_bytes==bytes+(generation?4:1024));
  assert(std::equal(oracle.begin(),oracle.end(),p));
  unsigned index=(generation*37)%256;
  oracle[index]=generation+100;p[index]=oracle[index];
  assert(partial->PresentCanvasChecked(gfx::Rect(index%16,index/16,1,1)));
  assert(nativeguest::submitted_damage.x()==int(index%16)&&nativeguest::submitted_damage.y()==int(index/16)&&
    nativeguest::submitted_damage.width()==1&&nativeguest::submitted_damage.height()==1);
  auto* front=reinterpret_cast<uint32_t*>(pair+nativeguest::front*4096);
  assert(std::equal(oracle.begin(),oracle.end(),front));
 }
 for(unsigned generation=0;generation<256;++generation){
  unsigned copies=base::repair_copies;uint64_t bytes=base::repair_bytes;
  p=static_cast<uint32_t*>(partial->GetCanvas()->pixels);
  assert(base::repair_copies==copies+(generation?4:1));
  assert(base::repair_bytes==bytes+(generation?48:4));
  assert(std::equal(oracle.begin(),oracle.end(),p));
  unsigned x=(generation*7)%14,y=(generation*5)%13;
  for(unsigned row=y;row<y+4;++row)for(unsigned col=x;col<x+3;++col)
   oracle[row*16+col]=p[row*16+col]=generation+9000;
  assert(partial->PresentCanvasChecked(gfx::Rect(x,y,3,4)));
  assert(nativeguest::submitted_damage.x()==int(x)&&nativeguest::submitted_damage.y()==int(y)&&
    nativeguest::submitted_damage.width()==3&&nativeguest::submitted_damage.height()==4);
  auto* front=reinterpret_cast<uint32_t*>(pair+nativeguest::front*4096);
  assert(std::equal(oracle.begin(),oracle.end(),front));
 }
 // Full-width strips at nonzero y must coalesce without touching the rows
 // before/after the strip. This also checks a full-frame repair after abort.
 for(unsigned generation=0;generation<8;++generation){
  unsigned copies=base::repair_copies;uint64_t bytes=base::repair_bytes;
  p=static_cast<uint32_t*>(partial->GetCanvas()->pixels);
  assert(base::repair_copies==copies+(generation?1:4));
  assert(base::repair_bytes==bytes+(generation?3*16*4:48));
  assert(std::equal(oracle.begin(),oracle.end(),p));
  for(unsigned row=5;row<8;++row)for(unsigned col=0;col<16;++col)
   oracle[row*16+col]=p[row*16+col]=generation+12000;
  assert(partial->PresentCanvasChecked(gfx::Rect(0,5,16,3)));
  assert(nativeguest::submitted_damage.x()==0&&nativeguest::submitted_damage.y()==5&&
    nativeguest::submitted_damage.width()==16&&nativeguest::submitted_damage.height()==3);
 }
 for(auto invalid:{gfx::Rect(-1,0,1,1),gfx::Rect(0,0,17,1),
                   gfx::Rect(15,15,2,2),gfx::Rect(0,0,0,1)}){
  assert(partial->GetCanvas());assert(!partial->PresentCanvasChecked(invalid));
  auto* front=reinterpret_cast<uint32_t*>(pair+nativeguest::front*4096);
  assert(std::equal(oracle.begin(),oracle.end(),front));
 }
 // Abort after a partially overwriting readback. A different next damage
 // rectangle must not leave the cancelled pixels behind.
 p=static_cast<uint32_t*>(partial->GetCanvas()->pixels);p[203]=0xdead;
 partial->CancelCanvasFrame();
 unsigned abort_copies=base::repair_copies;
 uint64_t abort_bytes=base::repair_bytes;
 p=static_cast<uint32_t*>(partial->GetCanvas()->pixels);
 assert(base::repair_copies==abort_copies+1&&base::repair_bytes==abort_bytes+1024);
 assert(std::equal(oracle.begin(),oracle.end(),p));
 assert(partial->PresentCanvasChecked(gfx::Rect(0,0,1,1)));
 auto alternate=f.CreateCanvasForWidget(7);alternate->EnablePartialFrameUpdates();
 p=static_cast<uint32_t*>(alternate->GetCanvas()->pixels);
 assert(std::equal(oracle.begin(),oracle.end(),p));
 oracle[27]=777;p[27]=777;
 assert(alternate->PresentCanvasChecked(gfx::Rect(11,1,1,1)));alternate.reset();
 p=static_cast<uint32_t*>(partial->GetCanvas()->pixels);
 assert(std::equal(oracle.begin(),oracle.end(),p));
 partial->CancelCanvasFrame();
 // A producer unknown to this state advances the token: reject a partial
 // baseline and request full damage rather than showing stale pixels.
 ++nativeguest::acquires;
 assert(!partial->GetCanvas()&&partial->CanvasFrameNeedsRetry());
 assert(partial->RequiresFullFrame());
 p=static_cast<uint32_t*>(partial->GetCanvas()->pixels);
 std::copy(oracle.begin(),oracle.end(),p);
 assert(partial->PresentCanvasChecked(gfx::Rect(0,0,16,16)));
 assert(!partial->RequiresFullFrame());
 // UI resize while a canvas lease is retained is also retryable.
 assert(partial->GetCanvas());partial_state->size={8,8};
 assert(!partial->GetCanvas()&&partial->CanvasFrameNeedsRetry());
 partial_state->size={16,16};partial_state->has_presented=false;
 assert(partial->RequiresFullFrame());
 p=static_cast<uint32_t*>(partial->GetCanvas()->pixels);
 std::copy(oracle.begin(),oracle.end(),p);
 assert(partial->PresentCanvasChecked(gfx::Rect(0,0,16,16)));
 auto default_software=f.CreateCanvasForWidget(7);
 assert(default_software->RequiresFullFrame()); // Software path stays opt-out.
 for(unsigned slot=0;slot<2;++slot)for(unsigned offset=1024;offset<4096;++offset)
  assert(pair[slot*4096+offset]==0); // Allocation/row canaries untouched.
 partial.reset();f.UnregisterWindow(7);
 // The UI and GPU have different queues. Neither an old GPU viewport nor a
 // new GPU viewport may undo the authoritative native window geometry.
 auto raced_state=f.RegisterWindow(7,pair,sizeof(pair),{16,16},true);
 auto raced=f.CreateCanvasForWidget(7);raced->EnablePartialFrameUpdates();
 raced->ResizeCanvas({16,16},1);
 raced_state->size={8,8};nativeguest::begin_width=8;nativeguest::begin_height=8;nativeguest::begin_stride=32;
 unsigned old_acquires=nativeguest::acquires,old_commands=nativeguest::calls;
 assert(!raced->GetCanvas()&&raced->CanvasFrameNeedsRetry());
 assert(nativeguest::acquires==old_acquires);
 raced->ResizeCanvas({32,16},1);
 assert(raced_state->size==gfx::Size(8,8)&&nativeguest::calls==old_commands);
 assert(!raced->GetCanvas()&&raced->CanvasFrameNeedsRetry());
 raced_state->size={32,16};raced_state->has_presented=false;
 nativeguest::begin_width=32;nativeguest::begin_height=16;nativeguest::begin_stride=128;
 p=static_cast<uint32_t*>(raced->GetCanvas()->pixels);assert(p);
 std::fill_n(p,512,123);
 assert(raced->PresentCanvasChecked(gfx::Rect(0,0,32,16)));
 assert(!raced->RequiresFullFrame());
 raced.reset();f.UnregisterWindow(7);
 // Bad allocation metadata must be rejected BEFORE BEGIN, without obtaining
 // a lease or dereferencing any forged address.
 for(unsigned fault=0;fault<5;++fault){
  auto invalid_state=f.RegisterWindow(7,pair,sizeof(pair),{16,16},true);
  auto invalid=f.CreateCanvasForWidget(7);
  if(fault==0)invalid_state->pixels=nullptr;
  if(fault==1)invalid_state->buffer_size=0;
  if(fault==2)invalid_state->buffer_size=8191;
  if(fault==3)invalid_state->pixels=reinterpret_cast<uint8_t*>(UINT64_MAX-4095);
  if(fault==4)invalid_state->size={0,16};
  unsigned n=nativeguest::acquires;
  assert(!invalid->GetCanvas()&&!invalid->CanvasFrameNeedsRetry());
  assert(nativeguest::acquires==n&&!nativeguest::leased);
  invalid.reset();f.UnregisterWindow(7);
 }
}
'''
with tempfile.TemporaryDirectory(prefix='chrome-surface-lifetime-') as directory:
    base=Path(directory);(base/'test.cc').write_text(code)
    subprocess.run(['c++','-std=c++20','-pthread','-Wall','-Wextra','-Werror',
                    '-fsanitize=address,undefined','-fno-pie','-no-pie',
                    str(base/'test.cc'),'-o',str(base/'test')],check=True)
    subprocess.run([str(base/'test')],check=True,timeout=30)
print('Native surface lifetime: concurrent lookup/close, in-flight present drain, '
      'fd reuse, stale geometry, 2000 full-frame and 4096 partial rotations, cancellation repair and shared producer baselines passed')
print('Repair copy counts: full frame and full-width strips use one bounded copy; narrow 3x4 repairs use four rows; abort recovery and canaries passed')
