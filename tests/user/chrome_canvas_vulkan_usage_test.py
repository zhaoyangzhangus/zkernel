"""Execute production canvas usage selection and shared-image context binding.

The runtime failure was CanvasResourceRaster usage=0x40007 with no GL context.
Only the optional GL-read flag may be omitted; explicit caller usages survive.
"""
from pathlib import Path
import re
import subprocess
import sys
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from chromium_source import chromium_source

root = chromium_source()
graphics = root / 'third_party/blink/renderer/platform/graphics'
utils = (graphics / 'gpu/canvas_utils.cc').read_text()
start = utils.index('gpu::SharedImageUsageSet CanvasRasterSharedImageUsage(')
selection = utils[start:utils.index('bool Accelerated2DCanvasFeatureEnabled(', start)]
for name in ('canvas_2d_resource_provider.cc', 'canvas_non_2d_resource_provider.cc'):
    text = (graphics / name).read_text()
    assert text.count('shared_image_usage_flags = CanvasRasterSharedImageUsage(') == 1
    assert 'context_provider_wrapper_->ContextProvider().GetGpuFeatureInfo());' in text
    assert 'gpu::SHARED_IMAGE_USAGE_RASTER_WRITE |\n' not in text

stub = (root / 'gpu/ipc/service/shared_image_stub.cc').read_text()
start = stub.index('bool SharedImageStub::MakeContextCurrent(')
binding = stub[start:stub.index('ContextResult SharedImageStub::Initialize()', start)]
usage_source = (root / 'gpu/command_buffer/common/shared_image_usage.h').read_text()
flags = re.findall(r'(SHARED_IMAGE_USAGE_\w+) = 1[uU]? << (\d+)', usage_source)
assert len(flags) > 20
enum_flags = ',\n'.join(f'{name}=1U<<{bit}' for name, bit in flags)
usage_cc = (root / 'gpu/command_buffer/common/shared_image_usage.cc').read_text()
start = usage_cc.index('bool HasGLES2ReadOrWriteUsage(')
has_gl = usage_cc[start:usage_cc.index('\n}', start) + 2]
wrapped = (root / 'gpu/command_buffer/service/shared_image/wrapped_sk_image_backing_factory.cc').read_text()
start = wrapped.index('constexpr SharedImageUsageSet kSupportedUsage =')
supported = wrapped[start:wrapped.index(';', start) + 1]

