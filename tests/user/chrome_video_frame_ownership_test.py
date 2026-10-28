"""Execute Chromium's actual Drain with an aggressively reused decoder buffer.

This verifies the CPU frame handoff, not Vulkan synchronization or scanout.
"""
from pathlib import Path
import subprocess
import sys
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from chromium_source import chromium_source

root = Path(__file__).resolve().parents[2]
source = (chromium_source() / 'media/renderers/native_vulkan_video_decoder.cc').read_text()
drain = source[source.index('DecoderStatus NativeVulkanVideoDecoder::Drain('):
               source.index('void NativeVulkanVideoDecoder::Decode(')]
prefix = r'''
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <memory>
#include <vector>
#include "uapi/video_decoder.h"
#define UNSAFE_TODO(x) x
#define LOG(x) Log{}
struct Log {template<class T> Log& operator<<(const T&) {return *this;}};
template<class T> using scoped_refptr = std::shared_ptr<T>;
namespace base {static int64_t Microseconds(int64_t n) {return n;}}
namespace gfx {
struct Size {
 int w,h; Size(int w,int h):w(w),h(h){}
 int width() const {return w;} int height() const {return h;}
 bool operator!=(Size b) const {return w!=b.w || h!=b.h;}
 const char* ToString() const {return "fixture";}
};
}
struct DecoderStatus {
 enum class Codes {kOk,kPlatformDecodeFailure,kUnsupportedConfig,kOutOfMemory};
 Codes value; DecoderStatus(Codes value):value(value){}
};
static constexpr int PIXEL_FORMAT_NV12=1;
struct Color {int ToGfxColorSpace() const {return 42;}};
struct Config {
 gfx::Size size{1,1};
 gfx::Size coded_size() const {return size;}
 gfx::Size visible_rect() const {return size;}
 gfx::Size natural_size() const {return size;}
 Color color_space_info() const {return {};}
};
struct VideoFrame {
 enum {kY,kUV};
 static inline bool fail_allocation=false;
 gfx::Size size; int strides[2]; int64_t timestamp; int color=0;
 std::vector<uint8_t> y,uv;
 VideoFrame(gfx::Size s,int64_t t):size(s),timestamp(t) {
  strides[0]=s.w+7;strides[1]=((s.w+1)/2)*2+9;
  y.assign(strides[0]*s.h+2,0xa5);
  uv.assign(strides[1]*((s.h+1)/2)+2,0xa5);
 }
 static scoped_refptr<VideoFrame> CreateFrame(int format,gfx::Size s,
      gfx::Size visible,gfx::Size natural,int64_t t) {
  assert(format==PIXEL_FORMAT_NV12 && !(visible!=s) && !(natural!=s));
  if(fail_allocation)return {};
  return std::make_shared<VideoFrame>(s,t);
 }
 uint8_t* writable_data(int plane) {return (plane==kY?y:uv).data()+1;}
 int stride(int plane) const {return strides[plane];}
 void set_color_space(int n) {color=n;}
};
// Each receive overwrites exactly the same borrowed allocation, including
// AGAIN/EOF/error calls. Consumers must already own previous frame pixels.
struct os_video_decoder {
 unsigned serial=0,count=0;int mode=0;gfx::Size size{1,1};
 std::vector<uint8_t> bytes;
};
static uint8_t pixel(unsigned serial,int plane,int row,int col) {
 return (serial*31+plane*73+row*11+col*7)%251;
}
static int receive(os_video_decoder_t* d,os_video_frame_t* out) {
 std::fill(d->bytes.begin(),d->bytes.end(),0xee);
 if(d->serial==d->count)return d->mode==1?OS_VIDEO_EOF:
                                d->mode==2?-5:OS_VIDEO_AGAIN;
 int w=d->size.w,h=d->size.h,sy=w+13,suv=((w+1)/2)*2+15;
 d->bytes.assign(sy*h+suv*((h+1)/2),0xee);
 auto* y=d->bytes.data();auto* uv=y+sy*h;
 for(int r=0;r<h;++r)for(int c=0;c<w;++c)y[r*sy+c]=pixel(d->serial,0,r,c);
 for(int r=0;r<(h+1)/2;++r)for(int c=0;c<((w+1)/2)*2;++c)
  uv[r*suv+c]=pixel(d->serial,1,r,c);
 *out={uint32_t(w),uint32_t(h),sy,suv,y,uv,int64_t(d->serial)*33333-1000};
 ++d->serial;
 if(d->mode==3)out->stride_uv=1;
 if(d->mode==4)out->timestamp_us=INT64_MIN;
 if(d->mode==5)out->y=nullptr;
 return 0;
}
struct NativeVulkanVideoDecoder {
 os_video_decoder_t* decoder_;int (*receive_)(os_video_decoder_t*,os_video_frame_t*)=receive;
 Config config_;bool traced_frame_=false;
 explicit NativeVulkanVideoDecoder(os_video_decoder_t* decoder):decoder_(decoder){}
 DecoderStatus Drain(std::vector<scoped_refptr<VideoFrame>>* frames);
};
'''
suffix = r'''
static void verify(const std::vector<scoped_refptr<VideoFrame>>& frames) {
 for(unsigned i=0;i<frames.size();++i) {
  const auto& f=*frames[i];assert(f.timestamp==int64_t(i)*33333-1000 && f.color==42);
  for(int p=0;p<2;++p) {
   auto& bytes=p==0?f.y:f.uv;
   int rows=p==0?f.size.h:(f.size.h+1)/2;
   int columns=p==0?f.size.w:((f.size.w+1)/2)*2;
   assert(bytes.front()==0xa5 && bytes.back()==0xa5);
   for(int r=0;r<rows;++r)for(int c=0;c<f.stride(p);++c)
    assert(bytes[1+r*f.stride(p)+c]==(c<columns?pixel(i,p,r,c):0xa5));
  }
 }
}
int main() {
 unsigned scenarios=0;
 for(auto size:{gfx::Size(1,1),gfx::Size(7,5),gfx::Size(64,36),gfx::Size(1920,1080)}) {
  for(int mode=0;mode<3;++mode) {
   os_video_decoder decoder;decoder.size=size;decoder.count=size.w>100?3:80;decoder.mode=mode;
   NativeVulkanVideoDecoder adapter{&decoder};adapter.config_.size=size;
   std::vector<scoped_refptr<VideoFrame>> frames;
   auto status=adapter.Drain(&frames);
   assert(status.value==(mode==2?DecoderStatus::Codes::kPlatformDecodeFailure:DecoderStatus::Codes::kOk));
   assert(frames.size()==decoder.count);verify(frames);
   // Reset/destroy may overwrite and release the borrowed storage too.
   std::fill(decoder.bytes.begin(),decoder.bytes.end(),0x19);
   decoder.bytes.clear();decoder.bytes.shrink_to_fit();verify(frames);
   ++scenarios;
  }
 }
 for(int mode=3;mode<=7;++mode) {
  os_video_decoder decoder;decoder.size={7,5};decoder.count=1;decoder.mode=mode;
  NativeVulkanVideoDecoder adapter{&decoder};adapter.config_.size={7,5};
  if(mode==6)adapter.config_.size={8,5};
  VideoFrame::fail_allocation=mode==7;
  std::vector<scoped_refptr<VideoFrame>> frames;
  auto status=adapter.Drain(&frames);assert(frames.empty());
  auto expected=mode==4||mode==6?DecoderStatus::Codes::kUnsupportedConfig:
    mode==7?DecoderStatus::Codes::kOutOfMemory:DecoderStatus::Codes::kPlatformDecodeFailure;
  assert(status.value==expected);++scenarios;
 }
 assert(scenarios==17);
}
'''
with tempfile.TemporaryDirectory(prefix='chrome-video-frame-ownership-') as directory:
    path = Path(directory)
    for name, body in (
        ('actual', drain),
        ('negative', drain.replace('decoded.y + size_t(row) * decoded.stride_y',
                                   'decoded.y + size_t(0) * decoded.stride_y')),
    ):
        assert name == 'actual' or body != drain
        unit = path / f'{name}.cc'
        unit.write_text(prefix + body + suffix)
        executable = path / name
        subprocess.run(['c++', '-std=c++20', '-O1', '-g', '-Wall', '-Wextra',
                        '-Werror', '-fsanitize=address,undefined', '-fno-pie',
                        '-no-pie', '-I', str(root/'include'), str(unit), '-o',
                        str(executable)], check=True, timeout=45)
        result = subprocess.run([str(executable)], capture_output=True, timeout=30)
        if name == 'actual':
            if result.returncode:
                raise RuntimeError(result.stderr.decode())
        elif result.returncode == 0 or b'Assertion' not in result.stderr:
            raise AssertionError('Negative row-copy control did not trip pixel verification')
print('Chrome Drain ownership: 17 scenarios, reused source, retained NV12 pixels, '
      'odd sizes, padded strides, reset/release, EOF/errors and negative control passed')
