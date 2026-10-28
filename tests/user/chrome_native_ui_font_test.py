"""Execute the UI fallback with the actual deployed Latin/CJK glyph maps."""
from pathlib import Path
import shlex
import subprocess
import sys
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from chromium_source import chromium_source

root = Path(__file__).resolve().parents[2]
chromium = chromium_source()


def function(path, signature):
    source = path.read_text()
    start = source.index(signature)
    cursor = source.index('{', start) + 1
    depth = 1
    while depth:
        depth += (source[cursor] == '{') - (source[cursor] == '}')
        cursor += 1
    return source[start:cursor]


fallback = function(chromium/'ui/gfx/font_fallback_linux.cc',
                    'bool GetFallbackFont(const Font& font,')
assert '#if defined(__NATIVE_OS__)' in fallback, 'UI fallback still requires Fontconfig'
resolver = function(chromium/'skia/ext/font_utils.cc',
                    'bool GetNativeFallbackFontForCharacter(')
fallback_list = function(chromium/'ui/gfx/font_fallback_linux.cc',
                         'std::vector<Font> GetFallbackFonts(')
code = r'''
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>
#include <ft2build.h>
#include FT_FREETYPE_H
#define __NATIVE_OS__ 1
#define TRACE_EVENT0(...)
#define UNSAFE_TODO(x) x
#define U16_NEXT(s,i,n,c) do { \
 (c)=(s)[(i)++]; \
 if ((c)>=0xd800 && (c)<=0xdbff && (i)<(n) && \
     (s)[i]>=0xdc00 && (s)[i]<=0xdfff) \
   (c)=0x10000+(((c)-0xd800)<<10)+((s)[(i)++]-0xdc00); \
} while(0)
using UChar32=int32_t;using SkUnichar=int32_t;using SkTypefaceID=uint32_t;
template<class T>using sk_sp=std::shared_ptr<T>;
namespace absl {template<class T>using flat_hash_set=std::unordered_set<T>;}
namespace base {using FilePath=std::string;}
struct SkFontStyle {};
static FT_Library library;
static std::string font_root;
static unsigned file_reads, next_id;
static bool missing_cjk;
struct SkTypeface {
 FT_Face face;unsigned id;
 SkTypeface(FT_Face f):face(f),id(++next_id){}
 ~SkTypeface(){FT_Done_Face(face);}
 unsigned unicharToGlyph(SkUnichar c)const{return FT_Get_Char_Index(face,c);}
 unsigned uniqueID()const{return id;}
};
struct Manager {
 sk_sp<SkTypeface> makeFromFile(const char* name,int index=0){
  ++file_reads;
  if(missing_cjk && strstr(name,"CJK"))return nullptr;
  const std::string path=font_root+name;
  FT_Face face=nullptr;
  if(FT_New_Face(library,path.c_str(),index,&face))return nullptr;
  return std::make_shared<SkTypeface>(face);
 }
};
namespace skia {
static Manager manager;
Manager* DefaultFontMgr(){return &manager;}
sk_sp<SkTypeface> MakeTypefaceFromName(const char*,SkFontStyle){
 static auto latin=manager.makeFromFile("/EFI/OS/fonts/default.ttf");return latin;
}
''' + resolver + r'''
}
sk_sp<SkTypeface> GetSkTypefaceFromPathAndIndex(const base::FilePath& path,int index){
 static std::map<std::pair<std::string,int>,sk_sp<SkTypeface>> cache;
 auto key=std::make_pair(path,index);
 auto [it,added]=cache.try_emplace(key);
 if(added)it->second=skia::DefaultFontMgr()->makeFromFile(path.c_str(),index);
 return it->second;
}
struct PlatformFont {
 sk_sp<SkTypeface> face;int size;
 static PlatformFont* CreateFromSkTypeface(sk_sp<SkTypeface> face,int size,std::nullopt_t){
  return new PlatformFont{std::move(face),size};
 }
};
struct Font {
 std::shared_ptr<PlatformFont> platform;
 Font():platform(std::make_shared<PlatformFont>(PlatformFont{nullptr,17})){}
 explicit Font(PlatformFont* p):platform(p){}
 int GetFontSize()const{return platform->size;}
};
''' + fallback + '\n' + fallback_list + r'''
static void check(std::u16string_view text,bool expected,bool full=true){
 Font primary,result;auto old=result.platform;
 assert(GetFallbackFont(primary,"zh-CN",text,&result)==expected);
 if(!expected){assert(result.platform==old);return;}
 assert(result.GetFontSize()==17 && result.platform->face);
 if(full){
  size_t i=0;while(i<text.size()){
   UChar32 c;U16_NEXT(text.data(),i,text.size(),c);
   assert(result.platform->face->unicharToGlyph(c));
  }
 }
}
int main(int argc,char** argv){
 assert(argc==3);font_root=argv[1];missing_cjk=!strcmp(argv[2],"missing");
 assert(!FT_Init_FreeType(&library));
 assert(GetFallbackFonts(Font()).empty());
 check(u"",false);check(u"Chrome ABC 123",true);
 if(missing_cjk){
  check(u"抖音",false);unsigned before=file_reads;
  for(unsigned i=0;i<100;++i)check(u"中文标题",false);
  assert(file_reads==before);
 }else{
  check(u"抖音精选 - 视频",true);check(u"Chrome 中文 ABC",true);
  check(u"日本語 한글",true);
  // An astral CJK code point must be decoded as one scalar, not two glyphs.
  check(u"\U0002000B",true);
  check(u"\U0010FFFF",false);
  check(std::u16string_view(u"\xD800",1),false);
  check(u"中文\U0010FFFF",true,false);
  unsigned before=file_reads;
  for(unsigned i=0;i<100;++i)check(u"中文 - Chrome",true);
  assert(file_reads==before);
 }
 puts("Native UI font fallback: actual bundled glyphs, mixed scripts, UTF-16, unchanged failure result and cached file reads PASS");
}
'''
flags = shlex.split(subprocess.check_output(
    ['pkg-config', '--cflags', '--libs', 'freetype2'], text=True))
with tempfile.TemporaryDirectory(prefix='chrome-native-ui-font-') as directory:
    path = Path(directory)
    (path/'test.cc').write_text(code)
    subprocess.run(['c++', '-std=c++20', '-O2', '-Wall', '-Wextra', '-Werror',
                    '-Wno-unused-parameter', str(path/'test.cc'),
                    '-o', str(path/'test'), *flags], check=True)
    for scenario in ('present', 'missing'):
        subprocess.run([str(path/'test'), str(root/'build/esp'), scenario],
                       check=True, timeout=20)
    marker = 'if (cjk && cjk->unicharToGlyph(character) != 0)'
    assert marker in code
    (path/'test.cc').write_text(code.replace(marker,
        'if (false && cjk && cjk->unicharToGlyph(character) != 0)'))
    subprocess.run(['c++', '-std=c++20', '-O2', '-Wall', '-Wextra', '-Werror',
                    '-Wno-unused-parameter', str(path/'test.cc'),
                    '-o', str(path/'old'), *flags], check=True)
    old = subprocess.run([str(path/'old'), str(root/'build/esp'), 'present'],
                         capture_output=True, timeout=20)
    assert old.returncode != 0 and b'Assertion' in old.stderr
    print('Negative control rejected: missing bundled CJK selection')
