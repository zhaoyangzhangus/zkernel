"""真实字形绘制的像素对照与索引访问量，覆盖整段排除及长文字局部重绘。"""
from pathlib import Path
import resource
import subprocess
import tempfile

root=Path(__file__).resolve().parents[2]
header=(root/'include/kernel/control_text.h').read_text()
header=header.replace('    while (first<last) {','    while (first<last) {\n        ++search_visits;',1)
header=header.replace('    for (uint32_t i=first;i<run->count;++i) {',
    '    for (uint32_t i=first;i<run->count;++i) {\n        ++glyph_visits;',1)
program=r'''
#include <assert.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
static unsigned search_visits,glyph_visits;
#include <kernel/control_text.h>
#define STRIDE 80U
#define HEIGHT 48U
static uint32_t frame[STRIDE*HEIGHT+2];
static control_text_cell_t cells[65536];
static control_text_glyph_t glyphs[32];
static uint8_t ink[32*24*HEIGHT];
static uint32_t seed=71;
static uint32_t next(void){seed^=seed<<13;seed^=seed>>17;seed^=seed<<5;return seed;}
static uint32_t reference(uint32_t under,uint32_t color,unsigned alpha){
 uint32_t out=under&0xff000000U;
 for(unsigned shift=0;shift<24;shift+=8)
  out|=(((((under>>shift)&255U)*(255U-alpha)+((color>>shift)&255U)*alpha+127U)/255U)<<shift);
 return out;
}
static void verify(const control_text_run_t *run,int64_t x,int64_t y,Rect clip){
 const uint32_t color=0x8a73c190U;
 for(unsigned i=0;i<STRIDE*HEIGHT+2;++i)frame[i]=0x9a174b8eU;
 control_raster_target_t target={frame+1,STRIDE,clip};
 search_visits=glyph_visits=0;control_text_draw(&target,run,x,y,color);
 bool outside=rect_is_empty(clip)||x>=clip.x1||x+run->width<=clip.x0||y>=clip.y1||y+run->height<=clip.y0;
 if(outside)assert(!search_visits&&!glyph_visits);
 for(unsigned py=0;py<HEIGHT;++py)for(unsigned px=0;px<STRIDE;++px){
  uint32_t expected=0x9a174b8eU;
  if((int)px>=clip.x0&&(int)px<clip.x1&&(int)py>=clip.y0&&(int)py<clip.y1&&
     py>=y&&(int64_t)py<y+run->height){
   int64_t left=x;
   for(unsigned i=0;i<run->count;++i){
    const control_text_glyph_t *glyph=run->glyphs+run->cells[i].glyph;
    if(px>=left&&(int64_t)px<left+glyph->width){
     unsigned alpha=run->pixels[glyph->offset+(py-y)*glyph->width+(px-left)];
     expected=reference(expected,color,alpha);break;
    }
    left+=glyph->width;
   }
  }
  assert(frame[1+py*STRIDE+px]==expected);
 }
 assert(frame[0]==0x9a174b8eU&&frame[STRIDE*HEIGHT+1]==0x9a174b8eU);
}
int main(void){
 control_text_run_t run={.cells=cells,.glyphs=glyphs,.pixels=ink};
 for(unsigned sample=0;sample<1200;++sample){
  run.count=next()%33;run.height=1+next()%HEIGHT;run.width=0;
  unsigned used=0;
  for(unsigned i=0;i<run.count;++i){
   unsigned width=1+next()%24;
   glyphs[i]=(control_text_glyph_t){.width=width,.offset=used};
   for(unsigned p=0;p<width*run.height;++p)ink[used+p]=(uint8_t)next();
   used+=width*run.height;run.width+=width;
   cells[i]=(control_text_cell_t){.end=run.width,.glyph=i,.byte_offset=i};
  }
  int64_t x=(int)(next()%180)-100,y=(int)(next()%100)-60;
  if(sample%11==0)x=(int64_t)INT32_MIN-100;
  if(sample%13==0)x=(int64_t)INT32_MAX+100;
  Rect clip={next()%8,next()%8,8+next()%57,8+next()%41};
  verify(&run,x,y,clip);
 }
 /* 64 KiB 字符索引只保留重复字形，单像素损坏不能扫描整段文字。 */
 glyphs[0]=(control_text_glyph_t){.width=8,.offset=0};
 glyphs[1]=(control_text_glyph_t){.width=16,.offset=8*24};
 for(unsigned i=0;i<24*24;++i)ink[i]=(uint8_t)i;
 run.count=65536;run.width=0;run.height=24;
 for(unsigned i=0;i<run.count;++i){run.width+=i&1?16:8;cells[i]=(control_text_cell_t){run.width,i&1,i};}
 Rect clip={30,10,31,11};
 search_visits=glyph_visits=0;
 control_raster_target_t target={frame+1,STRIDE,clip};
 control_text_draw(&target,&run,31,0,0xff123456U);assert(!search_visits&&!glyph_visits);
 control_text_draw(&target,&run,30-(int64_t)run.width,0,0xff123456U);assert(!search_visits&&!glyph_visits);
 control_text_draw(&target,&run,31-(int64_t)run.width,0,0xff123456U);
 assert(search_visits<=17&&glyph_visits==1);
 printf("Text clip: 4608000 pixel oracles, mixed widths/alpha/padding/extreme origins; 64 KiB outside=0 index visits, single-pixel search=%u glyphs=%u PASS\n",search_visits,glyph_visits);
}
'''
with tempfile.TemporaryDirectory(prefix='control-text-clip-') as directory:
    path=Path(directory)
    resource.setrlimit(resource.RLIMIT_CORE,(0,0))
    (path/'kernel').mkdir()
    (path/'kernel/control_text.h').write_text(header)
    (path/'test.c').write_text(program)
    subprocess.run(['cc','-std=c11','-O2','-Wall','-Wextra','-Werror',
                    '-fsanitize=address,undefined','-fno-pie','-no-pie',
                    '-I',str(path),'-I',str(root/'include'),str(path/'test.c'),'-o',str(path/'test')],check=True)
    subprocess.run([str(path/'test')],check=True)

    for name,condition in {
        'missing-right-reject':'x>=clip.x1 || ',
        'missing-left-reject':'x+run->width<=clip.x0 ||',
    }.items():
        assert header.count(condition)==1
        (path/'kernel/control_text.h').write_text(header.replace(condition,'',1))
        subprocess.run(['cc','-std=c11','-O2','-Wall','-Wextra','-Werror',
                        '-fsanitize=address,undefined','-fno-pie','-no-pie',
                        '-I',str(path),'-I',str(root/'include'),str(path/'test.c'),'-o',str(path/'test')],check=True)
        result=subprocess.run([str(path/'test')],capture_output=True,text=True)
        assert result.returncode!=0 and 'search_visits' in result.stderr,name
        print('Negative control rejected:',name)
