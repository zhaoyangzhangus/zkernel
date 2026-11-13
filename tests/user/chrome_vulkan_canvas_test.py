"""Execute production Vulkan-to-native presentation with bounded pixel fixtures."""
from pathlib import Path
import re
import subprocess
import sys
import tempfile
sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
from chromium_source import chromium_source
root=chromium_source()/'components/viz/service/display_embedder'
header=(root/'skia_output_device_vulkan_canvas.h').read_text()
source=(root/'skia_output_device_vulkan_canvas.cc').read_text()
body=re.sub(r'^#include.*$', '',header+'\n'+source,flags=re.M)
code=r'''
#include <cassert>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>
#include <algorithm>
static unsigned present_logs=0;
struct Log {template<class T> Log& operator<<(T const&){return *this;} ~Log(){++present_logs;}};
#define LOG(x) Log{}
template<class T>using scoped_refptr=std::shared_ptr<T>;
namespace gpu {struct SharedContextState{};struct MemoryTracker{};}
namespace gfx {
struct Size{int w=0,h=0;int width()const{return w;}int height()const{return h;}};
enum class SurfaceOrigin{kTopLeft,kBottomLeft};
enum class SwapResult{SWAP_ACK,SWAP_FAILED,SWAP_NAK_RECREATE_BUFFERS};
struct SwapCompletionResult{SwapResult result;explicit SwapCompletionResult(SwapResult v):result(v){}};
struct Rect{
 int a=0,b=0,w=0,h=0;Rect()=default;explicit Rect(Size s):w(s.w),h(s.h){}Rect(int x,int y,int width,int height):a(x),b(y),w(width),h(height){}
 int x()const{return a;}int y()const{return b;}int width()const{return w;}int height()const{return h;}
 bool IsEmpty()const{return w<=0||h<=0;}
 void Intersect(Rect r){int right=std::min(a+w,r.a+r.w),bottom=std::min(b+h,r.b+r.h);a=std::max(a,r.a);b=std::max(b,r.b);w=std::max(0,right-a);h=std::max(0,bottom-b);}
};
}
struct SkIRect {int x,y,w,h;static SkIRect MakeXYWH(int x,int y,int w,int h){return{x,y,w,h};}};
struct SkPixmap {
 uint32_t *p=nullptr;int w=0,h=0,stride=0;
 int width()const{return w;}int height()const{return h;}
 bool extractSubset(SkPixmap* dst,SkIRect r)const{
  assert(r.x>=0&&r.y>=0&&r.w>0&&r.h>0&&r.x+r.w<=w&&r.y+r.h<=h);
  *dst={p+r.y*stride+r.x,r.w,r.h,stride};return true;
 }
};
struct SkCanvas {
 SkPixmap pixels;bool peek_allowed=true;
 bool peekPixels(SkPixmap* p){if(!peek_allowed)return false;*p=pixels;return true;}
};
struct SkSurface {
 bool allowed=true;unsigned reads=0;uint64_t pixels_read=0;SkIRect last{};
 bool readPixels(SkPixmap p,int x,int y){++reads;pixels_read+=uint64_t(p.w)*p.h;last={x,y,p.w,p.h};if(!allowed){p.p[0]=0xdead;return false;}
  for(int row=0;row<p.h;row++)for(int col=0;col<p.w;col++)p.p[row*p.stride+col]=(y+row)*1000+x+col+1;
  return true;
 }
};
namespace ui {
struct SurfaceOzoneCanvas {
 bool full_frame=false,present_allowed=true,retry=false,partial_enabled=false;unsigned cancels=0;
 bool alive=true;SkCanvas raster;std::vector<uint32_t> storage;
 unsigned resizes=0,presents=0,acquires=0;gfx::Rect last;
 virtual ~SurfaceOzoneCanvas()=default;
 void ResizeCanvas(gfx::Size s,float scale){assert(scale==1);++resizes;storage.assign(s.w*s.h+2,0xabc);
  raster.pixels={storage.data()+1,s.w,s.h,s.w};}
 SkCanvas* GetCanvas(){++acquires;return alive?&raster:nullptr;}
 void PresentCanvas(gfx::Rect r){assert(!r.IsEmpty());last=r;++presents;}
 bool RequiresFullFrame()const{return full_frame;}
 void EnablePartialFrameUpdates(){partial_enabled=true;}
 bool CanvasFrameNeedsRetry()const{return retry;}
 bool PresentCanvasChecked(gfx::Rect r){if(!present_allowed)return false;PresentCanvas(r);return true;}
 void CancelCanvasFrame(){++cancels;}
};
}
namespace viz {
using OutputSurfaceFrame=int;
struct SkiaOutputDeviceOffscreen {
 using DidSwapBufferCompleteCallback=int;using BufferPresentedCallback=int;
 struct ReshapeParams{gfx::Size size;float device_scale_factor=1;bool valid=true;};
 gfx::Size size_;SkSurface surface;SkSurface* sk_surface_=nullptr;
 unsigned starts=0,finishes=0;gfx::SwapResult result=gfx::SwapResult::SWAP_FAILED;
 SkiaOutputDeviceOffscreen(scoped_refptr<gpu::SharedContextState>,gfx::SurfaceOrigin origin,bool,scoped_refptr<gpu::MemoryTracker>,int){assert(origin==gfx::SurfaceOrigin::kTopLeft);}
 virtual ~SkiaOutputDeviceOffscreen()=default;
 virtual bool Reshape(const ReshapeParams& p){if(!p.valid)return false;size_=p.size;sk_surface_=&surface;return true;}
 virtual void Present(const std::optional<gfx::Rect>&,int,int)=0;
 void StartSwapBuffers(int){++starts;}
 void FinishSwapBuffers(gfx::SwapCompletionResult r,gfx::Size s,int){assert(s.w==size_.w&&s.h==size_.h);result=r.result;++finishes;}
};
}
''' + body + r'''
int main(){
 auto canvas=std::make_unique<ui::SurfaceOzoneCanvas>();auto* c=canvas.get();
 viz::SkiaOutputDeviceVulkanCanvas d({},true,{},0,std::move(canvas));
 assert(c->partial_enabled);
 d.Present(std::nullopt,0,0);assert(d.result==gfx::SwapResult::SWAP_FAILED);
 assert(d.Reshape({{100,50},1,true}));
 d.Present(gfx::Rect(5,6,7,8),0,0);
 assert(d.result==gfx::SwapResult::SWAP_ACK&&d.surface.reads==1&&c->presents==1);
 assert(present_logs==1);
 assert(c->last.x()==0&&c->last.y()==0&&c->last.width()==100&&c->last.height()==50);
 assert(c->storage.front()==0xabc&&c->storage.back()==0xabc);
 assert(c->storage[1]==1&&c->storage[5000]==49000+100);
 d.Present(gfx::Rect(98,48,20,20),0,0);
 assert(d.surface.last.x==98&&d.surface.last.y==48&&d.surface.last.w==2&&d.surface.last.h==2);
 unsigned reads=d.surface.reads,presents=c->presents,empty_acquires=c->acquires;
 d.Present(gfx::Rect(-10,-10,2,2),0,0);
 assert(d.surface.reads==reads&&c->presents==presents&&d.result==gfx::SwapResult::SWAP_ACK);
 assert(c->acquires==empty_acquires);
 d.surface.allowed=false;d.Present(std::nullopt,0,0);
 assert(d.result==gfx::SwapResult::SWAP_FAILED&&c->presents==presents);
 // GPU 读取失败时，可能已经修改了下一损坏矩形之外的目标像素。
 // 恢复时必须还原整个表面。
 assert(c->storage[1]==0xdead);
 d.surface.allowed=true;d.Present(gfx::Rect(5,6,7,8),0,0);
 assert(d.result==gfx::SwapResult::SWAP_ACK&&c->presents==presents+1);
 assert(d.surface.last.x==0&&d.surface.last.y==0&&d.surface.last.w==100&&d.surface.last.h==50);
 assert(c->storage[1]==1&&c->storage[5000]==49000+100);
 presents=c->presents;
 d.Present(gfx::Rect(5,6,7,8),0,0);
 assert(d.surface.last.x==5&&d.surface.last.y==6&&d.surface.last.w==7&&d.surface.last.h==8);
 d.surface.allowed=true;c->raster.peek_allowed=false;d.Present(std::nullopt,0,0);assert(d.result==gfx::SwapResult::SWAP_FAILED);
 c->raster.peek_allowed=true;c->raster.pixels.w=99;d.Present(std::nullopt,0,0);assert(d.result==gfx::SwapResult::SWAP_FAILED);
 c->alive=false;d.Present(std::nullopt,0,0);assert(d.result==gfx::SwapResult::SWAP_FAILED);
 assert(!d.Reshape({{10,10},1,true}));c->alive=true;
 assert(!d.Reshape({{10,10},1,false}));assert(d.Reshape({{10,10},1,true}));
 d.Present(gfx::Rect(1,1,2,2),0,0);assert(c->last.width()==10&&c->last.height()==10);
 assert(d.starts==d.finishes&&d.result==gfx::SwapResult::SWAP_ACK);
 c->full_frame=true;
 // 原生调整大小通知/后备槽位可用状态可能晚于 GPU reshape。
 // reshape 不得获取该槽位，也不得宣告 GPU 上下文丢失。
 c->alive=false;c->retry=true;unsigned acquires=c->acquires;
 assert(d.Reshape({{10,10},1,true})&&c->acquires==acquires);
 d.Present(std::nullopt,0,0);
 assert(d.result==gfx::SwapResult::SWAP_NAK_RECREATE_BUFFERS);
 c->alive=true;c->retry=false;
 d.Present(gfx::Rect(1,1,2,2),0,0);
 assert(d.surface.last.w==10&&d.surface.last.h==10);
 c->present_allowed=false;unsigned cancelled=c->cancels;
 d.Present(std::nullopt,0,0);
 assert(d.result==gfx::SwapResult::SWAP_FAILED&&c->cancels==cancelled+1);
 c->retry=true;
 d.Present(std::nullopt,0,0);
 assert(d.result==gfx::SwapResult::SWAP_NAK_RECREATE_BUFFERS);
 assert(present_logs==1);
 // 统计生产环境实际读回的矩形数量，而不是宿主/GPU 耗时。
 c->retry=false;c->present_allowed=true;c->full_frame=false;
 assert(d.Reshape({{1920,1080},1,true}));d.Present(std::nullopt,0,0);
 uint64_t pixels_before=d.surface.pixels_read;
 for(unsigned i=0;i<100;++i)d.Present(gfx::Rect(100,200,64,32),0,0);
 assert(d.surface.pixels_read-pixels_before==100*64*32);
 assert(d.starts==d.finishes);
}
'''
with tempfile.TemporaryDirectory(prefix='chrome-vulkan-canvas-') as directory:
    base=Path(directory);(base/'test.cc').write_text(code)
    subprocess.run(['c++','-std=c++20','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',
                    '-fno-pie','-no-pie',str(base/'test.cc'),'-o',str(base/'test')],check=True)
    subprocess.run([str(base/'test')],check=True)
print('Vulkan canvas: full first frame, partial readback, clipping/canaries, resize reset, surface loss and paired failure/ACK callbacks passed')
print('Readback fixture: 100 updates of 64x32 at 1920x1080 read 819200 bytes instead of 829440000; this is byte coverage, not measured GPU timing')
