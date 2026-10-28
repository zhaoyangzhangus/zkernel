"""Actual TTF parser/raster/cache: initialize each glyph once, exact old pixels."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'kernel/graphics/ascii_font.c').read_text()
parser = (root / 'kernel/graphics/ascii_font_parser.c').read_text().replace(
    'bool ascii_font_parser_decode_simple_glyph(', 'bool real_decode(')
body = source[source.index('static ascii_font_point_t font_midpoint('):
              source.index('bool ascii_font_set_size(')]
body = body.replace('destination[(uint64_t)y * width + x] =',
                    '++raster_writes; destination[(uint64_t)y * width + x] =')
body = body.replace('memset(destination, 0, (size_t)glyph_bytes);',
                    'test_fallback_zero(destination, 0, (size_t)glyph_bytes);')
body = body.replace('memset(', 'test_zero(')
body = body.replace('                    if (font_outline_contains(',
                    '                    ++sample_queries;\n                    if (font_outline_contains(')
fixture = r'''
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ascii_font_internal.h"
#define ASCII_FONT_MAX_DIMENSION 96U
#define ASCII_FONT_MAX_CACHE_BYTES (1024U*1024U)
#define ASCII_FONT_SUPERSAMPLE 4U
static ascii_font_parser_t g_ascii_font_parser;
static unsigned mode,failed_glyphs,allocations,frees;
static size_t raster_writes,cleared_bytes,failed_clear_bytes,sample_queries,reference_queries;
static bool fail_allocation;
static void *kmalloc(size_t n,unsigned flags) {
 assert(!flags);if(fail_allocation)return NULL;
 uint8_t *p=malloc(n+32);assert(p);memset(p,0xa5,n+32);
 memcpy(p,&n,sizeof(n));++allocations;return p+16;
}
static void kfree(void *opaque) {
 uint8_t *p=(uint8_t*)opaque-16;size_t n;memcpy(&n,p,sizeof(n));
 for(unsigned i=sizeof(n);i<16;++i)assert(p[i]==0xa5);
 for(unsigned i=0;i<16;++i)assert(p[n+16+i]==0xa5);
 ++frees;free(p);
}
static void test_zero(void *p,int value,size_t n) { assert(!value);cleared_bytes+=n;memset(p,value,n); }
static void test_fallback_zero(void *p,int value,size_t n) { failed_clear_bytes+=n;test_zero(p,value,n); }
'''
decode = r'''
bool ascii_font_parser_decode_simple_glyph(const ascii_font_parser_t *p,uint16_t glyph,
 ascii_font_point_t *points,uint16_t *ends,uint32_t *point_count,uint32_t *contour_count) {
 if(mode>=3) {
  unsigned shape=mode-3;
  if(shape==0){*point_count=*contour_count=0;return true;}
  int32_t dx=((int32_t)(shape%5)-2)*1300;
  int32_t dy=((int32_t)(shape/5)-1)*1300;
  points[0]=(ascii_font_point_t){dx,dy,true};
  points[1]=(ascii_font_point_t){dx+700,dy+1200,(shape&1)!=0};
  points[2]=(ascii_font_point_t){dx+1200,dy,true};
  points[3]=(ascii_font_point_t){dx+700,dy-500,true};
  *point_count=4;*contour_count=1;ends[0]=3;return true;
 }
 bool ok=mode==2 || (mode==1 && glyph%5==0)?false:
   real_decode(p,glyph,points,ends,point_count,contour_count);
 if(!ok)++failed_glyphs;
 return ok;
}
'''
reference_curve = r'''
static void reference_test_line(ascii_font_point_t from,ascii_font_point_t to,
                               int32_t x,int32_t y,bool *inside) {
 if((from.y>y)==(to.y>y) || from.y==to.y)return;
 int64_t intersection=(int64_t)from.x+
  ((int64_t)(y-from.y)*(to.x-from.x))/(to.y-from.y);
 if(intersection>x)*inside=!*inside;
}
/* Keep the original curve subdivision: production early-outs must not
 * silently enter the full-font pixel compatibility oracle. */
static void reference_test_quadratic(ascii_font_point_t from,
 ascii_font_point_t control,ascii_font_point_t to,int32_t x,int32_t y,bool *inside) {
 ascii_font_point_t previous=from;
 for(uint32_t step=1;step<=8;++step) {
  int64_t t=(int64_t)step*65536/8,u=65536-t;
  ascii_font_point_t current={
   (int32_t)((u*u*from.x+2*u*t*control.x+t*t*to.x)>>32),
   (int32_t)((u*u*from.y+2*u*t*control.y+t*t*to.y)>>32),true};
  reference_test_line(previous,current,x,y,inside);previous=current;
 }
}
'''
reference_outline = source[source.index('static bool font_outline_contains('):
                           source.index('static bool font_rasterize_from_parser(')]
reference_outline = reference_outline.replace('font_outline_contains', 'reference_outline_contains').replace(
    'font_test_quadratic', 'reference_test_quadratic').replace('font_test_line', 'reference_test_line')
reference_raster = r'''
/* Frozen pre-clipping rasterizer: do not derive its pixel traversal from
 * the optimized implementation or the oracle would share clipping bugs. */
static bool reference_rasterize_glyph(uint16_t glyph,uint8_t *destination,
                                      uint32_t width,uint32_t height) {
 ascii_font_point_t points[ASCII_FONT_MAX_POINTS];uint16_t ends[ASCII_FONT_MAX_CONTOURS];
 uint32_t count=0,contours=0;
 int32_t em_height=(int32_t)g_ascii_font_parser.ascender-(int32_t)g_ascii_font_parser.descender;
 int32_t scale=(int32_t)(((uint64_t)height<<16U)/(uint32_t)em_height);
 int64_t advance=(int64_t)ascii_font_parser_advance_width(&g_ascii_font_parser,glyph)*scale;
 int64_t origin=((int64_t)width*65536-advance)/2+
   (int64_t)ascii_font_parser_left_side_bearing(&g_ascii_font_parser,glyph)*scale;
 int64_t baseline=(int64_t)height*65536+(int64_t)g_ascii_font_parser.descender*scale;
 if(!ascii_font_parser_decode_simple_glyph(&g_ascii_font_parser,glyph,points,ends,&count,&contours))return false;
 for(uint32_t i=0;i<count;++i){
  points[i].x=(int32_t)(origin+(int64_t)points[i].x*scale);
  points[i].y=(int32_t)(baseline-(int64_t)points[i].y*scale);
 }
 for(uint32_t y=0;y<height;++y)for(uint32_t x=0;x<width;++x){
  uint32_t coverage=0;
  for(uint32_t sy=0;sy<4;++sy)for(uint32_t sx=0;sx<4;++sx){
   int32_t px=(int32_t)x*65536+(int32_t)((sx*2U+1U)*65536U/8U);
   int32_t py=(int32_t)y*65536+(int32_t)((sy*2U+1U)*65536U/8U);
   ++reference_queries;
   if(reference_outline_contains(points,ends,contours,px,py))++coverage;
  }
  destination[(uint64_t)y*width+x]=(uint8_t)(coverage*255U/16U);
 }
 return true;
}
'''
checks = r'''
/* The prior whole-cache-clear implementation, using the unchanged actual
 * parser/rasterizer, is the pixel compatibility oracle. */
static void old_cache(unsigned width,unsigned height,uint8_t **out,size_t *size) {
 size_t glyph_bytes=(size_t)width*height;*size=glyph_bytes*95;
 *out=kmalloc(*size,0);memset(*out,0,*size);
 for(unsigned character=ASCII_FONT_FIRST;character<=ASCII_FONT_LAST;++character) {
  uint16_t glyph=ascii_font_parser_glyph_for_code(&g_ascii_font_parser,character);
  (void)reference_rasterize_glyph(glyph,*out+(character-ASCII_FONT_FIRST)*glyph_bytes,width,height);
 }
}
int main(int argc,char **argv) {
 (void)test_zero;
 (void)test_fallback_zero;
 assert(argc==2);FILE *file=fopen(argv[1],"rb");assert(file);
 assert(!fseek(file,0,SEEK_END));long n=ftell(file);assert(n>0);rewind(file);
 uint8_t *data=malloc((size_t)n);assert(data && fread(data,1,(size_t)n,file)==(size_t)n);fclose(file);
 assert(ascii_font_parser_init(&g_ascii_font_parser,data,(size_t)n));
 unsigned sizes[][2]={{1,1},{1,64},{64,1},{8,16},{12,24},{16,32}},cases=0;
 size_t omitted=0,total_samples=0,total_reference=0;
 for(unsigned s=0;s<6;++s)for(mode=0;mode<3;++mode) {
  unsigned w=sizes[s][0],h=sizes[s][1];uint8_t *actual=NULL,*expected=NULL;
  size_t actual_size=0,expected_size=0;raster_writes=cleared_bytes=failed_clear_bytes=failed_glyphs=0;
  sample_queries=reference_queries=0;
  assert(font_build_cache(w,h,&actual,&actual_size));
  assert(actual_size==(size_t)w*h*95);
  assert(failed_clear_bytes==(size_t)failed_glyphs*w*h);
  assert(raster_writes+cleared_bytes==actual_size);
  omitted+=actual_size-failed_clear_bytes;
  old_cache(w,h,&expected,&expected_size);
  assert(actual_size==expected_size && !memcmp(actual,expected,actual_size));
  assert(sample_queries<=reference_queries);
  total_samples+=sample_queries;total_reference+=reference_queries;
  kfree(actual);kfree(expected);assert(allocations==frees);++cases;
 }
 for(mode=3;mode<23;++mode)for(unsigned s=0;s<6;++s){
  unsigned w=sizes[s][0],h=sizes[s][1];size_t n=(size_t)w*h;
  uint8_t *a=kmalloc(n,0),*b=kmalloc(n,0);
  memset(b,0,n);raster_writes=cleared_bytes=sample_queries=reference_queries=0;
  assert(font_rasterize_glyph(0,a,w,h));
  assert(reference_rasterize_glyph(0,b,w,h));
  assert(!memcmp(a,b,n) && raster_writes+cleared_bytes==n && sample_queries<=reference_queries);
  if(mode==3)assert(!sample_queries);
  kfree(a);kfree(b);
 }
 assert(total_samples<total_reference);
 mode=2;uint8_t *actual;size_t size;
 raster_writes=cleared_bytes=failed_glyphs=0;
 assert(font_build_cache(64,64,&actual,&size));
 assert(size==64U*64U*95U && cleared_bytes==size && !raster_writes && failed_glyphs==95);
 for(size_t i=0;i<size;++i)assert(!actual[i]);
 kfree(actual);
 unsigned before=allocations;actual=(uint8_t*)1;size=999;
 fail_allocation=true;assert(!font_build_cache(8,16,&actual,&size));
 assert(actual==(uint8_t*)1 && size==999 && allocations==before);fail_allocation=false;
 assert(!font_build_cache(0,16,&actual,&size));assert(!font_build_cache(8,0,&actual,&size));
 assert(font_build_cache(48,96,&actual,&size));kfree(actual);actual=(uint8_t*)1;size=999;
 assert(!font_build_cache(97,16,&actual,&size));assert(!font_build_cache(8,97,&actual,&size));
 assert(!font_build_cache(8,16,NULL,&size));assert(!font_build_cache(8,16,&actual,NULL));
 const uint8_t *saved=g_ascii_font_parser.file;g_ascii_font_parser.file=NULL;
 assert(!font_build_cache(8,16,&actual,&size));g_ascii_font_parser.file=saved;
 assert(allocations==frees && actual==(uint8_t*)1 && size==999);
 free(data);
 printf("%u actual-font and 120 synthetic empty/clipped/curved comparisons: exact pixels/initialization/guards, decode fallback and OOM passed; samples %zu -> %zu, %zu redundant byte clears removed\n",cases,total_reference,total_samples,omitted);
}
'''
with tempfile.TemporaryDirectory(prefix='liteos-font-init-') as directory:
    path = Path(directory)
    def run(actual):
        (path / 'test.c').write_text(fixture+parser+decode+actual+reference_curve+reference_outline+reference_raster+checks)
        subprocess.run(['cc', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                        '-I'+str(root/'include'), '-I'+str(root/'kernel/graphics'),
                        '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
                        str(path/'test.c'), '-o', str(path/'test')], check=True)
        return subprocess.run([str(path/'test'),str(root/'assets/fonts/default.ttf')],
                              capture_output=True,text=True,timeout=45)
    result=run(body)
    assert result.returncode==0,result.stdout+result.stderr
    print(result.stdout.strip())
    negative=body.replace('test_fallback_zero(destination, 0, (size_t)glyph_bytes);','(void)destination;')
    result=run(negative)
    assert result.returncode!=0 and 'failed_clear_bytes==' in result.stderr,result.stderr
    print('Negative control rejected: failed glyphs expose poisoned allocation bytes')
    negative=body.replace('    for (uint32_t character = ASCII_FONT_FIRST;',
                          '    test_zero(scaled, 0, (size_t)cache_bytes);\n    for (uint32_t character = ASCII_FONT_FIRST;')
    result=run(negative)
    assert result.returncode!=0 and 'raster_writes+cleared_bytes' in result.stderr,result.stderr
    print('Negative control rejected: redundant whole-cache clearing')
    negative=body.replace('(uint32_t)max_x / 65536U + 1U', '(uint32_t)max_x / 65536U')
    result=run(negative)
    assert result.returncode!=0 and 'memcmp' in result.stderr,result.stderr
    print('Negative control rejected: clipped boundary pixels differ from the original rasterizer')
