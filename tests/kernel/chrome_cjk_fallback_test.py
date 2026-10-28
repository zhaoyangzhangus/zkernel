#!/usr/bin/env python3
"""Compile the real selector; verify deployed TTC index and glyph coverage."""
from pathlib import Path as _TestPath
import sys as _test_sys
_test_sys.path.insert(0, str(_TestPath(__file__).resolve().parents[1]))
from chromium_source import chromium_source
import os
import pathlib
import subprocess
import tempfile

chromium = chromium_source()
source = (chromium / 'skia/ext/font_utils.cc').read_text()
start = source.index('bool GetNativeFallbackFontForCharacter(')
method = source[start:source.index('#endif  // defined(__NATIVE_OS__)', start)]
gfx_source = (chromium / 'ui/gfx/font_fallback_linux.cc').read_text()
gfx_method = gfx_source[gfx_source.rindex('bool GetFallbackFontForChar('):gfx_source.rindex('}  // namespace gfx')]
blink_source = (chromium / 'third_party/blink/renderer/platform/fonts/linux/font_cache_linux.cc').read_text()
blink_method = blink_source[blink_source.index('bool FontCache::GetFontForCharacter('):blink_source.index('const SimpleFontData* FontCache::PlatformFallbackFontForCharacter(')]
loader_source = (chromium / 'third_party/blink/renderer/platform/fonts/skia/font_cache_skia.cc').read_text()
loader_start = loader_source.index('  if (creation_params.CreationType() == kCreateFontByFciIdAndTtcIndex) {')
loader_end = loader_source.index('\n  }\n#endif', loader_start) + len('\n  }')
loader_branch = loader_source[loader_start:loader_end]
routes = r'''
#define __NATIVE_OS__ 1
namespace skia { using ::GetNativeFallbackFontForCharacter; }
namespace base {
struct FilePath {
 std::string path;
 FilePath()=default;
 explicit FilePath(const char *p):path(p) {}
};
}
using UChar32=int32_t;
namespace gfx {
struct FallbackFontData { std::string name; base::FilePath filepath; int ttc_index=0; };
''' + gfx_method + r'''
}
namespace blink {
struct FontCache {
 static bool GetFontForCharacter(UChar32, const char *, gfx::FallbackFontData *);
};
''' + blink_method + '\n}\n' + r'''
constexpr int kCreateFontByFciIdAndTtcIndex=1;
struct CreationParams {
 int CreationType() const { return 1; }
 int TtcIndex() const { return 2; }
 std::string Filename() const { return "/EFI/OS/fonts/NotoSansCJK-Regular.ttc"; }
};
struct SkTypeface_Factory {
 static sk_sp<SkTypeface> FromFilenameAndTtcIndex(const char *path, int index) {
  assert(!strcmp(path,"/EFI/OS/fonts/NotoSansCJK-Regular.ttc") && index==2);
  return std::make_shared<SkTypeface>(SkTypeface{false});
 }
};
static sk_sp<SkTypeface> LoadSelectedFace(const CreationParams& creation_params) {
''' + loader_branch + r'''
 return nullptr;
}
'''
program = r'''
#include <cassert>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <initializer_list>
#include <cstdio>
using SkUnichar=int32_t;
template<class T> using sk_sp=std::shared_ptr<T>;
struct SkFontStyle {};
struct SkTypeface {
 bool primary;
 int unicharToGlyph(int c) const {
  return primary ? (c>=32 && c<=126) : (c==0x4e2d || c==0x65e5 || c==0xac00);
 }
};
static unsigned loads;
static bool missing;
struct SkFontMgr {
 sk_sp<SkTypeface> makeFromFile(const char *path, int index) {
  ++loads; assert(!strcmp(path,"/EFI/OS/fonts/NotoSansCJK-Regular.ttc") && index==2);
  return missing ? nullptr : std::make_shared<SkTypeface>(SkTypeface{false});
 }
};
static SkFontMgr *DefaultFontMgr() { static SkFontMgr mgr; return &mgr; }
static sk_sp<SkTypeface> MakeTypefaceFromName(const char *, SkFontStyle) {
 static auto primary=std::make_shared<SkTypeface>(SkTypeface{true}); return primary;
}
''' + method + routes + r'''
int main(int argc, char **) {
 missing=argc>1;
 const char *path="unchanged"; int index=-1;
 assert(!GetNativeFallbackFontForCharacter('a',nullptr,&index));
 assert(!GetNativeFallbackFontForCharacter('a',&path,nullptr));
 assert(!loads && index==-1);
 assert(GetNativeFallbackFontForCharacter('A',&path,&index));
 assert(!strcmp(path,"/EFI/OS/fonts/default.ttf") && index==0 && !loads);
 for(int c: {0x4e2d,0x65e5,0xac00}) {
  for(unsigned i=0;i<100;++i) {
   path="unchanged"; index=-1;
   bool found=GetNativeFallbackFontForCharacter(c,&path,&index);
   assert(found==!missing);
   if(found) assert(!strcmp(path,"/EFI/OS/fonts/NotoSansCJK-Regular.ttc") && index==2);
   else assert(!strcmp(path,"unchanged") && index==-1);
  }
 }
 assert(loads==1);
 path="unchanged"; index=-1;
 assert(!GetNativeFallbackFontForCharacter(0x10ffff,&path,&index));
 assert(!strcmp(path,"unchanged") && index==-1 && loads==1);
 gfx::FallbackFontData route;
 assert(!gfx::GetFallbackFontForChar(0x4e2d,"",nullptr));
 assert(blink::FontCache::GetFontForCharacter(0x4e2d,nullptr,&route)==!missing);
 if(!missing) assert(route.ttc_index==2 && route.filepath.path=="/EFI/OS/fonts/NotoSansCJK-Regular.ttc");
 auto face=LoadSelectedFace(CreationParams{});
 assert(face && face->unicharToGlyph(0x4e2d));
 puts("CJK selection: primary face, TTC index, unsupported/missing glyphs and one-time load passed");
}
'''
with tempfile.TemporaryDirectory(prefix='system-cjk-fallback-') as directory:
    binary = str(pathlib.Path(directory) / 'test')
    subprocess.run(['c++', '-std=c++17', '-Wall', '-Wextra', '-Werror', '-Wno-unused-parameter', '-x', 'c++', '-', '-o', binary],
                   input=program, text=True, check=True)
    subprocess.run([binary], check=True)
    subprocess.run([binary, 'missing'], check=True)

font = os.environ.get('OS_CJK_FONT', '/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc')
charset = subprocess.check_output(['fc-query', '--index', '2', '--format', '%{charset}', font], text=True)
ranges = [tuple(int(x, 16) for x in entry.split('-')) for entry in charset.split()]
for character in (0x4e2d, 0x6587, 0x65e5, 0x6f22, 0x8a9e, 0xac00, 0x3002):
    assert any(r[0] <= character <= r[-1] for r in ranges), hex(character)
print('Actual Noto TTC index 2 covers Chinese, Japanese, Korean and punctuation')
