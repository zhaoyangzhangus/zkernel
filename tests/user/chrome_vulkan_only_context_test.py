"""Execute real Vulkan-only context metadata and GL-free lifetime methods."""
from pathlib import Path
import subprocess
import sys
import tempfile
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from chromium_source import chromium_source
root = chromium_source()
state = (root/'gpu/command_buffer/service/shared_context_state.cc').read_text()
feature = (root/'gpu/command_buffer/service/feature_info.cc').read_text()

def function(source, signature):
    start = source.index(signature)
    opening = source.index('{', start)
    depth = 1
    end = opening + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]

methods = '\n'.join(function(state, signature) for signature in (
    'bool SharedContextState::InitializeVulkanOnly(',
    'bool SharedContextState::MakeCurrent(',
    'void SharedContextState::ReleaseCurrent(',
    'bool SharedContextState::IsCurrent(',
    'gl::GLSurface* SharedContextState::surface()',
    'gl::GLDisplay* SharedContextState::display()',
))
metadata = function(feature, 'void FeatureInfo::InitializeWithoutGL()')
manager = (root/'gpu/ipc/service/gpu_channel_manager.cc').read_text()
start = manager.index('// A genuine Vulkan-only context:')
end = manager.index('#endif', start)
branch = manager[start:end]
assert 'nullptr, nullptr, nullptr, false' in branch
assert 'InitializeVulkanOnly' in branch and 'InitializeSkia' in branch
assert 'CreateGLContext' not in branch and 'InitializeGL(' not in branch
assert branch.index('InitializeSkia') < branch.index('SetForCurrentThread')
factory = (root/'gpu/command_buffer/service/shared_image/shared_image_factory.cc').read_text()
assert 'if (use_gl && context_state_->context())' in factory
start = factory.index('  bool vulkan_without_gl = false;')
factory_gate = factory[start:factory.index('  CHECK(context_state_);', start)]
external = (root/'gpu/command_buffer/service/shared_image/external_vk_image_backing_factory.cc').read_text()
start = external.index('  if (!context_state_->context() &&')
external_gate = external[start:external.index('  if (!IsFormatSupported(', start)]
backing = (root/'gpu/command_buffer/service/shared_image/external_vk_image_backing.cc').read_text()
start = backing.index('  auto* gr_context = context_state()->gr_context();',
                      backing.index('ExternalVkImageBacking::~ExternalVkImageBacking()'))
semaphore_cleanup = backing[start:backing.index('  for (auto& vk_texture', start)]
start = backing.index('  if (context_state_->context_lost()', backing.index('bool ExternalVkImageBacking::BeginAccess('))
access_guard = backing[start:backing.index('  DLOG_IF(', start)]
internal = function(backing, 'bool ExternalVkImageBacking::BeginAccessInternal(')
assert access_guard.strip() in internal
start = backing.index('  if (stream == SharedImageAccessStream::kGL',
                      backing.index('bool ExternalVkImageBacking::CheckSupportForAccessStream('))
