"""真实文字绘制与逐像素参考公式比较，验证裁剪在字形查询之前完成。"""
from pathlib import Path
import subprocess
import tempfile
import resource

root = Path(__file__).resolve().parents[2]
source = (root/'kernel/graphics/control_document.c').read_text()
body = source[source.index('static void control_paint_text('):source.index('static void control_paint_node(')]
program = r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <kernel/control_tree.h>
#include <kernel/control_text.h>
#include <uapi/window_theme.h>
#include <ascii_font.h>
static uint32_t pixels[2+40*28];
static struct {uint32_t display_stride;uint32_t *composite_framebuffer;}
 g_window_server={40,pixels+1};
static struct {Rect damage_bounds;} g_compositor_snapshot;
static unsigned queries,fw=3,fh=5;
static uint8_t glyphs[128][15];
uint32_t ascii_font_width(void){return fw;}
uint32_t ascii_font_height(void){return fh;}
const uint8_t *ascii_font_glyph(uint8_t c){
 assert(c<128);++queries;return c=='~'?0:glyphs[c];
}
''' + body + r'''
static const char text[]="AB~CD\xc3\xa9 xyz";
static void verify(os_control_node_t n,int64_t x,int64_t y,uint32_t w,uint32_t h){
 const uint32_t under=0x9a174b8e;
 for(unsigned i=0;i<2+40*28;++i)pixels[i]=under;
 uint32_t th=n.font_size?n.font_size:fh,tw=th/2;
 int64_t tx=x+n.padding,ty=y+n.padding;
 if(n.kind==OS_CONTROL_BUTTON){tx=x+((int64_t)w-(int64_t)n.text_length*tw)/2;ty=y+((int64_t)h-th)/2;}
 queries=0;control_paint_text(&n,text,tx,ty,0);
 assert(pixels[0]==under&&pixels[1+40*28]==under);
 Rect clip=g_compositor_snapshot.damage_bounds;
 for(int py=0;py<28;++py)for(int px=0;px<40;++px){
  uint32_t expected=under;
  if(tw&&th&&px>=clip.x0&&px<clip.x1&&py>=clip.y0&&py<clip.y1&&
     px>=tx&&py>=ty&&py<ty+th&&(int64_t)px<tx+(int64_t)n.text_length*tw){
   unsigned i=(unsigned)(((int64_t)px-tx)/tw),col=(unsigned)(((int64_t)px-tx)%tw);
   unsigned row=(unsigned)((int64_t)py-ty);
   unsigned char c=(unsigned char)text[i];if(c>=128)c='?';
   uint32_t a=c=='~'?0:glyphs[c][row*fh/th*fw+col*fw/tw];
   expected=under&0xff000000U;
   for(unsigned s=0;s<24;s+=8)expected|=(((((under>>s)&255)*(255-a)+((n.foreground>>s)&255)*a+127)/255)<<s);
  }
  assert(pixels[1+py*40+px]==expected);
 }
}
int main(void){
 assert(control_button_background(0x80ffffffU,false)==(0x80000000U|OS_THEME_HOVER));
 assert(control_button_background(0x80ffffffU,true)==(0x80000000U|OS_THEME_SELECTED));
 assert(control_button_background(0x80000000U,false)==0x801f1f1fU);
 assert(control_button_background(0x80000000U,true)==0x80000000U);
 for(unsigned c=0;c<128;++c)for(unsigned i=0;i<15;++i)
  glyphs[c][i]=(uint8_t)((c*53+i*17)%3==0?0:(c*53+i*17)%3==1?255:117);
 unsigned long cases=0;
 for(unsigned sample=0;sample<3500;++sample){
  os_control_node_t n={.kind=sample%2?OS_CONTROL_BUTTON:OS_CONTROL_TEXT,
   .font_size=sample%97,.padding=sample%8,.foreground=0xffe36291,
   .text_length=sample%sizeof(text)};
  int64_t x=(int)(sample%101)-70,y=(int)(sample%73)-48;
  if(sample%113==0)x=(int64_t)INT32_MIN-100;
  if(sample%127==0)x=(int64_t)INT32_MAX+100;
  if(sample%139==0)y=(int64_t)INT32_MIN-100;
  g_compositor_snapshot.damage_bounds=(Rect){sample%7,sample%5,25+(int)(sample%15),20+(int)(sample%8)};
  verify(n,x,y,sample%61,sample%47);cases+=40*28;
 }
 char long_text[65536];memset(long_text,'A',sizeof(long_text));
 os_control_node_t n={.kind=OS_CONTROL_TEXT,.text_length=sizeof(long_text)};
 g_compositor_snapshot.damage_bounds=(Rect){2,3,3,4};
 queries=0;control_paint_text(&n,long_text,-120000,0,0);
 assert(queries==1);
 g_compositor_snapshot.damage_bounds=(Rect){3,3,3,4};
 queries=0;control_paint_text(&n,long_text,0,0,0);assert(!queries);
 g_compositor_snapshot.damage_bounds=(Rect){0,0,40,28};
 queries=0;control_paint_text(&n,long_text,0,100,0);assert(!queries);
 /* 目标字号缓存必须直接使用，不能再次从默认低分辨率字形采样。 */
 static uint8_t native[95*58*96];
 for(unsigned th=20;th<=96;th+=4){
  unsigned tw=th/2;
  for(unsigned c=32;c<=126;++c)for(unsigned y=0;y<th;++y)for(unsigned x=0;x<tw;++x)
   native[((c-32)*th+y)*tw+x]=(uint8_t)(c*17+y*29+x*47);
  const uint32_t under=0x9a174b8e;
  for(unsigned i=0;i<2+40*28;++i)pixels[i]=under;
  os_control_node_t exact={.kind=OS_CONTROL_TEXT,.font_size=th,.text_length=2,.foreground=0xe36291};
  queries=0;control_paint_text(&exact,"AB",0,0,native);assert(!queries);
  for(unsigned y=0;y<28;++y)for(unsigned x=0;x<40;++x){
   uint32_t expected=under;
   if(y<th&&x<tw*2){
    unsigned a=(uint8_t)((unsigned)('A'+x/tw)*17+y*29+(x%tw)*47);
    expected=under&0xff000000U;
    for(unsigned s=0;s<24;s+=8)expected|=(((((under>>s)&255)*(255-a)+((exact.foreground>>s)&255)*a+127)/255)<<s);
   }
   assert(pixels[1+y*40+x]==expected);
  }
  assert(pixels[0]==under&&pixels[1+40*28]==under);
 }
 fw=fh=0;verify(n,0,0,40,28);assert(!queries);
 printf("Control text: %lu pixel oracles, scaling/centering/edges/alpha/extreme origins; 64 KiB clipped text queries one glyph PASS\n",cases);
}
'''
with tempfile.TemporaryDirectory(prefix='control-text-') as directory:
    path = Path(directory)
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
    def compile_test(code):
        (path/'test.c').write_text(code)
        subprocess.run(['cc','-std=c11','-O2','-Wall','-Wextra','-Werror',
                    '-fsanitize=address,undefined','-fno-pie','-no-pie',
                    '-I'+str(root/'include'),str(path/'test.c'),'-o',str(path/'test')],check=True)
    compile_test(program)
    subprocess.run([str(path/'test')],check=True)
    original = 'int64_t first = clip.x0 > text_x ? ((int64_t)clip.x0 - text_x) / target_w : 0;'
    assert original in program
    compile_test(program.replace(original, 'int64_t first = 0;'))
    failed = subprocess.run([str(path/'test')], capture_output=True, text=True, timeout=10)
    assert failed.returncode != 0 and 'queries==1' in failed.stderr, failed
print('负面对照通过：恢复从首字符遍历时，字形查询数量回归会拒绝该实现')