program = r'''
#include <cassert>
#include <cstdint>
#include <cstdio>
struct Log {template<class T> Log& operator<<(const T&){return *this;}};
#define LOG(x) Log{}
namespace gpu {
enum SharedImageUsage : uint32_t {@FLAGS@};
struct SharedImageUsageSet {
 uint32_t value;
 constexpr SharedImageUsageSet(uint32_t v=0):value(v){}
 constexpr bool HasAny(SharedImageUsageSet b)const{return value&b.value;}
 SharedImageUsageSet& operator|=(SharedImageUsageSet b){value|=b.value;return *this;}
};
constexpr SharedImageUsageSet operator|(SharedImageUsageSet a,SharedImageUsageSet b){return a.value|b.value;}
enum {GPU_FEATURE_TYPE_VULKAN,GPU_FEATURE_TYPE_ACCELERATED_GL,kGpuFeatureStatusEnabled=1};
struct GpuFeatureInfo {int status_values[2];};
@HAS_GL@
@SUPPORTED@
}
namespace blink {@SELECTION@}
struct GLContext {bool IsCurrent(void*){return true;}};
struct State {
 GLContext* gl=nullptr;
 bool lost=false;
 bool context_lost(){return lost;}
 GLContext* real_context(){return gl;}
 bool CheckResetStatus(bool){return lost;}
 bool MakeCurrent(void*,bool needs_gl){return !lost&&(!needs_gl||gl);}
};
struct SharedImageStub {
 State* context_state_;
 bool MakeContextCurrent(bool);
};
@BINDING@
int main(){
 using namespace gpu;
 State state;SharedImageStub stub{&state};
 for(int vk=0;vk<6;++vk)for(int gl=0;gl<6;++gl)for(unsigned mask=0;mask<128;++mask){
  const unsigned optional[]={SHARED_IMAGE_USAGE_SCANOUT,SHARED_IMAGE_USAGE_CPU_UPLOAD,
   SHARED_IMAGE_USAGE_MIPMAP,SHARED_IMAGE_USAGE_WEBGPU_READ,
   SHARED_IMAGE_USAGE_GLES2_READ,SHARED_IMAGE_USAGE_GLES2_WRITE,
   SHARED_IMAGE_USAGE_CONCURRENT_READ_WRITE};
  unsigned requested=SHARED_IMAGE_USAGE_DISPLAY_READ;
  for(unsigned bit=0;bit<7;++bit)if(mask&(1U<<bit))requested|=optional[bit];
  GpuFeatureInfo info{{vk,gl}};
  auto result=blink::CanvasRasterSharedImageUsage(requested,info);
  unsigned expected=requested|SHARED_IMAGE_USAGE_RASTER_READ|SHARED_IMAGE_USAGE_RASTER_WRITE;
#if defined(__NATIVE_OS__)
  if(!(vk==kGpuFeatureStatusEnabled&&gl!=kGpuFeatureStatusEnabled))
#endif
   expected|=SHARED_IMAGE_USAGE_GLES2_READ;
  assert(result.value==expected && (result.value&requested)==requested);
 }
 GpuFeatureInfo vk_only{{kGpuFeatureStatusEnabled,0}};
 auto canvas=blink::CanvasRasterSharedImageUsage(SHARED_IMAGE_USAGE_DISPLAY_READ,vk_only);
#if defined(__NATIVE_OS__)
 assert(canvas.value==0x40006);
 assert((canvas.value&~kSupportedUsage.value)==0);
 assert(!HasGLES2ReadOrWriteUsage(canvas));
 assert(stub.MakeContextCurrent(HasGLES2ReadOrWriteUsage(canvas)));
#else
 assert(canvas.value==0x40007);
 assert(!stub.MakeContextCurrent(HasGLES2ReadOrWriteUsage(canvas)));
#endif
 // A real GL request must still fail when GL is absent, not silently succeed.
 auto explicit_gl=blink::CanvasRasterSharedImageUsage(
  SHARED_IMAGE_USAGE_DISPLAY_READ|SHARED_IMAGE_USAGE_GLES2_WRITE,vk_only);
 assert(!stub.MakeContextCurrent(HasGLES2ReadOrWriteUsage(explicit_gl)));
 GLContext gl;state.gl=&gl;
 assert(stub.MakeContextCurrent(HasGLES2ReadOrWriteUsage(explicit_gl)));
 state.lost=true;assert(!stub.MakeContextCurrent(false));
 puts("PASS: 4608 feature/usage combinations; Vulkan canvas uses supported backing, real GL requirements preserved");
}
'''
for token, value in (('FLAGS', enum_flags), ('HAS_GL', has_gl), ('SUPPORTED', supported),
                     ('SELECTION', selection), ('BINDING', binding)):
    program = program.replace('@' + token + '@', value)
with tempfile.TemporaryDirectory(prefix='chrome-canvas-vulkan-') as directory:
    binary = str(Path(directory) / 'test')
    def run(text, native):
        command = ['c++', '-std=c++20', '-Wall', '-Wextra', '-Werror',
                   '-fsanitize=address,undefined', '-fno-pie', '-no-pie']
        if native:
            command += ['-D__NATIVE_OS__=1']
        subprocess.run(command + ['-x', 'c++', '-', '-o', binary],
                       input=text, text=True, check=True)
        return subprocess.run([binary], capture_output=True, text=True, timeout=10)
    for native in (False, True):
        result = run(program, native)
        assert result.returncode == 0, result.stderr
        print(('Native: ' if native else 'Other platforms: ') + result.stdout.strip())
    negative = program.replace('    return usage;','    return usage | gpu::SHARED_IMAGE_USAGE_GLES2_READ;')
    assert negative != program
    result = run(negative, True)
    assert result.returncode != 0
    print('Negative control rejected: unconditional GLES2_READ reproduces the Vulkan-only context failure')
