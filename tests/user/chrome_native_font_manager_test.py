"""原生字体管理器直接加载系统字体，不触发宿主字体数据库初始化。"""
from pathlib import Path
import subprocess
import sys
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from chromium_source import chromium_source

source = (chromium_source() / 'skia/ext/font_utils.cc').read_text()
start = source.index('static sk_sp<SkFontMgr> fontmgr_factory() {')
end = source.index('\nsk_sp<SkFontMgr> DefaultFontMgr()', start)
factory = source[start:end]
fixture = r'''
#include <cassert>
#include <memory>
#define BUILDFLAG(x) x
#define IS_ANDROID 0
#define IS_APPLE 0
#define IS_LINUX 1
#define IS_CHROMEOS 0
#define IS_FUCHSIA 0
#define IS_WIN 0
template<class T> using sk_sp = std::shared_ptr<T>;
struct SkFontMgr {};
static sk_sp<SkFontMgr> override_owner;
static SkFontMgr* g_fontmgr_override;
static int creations;
static sk_sp<SkFontMgr> sk_ref_sp(SkFontMgr* value) {
  assert(value == override_owner.get());
  return override_owner;
}
static sk_sp<SkFontMgr> SkFontMgr_New_Fontations_Empty() {
  ++creations;
  return std::make_shared<SkFontMgr>();
}
''' + factory + r'''
int main() {
  auto native = fontmgr_factory();
  assert(native && creations == 1);
  override_owner = std::make_shared<SkFontMgr>();
  g_fontmgr_override = override_owner.get();
  auto overridden = fontmgr_factory();
  assert(overridden == override_owner && creations == 1);
}
'''

with tempfile.TemporaryDirectory() as directory:
    directory = Path(directory)
    cpp = directory / 'font-manager.cc'
    binary = directory / 'font-manager'
    cpp.write_text(fixture)
    subprocess.run(['c++', '-std=c++17', '-D__NATIVE_OS__=1',
                    str(cpp), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
    # 缺少原生分支时必须失败，防止测试只验证替身而未覆盖实际选择路径。
    control = subprocess.run(['c++', '-std=c++17', str(cpp),
                              '-o', str(binary)], capture_output=True)
    assert control.returncode != 0, '未启用原生路径时错误进入了文件字体管理器'
print('native font manager: PASS')
