"""Execute renderer profile forwarding together with the real raster gate."""
import ast
from pathlib import Path
import subprocess
import sys
import tempfile
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from chromium_source import chromium_source
root = Path(__file__).resolve().parents[2]
source_root = chromium_source()
source = (source_root/'content/browser/renderer_host/render_process_host_impl.cc').read_text()
start = source.index('#if defined(__NATIVE_OS__)\n  // Vulkan-only raster binding')
forward = source[start:source.index('#endif', start) + len('#endif')]
function_start = source.index('void RenderProcessHostImpl::PropagateBrowserCommandLineToRenderer(')
assert function_start < source.index('renderer_cmd->CopySwitchesFrom(browser_cmd, kSwitchNames);', function_start) < start
gate_source = (source_root/'services/viz/public/cpp/gpu/context_provider_command_buffer.cc').read_text()
start = gate_source.index('  bool allow_vulkan_raster = false;')
gate = gate_source[start:gate_source.index('  // Any early-out', start)]
tree = ast.parse((root/'tests/user/chrome_vulkan_raster_policy_test.py').read_text())
fixture = next(ast.literal_eval(node.value) for node in tree.body
               if isinstance(node, ast.Assign) and any(isinstance(t, ast.Name) and t.id == 'fixture' for t in node.targets))
fixture = fixture[:fixture.index('int main() {')].replace('@GATE@', gate)
fixture = fixture.replace(' std::string platform, mode;', ''' std::string platform, mode;
 void AppendSwitchASCII(const char* name, const char* value) {
  assert(std::string(name)=="use-vulkan"); mode=value;
 }''')
checks = r'''
void forward(const base::CommandLine& browser_cmd, base::CommandLine* renderer_cmd) {
 (void)browser_cmd;(void)renderer_cmd;
 @FORWARD@
}
int main() {
 for(bool native_platform:{false,true}) for(const char* mode:{"native","swiftshader","software",""})
 for(bool vk:{false,true}) for(int tag=0;tag<3;++tag) {
  base::CommandLine browser;browser.platform=native_platform?"nativeguest":"x11";browser.mode=mode;
  auto* renderer=base::CommandLine::ForCurrentProcess();renderer->platform=browser.platform;renderer->mode="";
  forward(browser,renderer);
  bool forwarded=false;
 #ifdef __NATIVE_OS__
  forwarded=native_platform&&std::string(mode)=="native";
 #endif
  assert(renderer->mode==(forwarded?"native":""));
  Channel channel{{{vk}},{{gl::kGLImplementationDisabled}}};
  gpu::mojom::ContextCreationAttribs attributes{static_cast<gpu::mojom::ContextCreationAttribs::Tag>(tag)};
  bool allowed=forwarded&&vk&&tag==0;
  assert((bind(&channel,&attributes)==gpu::ContextResult::kSuccess)==allowed);
  // 复现旧的子进程配置：渲染器未收到后端选择时，浏览器 Vulkan 不会启用光栅绑定。
  renderer->mode="";
  assert(bind(&channel,&attributes)==gpu::ContextResult::kFatalFailure);
 }
}
'''
with tempfile.TemporaryDirectory() as directory:
    path = Path(directory)
    (path/'test.cc').write_text(fixture + checks.replace('@FORWARD@', forward))
    for native in (False, True):
        command = ['c++', '-std=c++20', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined', '-fno-pie', '-no-pie']
        if native: command.append('-D__NATIVE_OS__=1')
        subprocess.run(command + [str(path/'test.cc'), '-o', str(path/'test')], check=True)
        subprocess.run([str(path/'test')], check=True)
print('Renderer Vulkan profile + actual raster binding: native feature-backed raster succeeds; missing flag, upstream, foreign platform, software, GLES and WebGPU stay rejected')
