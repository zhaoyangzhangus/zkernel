"""Actual quadratic helper versus its original eight-segment algorithm."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'kernel/graphics/ascii_font.c').read_text()
body = source[source.index('static void font_test_line('):
              source.index('static bool font_outline_contains(')]
body = body.replace('    intersection =', '    ++intersections;\n    intersection =')
body = body.replace('        font_test_line(previous, current,',
                    '        ++segments; font_test_line(previous, current,')
fixture = r'''
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include "ascii_font_internal.h"
static unsigned long segments,intersections;
'''
oracle = r'''
/* 固定使用原始细分和交点计算，独立于生产代码的提前退出和线段辅助函数。
 * 所有坐标都在支持的光栅范围内。 */
static void reference(ascii_font_point_t a,ascii_font_point_t b,
                      ascii_font_point_t c,int32_t x,int32_t y,bool *inside) {
 ascii_font_point_t previous=a;
 for(unsigned step=1;step<=8;++step) {
  int64_t t=(int64_t)step*65536/8,u=65536-t;
  ascii_font_point_t next={
   (int32_t)((u*u*a.x+2*u*t*b.x+t*t*c.x)>>32),
   (int32_t)((u*u*a.y+2*u*t*b.y+t*t*c.y)>>32),true};
  if((previous.y>y)!=(next.y>y) && previous.y!=next.y) {
   int64_t crossing=(int64_t)previous.x+
    ((int64_t)(y-previous.y)*(next.x-previous.x))/(next.y-previous.y);
   if(crossing>x)*inside=!*inside;
  }
  previous=next;
 }
}
static uint32_t state=0x17598432;
static int32_t coordinate(void) {
 state=state*1664525U+1013904223U;
 return (int32_t)(state%16777217U)-8388608;
}
static unsigned long cases,unskipped;
static unsigned long line_cases,line_old_divisions,line_new_divisions;
static void check_line(ascii_font_point_t a,ascii_font_point_t b,int32_t x,int32_t y) {
 for(unsigned initial=0;initial<2;++initial) {
  bool actual=initial,expected=initial;
  unsigned long before=intersections;
  font_test_line(a,b,x,y,&actual);
  line_new_divisions+=intersections-before;
  if((a.y>y)!=(b.y>y) && a.y!=b.y) {
   ++line_old_divisions;
   int64_t crossing=(int64_t)a.x+((int64_t)(y-a.y)*(b.x-a.x))/(b.y-a.y);
   if(crossing>x)expected=!expected;
  }
  assert(actual==expected);++line_cases;
 }
}
static void check(ascii_font_point_t a,ascii_font_point_t b,
                  ascii_font_point_t c,int32_t x,int32_t y) {
 for(unsigned initial=0;initial<2;++initial) {
  bool actual=initial,expected=initial;
  unsigned long before=segments;
  font_test_quadratic(a,b,c,x,y,&actual);
  reference(a,b,c,x,y,&expected);
  assert(actual==expected);
  assert(segments-before==0 || segments-before==8);
  unskipped+=8;++cases;
 }
}
int main(void) {
 for(unsigned i=0;i<100000;++i) {
  ascii_font_point_t a={coordinate(),coordinate(),true};
  ascii_font_point_t b={coordinate(),coordinate(),false};
  ascii_font_point_t c={coordinate(),coordinate(),true};
  check(a,b,c,coordinate(),coordinate());
  /* 精确顶点、左右各一个定点单位以及退化情况。 */
  check(a,b,c,a.x,a.y);check(a,b,c,b.x,b.y);check(a,b,c,c.x,c.y);
  check(a,b,c,b.x-1,b.y-1);check(a,b,c,b.x+1,b.y+1);
  check(a,a,a,a.x,a.y);check(a,b,a,a.x,b.y);
  check_line(a,b,coordinate(),coordinate());
  check_line(b,a,a.x,a.y);check_line(a,b,b.x,b.y);
  check_line(a,b,a.x-1,a.y);check_line(a,b,a.x+1,a.y);
  check_line(a,a,a.x,a.y);check_line(a,b,b.x,b.y-1);
  check_line(a,b,b.x,b.y+1);
 }
 assert(segments<unskipped && intersections);
 assert(line_new_divisions<line_old_divisions);
 printf("%lu line parity/boundary/degenerate comparisons passed; divisions %lu -> %lu\n",
        line_cases,line_old_divisions,line_new_divisions);
 printf("%lu curve parity/boundary/degenerate comparisons passed; subdivisions %lu -> %lu\n",
        cases,unskipped,segments);
}
'''
with tempfile.TemporaryDirectory(prefix='liteos-font-curve-') as directory:
    path = Path(directory)
    def run(actual, sanitize=False):
        (path/'test.c').write_text(fixture+actual+oracle)
        subprocess.run(['cc', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                        *(['-fsanitize=undefined', '-fno-sanitize-recover=all'] if sanitize else []),
                        '-I'+str(root/'include'), '-I'+str(root/'kernel/graphics'),
                        str(path/'test.c'), '-o', str(path/'test')], check=True)
        return subprocess.run([str(path/'test')], capture_output=True, text=True)
    result = run(body)
    assert result.returncode == 0, result.stderr
    print(result.stdout.strip())
    result = run(body, sanitize=True)
    assert result.returncode == 0, result.stderr
    print('Undefined-behavior sanitizer passed: line and curve coordinate arithmetic')
    negative = body.replace('from.x <= sample_x && control.x <= sample_x && to.x <= sample_x',
                            'from.x <= sample_x || control.x <= sample_x || to.x <= sample_x')
    result = run(negative)
    assert result.returncode != 0 and 'actual==expected' in result.stderr, result.stderr
    print('Negative control rejected: unsafe partial curve bounds change parity')
    negative = body.replace('from.x > sample_x && to.x > sample_x',
                            'from.x >= sample_x && to.x >= sample_x')
    result = run(negative)
    assert result.returncode != 0 and 'actual==expected' in result.stderr, result.stderr
    print('Negative control rejected: non-strict endpoint bounds change parity')
