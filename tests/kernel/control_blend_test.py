"""逐通道独立公式验证真实混色器，覆盖舍入、通道隔离及目标透明度。"""
from pathlib import Path
import resource
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
header = (root/'include/kernel/control_raster.h').read_text()
program = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <kernel/control_raster.h>
static uint32_t reference(uint32_t under, uint32_t source) {
 uint32_t alpha=source>>24U, out=under&0xff000000U;
 for(unsigned channel=0;channel<3;++channel) {
  unsigned shift=channel*8U;
  uint32_t value=(((source>>shift)&255U)*alpha+
      ((under>>shift)&255U)*(255U-alpha)+127U)/255U;
  out|=value<<shift;
 }
 return out;
}
static uint32_t state=17;
static uint32_t next(void) {state^=state<<13;state^=state>>17;state^=state<<5;return state;}
int main(void) {
 /* 穷举任意一个通道的来源、目标与透明度，红蓝同时取极值检验通道间进位。 */
 for(unsigned alpha=0;alpha<256;++alpha)
  for(unsigned source=0;source<256;++source)
   for(unsigned dest=0;dest<256;++dest) {
    uint32_t color=(alpha<<24U)|(source*0x010101U);
    uint32_t under=0x9a000000U|(dest*0x010101U);
    assert(control_blend_argb(under,color)==reference(under,color));
   }
 /* 各通道独立，覆盖来源与目标高字节不同、零和完全不透明的快捷路径。 */
 for(unsigned i=0;i<1000000;++i) {
  uint32_t under=next(),color=next();
  assert(control_blend_argb(under,color)==reference(under,color));
  assert(control_blend_argb(under,color&0xffffffU)==under);
  assert(control_blend_argb(under,color|0xff000000U)==
      ((under&0xff000000U)|(color&0xffffffU)));
 }
 puts("Control blend: 16777216 exhaustive channel cases, 1000000 independent ARGB pairs, preserved destination alpha and transparent/opaque paths PASS");
}
'''
with tempfile.TemporaryDirectory(prefix='control-blend-') as directory:
    path=Path(directory)
    resource.setrlimit(resource.RLIMIT_CORE,(0,0))
    (path/'kernel').mkdir()
    (path/'test.c').write_text(program)
    binary=path/'test'
    def compile_header(text):
        (path/'kernel/control_raster.h').write_text(text)
        subprocess.run(['cc','-std=c11','-O2','-Wall','-Wextra','-Werror',
                        '-fsanitize=address,undefined','-fno-pie','-no-pie',
                        '-I',str(path),'-I',str(root/'include'),str(path/'test.c'),
                        '-o',str(binary)],check=True)
    compile_header(header)
    subprocess.run([str(binary)],check=True)
    for name,broken in {
        'wrong-half-rounding':header.replace('0x00800080U','0x007f007fU'),
        'source-alpha-leak':header.replace('result = under & 0xff000000U','result = color & 0xff000000U'),
    }.items():
        assert broken!=header
        compile_header(broken)
        result=subprocess.run([str(binary)],capture_output=True,text=True)
        assert result.returncode!=0 and 'Assertion' in result.stderr,name
        print('Negative control rejected:',name)
