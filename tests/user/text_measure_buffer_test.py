"""内置应用文字度量必须与服务端 UTF-8 控件单元一致，且不加载独立字库。"""
from pathlib import Path
import subprocess
import tempfile
root=Path(__file__).resolve().parents[2]
source=(root/'user/runtime/text.c').read_text()
a=source.index('uint32_t ui_text_measure_range(')
b=source.index('\n}',a)+2
body=source[a:b]
code=r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <uapi/control_tree.h>
#include <uapi/utf8.h>
#include "user/runtime/text_metrics.h"
static struct {bool font_ready;uint32_t size;} g_text;
'''+body+r'''
int main(void){
 assert(!ui_text_measure_range("打开",6));
 g_text.font_ready=true;
 for(unsigned h=14;h<=96;++h){
  g_text.size=h;
  assert(ui_text_measure_range("打开",6)==2*h);
  assert(ui_text_measure_range("A打开B",8)==2*h+2*(h/2));
  assert(ui_text_measure_range("整理：从左向右",21)==7*h);
  assert(ui_text_measure_range("确认结束进程",18)==6*h);
  assert(ui_text_measure_range("\xf0\x9f\x98\x80",4)==h);
  assert(!ui_text_measure_range(0,8)&&!ui_text_measure_range("",0));
  for(unsigned n=1;n<6;++n)assert(ui_text_measure_range("打开",n)>0);
 }
 char long_text[65536];memset(long_text,'A',sizeof(long_text));
 g_text.size=50;assert(ui_text_measure_range(long_text,sizeof(long_text))==65536*25U);
 puts("Shared text metrics: Chinese/ASCII/emoji, odd sizes, truncated UTF-8 and 64 KiB text without font engine allocations PASS");
}
'''
assert 'bl_' not in body
with tempfile.TemporaryDirectory() as tmp:
 p=Path(tmp);(p/'test.c').write_text(code)
 subprocess.run(['cc','-std=c11','-O2','-Wall','-Wextra','-Werror','-I'+str(root),'-I'+str(root/'include'),str(p/'test.c'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
