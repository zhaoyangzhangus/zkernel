"""Execute the actual video priority rule for host and native builds."""
from pathlib import Path
import subprocess
import tempfile
import sys
sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
from chromium_source import chromium_source

source=(chromium_source()/'media/filters/decoder_selector.cc').read_text()
start=source.index('DecoderPriority SelectDecoderPriority(const VideoDecoderConfig&')
end=source.index('DecoderPriority SelectDecoderPriority(const AudioDecoderConfig&',start)
function=source[start:end]
fixture=r'''
#include <cassert>
enum class DecoderPriority { kNormal,kDeprioritized,kSkipped };
enum class VideoDecoderType { kNativeVulkan,kOther };
struct Rect {int h;int height()const{return h;}};
struct VideoDecoderConfig {int h;Rect visible_rect()const{return {h};}};
struct VideoDecoder {
 bool platform;VideoDecoderType type;
 bool IsPlatformDecoder()const{return platform;}
 VideoDecoderType GetDecoderType()const{return type;}
};
''' + function + r'''
int main(){
 for(int h:{180,359,360,720,1080,2160}){
  VideoDecoderConfig config{h};
  VideoDecoder native{true,VideoDecoderType::kNativeVulkan};
  VideoDecoder other{true,VideoDecoderType::kOther};
  VideoDecoder software{false,VideoDecoderType::kOther};
#ifdef __NATIVE_OS__
  assert(SelectDecoderPriority(config,native)==DecoderPriority::kNormal);
#else
  assert(SelectDecoderPriority(config,native)==(h>=360?DecoderPriority::kNormal:DecoderPriority::kDeprioritized));
#endif
  assert(SelectDecoderPriority(config,other)==(h>=360?DecoderPriority::kNormal:DecoderPriority::kDeprioritized));
  assert(SelectDecoderPriority(config,software)==(h<360?DecoderPriority::kNormal:DecoderPriority::kDeprioritized));
 }
}
'''
with tempfile.TemporaryDirectory(prefix='chrome-native-priority-') as temporary:
    for native in (False,True):
        binary=str(Path(temporary)/str(native))
        command=['c++','-std=c++20','-Wall','-Wextra','-Werror',
                 '-include','initializer_list','-x','c++','-','-o',binary]
        if native:command.append('-D__NATIVE_OS__')
        subprocess.run(command,input=fixture,text=True,check=True)
        subprocess.run([binary],check=True)
print('Native Vulkan priority: 180p to 4K prefer hardware, host/other/software ordering preserved PASS')
