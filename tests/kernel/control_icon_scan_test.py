"""图标扫描坐标精确递推，覆盖任意裁剪起点和接近整数上限的尺寸。"""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "icons.h"
static uint32_t random_state=0x93a5c7e1;
static uint32_t random_value(void) {
 random_state^=random_state<<13;random_state^=random_state>>17;
 random_state^=random_state<<5;return random_state;
}
static void axis_case(uint32_t size,uint32_t start,uint32_t length) {
 control_icon_axis_t axis;
 assert(control_icon_axis_start(&axis,start,size));
 for(uint32_t n=0;n<length;++n) {
  for(uint32_t i=0;i<4;++i) {
   uint64_t numerator=((uint64_t)(start+n)*8+i*2+1)*128;
   assert(axis.samples[i]==numerator/size);
   assert(axis.remainders[i]==numerator%size);
  }
  control_icon_axis_next(&axis);
 }
}
int main(void) {
 control_icon_axis_t axis,saved;
 memset(&axis,0x5a,sizeof(axis));saved=axis;
 assert(!control_icon_axis_start(0,0,1));
 assert(!control_icon_axis_start(&axis,0,0));
 assert(!control_icon_axis_start(&axis,3,3));
 assert(!memcmp(&axis,&saved,sizeof(axis)));
 unsigned cases=0;
 for(uint32_t size=1;size<=1025;++size) {
  axis_case(size,0,size);++cases;
  for(unsigned j=0;j<4;++j) {
   uint32_t start=random_value()%size;
   axis_case(size,start,size-start);++cases;
  }
 }
 for(unsigned i=0;i<20000;++i) {
  uint32_t size=random_value()|1U,start=random_value()%size;
  uint32_t length=size-start;
  if(length>128)length=128;
  axis_case(size,start,length);++cases;
 }
 for(uint32_t size=UINT32_MAX-10;size<UINT32_MAX;++size) {
  axis_case(size,size-64,64);++cases;
 }
 /* 和原单点覆盖率逐像素对照，非零裁剪起点不能改变几何位置。 */
 unsigned sizes[]={1,7,16,17,31,48,63,96,127};
 for(unsigned s=0;s<sizeof(sizes)/sizeof(*sizes);++s) {
  unsigned size=sizes[s];
  for(unsigned symbol=OS_CONTROL_SYMBOL_BACK;symbol<=OS_CONTROL_SYMBOL_FILE;++symbol)
   for(unsigned clipped=0;clipped<2;++clipped) {
    unsigned start=clipped?size/3:0;
    control_icon_axis_t rows,columns;
    assert(control_icon_axis_start(&rows,start,size));
    assert(control_icon_axis_start(&columns,start,size));
    for(unsigned y=start;y<size;++y) {
     control_icon_axis_t cursor=columns;
     for(unsigned x=start;x<size;++x) {
      assert(control_icon_samples_alpha(symbol,cursor.samples,rows.samples)==
       control_symbol_alpha(symbol,x,y,size));
      control_icon_axis_next(&cursor);
     }
     control_icon_axis_next(&rows);
    }
   }
 }
 printf("icon scan: %u axis cases, clipped masks equal scalar coverage, bounds passed\n",cases);
}
'''
with tempfile.TemporaryDirectory(prefix='control-icon-scan-') as directory:
    path = Path(directory)
    header = (root / 'include/kernel/control_icons.h').read_text()
    (path / 'icons.h').write_text(header)
    (path / 'test.c').write_text(source)
    command = ['cc', '-O2', '-Wall', '-Wextra', '-Werror',
               '-fsanitize=undefined', '-I' + str(root / 'include'),
               str(path / 'test.c'), '-o', str(path / 'test')]
    subprocess.run(command, check=True)
    subprocess.run([str(path / 'test')], check=True)
    # 丢失余数进位会让非整数缩放出现错位；基准必须能拒绝此类退化。
    broken = header.replace('++axis->samples[i];', '/* 故意遗漏进位。 */')
    assert broken != header
    (path / 'icons.h').write_text(broken)
    subprocess.run(command, check=True)
    result = subprocess.run([str(path / 'test')], capture_output=True)
    assert result.returncode != 0, '未拒绝余数进位错误'
    print('Negative control rejected: missing coordinate carry changes icon coverage')
