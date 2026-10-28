"""Execute the actual HEVC probe/cache and software-mode factory guard."""
from pathlib import Path
import subprocess
import tempfile
import sys
sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
from chromium_source import chromium_source

root=Path(__file__).resolve().parents[2]
chrome=chromium_source()
source=(chrome/'media/base/supported_types.cc').read_text()
def function(text, name):
    start=text.index(name);opening=text.index('{',start)
    depth,end=1,opening+1
    while depth:
        depth+=(text[end]=='{')-(text[end]=='}');end+=1
    return text[start:end]
query=function(source,'bool QueryNativeHevcMainSupport(')
cache=function(source,'bool IsNativeHevcMainSupported(')
policy=function(source,'bool IsDecoderHevcProfileSupported(')
factory=(chrome/'media/renderers/default_decoder_factory.cc').read_text()
guard=function(factory,'if (IsNativeVideoHardwareEnabled())')
native_policy=(chrome/'media/base/native_video_policy.h').read_text()
mode_policy=function(native_policy,
                     'inline bool IsNativeVideoHardwareEnabled(')
hardware_query=function(native_policy, 'inline bool QueryNativeVideoDecodeSupport(')
availability=function(native_policy, 'inline bool IsNativeVideoDecodeAvailable(')
status=(chrome/'content/browser/gpu/compositor_util.cc').read_text()
decode_status=status[status.index('"video_decode"'):status.index('"video_encode"')]
assert 'GetFakeFeatureStatus(media::IsNativeVideoDecodeAvailable())' in decode_status
assert 'IsNativeVideoDecodeAvailable' not in status[status.index('"video_encode"'):status.index('"rasterization"')]
code=r'''
#include <cassert>
#include <cstring>
#include <cstdlib>
#include <memory>
#include <optional>
#include <string>
#include <vector>
#include <uapi/video_decoder.h>
#define __NATIVE_OS__ 1
#define BUILDFLAG(x) 0
#define RTLD_NOW 2
#define RTLD_LOCAL 0
static int mode;
static unsigned opens,closes,queries;
static bool disabled;
namespace base {
struct Environment {
 static std::unique_ptr<Environment> Create(){return std::make_unique<Environment>();}
 std::optional<std::string> GetVar(const char *name){
  const char *value=std::getenv(name);
  return value?std::optional<std::string>(value):std::nullopt;
 }
};
struct CommandLine {
 static CommandLine* ForCurrentProcess(){static CommandLine c;return &c;}
 bool HasSwitch(const char *s)const{
  assert(!strcmp(s,"disable-gpu")||!strcmp(s,"disable-accelerated-video-decode"));return disabled;
 }
};
}
static void *dlopen(const char *name,int flags){
 // 必须从 Chrome 私有目录加载视频解码库。
 assert(!strcmp(name,"/usr/apps/chrome/lib/libos-vulkan-video.so.1")&&flags==RTLD_NOW);
 ++opens;return mode==1?nullptr:reinterpret_cast<void*>(1);
}
static uint32_t abi(){return mode==3?99:OS_VIDEO_DECODER_ABI;}
static int capability(uint32_t codec,os_video_decode_capabilities_t *out){
 assert(codec>=OS_VIDEO_CODEC_H264&&codec<=OS_VIDEO_CODEC_AV1&&out);++queries;
 if(mode==5)return -5;
 if(mode==8&&codec!=OS_VIDEO_CODEC_HEVC)return -5;
 if(mode==9&&codec!=OS_VIDEO_CODEC_AV1)return -5;
 *out={16,16,8192,8192};
 if(mode==6)out->min_width=0;
 if(mode==7)out->max_height=8;
 return 0;
}
static void *dlsym(void *p,const char *name){
 assert(p);
 if(!strcmp(name,"os_video_decoder_abi"))return mode==2?nullptr:reinterpret_cast<void*>(&abi);
 assert(!strcmp(name,"os_video_decoder_query"));
 return mode==4?nullptr:reinterpret_cast<void*>(&capability);
}
static int dlclose(void *p){assert(p);++closes;return 0;}
enum {HEVCPROFILE_MAIN=16,HEVCPROFILE_MAIN10=17};
struct VideoType{int profile,color_space;};
static bool IsColorSpaceSupported(int c){return c==1;}
''' + mode_policy + '\n' + hardware_query + '\n' + availability + '\n' + query + '\n' + cache + '\n' + policy + r'''
struct NativeVulkanVideoDecoder{explicit NativeVulkanVideoDecoder(int){}};
static void populate(std::vector<std::unique_ptr<NativeVulkanVideoDecoder>>& storage){
 auto *video_decoders=&storage;
 int task_runner=0;
''' + guard + r'''
}
int main(int argc,char **argv){
 if(argc==2){
  mode=atoi(argv[1]);
  setenv("OS_CHROME_GPU_MODE","software",1);
  assert(!IsNativeVideoDecodeAvailable()&&!opens);
  setenv("OS_CHROME_GPU_MODE","vulkan",1);
  bool expected=mode==0||mode==8||mode==9;
  assert(IsNativeVideoDecodeAvailable()==expected);
  assert(opens==(mode==0?1u:mode==8?2u:3u));
  unsigned before=opens;
  for(int i=0;i<10000;++i)assert(IsNativeVideoDecodeAvailable()==expected);
  assert(opens==before);
  disabled=true;assert(!IsNativeVideoDecodeAvailable()&&opens==before);
  return 0;
 }
 unsetenv("OS_CHROME_GPU_MODE");
 for(mode=1;mode<=7;++mode){
  unsigned before=closes;
  assert(!QueryNativeHevcMainSupport());
  assert(closes==before+(mode==1?0:1));
 }
 mode=0;disabled=true;unsigned before=opens;
 assert(!IsNativeHevcMainSupported()&&opens==before);
 std::vector<std::unique_ptr<NativeVulkanVideoDecoder>> decoders;
 populate(decoders);assert(decoders.empty());
 disabled=false;assert(IsNativeHevcMainSupported());
 before=opens;mode=1;assert(IsNativeHevcMainSupported()&&opens==before);
 assert(IsDecoderHevcProfileSupported({HEVCPROFILE_MAIN,1}));
 assert(!IsDecoderHevcProfileSupported({HEVCPROFILE_MAIN10,1}));
 assert(!IsDecoderHevcProfileSupported({HEVCPROFILE_MAIN,0}));
 populate(decoders);assert(decoders.size()==1);
 disabled=true;assert(!IsDecoderHevcProfileSupported({HEVCPROFILE_MAIN,1}));
 disabled=false;setenv("OS_CHROME_GPU_MODE","software",1);
 decoders.clear();populate(decoders);assert(decoders.empty());
 assert(!IsDecoderHevcProfileSupported({HEVCPROFILE_MAIN,1}));
 setenv("OS_CHROME_GPU_MODE","invalid",1);assert(!IsNativeVideoHardwareEnabled());
 setenv("OS_CHROME_GPU_MODE","vulkan",1);assert(IsNativeVideoHardwareEnabled());
 assert(queries&&closes+1==opens); // only the failed dlopen has no handle
}
'''
with tempfile.TemporaryDirectory(prefix='native-hevc-capability-') as temporary:
    binary=str(Path(temporary)/'test')
    subprocess.run(['c++','-std=c++20','-Wall','-Wextra','-Werror',
                    '-fsanitize=address,undefined','-fno-pie','-no-pie',
                    '-I'+str(root/'include'),'-x','c++','-','-o',binary],
                   input=code,text=True,check=True)
    subprocess.run([binary],check=True)
    for mode in range(10):
        subprocess.run([binary,str(mode)],check=True)
print('Native HEVC capability: loader/ABI/query/extent failures closed, probe cached, Main10 excluded, software mode bypass PASS')
