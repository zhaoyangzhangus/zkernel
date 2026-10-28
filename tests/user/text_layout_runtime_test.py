"""控件树文字度量与合成器一致，且缩放不依赖应用自绘字体。"""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
program = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "user/runtime/text.h"
#include <uapi/control_tree.h>

int main(void) {
    assert(!ui_text_size() && !ui_text_advance() && !ui_text_line_height());
    assert(!ui_text_adjust(1) && !ui_text_measure("abc"));
    assert(!ui_text_init(OS_TEXT_MIN_SIZE-1U));
    assert(!ui_text_init(OS_TEXT_MAX_SIZE+1U));
    for (uint32_t size=OS_TEXT_MIN_SIZE;size<=OS_TEXT_MAX_SIZE;++size) {
        assert(ui_text_init(size) && ui_text_size()==size);
        assert(ui_text_line_height()==size+2U);
        assert(ui_text_advance()==os_control_text_cell_width(' ',size));
        assert(ui_text_measure("A打开B")==2U*size+2U*(size/2U));
        assert(ui_text_measure("\xf0\x9f\x98\x80")==size);
        assert(ui_text_measure_range("打开abc",6U)==2U*size);
        assert(!ui_text_measure(0) && !ui_text_measure_range(0,12U));
        assert(!ui_text_measure_range("x",0));
        for (size_t n=1;n<6;++n) assert(ui_text_measure_range("打开",n)>0);
        assert(!ui_text_init(OS_TEXT_MAX_SIZE+1U) && ui_text_size()==size);
        assert(!ui_text_adjust(0) && ui_text_size()==size);
        uint32_t higher=size>=OS_TEXT_MAX_SIZE-OS_TEXT_SIZE_STEP ?
            OS_TEXT_MAX_SIZE : size+OS_TEXT_SIZE_STEP;
        assert(ui_text_adjust(1)==(higher!=size) && ui_text_size()==higher);
        assert(ui_text_init(size));
        uint32_t lower=size<=OS_TEXT_MIN_SIZE+OS_TEXT_SIZE_STEP ?
            OS_TEXT_MIN_SIZE : size-OS_TEXT_SIZE_STEP;
        assert(ui_text_adjust(-1)==(lower!=size) && ui_text_size()==lower);
    }
    char long_text[65536];
    memset(long_text,'A',sizeof(long_text));
    assert(ui_text_init(50));
    assert(ui_text_measure_range(long_text,sizeof(long_text))==65536U*25U);
    ui_text_shutdown();
    assert(!ui_text_size() && !ui_text_advance() && !ui_text_line_height());
    assert(!ui_text_adjust(-1) && !ui_text_measure("abc"));
    puts("Control-tree text: size bounds, zoom, Unicode, metrics and reset PASS");
}
'''

with tempfile.TemporaryDirectory(prefix='text-layout-') as directory:
    work = Path(directory)
    (work/'test.c').write_text(program)
    subprocess.run(['cc','-std=c11','-O2','-Wall','-Wextra','-Werror',
                    '-fsanitize=address,undefined','-fno-pie','-no-pie',
                    '-I'+str(root),'-I'+str(root/'include'),str(work/'test.c'),
                    str(root/'user/runtime/text_layout.c'),'-o',str(work/'test')],check=True)
    subprocess.run([str(work/'test')],check=True)
    subprocess.run(['cc','-std=c11','-O2','-ffreestanding','-fno-builtin',
                    '-I'+str(root/'include'),'-c',str(root/'user/runtime/text_layout.c'),
                    '-o',str(work/'layout.o')],check=True)
    symbols=subprocess.check_output(['nm','-u',str(work/'layout.o')],text=True)
    assert not any(name in symbols for name in ('bl_','malloc','calloc','realloc','open','read'))
