#!/usr/bin/env python3
"""Exercise actual Blink/GFX selection, glyph checks and metadata reset."""
from chromium_source import chromium_source
import pathlib
import subprocess
import tempfile

source = (chromium_source() / 'third_party/blink/renderer/platform/fonts/linux/font_cache_linux.cc').read_text()
start = source.index('#if defined(__NATIVE_OS__)', source.index('bool FontCache::GetFontForCharacter('))
branch = source[start:source.index('#else', start)] + '\n#endif\n'
selector_source = (chromium_source() / 'skia/ext/font_utils.cc').read_text()
start = selector_source.index('bool GetNativeFallbackFontForCharacter(')
selector = selector_source[start:selector_source.index('#endif  // defined(__NATIVE_OS__)', start)]
gfx_source = (chromium_source() / 'ui/gfx/font_fallback_linux.cc').read_text()
gfx_method = gfx_source[gfx_source.rindex('bool GetFallbackFontForChar('):gfx_source.rindex('}  // namespace gfx')]
mock = r'''
#include <cassert>
#include <memory>
#include <string>
using SkUnichar = int;
using UChar32 = int;
template<class T> using sk_sp = std::shared_ptr<T>;
struct SkFontStyle {};
struct SkTypeface { int unicharToGlyph(int c) { return c == 65 ? 1 : 0; } };
static bool available = true;
namespace skia {
struct FontManager {
 sk_sp<SkTypeface> makeFromFile(const char*, int) { return nullptr; }
};
static FontManager* DefaultFontMgr() { static FontManager manager; return &manager; }
sk_sp<SkTypeface> MakeTypefaceFromName(const char*, SkFontStyle) {
 return available ? std::make_shared<SkTypeface>() : nullptr;
}
''' + selector + r'''
}
namespace base { using FilePath = std::string; }
namespace gfx {
struct FallbackFontData {
 std::string name, filepath;
 int fontconfig_interface_id = 0, ttc_index = 0;
 bool is_bold = false, is_italic = false;
};
''' + gfx_method + r'''
}
bool route(int c, const char* preferred_locale, gfx::FallbackFontData* fallback_font) {
''' + branch + r'''
}
int main() {
 gfx::FallbackFontData data;
 data.ttc_index=7;data.is_bold=true;
 assert(route(65,"en",&data));
 assert(data.filepath=="/EFI/OS/fonts/default.ttf" && data.ttc_index==0 && !data.is_bold);
 assert(!route(0x4e2d,"zh",&data));
 assert(!route(65,nullptr,nullptr));
 available=false;
 assert(!route(65,"en",&data));
}
'''
with tempfile.TemporaryDirectory(prefix='character-font-') as directory:
    binary = str(pathlib.Path(directory) / 'test')
    subprocess.run(['c++', '-std=c++17', '-Wall', '-Wextra', '-Werror', '-Wno-unused-parameter',
                    '-D__NATIVE_OS__', '-x', 'c++', '-', '-o', binary],
                   input=mock, text=True, check=True)
    subprocess.run([binary], check=True)
print('character fallback: supported glyph, absent glyph/font, null output and metadata reset passed')