stream_guard = backing[start:backing.index('  if (base::FeatureList', start)]
code = r'''
#include <cassert>
#include <memory>
#include <utility>
#include <vector>
#define __NATIVE_OS__ 1
#define BUILDFLAG(x) 1
#define CHECK(x) assert(x)
struct Log {template<class T> Log& operator<<(T const&){return *this;}};
#define LOG(x) Log{}
template<class T> struct Ptr {
 T* p=nullptr; Ptr()=default; Ptr(T* v):p(v){} T* get()const{return p;}
 T* operator->()const{return p;} operator T*()const{return p;}
 Ptr& operator=(T* v){p=v;return *this;}
};
namespace base {template<class T,class... A> Ptr<T> MakeRefCounted(A&&... a){return new T(std::forward<A>(a)...);}}
namespace gfx {struct ExtensionSet{};}
namespace gl {
 enum {kGLImplementationDisabled=0};static int implementation=0;
 int GetGLImplementation(){return implementation;}
 // 缺少 GL 实现时没有版本字符串。真实 ES 解析器会拒绝原先的 "0.0" 占位值，
 // 而该测试夹具曾接受这个值。
 struct GLVersionInfo {GLVersionInfo(const char* v,const char* r,gfx::ExtensionSet){assert(v==nullptr&&std::string(r).empty());}};
 struct GLDisplay{};
 struct GLSurface {GLDisplay display;GLDisplay* GetGLDisplay(){return &display;}};
 struct GLContext {GLSurface surface;unsigned makes=0,releases=0;bool allowed=true;
  GLSurface* default_surface(){return &surface;}
  bool MakeCurrent(GLSurface*){++makes;return allowed;}
  void ReleaseCurrent(GLSurface*){++releases;}
  bool IsCurrent(GLSurface*){return allowed;}
 };
}
struct GpuDriverBugWorkarounds{}; struct GpuFeatureInfo{};
enum class GrContextType{kGL,kVulkan};
namespace error {enum ContextLostReason{kMakeCurrentFailed};}
struct FeatureInfo {
 struct FeatureFlags{bool gl_extensions=false;};
 FeatureFlags feature_flags_;bool initialized_=false;
 std::unique_ptr<gl::GLVersionInfo> gl_version_info_;
 FeatureInfo(GpuDriverBugWorkarounds,GpuFeatureInfo){}
 void InitializeWithoutGL();
};
namespace gpu {namespace gles2 {using FeatureInfo=::FeatureInfo;}}
struct SharedContextState {
 Ptr<gl::GLContext> context_;Ptr<gl::GLSurface> last_current_surface_;
 void* vk_context_provider_=this;GrContextType gr_context_type_=GrContextType::kVulkan;
 Ptr<FeatureInfo> feature_info_;bool lost=false,reset=false;
 ~SharedContextState(){delete feature_info_.get();}
 bool context_lost()const{return lost;} bool IsUsingGL()const{return gr_context_type_==GrContextType::kGL;}
 gl::GLContext* context(){return context_.get();}void* vk_context_provider(){return vk_context_provider_;}
 bool GrContextIsVulkan(){return gr_context_type_==GrContextType::kVulkan;}
 void MarkContextLost(error::ContextLostReason){lost=true;}
 bool CheckResetStatus(bool){return reset;}
 bool InitializeVulkanOnly(const GpuDriverBugWorkarounds&,const GpuFeatureInfo&);
 bool MakeCurrent(gl::GLSurface*,bool=false);void ReleaseCurrent(gl::GLSurface*);
 bool IsCurrent(gl::GLSurface*,bool=false);gl::GLSurface* surface()const;gl::GLDisplay* display();
};
''' + metadata + methods + '\nvoid* SkipFactories(SharedContextState* context_state_) {\n' + factory_gate.replace('return;', 'return (void*)1;') + r'''
 return nullptr;
}
enum {SHARED_IMAGE_USAGE_GLES2_READ=1,SHARED_IMAGE_USAGE_GLES2_WRITE=2};
struct Usage{unsigned bits;bool Has(unsigned v){return (bits&v)!=0;}};
bool ExternalUsageAllowed(SharedContextState* context_state_,Usage usage){
''' + external_gate + r'''
 return true;
}
struct GrContext {bool lost=false;bool abandoned(){return lost;}};
struct BackingContext {GrContext* gr=nullptr;bool lost=false;bool context_lost(){return lost;}GrContext* gr_context(){return gr;}};
struct Backing {
 BackingContext state;unsigned waits=0,returns=0;
 BackingContext* context_state(){return &state;}
 void WaitSemaphoresOnGrContext(GrContext* gr,std::vector<int>* pending){assert(gr&&!gr->abandoned()&&!pending->empty());++waits;}
 void ReturnPendingSemaphoresWithFenceHelper(std::vector<int> pending){assert(!pending.empty());++returns;}
 void Cleanup(std::vector<int> semaphores){
''' + semaphore_cleanup + r'''
 }
};
bool CanBeginAccess(BackingContext* context_state_){
''' + access_guard + r'''
 return true;
}
enum class SharedImageAccessStream{kGL,kSkia};
struct AccessParams{SharedContextState* context_state=nullptr;};
bool StreamAllowed(SharedImageAccessStream stream,AccessParams params){
''' + stream_guard + r'''
 return true;
}
int main(){
 for(int lost=0;lost<2;++lost)for(int present=0;present<2;++present){
  GrContext gr;BackingContext state;state.lost=lost;state.gr=present?&gr:nullptr;
  assert(CanBeginAccess(&state)==(!lost&&present));
 }
 for(int condition=0;condition<3;++condition)for(int count=0;count<2;++count){
  Backing backing;GrContext gr;gr.lost=condition==1;
  backing.state.gr=condition==2?nullptr:&gr;
  backing.Cleanup(std::vector<int>(count,1));
  assert(backing.waits==(condition==0&&count?1u:0u));
  assert(backing.returns==backing.waits);
 }
 SharedContextState vk;
 assert(vk.InitializeVulkanOnly({},{}));
 assert(vk.feature_info_->initialized_ && vk.feature_info_->gl_version_info_);
 assert(!vk.feature_info_->feature_flags_.gl_extensions);
 assert(!vk.InitializeVulkanOnly({},{}));
 assert(vk.MakeCurrent(nullptr)&&vk.IsCurrent(nullptr));
 assert(!vk.MakeCurrent(nullptr,true)&&!vk.IsCurrent(nullptr,true)&&!vk.lost);
 assert(!vk.surface()&&!vk.display());vk.ReleaseCurrent(nullptr);
 assert(!SkipFactories(&vk));
 assert(!StreamAllowed(SharedImageAccessStream::kGL,{&vk}));
 assert(StreamAllowed(SharedImageAccessStream::kSkia,{&vk}));
 assert(StreamAllowed(SharedImageAccessStream::kGL,{}));
 assert(SkipFactories(nullptr));
 for(unsigned usage=0;usage<4;++usage){
  assert(ExternalUsageAllowed(&vk,{usage})==(usage==0));
 }
 {SharedContextState invalid;invalid.vk_context_provider_=nullptr;assert(SkipFactories(&invalid));}
 {SharedContextState invalid;invalid.gr_context_type_=GrContextType::kGL;assert(SkipFactories(&invalid));}
 vk.lost=true;assert(!vk.MakeCurrent(nullptr));vk.ReleaseCurrent(nullptr);
 for(int failure=0;failure<3;++failure){SharedContextState invalid;gl::GLContext gl;
  if(failure==0)invalid.context_=&gl;
  if(failure==1)invalid.vk_context_provider_=nullptr;
  if(failure==2)invalid.gr_context_type_=GrContextType::kGL;
  assert(!invalid.InitializeVulkanOnly({},{}));
 }
 SharedContextState interop;gl::GLContext gl;interop.context_=&gl;interop.gr_context_type_=GrContextType::kGL;
 assert(interop.MakeCurrent(nullptr));assert(gl.makes==1&&interop.IsCurrent(nullptr));
 assert(StreamAllowed(SharedImageAccessStream::kGL,{&interop}));
 for(unsigned usage=0;usage<4;++usage)assert(ExternalUsageAllowed(&interop,{usage}));
 gl::implementation=1;assert(!SkipFactories(&interop));gl::implementation=0;
 assert(interop.surface()==&gl.surface&&interop.display()==&gl.surface.display);
 interop.ReleaseCurrent(nullptr);assert(gl.releases==1);
 gl.allowed=false;assert(!interop.MakeCurrent(nullptr)&&interop.lost);
}
'''
with tempfile.TemporaryDirectory(prefix='chrome-vulkan-only-context-') as directory:
    base = Path(directory)
    (base/'test.cc').write_text(code)
    subprocess.run(['c++', '-std=c++20', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
                    str(base/'test.cc'), '-o', str(base/'test')], check=True)
    subprocess.run([str(base/'test')], check=True)
print('Vulkan-only context: actual metadata, invalid/duplicate initialization, '
      'GL-free bind/release/display, unsupported GL requests, lost context '
      'and preserved real GL behavior passed')
